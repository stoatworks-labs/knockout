"""The demo's copies of the plugin must be the plugin, character for character.

    python3 demo/tools/check_shaders.py           compare; exit 1 on any drift
    python3 demo/tools/check_shaders.py --write   regenerate demo/shaders.js

Called from `tools/verify.sh`. Exit code 1 means a copy has drifted.

------------------------------------------------------------------- why

The page at knockout-demo.stoatworks-labs.com carries three things that are
copies, and copies drift quietly -- a mask whose distance has lost a term, or
whose winding counts a graze, still looks like letters.

1. **The GLSL.** `demo/shaders.js` holds the five GLSL pieces of
   `source/Shaders.cpp` (kCommon, kGlyphVertex, kGlyphFragment, kQuadVertex,
   kCompositeFragment) plus kVersion, AND the order the plugin joins them in:
   Knockout.cpp's InitGL builds each vertex stage as kVersion + its piece and
   hands each fragment stage to `Assemble()`, which is kVersion + kCommon + the
   pieces. This script reads that order out of the C++ and writes it into
   shaders.js as `PROGRAMS`. The page builds its expected text for every stage
   from that table, and at run time REQUIRES the text the compiled plugin hands
   to glShaderSource to equal one of them before anything is applied to it
   (demo/wasm/gl_shim.cpp, plugin.js's shaderSource). The pieces are compared
   exactly -- no whitespace normalisation, no comment stripping: a comment
   updated on one side only is drift worth catching. Then:
     - every `const char* const k... = R"(` piece in Shaders.cpp must be one
       this script knows, so a new piece cannot be added to the plugin and
       silently left out of the page;
     - every `.Compile(` in Knockout.cpp must be read into PROGRAMS, and
       Assemble() must still be kVersion + kCommon + its pieces in order;
     - and shaders.js must be byte for byte what `--write` produces, so
       nothing else can be typed into it.
   The one transformation is a decode: a backtick cannot sit raw in a template
   literal, so shaders.js would escape it as \\`. Any other backslash, or a
   `${`, on the JS side is rejected; the C++ bodies hold neither.

2. **The WebAssembly.** `demo/knockout-core.wasm` is the plugin's C++ compiled
   by `demo/tools/build-wasm.sh`, and committed, because the deploy has no
   build step. Building it needs emscripten, which verify.sh cannot assume, so
   this checks the next best thing: `demo/wasm/inputs.sha256`, written by the
   build, records the hash of every file that went in (every source/*.h, the
   eight plugin .cpp files, stb_truetype.h, the SDK files, the glue, the shim
   and the build script) and of both outputs, and the SDK commit. A source file
   changed since the last build -- or an output that is not the one the build
   wrote -- fails here, and the fix is to rerun build-wasm.sh.

3. **The fonts.** The page has no installed fonts, so it ships its own in
   demo/fonts/, listed in demo/fonts/fonts.json with each file's hash and its
   licence. Every listed file must be there with that hash, every font file
   there must be listed, and every family's folder must hold the licence the
   manifest names: a font served without its licence is a licence broken.

------------------------------------------------------------------- what it cannot

It cannot tell whether the .wasm really is what those inputs compile to (only
rebuilding can), and it says nothing about demo/plugin.js, the page around the
plugin: its panel is built from the plugin's own declarations at run time, but
its read-out units, its tooltips, its layout and its texture-buffer rewrite of
the GLSL (which happens after the check above, on the page) are the page's.
"""
import hashlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.dont_write_bytecode = True

# JS constant, C++ symbol (all in source/Shaders.cpp), in the order they are
# declared there. kVersion is a plain string and is handled on its own.
SHADERS = [
    ("COMMON", "kCommon"),
    ("GLYPH_VERTEX", "kGlyphVertex"),
    ("GLYPH_FRAGMENT", "kGlyphFragment"),
    ("QUAD_VERTEX", "kQuadVertex"),
    ("COMPOSITE_FRAGMENT", "kCompositeFragment"),
]
JS_NAME = dict((symbol, name) for name, symbol in SHADERS)
JS_NAME["kVersion"] = "VERSION"

HEADER = """// GENERATED from source/Shaders.cpp by demo/tools/check_shaders.py --write.
// Do not edit: tools/verify.sh fails if a character of this differs from the
// plugin's. The one escape is \\` for a backtick inside a comment.
"""

MANIFEST = os.path.join("demo", "wasm", "inputs.sha256")
OUTPUTS = ("demo/knockout-core.js", "demo/knockout-core.wasm")
FONTS = os.path.join("demo", "fonts")
FONT_EXTENSIONS = (".ttf", ".otf", ".ttc", ".otc")


class Stale(Exception):
    """The C++ is not shaped the way this script reads it: fail, never guess."""


def read(*parts):
    with open(os.path.join(REPO, *parts)) as handle:
        return handle.read()


def cpp_version(source):
    match = re.search(r'const char\* const kVersion = "(.*?)";', source)
    return None if match is None else match.group(1)


def from_cpp(source, symbol):
    match = re.search(r'const char\* const ' + symbol + r' = R"\((.*?)\)";', source, re.S)
    return None if match is None else match.group(1)


def from_js(source, name):
    match = re.search(r'^export const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None
    body = match.group(1)
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        line = body[: stray.start()].count("\n") + 1
        return None, f"backslash that is not an escaped backtick, at line {line}"
    if "${" in body:
        return None, "template substitution"
    return body.replace("\\`", "`"), None


def unknown_pieces(cpp):
    """Raw-string pieces in Shaders.cpp that this script does not copy."""
    declared = re.findall(r'const char\* const (k\w+) = R"\(', cpp)
    return [symbol for symbol in declared if symbol not in JS_NAME]


def programs(cpp, plugin_cpp):
    """The plugin's own assembly, from Assemble() and InitGL, in JS names:
    [(program, vertex pieces, fragment pieces)]."""
    body = re.search(r"std::string Assemble\( std::initializer_list< const char\* > pieces \)\s*\{(.*?)\n\}", cpp, re.S)
    if body is None:
        raise Stale("no Assemble( std::initializer_list< const char* > pieces ) in source/Shaders.cpp")
    lines = [line.strip() for line in body.group(1).strip().splitlines()]
    start = re.fullmatch(r"std::string text = std::string\( (k\w+) \)((?: \+ k\w+)*);", lines[0]) if lines else None
    if start is None or lines[1:] != ["for( const char* piece : pieces )", "text += piece;", "return text;"]:
        raise Stale("Assemble() is no longer a run of pieces then the arguments in order")
    preamble = [start.group(1)] + re.findall(r"k\w+", start.group(2))

    init = re.search(r"FFResult KnockoutPlugin::InitGL\(.*?\n\}", plugin_cpp, re.S)
    if init is None:
        raise Stale("no KnockoutPlugin::InitGL in source/Knockout.cpp")
    init = init.group(0)
    strings = dict((name, [first] + re.findall(r"k\w+", rest))
                   for name, first, rest in re.findall(
                       r"const std::string (\w+)\s*= std::string\( (k\w+) \)((?: \+ k\w+)*);", init))
    calls = re.findall(r"(\w+)\.Compile\( (\w+)\.c_str\(\), Assemble\( \{ ([k\w, ]+) \} \)\.c_str\(\) \)", init)
    if len(calls) != plugin_cpp.count(".Compile("):
        raise Stale(f"Knockout.cpp compiles {plugin_cpp.count('.Compile(')} shaders and {len(calls)} read as"
                    " `x.Compile( vertex.c_str(), Assemble( { ... } ).c_str() )`")
    out = []
    for member, vertex, pieces in calls:
        if vertex not in strings:
            raise Stale(f"{member}'s vertex stage `{vertex}` is not `std::string( kVersion ) + ...` in InitGL")
        name = member[: -len("Shader")] if member.endswith("Shader") else member
        fragment = preamble + [p.strip() for p in pieces.split(",")]
        for symbol in strings[vertex] + fragment:
            if symbol not in JS_NAME:
                raise Stale(f"{name} uses {symbol}, a piece this script does not copy")
        out.append((name, [JS_NAME[s] for s in strings[vertex]], [JS_NAME[s] for s in fragment]))
    return out


def render(cpp, plugin_cpp):
    """shaders.js as --write produces it."""
    version = cpp_version(cpp)
    if version is None:
        raise Stale("kVersion not found in source/Shaders.cpp")
    out = [HEADER]
    # The C++ literal's escapes are JavaScript's too ("\\n"), so it is carried as written.
    out.append(f"export const VERSION = '{version}';\n")
    for name, symbol in SHADERS:
        body = from_cpp(cpp, symbol)
        if body is None:
            raise Stale(f"{symbol} not found in source/Shaders.cpp")
        if "\\" in body or "${" in body:
            raise Stale(f"{symbol} holds a backslash or ${{; the escape scheme cannot carry it")
        out.append(f"\n// {symbol}, source/Shaders.cpp\nexport const {name} = `{body.replace('`', chr(92) + '`')}`;\n")
    out.append(
        "\n// Each program's stages, piece by piece, in the order the plugin joins them:\n"
        "// the vertex stages as Knockout.cpp's InitGL builds them, the fragment\n"
        "// stages as Shaders.cpp's Assemble() does.\n"
        "export const PROGRAMS = {\n"
    )
    for name, vertex, fragment in programs(cpp, plugin_cpp):
        out.append(f"  {name}: {{ vertex: [{', '.join(repr(p) for p in vertex)}],"
                   f" fragment: [{', '.join(repr(p) for p in fragment)}] }},\n")
    out.append("};\n")
    return "".join(out)


def first_difference(a, b):
    left, right = a.splitlines(), b.splitlines()
    for i in range(max(len(left), len(right))):
        x = left[i] if i < len(left) else "<missing>"
        y = right[i] if i < len(right) else "<missing>"
        if x != y:
            return i + 1, x, y
    return None


def check_shaders(cpp, plugin_cpp, js):
    problems = 0
    for symbol in unknown_pieces(cpp):
        print(f"FAIL  {symbol} is a piece of source/Shaders.cpp this script does not copy -- add it to SHADERS")
        problems += 1

    version_cpp = cpp_version(cpp)
    version_js = re.search(r"^export const VERSION = '(.*?)';$", js, re.M)
    if version_cpp is None or version_js is None or version_cpp != version_js.group(1):
        print("FAIL  VERSION does not match kVersion")
        problems += 1
    else:
        print(f"ok    {'VERSION':<22} matches kVersion")

    for name, symbol in SHADERS:
        cpp_text = from_cpp(cpp, symbol)
        js_text, complaint = from_js(js, name)
        if cpp_text is None:
            print(f"FAIL  {symbol} not found in source/Shaders.cpp")
            problems += 1
        elif complaint is not None:
            print(f"FAIL  {name} in demo/shaders.js has a {complaint}")
            problems += 1
        elif js_text is None:
            print(f"FAIL  {name} not found in demo/shaders.js")
            problems += 1
        elif cpp_text == js_text:
            print(f"ok    {name:<22} matches {symbol} ({len(cpp_text)} chars)")
        else:
            problems += 1
            print(f"FAIL  {name} has drifted from {symbol}")
            where = first_difference(cpp_text, js_text)
            if where:
                print(f"        first difference at line {where[0]}")
                print(f"          C++: {where[1]}")
                print(f"          js : {where[2]}")

    try:
        table = programs(cpp, plugin_cpp)
    except Stale as stale:
        print(f"FAIL  the plugin's assembly could not be read: {stale}")
        return problems + 1
    block = re.search(r"^export const PROGRAMS = \{\n(.*?)^\};$", js, re.S | re.M)
    rows = {} if block is None else dict(
        (n, (v, f)) for n, v, f in re.findall(r"^  (\w+): \{ vertex: \[(.*?)\], fragment: \[(.*?)\] \},$", block.group(1), re.M))
    for name, vertex, fragment in table:
        want = (", ".join(repr(p) for p in vertex), ", ".join(repr(p) for p in fragment))
        if rows.get(name) == want:
            print(f"ok    {'PROGRAMS.' + name:<22} {' + '.join(vertex)} | {' + '.join(fragment)}")
        else:
            print(f"FAIL  PROGRAMS.{name} is {rows.get(name, '<missing>')}, the plugin's is {want}")
            problems += 1
    for name in sorted(set(rows) - set(n for n, _, _ in table)):
        print(f"FAIL  PROGRAMS.{name} is no program of the plugin's")
        problems += 1

    # Anything else typed into the file.
    try:
        expected = render(cpp, plugin_cpp)
    except Stale as stale:
        print(f"FAIL  {stale}")
        return problems + 1
    if expected != js:
        where = first_difference(expected, js)
        print("FAIL  demo/shaders.js is not exactly what --write produces")
        if where:
            print(f"        first difference at line {where[0]}")
            print(f"          want: {where[1]}")
            print(f"          have: {where[2]}")
        problems += 1
    return problems


def sha256(path):
    digest = hashlib.sha256()
    with open(os.path.join(REPO, path), "rb") as handle:
        for block in iter(lambda: handle.read(1 << 16), b""):
            digest.update(block)
    return digest.hexdigest()


def check_wasm():
    try:
        lines = read(MANIFEST).splitlines()
    except FileNotFoundError:
        print(f"FAIL  {MANIFEST} is missing -- run demo/tools/build-wasm.sh")
        return 1

    problems = 0
    entries = 0
    recorded = set()
    for line in lines:
        pin = re.match(r"^# FFGL SDK ([0-9a-f]{40})$", line)
        if pin:
            try:
                head = subprocess.run(["git", "-C", os.path.join(REPO, "external", "ffgl"), "rev-parse", "HEAD"],
                                      capture_output=True, text=True, check=True).stdout.strip()
            except (OSError, subprocess.CalledProcessError):
                print("FAIL  cannot read the FFGL SDK's commit -- is the submodule checked out?")
                problems += 1
                continue
            if head != pin.group(1):
                print(f"FAIL  the .wasm was built against FFGL SDK {pin.group(1)[:7]}, the submodule is at {head[:7]}")
                problems += 1
            continue
        if not line or line.startswith("#"):
            continue
        want, path = line.split(None, 1)
        recorded.add(path)
        entries += 1
        if not os.path.exists(os.path.join(REPO, path)):
            print(f"FAIL  {path} went into the .wasm and no longer exists")
            problems += 1
        elif sha256(path) != want:
            print(f"FAIL  {path} has changed since demo/knockout-core.wasm was built")
            problems += 1

    # A header or a source added to source/ since the build is an input the
    # build never saw (EffectPlugin.cpp is the registration the page replaces).
    for name in sorted(os.listdir(os.path.join(REPO, "source"))):
        path = f"source/{name}"
        if name.endswith(".h") and path not in recorded:
            print(f"FAIL  {path} is newer than the .wasm's manifest -- rebuild")
            problems += 1
        if name.endswith(".cpp") and name != "EffectPlugin.cpp" and path not in recorded:
            print(f"FAIL  {path} is not compiled into the .wasm -- add it to build-wasm.sh's PLUGIN_SOURCES")
            problems += 1
    for output in OUTPUTS:
        if output not in recorded:
            print(f"FAIL  {MANIFEST} does not record {output}")
            problems += 1
    if problems == 0:
        print(f"ok    {'WASM':<22} built from these sources ({entries} files hashed, SDK pin agrees)")
    return problems


def check_fonts():
    try:
        manifest = json.loads(read(FONTS, "fonts.json"))
    except FileNotFoundError:
        print(f"FAIL  {FONTS}/fonts.json is missing")
        return 1
    except json.JSONDecodeError as error:
        print(f"FAIL  {FONTS}/fonts.json does not parse: {error}")
        return 1

    problems = 0
    listed = set()
    for family in manifest.get("families", []):
        folder = family["folder"]
        licence = os.path.join(FONTS, folder, family["licence"])
        if not os.path.isfile(os.path.join(REPO, licence)):
            print(f"FAIL  {family['name']}: its licence {licence} is not beside its fonts")
            problems += 1
        for face in family["files"]:
            path = os.path.join(FONTS, folder, face["file"])
            listed.add(os.path.normpath(path))
            if not os.path.isfile(os.path.join(REPO, path)):
                print(f"FAIL  {path} is listed in fonts.json and missing")
                problems += 1
            elif sha256(path) != face["sha256"]:
                print(f"FAIL  {path} is not the file fonts.json records")
                problems += 1
    for root, _, files in os.walk(os.path.join(REPO, FONTS)):
        for name in files:
            path = os.path.normpath(os.path.relpath(os.path.join(root, name), REPO))
            if name.lower().endswith(FONT_EXTENSIONS) and path not in listed:
                print(f"FAIL  {path} is served and not in fonts.json (no licence on record)")
                problems += 1
    if problems == 0:
        faces = sum(len(f["files"]) for f in manifest["families"])
        print(f"ok    {'FONTS':<22} {len(manifest['families'])} families, {faces} files, each hash and licence present")
    return problems


def main(argv):
    cpp = read("source", "Shaders.cpp")
    plugin_cpp = read("source", "Knockout.cpp")
    if "--write" in argv:
        unknown = unknown_pieces(cpp)
        if unknown:
            print(f"FAIL  {', '.join(unknown)}: piece(s) this script does not copy -- add them to SHADERS")
            return 1
        try:
            text = render(cpp, plugin_cpp)
        except Stale as stale:
            print(f"FAIL  {stale}")
            return 1
        with open(os.path.join(REPO, "demo", "shaders.js"), "w") as handle:
            handle.write(text)
        print("wrote demo/shaders.js")
        return 0
    try:
        js = read("demo", "shaders.js")
    except FileNotFoundError:
        print("FAIL  demo/shaders.js is missing -- run demo/tools/check_shaders.py --write")
        return 1
    problems = check_shaders(cpp, plugin_cpp, js) + check_wasm() + check_fonts()
    print()
    if problems:
        print(f"{problems} problem(s) -- rerun demo/tools/check_shaders.py --write for the GLSL and"
              " demo/tools/build-wasm.sh for the .wasm; never edit either output by hand")
        return 1
    count = len(programs(cpp, plugin_cpp))
    print(f"all {len(SHADERS) + 1} shader pieces and the order both stages of all {count} programs join them in are"
          " the plugin's, the .wasm is built from the current sources, and every font has its licence")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

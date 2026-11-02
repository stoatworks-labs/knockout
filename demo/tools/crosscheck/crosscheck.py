#!/usr/bin/env python3
"""The served demo page against the plugin's own harness, and every control against the page.

    python3 demo/tools/crosscheck/crosscheck.py --kotest build/kotest --url http://127.0.0.1:8761/
    python3 demo/tools/crosscheck/crosscheck.py --controls --url http://127.0.0.1:8761/

`--url` is demo/ served by anything that serves it (the page is static);
`--gl metal` (the default) draws with this Mac's GPU through Chrome's ANGLE,
`--gl swiftshader` on the CPU. Needs Google Chrome, and for the comparison a
built kotest (cmake -DKNOCKOUT_BUILD_TOOLS=ON). Not run by tools/verify.sh: it
needs a browser and a server, and what it prints is a measurement, recorded on
the page, rather than a gate. Either way it fails on any page error, console
warning or WebGL error (Log.entryAdded).

------------------------------------------------------------------- what it does

It measures the PAGE, not a copy of it: harness.js is injected before the
page's own scripts and only gates requestAnimationFrame (so each load draws one
frame, the kit's clock at 0), turns on preserveDrawingBuffer and reads the
drawing buffer back. Parameters reach the plugin through the page's own query
string (`?font=1&outline=0.2`, the kit's Copy link format) or its own controls.

The comparison (default). Both sides are the same C++ with the same four
fonts: knockout-core.wasm is source/ compiled by emscripten, kotest is source/
compiled by Apple clang, run as `kotest --system-fonts --pipe` with
KNOCKOUT_FONT_DIR=demo/fonts (so its scan finds exactly the page's fonts and
nothing of the Mac's). For each clip, the page is loaded at Mix 0, where the
plugin hands the clip back to the bit, and that RGBA8 frame is the clip
kotest is given on stdin; then each scenario is loaded on the page and run
through kotest with the same settings, and the two frames are compared pixel
by pixel: the largest difference, the mean, and the share of pixels more than
1/255 and more than 8/255 apart.

The picture is not promised to be identical, and this says by how much it is
not. The GPU differs (Apple's GL 4.1 driver against ANGLE on Metal or
SwiftShader); the last rounding differs (kotest's lround of an RGBA32F output
against the GPU's RGBA8 store, one level at most); and the CPU arithmetic can
differ in the last bit (WebAssembly has no fused multiply-add, where Apple
clang fuses on arm64), which moves a placement by an ulp of a float.

The controls (`--controls`). For each control, the page is loaded with what
the control needs to matter (Weight needs a family with weights, Colour needs
Background Colour, Zoom X needs a zoom), paused, read; the control is then
changed in the DOM as a visitor would change it (slider input, select change,
toggle click, colour input, text typed, a font file picked), one frame is
released, and the picture must differ. It also checks that a released frame
with nothing changed is identical (so a difference is the control's), and that
Font Name typed but not committed changes nothing until Enter.
"""
import argparse
import base64
import json
import os
import select
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
DEMO = os.path.abspath(os.path.join(HERE, "..", ".."))
CHROME = os.environ.get("CHROME", "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome")
W, H = 640, 360
RESERVED = {"embed", "size", "clip", "bg"}

# name, the kit's clip, settings (kotest --set: options by element name)
SCENARIOS = [
    ("defaults (Anton, Inside, Transparent)", "scene", []),
    ("Mix 0 (the clip back)", "scene", [("Mix", "0")]),
    ("Barlow Black Italic, Outside on Colour", "scene", [("Text", "LIVE"), ("Font", "Barlow"), ("Italic", "1"), ("Mask", "Outside"), ("Background", "Colour")]),
    ("DM Serif Display Italic, Outline 0.04 em, Black", "scene", [("Text", "Last Orders"), ("Font", "DM Serif Display"), ("Italic", "1"), ("Outline", "0.2"), ("Background", "Black")]),
    ("Monoton, White", "bars", [("Text", "NEON"), ("Font", "Monoton"), ("Background", "White")]),
    ("Zoom 50 into an R", "grid", [("Text", "R"), ("Font", "DM Serif Display"), ("Zoom", "0.566323"), ("Zoom X", "0.669"), ("Zoom Y", "0.167"), ("Background", "White")]),
    ("Zoom 1000 onto the K's stem", "scene", [("Zoom", "1"), ("Zoom X", "0.06"), ("Zoom Y", "0.583"), ("Background", "Black")]),
    ("Barlow Thin, rotated, tracked", "scene", [("Text", "thin air"), ("Font", "Barlow"), ("Weight", "Thin"), ("Rotation", "0.47"), ("Tracking", "0.4"), ("Background", "White")]),
    ("Grow and Feather", "scene", [("Text", "SOFT"), ("Grow", "0.7"), ("Feather", "0.35"), ("Background", "Black")]),
    ("Reveal 0.55, Spread 0.35, Random", "scene", [("Reveal", "0.55"), ("Reveal Spread", "0.35"), ("Reveal Order", "Random"), ("Background", "Black")]),
    ("two lines, Left, Fit Size, moved", "ramp", [("Text", "DOORS\\nAT TEN"), ("Font", "Barlow"), ("Align", "Left"), ("Line Spacing", "0.2"), ("Fit", "Size"), ("Size", "0.75"), ("Position X", "0.45"), ("Position Y", "0.55"), ("Background", "Black")]),
    ("Outside, White, Mix 0.5, on transparency", "alpha", [("Mask", "Outside"), ("Background", "White"), ("Mix", "0.5")]),
    ("Font Name typed: monoton", "scene", [("Font Name", "monoton"), ("Background", "Colour"), ("Colour", "0.1"), ("Colour_Green", "0.2"), ("Colour_Blue", "0.9")]),
]


def address(name):
    a = name.lower().replace(" ", "")
    return f"param-{a}" if a in RESERVED else a


# ----------------------------------------------------------------- Chrome over a pipe
class Chrome:
    """Headless Chrome on --remote-debugging-pipe: no debugging port is opened."""

    def __init__(self, gl):
        self.profile = tempfile.mkdtemp(prefix="knockout-crosscheck-")
        args = [CHROME, "--headless=new", "--remote-debugging-pipe", f"--user-data-dir={self.profile}",
                "--no-first-run", "--no-default-browser-check", "--disable-extensions",
                "--disable-background-timer-throttling", "--disable-renderer-backgrounding",
                f"--window-size={W + 900},{H + 1200}"]
        args += ["--use-angle=swiftshader", "--enable-unsafe-swiftshader"] if gl == "swiftshader" else \
                ["--use-angle=metal", "--enable-gpu", "--ignore-gpu-blocklist"]
        to_chrome_r, self.w = os.pipe()
        self.r, from_chrome_w = os.pipe()

        def child():
            a, b = os.dup(to_chrome_r), os.dup(from_chrome_w)
            os.dup2(a, 3)
            os.dup2(b, 4)

        self.proc = subprocess.Popen(args, close_fds=False, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL, preexec_fn=child)
        os.close(to_chrome_r)
        os.close(from_chrome_w)
        self.buf, self.next, self.session = b"", 0, None
        self.problems = []  # page exceptions, console warnings and errors, browser log warnings and errors
        target = self.call("Target.createTarget", {"url": "about:blank"})["targetId"]
        self.session = self.call("Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]
        for domain in ("Runtime", "Log", "Page", "DOM"):
            self.call(f"{domain}.enable")
        with open(os.path.join(HERE, "harness.js")) as f:
            self.call("Page.addScriptToEvaluateOnNewDocument", {"source": f.read()})
        self.loads = 0

    def event(self, reply):
        method, p = reply.get("method"), reply.get("params", {})
        if method == "Runtime.exceptionThrown":
            d = p["exceptionDetails"]
            self.problems.append(f"exception: {d.get('exception', {}).get('description') or d.get('text')}")
        elif method == "Runtime.consoleAPICalled" and p.get("type") in ("error", "warning", "assert"):
            self.problems.append(f"console.{p['type']}: " + " ".join(str(a.get("value", a.get("description", ""))) for a in p.get("args", [])))
        elif method == "Log.entryAdded" and p["entry"].get("level") in ("warning", "error"):
            e = p["entry"]
            if "GPU stall due to ReadPixels" in e.get("text", ""):
                return  # harness.js's own readPixels (SwiftShader says so); the page never reads back
            self.problems.append(f"log {e['level']} ({e.get('source')}): {e.get('text')}")

    def call(self, method, params=None, timeout=600):
        self.next += 1
        msg = {"id": self.next, "method": method, "params": params or {}}
        if self.session:
            msg["sessionId"] = self.session
        data = json.dumps(msg).encode() + b"\0"
        while data:
            data = data[os.write(self.w, data):]
        end = time.time() + timeout
        while True:
            while b"\0" not in self.buf:
                if not select.select([self.r], [], [], max(0.0, end - time.time()))[0]:
                    raise TimeoutError(method)
                chunk = os.read(self.r, 1 << 22)
                if not chunk:
                    raise EOFError("Chrome went away")
                self.buf += chunk
            raw, self.buf = self.buf.split(b"\0", 1)
            reply = json.loads(raw)
            if "method" in reply:
                self.event(reply)
            if reply.get("id") == self.next:
                if "error" in reply:
                    raise RuntimeError(f"{method}: {reply['error']}")
                return reply.get("result", {})

    def evaluate(self, expression):
        r = self.call("Runtime.evaluate", {"expression": expression, "returnByValue": True, "awaitPromise": True})
        if "exceptionDetails" in r:
            raise RuntimeError(r["exceptionDetails"].get("exception", {}).get("description") or r["exceptionDetails"])
        return r["result"].get("value")

    def load(self, base, query):
        """The page with `query`, once its first frame is drawn."""
        self.loads += 1
        query = dict(query, size=str(W), _load=str(self.loads))
        self.call("Page.navigate", {"url": base + "?" + urllib.parse.urlencode(query)})
        end = time.time() + 60
        probe = (f"(() => {{ if (!location.search.includes('_load={self.loads}&') && !location.search.endsWith('_load={self.loads}')) return null;"
                 " const ko = window.__knockoutCrosscheck; const root = document.querySelector('#demo');"
                 " const error = document.querySelector('.stage__status.is-error')?.textContent"
                 "   || (root && root.textContent.startsWith('The demo could not start') ? root.textContent : '');"
                 " if (error) return 'ERROR ' + error; return ko && ko.frames() >= 1 ? 'ready' : null; })()")
        while time.time() < end:
            try:
                state = self.evaluate(probe)
            except RuntimeError:
                state = None
            if state == "ready":
                return
            if state and state.startswith("ERROR"):
                raise RuntimeError(state)
            time.sleep(0.05)
        raise TimeoutError(f"no frame for {query}")

    def read(self):
        got = self.evaluate("__knockoutCrosscheck.read()")
        assert (got["width"], got["height"]) == (W, H), got
        return base64.b64decode(got["rgba"])

    def close(self):
        try:
            self.call("Browser.close", timeout=5)
        except Exception:
            pass
        self.proc.wait(10)
        shutil.rmtree(self.profile, ignore_errors=True)


# ----------------------------------------------------------------- compare
def compare(a, b):
    worst, total, over1, over8 = 0, 0, 0, 0
    for i in range(0, len(a), 4):
        d = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]), abs(a[i + 3] - b[i + 3]))
        if d:
            worst = max(worst, d)
            total += abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]) + abs(a[i + 3] - b[i + 3])
            over1 += d > 1
            over8 += d > 8
    pixels = len(a) // 4
    return worst, total / (4 * pixels), 100.0 * over1 / pixels, 100.0 * over8 / pixels


def changed(a, b):
    return sum(1 for i in range(0, len(a), 4) if a[i:i + 4] != b[i:i + 4])


def options_of(chrome):
    return chrome.evaluate("Object.fromEntries([...document.querySelectorAll('select.prow__select')]"
                           ".map((s) => [s.id.slice(2), [...s.options].map((o) => o.textContent)]))")


def to_query(settings, options):
    q = {}
    for name, value in settings:
        key = address(name)
        q[key] = str(options[key].index(value)) if key in options and not value.replace(".", "").isdigit() else value
    return q


# ----------------------------------------------------------------- the comparison
def kotest_frame(kotest, settings, clip):
    env = dict(os.environ, KNOCKOUT_FONT_DIR=os.path.join(DEMO, "fonts"))
    args = [kotest, "--system-fonts", "--pipe", "--size", f"{W}x{H}"]
    for name, value in settings:
        args += ["--set", f"{name}={value}"]
    r = subprocess.run(args, input=clip, capture_output=True, env=env)
    if r.returncode != 0 or len(r.stdout) != W * H * 4:
        raise RuntimeError(f"kotest {args[1:]}: exit {r.returncode}, {len(r.stdout)} bytes: {r.stderr.decode()}")
    return r.stdout


def run_comparison(chrome, base, kotest, only, save):
    chrome.load(base, {})
    options = options_of(chrome)
    clips = {}
    rows = []
    for name, source, settings in SCENARIOS:
        if only and only not in name:
            continue
        if source not in clips:
            chrome.load(base, {"clip": source, "mix": "0"})
            clips[source] = chrome.read()
        chrome.load(base, dict(to_query(settings, options), clip=source))
        page = chrome.read()
        status = chrome.evaluate("new Promise((r) => setTimeout(() => r(__knockoutCrosscheck.status()), 300))")
        harness = kotest_frame(kotest, settings, clips[source])
        worst, mean, over1, over8 = compare(page, harness)
        rows.append((name, worst, mean, over1, over8))
        print(f"{name}:\n   max {worst:3d}/255, mean {mean:7.4f}/255, {over1:6.3f}% of pixels > 1/255, {over8:6.3f}% > 8/255"
              f"\n   page: {status}")
        if save:
            os.makedirs(save, exist_ok=True)
            stem = os.path.join(save, "".join(c if c.isalnum() else "-" for c in name).strip("-"))
            open(stem + ".page.rgba", "wb").write(page)
            open(stem + ".kotest.rgba", "wb").write(harness)
    return rows


# ----------------------------------------------------------------- the controls
SLIDER, SELECT, TOGGLE, COLOUR, TEXT, COMMIT, FILE = "slider", "select", "toggle", "colour", "text", "commit", "file"

# control, what it needs to matter (kotest-style settings), how it is changed, to what
CONTROLS = [
    ("Text", [], TEXT, "HELLO"),
    ("Font", [], SELECT, "Barlow"),
    ("Font Name", [], COMMIT, "Monoton"),
    ("Font File", [], FILE, "/System/Library/Fonts/Supplemental/Arial Black.ttf"),
    ("Weight", [("Font", "Barlow")], SELECT, "Thin"),
    ("Italic", [("Font", "Barlow")], TOGGLE, None),
    ("Fit", [], SELECT, "Height"),
    ("Size", [("Fit", "Size")], SLIDER, "0.4"),
    ("Margin", [], SLIDER, "0.5"),
    ("Align", [("Text", "A\\nBBBB")], SELECT, "Right"),
    ("Tracking", [], SLIDER, "0.6"),
    ("Line Spacing", [("Text", "AB\\nCD")], SLIDER, "0.9"),
    ("Position X", [], SLIDER, "0.6"),
    ("Position Y", [], SLIDER, "0.6"),
    ("Rotation", [], SLIDER, "0.6"),
    ("Zoom", [], SLIDER, "0.3"),
    ("Zoom X", [("Zoom", "0.3")], SLIDER, "0.2"),
    ("Zoom Y", [("Zoom", "0.3")], SLIDER, "0.2"),
    ("Grow", [], SLIDER, "0.8"),
    ("Feather", [], SLIDER, "0.4"),
    ("Outline", [], SLIDER, "0.3"),
    ("Reveal", [], SLIDER, "0.5"),
    ("Reveal Spread", [("Reveal", "0.5")], SLIDER, "0.8"),
    ("Reveal Order", [("Reveal", "0.5")], SELECT, "Backward"),
    ("Mask", [], SELECT, "Outside"),
    ("Background", [], SELECT, "Black"),
    ("Colour", [("Background", "Colour")], COLOUR, "#2040ff"),
    ("Mix", [], SLIDER, "0.5"),
]


def act(chrome, name, how, value, options):
    key = address(name)
    sel = json.dumps(f"p-{key}")
    if how == SLIDER:
        chrome.evaluate(f"(() => {{ const s = document.getElementById({sel}); s.value = {json.dumps(value)}; s.dispatchEvent(new Event('input', {{ bubbles: true }})); }})()")
    elif how == SELECT:
        index = options[key].index(value)
        chrome.evaluate(f"(() => {{ const s = document.getElementById({sel}); s.value = '{index}'; s.dispatchEvent(new Event('change', {{ bubbles: true }})); }})()")
    elif how == TOGGLE:
        chrome.evaluate(f"__knockoutCrosscheck.rowOf({json.dumps(name)}).querySelector('.prow__toggle').click()")
    elif how == COLOUR:
        chrome.evaluate(f"(() => {{ const s = document.getElementById({sel}); s.value = {json.dumps(value)}; s.dispatchEvent(new Event('input', {{ bubbles: true }})); }})()")
    elif how in (TEXT, COMMIT):
        chrome.evaluate(f"(() => {{ const s = document.getElementById({sel}); s.value = {json.dumps(value)}; s.dispatchEvent(new Event('input', {{ bubbles: true }})); }})()")
        if how == COMMIT:
            # Typed, not committed: nothing may change yet.
            before = chrome.read()
            frames = chrome.evaluate("__knockoutCrosscheck.frames()")
            chrome.evaluate("__knockoutCrosscheck.release()")
            chrome.evaluate(f"__knockoutCrosscheck.waitFrame({frames})")
            if chrome.read() != before:
                raise RuntimeError("Font Name changed the picture before it was committed")
            chrome.evaluate(f"document.getElementById({sel}).dispatchEvent(new Event('change', {{ bubbles: true }}))")
    elif how == FILE:
        doc = chrome.call("DOM.getDocument", {"depth": 0})["root"]["nodeId"]
        node = chrome.call("DOM.querySelector", {"nodeId": doc, "selector": "input[type=file][accept*='.ttf']"})["nodeId"]
        chrome.call("DOM.setFileInputFiles", {"nodeId": node, "files": [value]})
        for _ in range(200):
            if chrome.evaluate(f"document.getElementById({sel}).value"):
                break
            time.sleep(0.02)
        else:
            raise RuntimeError("the picked font never reached Font File")


def run_controls(chrome, base):
    chrome.load(base, {})
    options = options_of(chrome)
    failures = 0
    for i, (name, needs, how, value) in enumerate(CONTROLS):
        chrome.load(base, dict(to_query(needs, options), clip="bars"))
        chrome.evaluate("document.querySelector('.btn--play').click()")
        frames = chrome.evaluate("__knockoutCrosscheck.frames()")
        before = chrome.read()
        if i == 0:
            # A frame released with nothing changed must be the same frame.
            chrome.evaluate("__knockoutCrosscheck.release()")
            frames = chrome.evaluate(f"__knockoutCrosscheck.waitFrame({frames})")
            if chrome.read() != before:
                raise RuntimeError("a paused frame with nothing changed came out different")
        act(chrome, name, how, value, options)
        frames = chrome.evaluate("__knockoutCrosscheck.frames()")
        chrome.evaluate("__knockoutCrosscheck.release()")
        chrome.evaluate(f"__knockoutCrosscheck.waitFrame({frames})")
        after = chrome.read()
        n = changed(before, after)
        status = chrome.evaluate("new Promise((r) => setTimeout(() => r(__knockoutCrosscheck.status()), 300))")
        verdict = "changes" if n else "NO CHANGE"
        failures += n == 0
        context = ", ".join(f"{k} {v}" for k, v in needs)
        print(f"  {name:13s} {how:6s} -> {str(value):28.28s} {verdict}: {100.0 * n / (W * H):6.2f}% of pixels"
              f"{f'  (with {context})' if context else ''}")
        if name in ("Font File", "Font Name", "Weight", "Italic"):
            print(f"                {status}")
    print(f"\n{len(CONTROLS) - failures} of {len(CONTROLS)} controls change the picture"
          " (and a paused frame with nothing changed did not; Font Name waited for its commit)")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--kotest", help="a built tools/kotest, for the comparison")
    parser.add_argument("--url", default="http://127.0.0.1:8761/", help="demo/, served")
    parser.add_argument("--gl", default="metal", choices=["metal", "swiftshader"])
    parser.add_argument("--controls", action="store_true", help="check every control instead of comparing")
    parser.add_argument("--only", help="run the scenarios whose name contains this")
    parser.add_argument("--save", help="write each compared frame pair here as raw RGBA")
    args = parser.parse_args()
    if not args.controls and not args.kotest:
        parser.error("--kotest is needed for the comparison")

    chrome = Chrome(args.gl)
    try:
        base = args.url if args.url.endswith("/") else args.url + "/"
        if args.controls:
            bad = run_controls(chrome, base)
        else:
            run_comparison(chrome, base, args.kotest, args.only, args.save)
            bad = 0
        renderer = chrome.evaluate("__knockoutCrosscheck.renderer()")
        target = chrome.evaluate("__knockoutCrosscheck.gl.canvas.dataset.floatTarget")
        checked = chrome.evaluate("__knockoutCrosscheck.gl.canvas.dataset.shadersChecked")
        print(f"\nbrowser: {renderer}; mask target via {target}; {checked} shader stages checked and compiled; {chrome.loads} page loads")
        if chrome.problems:
            print(f"page problems ({len(chrome.problems)}):")
            for p in dict.fromkeys(chrome.problems):
                print("  ", p)
        else:
            print("no page exceptions, console warnings or errors, or WebGL errors")
        return 1 if bad or chrome.problems else 0
    finally:
        chrome.close()


if __name__ == "__main__":
    sys.exit(main())

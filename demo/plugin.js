/**
 * Knockout — browser demo.
 *
 * Text as a mask (SW Knockout, KO01, an effect): the clip shows through the
 * letters, or the letters are cut out of it. The plugin sends each glyph's
 * curves to the GPU once and draws one rectangle per letter; every pixel works
 * out from the curves themselves whether it is inside the letter and how far it
 * is from the edge, so there is no atlas and nothing to run out of resolution.
 *
 * ---------------------------------------------------------------------------
 * What runs here, and what does not
 * ---------------------------------------------------------------------------
 *
 * **This page does not port the plugin. It runs it.** `knockout-core.wasm` is
 * the plugin's own C++ compiled UNMODIFIED with emscripten
 * (demo/tools/build-wasm.sh): `source/Knockout.cpp` — the plugin class, its
 * constructor, font resolution, the setting, the piece table, the per-letter
 * instances worked out in double, both passes and every uniform — and
 * everything it calls (Fonts, Outline, Layout, Target, Controls, Shaders, Diag,
 * stb_truetype), with the FFGL SDK's CFFGLPlugin, its parameter bookkeeping,
 * FFGLShader and FFGLScreenQuad. Its GL calls go to this page's WebGL2 context
 * through emscripten's GL library.
 *
 * What is the page's and not the plugin's:
 *
 *   - **The host.** demo/wasm/glue.cpp constructs the plugin (what
 *     EffectPlugin.cpp's registration does in a bundle), reads its parameter
 *     declarations back through the SDK's host getters — the panel is built
 *     from them, not from a list typed here — and forwards SetFloatParameter,
 *     SetTextParameter and ProcessOpenGL, as tools/kotest's Rig does.
 *   - **The GL entry points WebGL2 cannot take** (demo/wasm/gl_shim.cpp):
 *     glShaderSource, which calls `shaderSource()` below; GL_PROGRAM_POINT_SIZE
 *     in glEnable / glDisable / glIsEnabled; and buffer textures, which WebGL2
 *     does not have, emulated as 2D textures. The shaders' half of that
 *     emulation is `rewriteBuffers()` below, applied after the check.
 *   - **The fonts.** Four OFL families, bundled (demo/fonts/fonts.json), are
 *     written into emscripten's in-memory file system before the first
 *     instance, and the plugin's own scan is pointed at them with
 *     KNOCKOUT_FONT_DIR — the variable its harness uses for the same thing.
 *   - **The panel's read-outs.** The number beside a slider is Controls.cpp's
 *     conversion, compiled in (glue.cpp's ko_convert); the units printed after
 *     it, and every tooltip, are this page's words.
 *
 * ---------------------------------------------------------------------------
 * Decisions this page made, and why
 * ---------------------------------------------------------------------------
 *
 * **Text is sent as you type; Font Name when committed** (Enter, or leaving the
 * field), as a host sends a typed name: half a family name is a family the
 * page has not got, and each one would log a warning.
 *
 * **The panel keeps what you set**, as Arena does: picking from Font fills
 * Font Name inside the plugin, and Arena never shows that, so neither does the
 * field here. The line under the picture says which face the plugin drew.
 *
 * **Font File is a picker**, writing the file into the in-memory file system
 * and handing the plugin that path, which it opens with its own loader.
 *
 * **The About block is not on the panel**, as on every page in the suite.
 */

import { mountDemo } from './vendor/demo.js';
import { port, GLError } from './vendor/gl.js';
import * as S from './shaders.js';
import createKnockout from './knockout-core.js';

//===========================================================================
// FFGL.h's parameter types, by number: what GetParamType returns.
//===========================================================================

const FF_TYPE_BOOLEAN = 0;
const FF_TYPE_EVENT = 1;
const FF_TYPE_RED = 2;
const FF_TYPE_GREEN = 3;
const FF_TYPE_BLUE = 4;
const FF_TYPE_STANDARD = 10;
const FF_TYPE_OPTION = 11;
const FF_TYPE_FILE = 14;
const FF_TYPE_TEXT = 100;

//===========================================================================
// The GLSL: the page's checked copy, assembled as the plugin assembles it
// (shaders.js's PROGRAMS, which check_shaders.py holds to Knockout.cpp's
// InitGL and Shaders.cpp's Assemble()).
//===========================================================================

const assemble = (pieces) => pieces.map((name) => S[name]).join('');
const PAGE_SHADERS = Object.entries(S.PROGRAMS).map(([program, stages]) => ({
  program,
  vertex: assemble(stages.vertex),
  fragment: assemble(stages.fragment),
}));

/**
 * The shaders' half of the buffer-texture emulation (gl_shim.cpp has the GL
 * half). WebGL2 has no samplerBuffer; each buffer texture is an RGBA32F 2D
 * texture of rows of 2048 texels, texel i at ( i % width, i / width ). So:
 *
 *   uniform samplerBuffer X;   ->  uniform highp sampler2D X;
 *   texelFetch( X, i )         ->  knockoutBufferFetch( X, i )
 *
 * and knockoutBufferFetch, which reads that texel, is added after the version
 * and precision lines. `highp` is explicit because a vertex stage's default
 * sampler precision in ES 3.00 is lowp, which would round the plugin's
 * coordinates. Nothing else is touched; anything left over is refused.
 */
const BUFFER_FETCH = `// demo/plugin.js: a buffer texture is a 2D texture here, rows of texels.
highp vec4 knockoutBufferFetch( highp sampler2D table, int i )
{
	int w = textureSize( table, 0 ).x;
	return texelFetch( table, ivec2( i % w, i / w ), 0 );
}
`;

function rewriteBuffers(ported) {
  const names = [];
  let out = ported.replace(/\buniform\s+samplerBuffer\s+(\w+)\s*;/g, (_, name) => {
    names.push(name);
    return `uniform highp sampler2D ${name};`;
  });
  let fetches = 0;
  for (const name of names) {
    out = out.replace(new RegExp(`\\btexelFetch(\\(\\s*${name}\\s*,)`, 'g'), (_, rest) => {
      fetches += 1;
      return `knockoutBufferFetch${rest}`;
    });
  }
  if (/\bsamplerBuffer\b/.test(out)) throw new Error('a samplerBuffer the rewrite does not recognise is left');
  for (const name of names) {
    if (new RegExp(`\\btexelFetch\\s*\\(\\s*${name}\\b`).test(out)) throw new Error(`a texelFetch of ${name} the rewrite does not recognise is left`);
  }
  if (names.length > 0) {
    const header = /^#version 300 es\n(?:precision [^\n]*;\n)*/.exec(out);
    if (!header) throw new Error('no version and precision header to put knockoutBufferFetch after');
    out = header[0] + BUFFER_FETCH + out.slice(header[0].length);
  }
  return { text: out, names, fetches };
}

/** What the rewrite does to each stage of the page's copy (for the notes). */
const REWRITES = [];
for (const s of PAGE_SHADERS) {
  for (const stage of ['vertex', 'fragment']) {
    const { names, fetches } = rewriteBuffers(port(s[stage], { stage }));
    if (names.length) REWRITES.push({ program: s.program, stage, names, fetches });
  }
}

const shaderCheck = { checked: 0, refused: [] };

/**
 * Called by gl_shim.cpp's glShaderSource with the text the compiled plugin
 * handed to GL. Anything but one of this page's own assemblies is refused (an
 * empty source, so the plugin's InitGL fails and says so) rather than compiled.
 */
function shaderSource(stage, text) {
  const ours = PAGE_SHADERS.find((s) => s[stage] === text);
  if (!ours) {
    shaderCheck.refused.push(`${stage} stage: not the page’s checked copy of source/Shaders.cpp`);
    return '';
  }
  try {
    const { text: rewritten } = rewriteBuffers(port(text, { stage }));
    shaderCheck.checked += 1;
    return rewritten;
  } catch (error) {
    shaderCheck.refused.push(`${ours.program} ${stage} stage: ${error.message}`);
    return '';
  }
}

//===========================================================================
// The module, and the fonts it is given before the first instance.
//===========================================================================

const printed = [];
let core;
let FONTS;

function cannotStart(why, error) {
  const root = document.querySelector('#demo');
  root.textContent = `The demo could not start: ${why}. The plugin, its source and its downloads are at https://github.com/stoatworks-labs/knockout.`;
  throw error;
}

try {
  core = await createKnockout({
    knockoutShaderSource: shaderSource,
    print: (text) => { printed.push(text); console.log(text); },
    printErr: (text) => { printed.push(text); console.warn(text); },
  });
} catch (error) {
  cannotStart(`this browser would not run its WebAssembly (${error.message})`, error);
}

try {
  FONTS = await (await fetch('./fonts/fonts.json')).json();
  const files = FONTS.families.flatMap((family) => family.files.map((f) => ({ folder: family.folder, file: f.file })));
  const loaded = await Promise.all(files.map(async ({ folder, file }) => {
    const response = await fetch(`./fonts/${folder}/${file}`);
    if (!response.ok) throw new Error(`${folder}/${file}: HTTP ${response.status}`);
    return { folder, file, bytes: new Uint8Array(await response.arrayBuffer()) };
  }));
  for (const { folder, file, bytes } of loaded) {
    core.FS.mkdirTree(`/fonts/${folder}`);
    core.FS.writeFile(`/fonts/${folder}/${file}`, bytes);
  }
} catch (error) {
  cannotStart(`its fonts would not load (${error.message})`, error);
}

const text = (pointer) => core.UTF8ToString(pointer);

/** A C string for one call: allocated, handed over, freed. */
function withCString(value, fn) {
  const pointer = core.stringToNewUTF8(value);
  try {
    return fn(pointer);
  } finally {
    core._free(pointer);
  }
}

// The font scan runs once, in the first constructor, as in a host.
withCString('/fonts', (s) => core._ko_prepare(s));

//===========================================================================
// The declarations, read from the plugin's constructor through the SDK.
//===========================================================================

function declarations() {
  const h = core._ko_new();
  const list = [];
  const n = core._ko_param_count(h);
  for (let i = 0; i < n; i += 1) {
    const type = core._ko_param_type(h, i);
    const d = { index: i, name: text(core._ko_param_name(h, i)), type, group: text(core._ko_param_group(h, i)) };
    if (type === FF_TYPE_TEXT || type === FF_TYPE_FILE) d.default = text(core._ko_param_default_text(h, i));
    else d.default = core._ko_param_default(h, i);
    if (type === FF_TYPE_OPTION) {
      d.elements = [];
      d.values = [];
      for (let e = 0; e < core._ko_param_element_count(h, i); e += 1) {
        d.elements.push(text(core._ko_param_element_name(h, i, e)));
        d.values.push(core._ko_param_element_value(h, i, e));
      }
    }
    if (type === FF_TYPE_FILE) {
      d.extensions = [];
      for (let e = 0; e < core._ko_param_extension_count(h, i); e += 1) d.extensions.push(text(core._ko_param_extension(h, i, e)));
    }
    list.push(d);
  }
  const maxInputs = core._ko_max_inputs(h);
  core._ko_delete(h);
  return { list, maxInputs };
}

const DECLARED = declarations();

//===========================================================================
// The read-outs: Controls.cpp's number, this page's unit.
//===========================================================================

const fixed = (n, d) => n.toFixed(d);
const signed = (n, d) => `${n < 0 ? '−' : '+'}${Math.abs(n).toFixed(d)}`;
const UNITS = {
  Size: (x) => `${fixed(x, 3)} h`,
  Margin: (x) => `${fixed(100 * x, 1)}%`,
  Tracking: (x) => `${signed(x, 3)} em`,
  'Line Spacing': (x) => `${fixed(x, 2)} lines`,
  'Position X': (x) => signed(x, 3),
  'Position Y': (x) => signed(x, 3),
  Rotation: (x) => `${signed(x, 1)}°`,
  Zoom: (x) => `× ${fixed(x, x < 10 ? 2 : x < 100 ? 1 : 0)}`,
  Grow: (x) => `${signed(x, 3)} em`,
  Feather: (x) => `${fixed(x, 4)} h`,
  Outline: (x) => `${fixed(x, 3)} em`,
};

function readout(d) {
  return (v) => {
    const x = core._ko_convert(d.index, Math.fround(v));
    if (Number.isNaN(x)) return fixed(v, 2);
    return (UNITS[d.name] ?? ((y) => fixed(y, 3)))(x);
  };
}

/** Tooltips: this page's words, from the README's description of each control. */
const HINTS = {
  Text: 'The words. \\n starts a new line and \\\\ is a backslash; up to 4096 characters are drawn. Sent as you type. Letters are placed with the font’s advances and kerning and nothing else: no ligatures, and scripts that join or reorder their letters come out wrong.',
  Font: 'The families the plugin found when it loaded. In Resolume that is every font on the machine; a web page cannot read yours, so here it is the four bundled with this page. A pick is saved as a number made from the family’s name, not its place in the list.',
  'Font Name': 'The same choice typed, any case, and it wins over Font. A name this page has not got is kept as typed while the plugin draws the default — the plugin’s own path for a composition opened on a machine without its font. Sent on Enter, or on leaving the field.',
  'Font File': 'A .ttf, .otf, .ttc or .otc you pick, written into this page’s memory and read by the plugin’s own loader. It overrides Font and Font Name. Nothing is uploaded.',
  Weight: 'Thin to Black, matched the way a browser does: the nearest weight the family has (500 looks down before up, above 500 up first, below 400 down). Of the bundled families only Barlow has more than one.',
  Italic: 'An italic before a weight, where the family has one: Barlow and DM Serif Display here.',
  Fit: 'Frame, Width, Height: the text scaled to fill it, less Margin. Size: Size sets the em.',
  Size: 'The em, 0.02 to 2 frame heights h, when Fit is Size.',
  Margin: 'Kept clear on each side when Fit scales the text: 0 to 0.45 of the frame.',
  Align: 'Left, Centre or Right, between lines.',
  Tracking: '−0.2 to +0.6 em between letters.',
  'Line Spacing': '0.5 to 2 lines.',
  'Position X': 'Across, ±1 frame.',
  'Position Y': 'Up and down, ±1 frame.',
  Rotation: '±180°.',
  Zoom: '1 to 1000 times. Worked out in double precision on the CPU, so the shader only ever sees numbers near the middle of the screen and the thousandth zoom still lands where it should.',
  'Zoom X': 'Where across the frame the zoom is centred.',
  'Zoom Y': 'Where up the frame the zoom is centred.',
  Grow: 'The outline pushed out or pulled in, ±0.1 em.',
  Feather: 'A soft edge, up to 0.1 frame heights h, centred where the edge was.',
  Outline: '0 for filled letters; up to 0.2 em of stroke centred on the outline.',
  Reveal: '0 nothing, 1 everything. In Resolume, put it on an envelope or the BPM and the text writes itself on.',
  'Reveal Spread': '0 one letter at a time, 1 every letter fading together.',
  'Reveal Order': 'Forward, Backward, Centre Out or Random.',
  Mask: 'Inside: the clip shows through the letters. Outside: the letters are cut out of the clip.',
  Background: 'What fills the rest: Transparent (alpha 0), Black, White or Colour.',
  Colour: 'The background when Background is Colour.',
  Mix: 'The effect against the clip. At 0 the clip comes back untouched, to the bit.',
};

//===========================================================================
// Kit parameters from the declarations. The id is the name as Arena
// addresses it (lower case, spaces removed).
//===========================================================================

const KIT_TYPES = {
  [FF_TYPE_STANDARD]: 'standard',
  [FF_TYPE_OPTION]: 'option',
  [FF_TYPE_BOOLEAN]: 'boolean',
  [FF_TYPE_RED]: 'colour',
  [FF_TYPE_GREEN]: 'colour',
  [FF_TYPE_BLUE]: 'colour',
  [FF_TYPE_FILE]: 'text',
  [FF_TYPE_TEXT]: 'text',
};

/**
 * The kit reads `embed`, `size`, `clip` and `bg` from the query string itself;
 * a parameter id equal to one would be set by it. Knockout's Size would be.
 */
const RESERVED_QUERY = new Set(['embed', 'size', 'clip', 'bg']);
const addressOf = (name) => {
  const address = name.toLowerCase().replace(/\s+/g, '');
  return RESERVED_QUERY.has(address) ? `param-${address}` : address;
};

function kitDefault(d) {
  switch (d.type) {
    case FF_TYPE_OPTION: {
      const i = d.values.findIndex((v) => v === d.default);
      return i >= 0 ? i : Math.round(d.default);
    }
    case FF_TYPE_BOOLEAN: return d.default > 0.5 ? 1 : 0;
    default: return d.default;
  }
}

const PARAMS = [];
for (const d of DECLARED.list) {
  if (d.group === 'About') continue;
  const p = { id: addressOf(d.name), name: d.name, type: KIT_TYPES[d.type], group: d.group, hint: HINTS[d.name], ff: d };
  if (p.type === undefined) throw new Error(`no kit control for FFGL type ${d.type} (${d.name})`);
  if (d.type === FF_TYPE_OPTION) p.elements = d.elements;
  if (d.type === FF_TYPE_STANDARD) p.display = readout(d);
  if (d.name === 'Text') p.placeholder = 'nothing drawn';
  p.default = kitDefault(d);
  PARAMS.push(p);
}

const query = new URLSearchParams(window.location.search);
const slot = (name, element) => {
  const d = DECLARED.list.find((x) => x.name === name);
  const i = d?.elements?.indexOf(element) ?? -1;
  if (i < 0) throw new Error(`no ${element} in ${name}`);
  return i;
};

//===========================================================================
// The renderer: one plugin instance, driven as a host drives it.
//===========================================================================

/** The value the host would send for a kit control. */
function hostValue(p, v) {
  const d = p.ff;
  switch (d.type) {
    case FF_TYPE_OPTION: return Math.fround(d.values[Math.round(v)] ?? 0);
    case FF_TYPE_BOOLEAN: return v > 0.5 ? 1 : 0;
    case FF_TYPE_FILE:
    case FF_TYPE_TEXT: return String(v);
    default: return Math.fround(v);
  }
}

function createRenderer(gl) {
  // The mask is an R16F target with MAX blending (Target.cpp). WebGL2 renders
  // into a 16-bit float only with one of these; blending into one needs no
  // more. Without either the plugin's InitGL would fail on an incomplete
  // framebuffer, so say why first.
  const floatTarget = ['EXT_color_buffer_float', 'EXT_color_buffer_half_float'].find((name) => gl.getExtension(name));
  if (!floatTarget) {
    throw new GLError('This browser cannot render into a 16-bit float texture (no EXT_color_buffer_float or EXT_color_buffer_half_float), and the plugin draws its mask into one. An 8-bit mask would quantise the edges, so the demo stops here rather than draw something else.');
  }

  // emscripten's GL library, pointed at the kit's context. Nothing else is
  // turned on behind the kit's back.
  const handle = core.GL.registerContext(gl, { majorVersion: 2, minorVersion: 0, enableExtensionsByDefault: false });
  core.GL.makeContextCurrent(handle);

  let instance = 0;
  let pushed = new Map();
  let clipName = 0;
  let clipTexture = null;
  let frames = 0;

  /** A kit texture under a GL name the plugin can bind, in emscripten's tables. */
  function glName(texture) {
    if (texture === clipTexture) return clipName;
    if (clipName) core.GL.textures[clipName] = null;
    clipName = core.GL.getNewId(core.GL.textures);
    core.GL.textures[clipName] = texture;
    texture.name = clipName;
    clipTexture = texture;
    return clipName;
  }

  function start(width, height) {
    instance = core._ko_new();
    pushed = new Map();
    frames = 0;
    if (!core._ko_init_gl(instance, width, height)) {
      const why = shaderCheck.refused.length
        ? `the GLSL the compiled plugin handed to GL was refused (${shaderCheck.refused.join('; ')}), so it was not compiled`
        : `the plugin’s InitGL failed${printed.length ? `: ${printed.slice(-3).join(' ')}` : ''}`;
      core._ko_delete(instance);
      instance = 0;
      throw new GLError(`SW Knockout could not start: ${why}.`);
    }
    gl.canvas.dataset.shadersChecked = String(shaderCheck.checked);
    gl.canvas.dataset.floatTarget = floatTarget;
  }

  function push(params) {
    for (const p of PARAMS) {
      const value = hostValue(p, params.get(p.id));
      if (pushed.get(p.id) === value) continue;
      pushed.set(p.id, value);
      if (typeof value === 'string') withCString(value, (s) => core._ko_set_text(instance, p.ff.index, s));
      else core._ko_set_float(instance, p.ff.index, value);
    }
  }

  return {
    render({ input, params, width, height }) {
      core.GL.makeContextCurrent(handle);
      if (!instance) start(width, height);
      push(params);
      if (!core._ko_process(instance, glName(input.texture), input.width, input.height)) {
        throw new GLError('The plugin’s ProcessOpenGL failed.');
      }
      // For anything checking the page from outside: frames drawn.
      frames += 1;
      gl.canvas.dataset.frames = String(frames);
    },
    /** What the last frame drew with (LastFrame, the harness's accessor). */
    frame() {
      if (!instance || frames === 0) return null;
      const n = (what) => core._ko_frame_number(instance, what);
      const f = {
        weight: n(0), italic: n(1) === 1, fromFile: n(2) === 1, glyphs: n(3), drawn: n(4), culled: n(5),
        pieces: n(6), lines: n(7), unitPx: n(8), zoom: n(9), unitsPerEm: n(10),
        family: text(core._ko_frame_text(instance, 0)), style: text(core._ko_frame_text(instance, 1)),
      };
      // The piece table's upload (gl_shim.cpp records each): two texels a piece.
      const uploads = Object.values(core.knockoutUploads ?? {});
      f.table = uploads.find((u) => u.texels === 2 * f.pieces) ?? null;
      return f;
    },
    log() {
      try {
        return core.FS.readFile(text(core._ko_log_path()), { encoding: 'utf8' });
      } catch {
        return '';
      }
    },
  };
}

//===========================================================================
// The page.
//===========================================================================

const rewriteNote = REWRITES.map((r) => `${r.program} ${r.stage} stage: ${r.names.join(', ')}, ${r.fetches} texelFetch${r.fetches === 1 ? '' : 'es'}`).join('; ');
const fontList = FONTS.families.map((f) => `${f.name} (${f.designer}; ${f.files.length === 1 ? 'one face' : `${f.files.length} faces`})`);

let renderer = null;

const mounted = mountDemo({
  name: 'Knockout',
  pluginId: 'KO01',
  kind: 'effect',
  tagline:
    'Text as a mask for Resolume: type a word, pick a font, and the clip shows through the letters, or the letters are cut out of it. Each letter is drawn from its font’s own curves on the GPU, with no atlas in between, so it stays sharp at any size and at any zoom up to a thousand times; Grow, Feather and Outline move, soften or stroke the same edge, and Reveal writes the text on a letter at a time.',
  repo: 'https://github.com/stoatworks-labs/knockout',
  page: 'https://stoatworks-labs.com/software/knockout/',

  blurb:
    'It is the plugin’s own C++ — the plugin class, its font scan and outline reader, layout and both GPU passes, with the FFGL SDK classes it uses — compiled unmodified to WebAssembly, drawing with its own GLSL in WebGL2. The host is this page: its fonts are four bundled with it, buffer textures are emulated, and the clip is generated.',

  // Background Transparent is alpha 0: the backdrop shows it.
  showBackdrop: true,

  sources: ['scene', 'bars', 'grid', 'spot', 'alpha', 'ramp'],

  params: PARAMS,

  presets: {
    'Cut out of colour': { text: 'LIVE', font: slot('Font', 'Barlow'), italic: 1, mask: slot('Mask', 'Outside'), background: slot('Background', 'Colour') },
    'Outlined italic serif': { text: 'Last Orders', font: slot('Font', 'DM Serif Display'), italic: 1, outline: 0.2, background: slot('Background', 'Black') },
    'Two lines, left': { text: 'DOORS\\nAT TEN', font: slot('Font', 'Barlow'), align: slot('Align', 'Left'), linespacing: 0.2, background: slot('Background', 'Black') },
    'Inline display face': { text: 'NEON', font: slot('Font', 'Monoton'), background: slot('Background', 'Black') },
    'Fifty times into a letter': { text: 'R', font: slot('Font', 'DM Serif Display'), zoom: 0.566323, zoomx: 0.669, zoomy: 0.167, background: slot('Background', 'White') },
    'Writing on': { reveal: 0.55, revealspread: 0.35, background: slot('Background', 'Black') },
    'Grown and feathered': { text: 'SOFT', grow: 0.7, feather: 0.35, background: slot('Background', 'Black') },
    'Tilted thin': { text: 'thin air', font: slot('Font', 'Barlow'), weight: slot('Weight', 'Thin'), rotation: 0.47, tracking: 0.4, background: slot('Background', 'White') },
  },

  differences: [
    'This page runs the plugin rather than a port of it. knockout-core.wasm is source/Knockout.cpp — the plugin class: its constructor, font resolution, the setting, the table of curve pieces, the per-letter placements worked out in double, both GPU passes and every uniform — and everything it calls (Fonts, Outline, Layout, Target, Controls, Shaders, Diag, and stb_truetype), with the FFGL SDK’s CFFGLPlugin, parameter bookkeeping, FFGLShader and FFGLScreenQuad, all compiled unmodified by emscripten (demo/tools/build-wasm.sh). Only EffectPlugin.cpp, the registration, is left out; the page constructs the plugin itself, as the plugin’s harness does. The panel is read back from the plugin’s own declarations through the SDK’s host getters, and the numbers beside the sliders are Controls.cpp’s conversions, compiled in; their units and every tooltip are this page’s.',
    'The GLSL is the plugin’s: demo/shaders.js is spliced from source/Shaders.cpp and the repository’s verify script fails if a character differs. At start-up the page also requires the text the compiled plugin hands to glShaderSource to equal one of its own assemblies of that copy, and refuses to compile anything else. The kit’s port() then changes the version line and adds the ES precision defaults. GL is emscripten’s WebGL2 library on this page’s context, not a GL 4.1 driver, and some entry points are the page’s (demo/wasm/gl_shim.cpp): glShaderSource, for that check; glEnable, glDisable and glIsEnabled, because GL_PROGRAM_POINT_SIZE — state the plugin saves and restores each frame, as a plugin in Resolume must — does not exist in WebGL2 (it reads as off; the plugin draws no points); and the buffer-texture calls below.',
    `Buffer textures are emulated. The plugin keeps its curve pieces (two RGBA32F texels each) and its per-letter placements (three each) in buffer textures, which WebGL2 does not have: they are OpenGL ES 3.2. The shim keeps each buffer’s bytes on the CPU and uploads them to an RGBA32F 2D texture in rows of 2048 texels, the last row padded with zeros, read with NEAREST, so the shader reads the floats the plugin wrote bit for bit; GL_MAX_TEXTURE_BUFFER_SIZE, which the plugin checks its table against, answers with what those rows can hold. After the check and port(), the page rewrites the shaders to match, and this is the only change made to the plugin’s GLSL beyond port(): each “uniform samplerBuffer X;” becomes “uniform highp sampler2D X;”, each texelFetch( X, i ) becomes knockoutBufferFetch( X, i ), and knockoutBufferFetch, a function that reads texel ( i mod width, i ÷ width ), is added after the precision lines (${rewriteNote}). The composite pass is untouched.`,
    `The fonts are this page’s. In Resolume, Font lists every family installed on the machine. A web page cannot read your fonts, and the fonts your browser uses for this text are not files the plugin can open, so the page bundles four families under the SIL Open Font License, unmodified from Google Fonts — ${fontList.join(', ')} — and writes them into the WebAssembly module’s in-memory file system, where the plugin’s own scan finds them (KNOCKOUT_FONT_DIR, the variable its harness uses for the same thing). Each licence is beside its font (linked under the picture). Anton is the default because it is first: the plugin starts on Arial Black, then Arial, then Helvetica, then the first family, and this page has none of the first three. Font File takes your own .ttf, .otf, .ttc or .otc, written into that memory and read by the plugin’s own loader; nothing is uploaded, and Copy link does not carry it. Font Name looks families up in the same four, so any other name is kept as typed and the default drawn.`,
    'Text is sent as you type; Font Name when you press Enter or leave the field. As in Arena, the panel keeps what you set: picking from Font fills in Font Name inside the plugin, and the field does not show it. The line under the picture says which face the plugin actually drew and how much it drew, from LastFrame, the accessor its harness reads.',
    'The clip is the kit’s generated one, premultiplied, or your own image or video, uploaded as 8 bits a channel at the output’s size. The About block (a text and three buttons) is declared by the plugin and left off this panel.',
    'Compared with the plugin’s own harness, kotest — the same C++ built natively for an Apple Silicon Mac and drawing through the Mac’s OpenGL driver — given the same four fonts, when this page was built: thirteen scenarios at 640 × 360, each run on this page and through kotest with the same settings and the same clip (the one this page drew, read back at Mix 0, where the plugin returns it to the bit). They cover every bundled family, Weight and Italic, Outline, Grow with Feather, Reveal part-way in Random order, two lines aligned left at Fit Size, Rotation with Tracking, Outside on Colour, Mix ½ over transparency, a typed Font Name, and Zoom 50 and Zoom 1000 onto the edge of a letter. No pixel differed by more than 1/255 in any of them, with Chrome drawing on the Mac’s GPU (ANGLE on Metal; at most 0.23/255 on average) and on its CPU (SwiftShader; at most 0.03/255): one level, which is the last rounding (kotest rounds a float output, this page’s canvas is 8 bits a channel). Every control on the panel was also checked to change the picture. That comparison (demo/tools/crosscheck) does not run again.',
    'The plugin’s harness, kotest, is what measures it: winding, distance, layout, fit, zoom, the composite and the fonts, each to a stated tolerance in the README. That harness, not this page, is the reason to believe the maths.',
  ],

  createRenderer: (gl) => {
    renderer = createRenderer(gl);
    return renderer;
  },
});

//===========================================================================
// The controls the kit has no type for, the status line and the plugin's log.
//===========================================================================

if (mounted) {
  const params = mounted.params;
  const embed = query.has('embed') && query.get('embed') !== '0';

  // Copy link never carries a file path (it is in this page's memory).
  const toQuery = params.toQuery.bind(params);
  params.toQuery = () => {
    const q = toQuery();
    for (const p of PARAMS) if (p.ff.type === FF_TYPE_FILE) q.delete(p.id);
    return q;
  };

  if (!embed) {
    const button = (label, title) => {
      const b = document.createElement('button');
      b.type = 'button';
      b.className = 'btn';
      b.textContent = label;
      if (title) b.title = title;
      return b;
    };

    for (const p of PARAMS) {
      const field = document.getElementById(`p-${p.id}`);
      if (!field || field.className !== 'prow__text') continue;

      // Font Name: sent when committed, as a host sends a typed name.
      if (p.ff.type === FF_TYPE_TEXT && p.name === 'Font Name') {
        field.addEventListener('input', (event) => event.stopImmediatePropagation(), true);
        field.addEventListener('change', () => params.set(p.id, field.value));
        field.placeholder = 'type a family, then Enter';
        continue;
      }

      // FF_TYPE_FILE: the path, and a picker.
      if (p.ff.type === FF_TYPE_FILE) {
        field.readOnly = true;
        field.placeholder = 'no file';
        const cell = document.createElement('div');
        cell.style.display = 'flex';
        cell.style.gap = '6px';
        cell.style.minWidth = '0';
        field.replaceWith(cell);
        field.style.flex = '1 1 auto';
        field.style.minWidth = '0';
        const picker = document.createElement('input');
        picker.type = 'file';
        picker.accept = p.ff.extensions.map((e) => `.${e}`).join(',');
        picker.style.display = 'none';
        const choose = button('Choose…', `${p.ff.extensions.join(', ')} — read in this page, never uploaded`);
        const clear = button('×', 'No file');
        choose.addEventListener('click', () => picker.click());
        clear.addEventListener('click', () => params.set(p.id, ''));
        picker.addEventListener('change', async () => {
          const file = picker.files?.[0];
          picker.value = '';
          if (!file) return;
          const bytes = new Uint8Array(await file.arrayBuffer());
          const dir = `/files/${Date.now().toString(36)}`;
          core.FS.mkdirTree(dir);
          const path = `${dir}/${file.name.replace(/[/\\]/g, '_')}`;
          core.FS.writeFile(path, bytes);
          params.set(p.id, path);
        });
        cell.append(field, choose, clear, picker);
      }
    }

    //-----------------------------------------------------------------------
    // What the plugin drew (LastFrame), the fonts' credits and licences, and
    // the plugin's own log (Diag.cpp). They report; none of them measures.
    //-----------------------------------------------------------------------
    const stage = document.querySelector('.stage');
    if (stage) {
      const line = (id, live) => {
        const p = document.createElement('p');
        p.className = 'stage__status';
        p.id = id;
        p.setAttribute('aria-live', live);
        return p;
      };
      const status = line('knockout-frame', 'polite');
      const credits = line('knockout-fonts', 'off');
      const log = line('knockout-log', 'off');
      log.style.fontFamily = 'ui-monospace, SFMono-Regular, Menlo, monospace';
      log.style.fontSize = '12px';
      log.style.whiteSpace = 'pre-wrap';

      credits.append('Fonts bundled with this page, unmodified, under the SIL Open Font License 1.1: ');
      FONTS.families.forEach((f, i) => {
        if (i > 0) credits.append(' · ');
        const name = document.createElement('a');
        name.href = f.source;
        name.rel = 'noopener';
        name.textContent = f.name;
        const licence = document.createElement('a');
        licence.href = `./fonts/${f.folder}/${f.licence}`;
        licence.textContent = 'licence';
        credits.append(name, ` by ${f.designer} (`, licence, ')');
      });
      credits.append('. The rest of this page is in your browser’s own fonts.');

      stage.append(status, credits, log);
      const plural = (n, word) => `${n} ${word}${n === 1 ? '' : 's'}`;
      let lastStatus = '';
      let lastLog = '';
      setInterval(() => {
        const f = renderer?.frame();
        if (f) {
          const face = f.family
            ? `${f.family} ${f.style} (weight ${f.weight}${f.italic ? ', italic' : ''}${f.fromFile ? ', from Font File' : ''})`
            : 'no font';
          const table = f.table ? ` in a ${f.table.width} × ${f.table.height} texture` : '';
          const said = `Drawn in ${face}: ${plural(f.glyphs, 'letter')} with ink on ${plural(f.lines, 'line')}, ${f.drawn} drawn this frame and ${f.culled} off the frame or not yet revealed; ${plural(f.pieces, 'curve piece')}${table}; ${f.unitPx.toPrecision(3)} px a font unit (${f.unitsPerEm} units an em)${f.zoom > 1 ? ` at zoom ${f.zoom.toPrecision(4)}` : ''}.`;
          if (said !== lastStatus) status.textContent = lastStatus = said;
        }
        const text = renderer?.log() ?? '';
        if (text === lastLog) return;
        lastLog = text;
        const lines = text.trim().split('\n').filter(Boolean).map((l) => l.replace(/^\S+ (INFO |WARN |ERROR) knockout: /, '$1 '));
        log.textContent = lines.length
          ? `The plugin’s own log (Diag.cpp, in this page’s memory):\n${lines.slice(-2).join('\n')}`
          : '';
      }, 250);
    }
  }
}

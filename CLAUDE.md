# knockout

User-defined text as a mask over the clip, for Resolume Arena/Avenue: one FFGL effect,
drawn per pixel from the font's own curves. C++/GLSL, CMake MODULE → universal `.bundle`
(macOS); the Windows `.dll` has never been built. MIT.

| | id | display name | bundle | type |
| --- | --- | --- | --- | --- |
| text as a mask: Inside, Outside, on any Background | `KO01` | `SW Knockout` | `Knockout.bundle`, `com.stoatworks.ffgl.knockout` | effect |

Read `AGENTS.md` before touching the anchor arithmetic (`ProcessOpenGL`'s per-glyph
centre and the shader's `glyphAnchor`), the boundary/internal split in `Outline.cpp`,
the cubic tolerance, or the order of the parameters (hosts store them by index).

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build -j8`
- Install to Resolume: `cmake --install build` (into Arena's Extra Effects)
- Render offline: `./build/kotest --system-fonts --out /tmp/frame.png`. Without
  `--system-fonts` the harness sees only the test fonts in `tools/fixtures`.
- Settings: `--set "Text=Last\\nOrders" --set "Font Name=Didot" --set "Italic=1"
  --set "Mask=Outside" --set "Background=Colour"`; numbers are the host's 0..1
- List parameters (and every dropdown's elements): `./build/kotest --list`
- Press something later: `--at 30 "Reveal=0.5"`; several frames: `--frames 60`
- A clip through it: `ffmpeg -i in.mov -vf scale=1280:720 -f rawvideo -pix_fmt rgba - |
  ./build/kotest --system-fonts --pipe --size 1280x720 --script cues | ffmpeg -f rawvideo
  -pix_fmt rgba -s 1280x720 -i - out.mp4` (cue sheet: `frame Name value` lines)
- Remake the test fonts: `python3 tools/make-test-font.py` (verify.sh fails if the
  committed TTFs differ from what it writes)

## Verify
- Everything: `tools/verify.sh` (`OXBOW=` points at an oxbow binary if
  `~/Projects/resolume/oxbow/build/oxbow` isn't built).
- **No GL** (`--offline`): the control mappings invert, the ends and middles are where
  `Controls.h` says, CSS weight matching, 2000 random cubics within tolerance of their
  quadratics, every fixture piece monotonic in y, the CPU winding and distance against
  the polygons, the disc's distance against brute force, overlap resolution, text
  decoding, shader pieces under 15 000 bytes, parameter names and ranges, the About line.
- **The mask against the shapes**: `--coverage` (every pixel against the test fonts'
  polygons and, for the C, its eight quadratics, at two rasters, 0.005 tolerance),
  `--zoom` (an edge at 10×/100×/1000×, and 1000× at the far end of a line at 4K),
  `--edge` (Grow, Feather, Outline; hairlines), `--overlap`, `--winding`, `--stb`
  (against stb_truetype's rasteriser).
- **Where the text goes**: `--layout` (advances, kerning, tracking, lines, alignment),
  `--fit`, `--transform`, `--reveal`.
- **The rest**: `--composite` (Inside/Outside × four Backgrounds × Mix), `--state`,
  `--resize`, `--fonts` (choosing a face), `--text` (escapes, limits), `--controls` (no
  dead control).
- **This Mac's fonts**: `--system-fonts --real` (real TrueType, CFF and variable faces
  against stb_truetype), `--system-fonts --bench`.
- The checks can fail: `./build/kotest --negative` (8 wrong models), `tools/mutate.sh`
  (23 mutants, ~40 s: one copy, one build, each mutant recompiles one file).
- `KOTEST_RENDERER=software` runs any of it on Apple's software renderer.
- `KOTEST_DUMP=<folder>` makes `--stb` and `--real` write their masks and stb's as PNGs.

## Notes
- **Two passes.** Glyph: one instanced quad per glyph into an R16F mask, MAX blending,
  per pixel the winding over all the glyph's pieces and the distance to its boundary
  pieces. Composite: the clip, the mask and the Background, texel for texel.
- **Pieces** are y-monotonic quadratics (a straight piece flagged), two RGBA32F texels
  each in a texture buffer, uploaded once per face and text. Instances are three texels
  each, rebuilt every frame on the CPU in double.
- **Fonts**: the scan reads only each face's `name`, `OS/2` and `head` tables; stb_truetype
  parses the outlines and kerning of the one face in use. `KNOCKOUT_FONT_DIR` replaces the
  OS font directories (the harness points it at `tools/fixtures`).
- All host parameters are 0..1 and mapped in `Controls.cpp`; options hold the element
  index. Font is the family list at load time, so Font Name is what identifies a choice.
- GLSL reserved words (`patch sample input output filter common active half layout flat
  packed smooth`, `noise1`–`noise4`) must not be identifiers; `verify.sh` greps for them.
  Every shader piece stays under 15 000 bytes (MSVC C2026).
- Override `SetTextParameter` to return `FF_SUCCESS` for the About block, or no host can
  instantiate the plugin.
- `knockout_core` is an OBJECT library: the plugin registers itself from a file-scope
  constructor nothing references.
- The macOS build must be universal. Check with `lipo`, never the build log.
- **Provisional, normally generated:** `source/StoatworksAbout.h` is a hand copy in the
  shape `stoatworks-backend/scripts/sync-about.py` writes, and `ATTRIBUTIONS.md` likewise
  for `sync-attributions.py`. Once Knockout is in the backend's lists the sync overwrites
  both; edit there after that, not here.

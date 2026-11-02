# AGENTS.md — knockout

The why. `CLAUDE.md` has the commands.

Built 2026-11-02 at Allan's request, from a showmode session: "new resolume plugin that
creates user defined text as a mask over other content". Nothing more was specified, so
the choices below are mine and open to his veto: one effect (not a source), Inside and
Outside, any installed font, and a mask exact enough to zoom into.

## The one idea

A glyph is a few dozen quadratic curves. Each pixel of a glyph's quad works out, from those
curves alone, whether it is inside the glyph and its distance to the nearest edge; the
mask is a one-pixel ramp of that signed distance. Nothing is rasterised ahead of time, so
there is no resolution to run out of: Zoom 1000 is the same shader on smaller numbers.

- **Inside** is the non-zero winding number of a ray to +x over every piece, half-open in
  y so a ray through a join counts once and a ray grazing a top or bottom counts none.
  Every piece is monotonic in y (Outline.cpp splits them so), so a piece crosses a row at
  most once. Even-odd is the negative control: it empties X's middle.
- **Distance** is to the boundary pieces only, exact: the minimum of |B(t) − q| by
  splitting [0, 1] where the derivative cubic turns, bisecting each rise, then Newton.
  Not the closed-form cubic, which divides by |p0 − 2p1 + p2|² and goes wrong on the gently
  curved pieces most of a font is made of. A piece is skipped when its control box is
  further than the nearest so far.
- **The mask** is `clamp(0.5 + (sd·UnitPx + GrowPx) / Ramp)`, Ramp = 1 px + Feather; an
  Outline folds it to `0.5 + (Outline/2 − |sd|) / Ramp`. Grow, Feather and Outline are the
  same function of one distance, and `--edge` holds all of them to the README's formula.

## Decisions, and why

- **No atlas.** A signed-distance atlas (polyhedral's `Typeface`) rounds corners when you
  zoom in and has a fixed resolution; a mask people zoom into must come from the curves.
  Per-pixel cost is the glyph's piece count, culled by control box: 0.6 ms at 4K for the
  defaults on an M4 Max, 1.7 ms at Zoom 1000 into a stroke.
- **Overlaps are resolved on the CPU, once per face.** Variable fonts' default instances
  (SF, New York, most Google variable fonts) draw letters as overlapping strokes. The
  winding handles the fill, but the distance would see the edge of one stroke inside the
  other: a seam when filled, a stroke through the crossbar when outlined. So every piece
  is split where another crosses it (subdivision, then Newton on A(t) = B(s)), and each
  split piece is classified by the winding just either side of its middle: inside on both
  sides means internal. Internal pieces still count for the winding and not for the
  distance (`--overlap`; the negative controls that leave them in or measure to them).
- **The anchor.** Glyph coordinates at Zoom 1000 are large numbers that differ in their
  last bits; float loses them. So the CPU works out, in double, where the frame's centre
  is in each glyph's own units (c_g), rounds that to a float (the anchor), and keeps what
  the rounding left (rest = c_g − anchor, small). The shader subtracts the anchor from
  every piece and adds `rest` to the pixel's offset from the frame's centre: every number
  it handles is as big as the pixel's distance from the middle of the screen, whatever the
  zoom. The 1000× case at the end of a long line at 4K is 0.0018 with it and 0.037 with a
  float centre (`--zoom`; `kTestFloatCentre` is the negative control).
- **CFF cubics become quadratics** within 2e-5 em: each candidate's control is where the
  cubic's end tangents meet (smooth joins, which a least-squares fit doesn't give), it is
  sampled at 32 points and accepted at 0.8 of the tolerance, since the worst deviation
  falls between samples; `--offline` measures 2000 random cubics at 513 points. Split at
  t = ½ otherwise, ten deep at most.
- **R16F mask, MAX blending.** Glyph quads overlap (kerned AV, negative Tracking); each
  glyph is its own mask and the picture is their maximum, so two letters touching don't
  sum their ramps into a dark seam. Where two letters' ink overlaps, the coverage is the
  larger of the two, not their union: an edge pixel shared by A and V can be up to half a
  pixel's coverage light. Accepted. `GLState.h` saves and restores the blend equation,
  because a host that finds MAX left behind draws everything wrong.
- **The composite** reads clip and mask texel for texel (`texelFetch`), all premultiplied:
  scaling a premultiplied colour by a coverage is masking it. Mix 0 hands back the clip
  bit for bit (`--composite`).
- **Hairlines get heavier, not fainter.** A stroke w < 1 px under the one-pixel ramp draws
  (1 + w)²/4 px of ink. A box filter would draw w, but would need a second ramp for
  Outline only; one ramp for everything was kept, and the behaviour is measured
  (`--edge`) and in the README's limits.
- **Choosing a face** is CSS Fonts 4 §5.2: the family, then style, then the nearest weight.
  The Font list is whatever families this machine has when the plugin loads, so a position
  in it means nothing on another machine (see the next point). **Font Name wins**: setting
  it (any case) beats the list; moving the list alone writes the family into Font Name; a
  typed name that isn't installed is kept as typed and the default drawn. A composition
  restore sets both, and the name wins (`--fonts`).
- **The Font list's element values are `FamilyValue`, never positions.** Arena 7.28.1 saves
  an option as its element value (`<ParamChoice name="Font" value="3837474">` is Didot) and
  never reads back a Font Name the plugin fills in: it keeps its own copy, and Resolume
  ignores `FF_EVENT_FLAG_VALUE` (afterglow's AGENTS.md). With positions as values, a picked
  family reopened as whatever sat at that position elsewhere. The value is FNV-1a of the
  lowered name folded to 24 bits (exact in a float); a taken value moves up one, so only a
  clashing pair depends on what else is installed. Checked in Arena on 2 November 2026 by
  reopening on a six-family `KNOCKOUT_FONT_DIR`: Didot came back; a missing Copperplate drew
  the default, and Arena kept its value through a re-save.
- **Left out of the list:** families whose names start with "." (Apple's mark for faces
  apps shouldn't list; Font File reaches them), faces whose glyphs are bitmaps (`sbix`,
  `CBDT`: Apple Color Emoji's `glyf` is placeholder boxes), and CFF2, which stb_truetype
  doesn't parse. Variable fonts give their default instance only, so a single-file
  variable family (SF) has one face and Weight can't vary it.
- **No shaping.** Advances and kerning pairs only. HarfBuzz would be the way to do it
  properly (MIT, but a large dependency and a font-loading rewrite); until someone asks
  for Arabic or Devanagari it isn't worth it.
- **Limits that protect the GPU:** 4096 codepoints (5000 drawn in 2 ms on the CPU is the
  test); the piece table checked against `GL_MAX_TEXTURE_BUFFER_SIZE` before upload.
- **Reveal:** glyph k at rank r_k of R steps fades in over σ = 1 + Spread·(R − 1) steps,
  alpha = clamp((Reveal·(R − 1 + σ) − r_k) / σ). Random's ranks are a PCG shuffle with a
  fixed seed, the same in every instance and every run.

## Invariants

- **Parameter order is append-only** (before the About block). Hosts store parameters by
  index; a reorder breaks every saved composition.
- **Never hand the shader absolute glyph coordinates in float.** Everything goes through
  the anchor. A new pass or a new per-glyph quantity follows the same scheme.
- **Winding over every piece, distance over boundary pieces only.** Mixing those up is the
  seam bug, and two negative controls exist to catch it.
- **The harness never asks the plugin for the answer.** `--coverage` predicts from the
  test fonts' own description (polygons, and the C's eight quadratics worked out from
  `make-test-font.py`'s recipe with a brute-force distance, `quadSd`); `--stb` and
  `--real` compare with stb_truetype's rasteriser; placement comes from the README's
  formula (`placementFor`). The offline checks that do call `Outline.cpp` measure it
  against brute force. A check that reused the plugin's code would pass its bugs.
- **Two empty frames agree perfectly.** Every coverage case asserts it has edge pixels on
  screen; the 4K zoom case asserts its corner is on screen before zooming. A new case must
  do the same.
- **The test fonts are generated.** `tools/fixtures/*.ttf` are what
  `tools/make-test-font.py` writes, byte for byte; verify.sh checks. Change the script and
  rerun it, never the fonts.
- **Slug.** Eric Lengyel's GPU font renderer covers much the same ground and has been
  under a US patent. Nothing here comes from it. Don't add its band structures or its
  root-classification scheme to speed this up without checking the patent's current
  status and claims first.
- **`source/StoatworksAbout.h`, `ATTRIBUTIONS.md` and `.github/{FUNDING.yml,ISSUE_TEMPLATE}`
  are generated** by stoatworks-backend's syncs (registered 2 November 2026). Change the
  masters there; a hand edit here is overwritten by the next sync.
- **Private API: none.** Everything is FFGL 2.1 and GL 4.1 core.

## Not done

- Loaded into Arena on one Mac and driven over REST only (the README's Status says what was
  checked). Never in Avenue, never played by hand. Windows is built and loaded by CI
  (`tools/wincheck.ps1`) but has never drawn a pixel or been in Resolume.
- Not in the backend's lists, so no generated `.github` files; no user guide, website
  listing, video or release.

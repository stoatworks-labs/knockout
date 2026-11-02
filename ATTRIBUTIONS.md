# Attributions

Knockout is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### The harness, the tooling and the GL state — Stoatworks isohypse

<https://github.com/stoatworks-labs/isohypse>  
Licence: MIT  
Copyright: Stoatworks Labs

The harness's rig, PNG writer, cue sheets, --pipe, --negative, --state and --offline, and tools/verify.sh, tools/glslc.sh and tools/mutate.sh (slipstream's before it); source/Diag.{h,cpp}, source/GLState.h (radar's, which is vectrix's, from resolume-scopes) and source/Target.{h,cpp}, the fleet's PassBuffer; tools/wincheck.ps1, without the worlds. All adapted.

### The font scan — Stoatworks polyhedral

<https://github.com/stoatworks-labs/polyhedral>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Fonts.cpp's walk of the OS font directories reading only each face's name table, its name-record scoring and its collection sort (downpour's before it). Widened here to keep every face with its weight and italic, for CSS-style matching, and to give each family a list value made from its name.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

### stb_truetype

<https://github.com/nothings/stb>  
Licence: MIT or Public Domain (Unlicense), at your choice  
Copyright: Sean Barrett

Single header vendored at external/stb/stb_truetype.h.

Reads the font files: outlines, metrics and kerning pairs, and, where a plugin draws from an atlas, rasterises the glyphs into it. A whole font stack would be a large dependency for one job this header already does.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### The Fowler–Noll–Vo hash (FNV-1a) — Glenn Fowler, Landon Curt Noll and Kiem-Phong Vo

<http://www.isthe.com/chongo/tech/comp/fnv/>

FamilyValue() in source/Fonts.cpp: a family's list value is FNV-1a over its lowered name, folded to 24 bits so a float holds it exactly, which is what lets a picked font travel with a composition. kotest --fonts and --real check the values.

### PCG random number generation — Melissa E. O'Neill (2014)

<https://www.pcg-random.org/>

Reveal Order Random's ranks are a shuffle drawn from a PCG generator with a fixed seed, so every instance and every run reveals in the same order.

## Standards and published specifications

What the implementation is measured against.

- **CSS Fonts Module Level 4, §5.2 (font matching)** — Choosing a face: the family, then the style, then the nearest weight, looking down before up at 500 and below 400 and up first above 500, as source/Fonts.cpp does it. kotest --fonts checks every case.
- **OpenType and TrueType font files** — The scan reads each face's name, OS/2 and head tables; outlines are filled by the non-zero winding rule the formats specify, which the glyph shader works out per pixel (kotest --winding, with even-odd as the negative control).

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.

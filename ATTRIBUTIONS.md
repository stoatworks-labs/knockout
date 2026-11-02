# Attributions

Knockout is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is normally generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Knockout isn't in those lists yet, so this
copy was written by hand in the generated shape (2026-11-02); the first sync after it is
listed replaces it.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### The harness, the tooling, the GL state — Stoatworks isohypse

<https://github.com/stoatworks-labs/isohypse>  
Licence: MIT  
Copyright: Stoatworks Labs

The harness's rig, PNG writer, cue sheets, --pipe, --negative, --state and --offline, and tools/verify.sh, tools/glslc.sh and tools/mutate.sh (slipstream's before it). source/Diag.{h,cpp}, source/GLState.h (radar's, which is vectrix's, from resolume-scopes) and source/Target.{h,cpp}, the fleet's PassBuffer. All adapted.

### The font scan — Stoatworks polyhedral

<https://github.com/stoatworks-labs/polyhedral>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Fonts.cpp's walk of the OS font directories reading only each face's name table, its name-record scoring and its collection sort (downpour's before it). Widened here to keep every face with its weight and italic, for CSS-style matching.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl.

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### stb_truetype

<https://github.com/nothings/stb>  
Licence: MIT or Public Domain (dual, at your option)  
Copyright: Sean Barrett

Vendored at external/stb/stb_truetype.h (v1.26).

Parses the face in use: its TrueType and CFF outlines, metrics and kerning pairs. The plugin draws nothing with it; the harness uses its rasteriser as an independent reference to measure the plugin's mask against (--stb, --real).

### zlib

<https://zlib.net>  
Licence: zlib  
Copyright: Jean-loup Gailly and Mark Adler

The system's copy, linked by the test harness only, to compress the PNGs it writes. Not in the plugin.

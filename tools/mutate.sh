#!/usr/bin/env bash
#
# Mutation testing: change ONE character of the shipped code and require that a
# check fails. A check that still passes against a mutant was not looking at
# the code it claims to cover -- and a harness that compiled its own copy of
# the shaders would pass every GLSL mutant here.
#
# The tree is copied once into a temporary directory (the FFGL SDK and stb are
# symlinked, not copied) and built arm64-only. Each mutant is written into the
# copy, its object file thrown away so the build cannot keep the last one, and
# only the named check run; then the file is put back the same way. Every
# named check is run once against the unmutated copy first, so a check that
# fails anyway cannot pass for one that caught something.
#
#     tools/mutate.sh
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK:?}"' EXIT

# file | the exact text | its one-character mutant | the check that must fail | what it is
MUTANTS=(
	"source/Shaders.cpp|winding += p2.y > p0.y ? 1 : -1;|winding += p2.y > p0.y ? 1 : +1;|--coverage|GLSL: every crossing counted upward, so a hole fills, - -> +"
	"source/Shaders.cpp|t1 <= 1.0001 ? t1 : t2;|t1 <= 1.0001 ? t1 : t1;|--stb|GLSL: a curve's crossing taken from the wrong root, t2 -> t1"
	"source/Shaders.cpp|vec2 r = d + ( 2.0 * a + b * t ) * t;|vec2 r = d + ( 2.0 * a - b * t ) * t;|--coverage|GLSL: the distance to a curve measured to the wrong curve, + -> -"
	"source/Shaders.cpp|p1 = a.zw - glyphAnchor|p1 = a.zw + glyphAnchor|--coverage|GLSL: a curve's control point not moved to the anchor, - -> +"
	"source/Shaders.cpp|m = clamp( 0.5 + sd / Ramp, 0.0, 1.0 );|m = clamp( 0.6 + sd / Ramp, 0.0, 1.0 );|--edge|GLSL: every edge a tenth of a pixel out, 0.5 -> 0.6"
	"source/Shaders.cpp|* UnitPx + GrowPx;|* UnitPx - GrowPx;|--edge|GLSL: Grow the wrong way, + -> -"
	"source/Shaders.cpp|( 0.5 * OutlinePx - abs( sd ) )|( 0.6 * OutlinePx - abs( sd ) )|--edge|GLSL: the Outline a fifth too wide, 0.5 -> 0.6"
	"source/Shaders.cpp|vec4( m * glyphAlpha, 0.0, 0.0, 1.0 )|vec4( m / glyphAlpha, 0.0, 0.0, 1.0 )|--reveal|GLSL: a half-revealed glyph's alpha inverted, * -> /"
	"source/Shaders.cpp|Outside == 1 ? 1.0 - m : m;|Outside == 0 ? 1.0 - m : m;|--composite|GLSL: Inside and Outside swapped, 1 -> 0"
	"source/Shaders.cpp|Ground * ( 1.0 - k );|Ground * ( 1.0 + k );|--composite|GLSL: the Background laid over the letters too, - -> +"
	"source/Knockout.cpp|iy + ( -s * ux + c * uy )|iy + ( +s * ux + c * uy )|--transform|C++: the frame's centre turned the wrong way, - -> +"
	"source/Knockout.cpp|+ f.zoomX - fx - f.offsetX;|+ f.zoomX - fx + f.offsetX;|--transform|C++: Position X the wrong way, - -> +"
	"source/Knockout.cpp|reveal * ( steps - 1.0 + spread )|reveal * ( steps + 1.0 + spread )|--reveal|C++: Reveal two glyphs short at the top, - -> +"
	"source/Knockout.cpp|const double room = 1.0 - 2.0 * MarginFromParam|const double room = 1.0 - 3.0 * MarginFromParam|--fit|C++: Margin taken half again, 2 -> 3"
	"source/Knockout.cpp|SizeFromParam( params[ PT_SIZE ] ) * H / em;|SizeFromParam( params[ PT_SIZE ] ) * W / em;|--fit|C++: Size a share of the width, H -> W"
	"source/Knockout.cpp|0.5 * f.rampPx + 1.0;|0.5 * f.rampPx - 1.0;|--edge|C++: each glyph's quad cut inside its edge, + -> -"
	"source/Layout.cpp|Align::Centre ? -0.5 * width : -width;|Align::Centre ? -0.4 * width : -width;|--layout|C++: centred lines a tenth off, 0.5 -> 0.4"
	"source/Layout.cpp|pen += font->Kern( previous, glyph );|pen -= font->Kern( previous, glyph );|--layout|C++: kerning the wrong way, + -> -"
	"source/Layout.cpp|pen += tracking;|pen -= tracking;|--layout|C++: Tracking the wrong way, + -> -"
	"source/Outline.cpp|p.boundary         = left != right;|p.boundary         = left == right;|--overlap|C++: an overlap's inside pieces taken for its edge, ! -> ="
	"source/Outline.cpp|constexpr double kAccept = 0.8;|constexpr double kAccept = 8.8;|--offline|C++: a CFF curve accepted at eleven times the tolerance, 0 -> 8"
	"source/Outline.cpp|winding += p.p2.y > p.p0.y ? 1 : -1;|winding += p.p2.y > p.p0.y ? 1 : +1;|--offline|C++: the classifier's winding counted upward, - -> +"
	"source/Fonts.cpp|return w <= wanted ? d : 10000 + d;|return w >= wanted ? d : 10000 + d;|--offline|C++: a light weight looks up first, not down, < -> >"
)

tree="$WORK/tree"
mkdir -p "$tree/external"
cp -R "$REPO/source" "$REPO/tools" "$REPO/cmake" "$REPO/CMakeLists.txt" "$tree/"
ln -s "$REPO/external/ffgl" "$tree/external/ffgl"
ln -s "$REPO/external/stb" "$tree/external/stb"
cmake -S "$tree" -B "$tree/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null
build() { cmake --build "$tree/build" --target kotest -j"$(sysctl -n hw.ncpu)" >"$WORK/build.log" 2>&1; }
# Throws away the object file built from $1, so the next build compiles it
# whatever the clock says.
forget() { find "$tree/build" -name "$( basename "$1" ).o" -delete; }
build || { tail -20 "$WORK/build.log"; echo "the unmutated copy does not build"; exit 1; }

checks=$( for entry in "${MUTANTS[@]}"; do IFS='|' read -r _ _ _ check _ <<<"$entry"; echo "$check"; done | sort -u )
for check in $checks; do
	"$tree/build/kotest" "$check" >"$WORK/log" 2>&1 || { grep FAIL "$WORK/log" | head -3; echo "kotest $check fails without a mutant"; exit 1; }
done
printf 'baseline: %s pass unmutated\n' "$( echo $checks )"

caught=0
for entry in "${MUTANTS[@]}"; do
	IFS='|' read -r file original mutant check what <<<"$entry"
	cp "$REPO/$file" "$tree/$file"

	python3 - "$tree/$file" "$original" "$mutant" <<'PY'
import sys, pathlib
path, original, mutant = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text()
if text.count(original) != 1:
    sys.exit(f"mutation target found {text.count(original)} times in {path}: '{original}'")
if sum(a != b for a, b in zip(original, mutant)) != 1 or len(original) != len(mutant):
    sys.exit("a mutant must differ by exactly one character")
path.write_text(text.replace(original, mutant))
PY

	printf '\n== mutant: %s\n' "$what"
	forget "$file"
	if ! build; then
		printf '   FAIL  the mutant does not build -- pick one that compiles\n'
		tail -5 "$WORK/build.log" | sed 's/^/        /'
	elif "$tree/build/kotest" "$check" >"$WORK/log" 2>&1; then
		printf '   FAIL  %s still PASSES -- the check does not cover this code\n' "$check"
	else
		printf '   ok    %s fails against the mutant:\n' "$check"
		{ grep -E '^  FAIL' "$WORK/log" || true; } | head -2 | cut -c1-160 | sed 's/^/        /'
		caught=$(( caught + 1 ))
	fi
	cp "$REPO/$file" "$tree/$file"
	forget "$file"
done

printf '\nmutants: %d, caught: %d\n' "${#MUTANTS[@]}" "$caught"
[[ "$caught" -eq "${#MUTANTS[@]}" ]]

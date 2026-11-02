#!/usr/bin/env bash
#
# Everything, in the order that fails fastest.
#
# The build is universal on purpose. An arm64-only bundle builds and tests
# perfectly well here and then fails to load in an Intel Resolume, and the
# build log calls it a success either way -- so the architecture is checked
# with lipo, never with the log.
#
#     tools/verify.sh [BUILD_DIR]
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$REPO/build-verify}"

cd "$REPO"

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
fail() { printf '\033[31mFAIL\033[0m %s\n' "$1"; exit 1; }

#---------------------------------------------------------------------------
# GLSL reserved words, declared as identifiers.
#
# glslc catches these, but glslc is optional -- so the trap most likely to be
# walked into gets its own grep that always runs. The list is GLSL 4.10's
# keywords reserved for future use (3.6) plus the ones already bitten:
# `packed` passed Apple's compiler and glslc and failed only Mesa (atrac).
# It looks for a declaration, not a mention, so prose may still use them.
#---------------------------------------------------------------------------
reserved_words() {
	local words="common partition active asm class union enum typedef template this resource goto inline noinline
	             public static extern external interface long short half fixed unsigned superp input output filter
	             sizeof cast namespace using packed sample patch layout flat smooth noperspective precise"
	local bad=0 word
	for word in $words; do
		if grep -nE "(float|int|uint|bool|vec[234]|ivec[234]|uvec[234]|mat[234])[[:space:]]+$word[[:space:]]*[;=,)]" \
		            source/Shaders.cpp >/dev/null 2>&1; then
			printf '   "%s" is declared as an identifier and is a GLSL reserved word\n' "$word"
			bad=$(( bad + 1 ))
		fi
	done
	[ "$bad" -eq 0 ] && printf '   none of the reserved words is used as an identifier\n'
	return "$bad"
}

step "GLSL reserved words"
reserved_words || fail "a GLSL reserved word is used as an identifier"

#---------------------------------------------------------------------------
step "GLSL built-ins the user's names would shadow"
#---------------------------------------------------------------------------
# noise1..noise4 are GLSL built-ins (deprecated, returning vectors): a user
# function of that name is shadowed, and Apple's compiler reports only an
# incompatible type where it is used (dice's trap). This plugin has no noise;
# keep any it grows away from those names.
if grep -nE '(float|vec[234])[[:space:]]+noise[1-4][[:space:]]*\(' source/Shaders.cpp; then
	fail "a function named noise1..noise4 shadows a GLSL built-in"
fi
echo "   none"

#---------------------------------------------------------------------------
step "Shaders"
#---------------------------------------------------------------------------
tools/glslc.sh || fail "a shader does not compile"

#---------------------------------------------------------------------------
step "Test fonts"
#---------------------------------------------------------------------------
# The fixtures are what tools/make-test-font.py writes, byte for byte: the
# checks' idea of every glyph is that file's, so a font edited by hand, or
# a script changed and not rerun, would have them measuring something else.
fonts="$( mktemp -d )"
python3 tools/make-test-font.py "$fonts" >/dev/null || fail "tools/make-test-font.py"
for font in "$fonts"/*.ttf; do
	cmp -s "$font" "tools/fixtures/$( basename "$font" )" \
		|| fail "tools/fixtures/$( basename "$font" ) is not what tools/make-test-font.py writes -- rerun it"
done
echo "ok   $( ls "$fonts" | wc -l | tr -d ' ' ) fixtures, all as tools/make-test-font.py writes them"
rm -rf "$fonts"

#---------------------------------------------------------------------------
step "Submodule"
#---------------------------------------------------------------------------
if [[ ! -f external/ffgl/CMakeLists.txt ]]; then
	fail "FFGL SDK missing -- run: git submodule update --init --recursive"
fi
pin="$(git -C external/ffgl rev-parse --short=7 HEAD)"
[[ "$pin" == "b1afaf9" ]] || fail "FFGL SDK at $pin, not the fleet's b1afaf9"
echo "ok   FFGL SDK pinned at $pin"

#---------------------------------------------------------------------------
step "Build (universal)"
#---------------------------------------------------------------------------
log="$( mktemp )"
cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >"$log" 2>&1 || { cat "$log"; fail "configure"; }
cmake --build "$BUILD" -j"$(sysctl -n hw.ncpu)" >"$log" 2>&1 || { tail -40 "$log"; fail "build"; }
# The SDK's own FFGLUtilities.cpp warns about RAND_MAX; anything else is ours.
if grep -E 'warning:' "$log" | grep -v 'external/ffgl' | grep -q .; then
	grep -E 'warning:' "$log" | grep -v 'external/ffgl' | sed 's/^/   /'
	fail "a warning in this repo's code"
fi
rm -f "$log"
echo "ok   built, no warnings outside the SDK"
KOTEST="$BUILD/kotest"

#---------------------------------------------------------------------------
step "Bundle"
#---------------------------------------------------------------------------
check_bundle() {
	local bundle="$1" executable="$2" identifier="$3" id="$4" name="$5" kind="$6"
	local binary="$bundle/Contents/MacOS/$executable"
	[[ -f "$binary" ]] || fail "no binary at $binary"

	local arches
	arches="$(lipo -archs "$binary")"
	[[ "$arches" == *arm64* ]]  || fail "$executable: no arm64 slice (got: $arches)"
	[[ "$arches" == *x86_64* ]] || fail "$executable: no x86_64 slice (got: $arches)"

	# Captured, then matched from a herestring -- never `nm ... | grep -q`:
	# under pipefail the grep's early exit SIGPIPEs nm and fails the pipeline.
	local symbols
	symbols=$( nm -gU "$binary" 2>/dev/null || true )
	grep -q '_plugMain' <<<"$symbols" || fail "$executable: plugMain not exported"
	echo "ok   $executable: $arches, plugMain exported"

	local plist="$bundle/Contents/Info.plist"
	[[ -f "$plist" ]] || fail "no Info.plist in $bundle"
	read_plist() { /usr/libexec/PlistBuddy -c "Print :$1" "$plist" 2>/dev/null || true; }
	local declared
	declared="$( sed -n 's/^[[:space:]]*VERSION \([0-9.]*\)$/\1/p' CMakeLists.txt | head -1 )"
	[[ -n "$declared" ]] || fail "no VERSION found in CMakeLists.txt"
	[[ "$( read_plist CFBundleIdentifier )" == "$identifier" ]] || fail "$executable: bundle id is '$( read_plist CFBundleIdentifier )'"
	[[ "$( read_plist CFBundleExecutable )" == "$executable" ]] || fail "$executable: CFBundleExecutable is wrong"
	[[ "$( read_plist CFBundlePackageType )" == "BNDL" ]] || fail "$executable: CFBundlePackageType is not BNDL"
	[[ "$( read_plist CFBundleVersion )" == "$declared" ]] || fail "$executable: plist version != CMakeLists $declared"
	grep -q "versionFallback = \"v$declared\"" source/StoatworksAbout.h \
		|| fail "StoatworksAbout.h's versionFallback is not v$declared"
	echo "ok   $identifier, BNDL, v$declared -- plist, CMakeLists and About agree"

	codesign --force --sign - --timestamp=none "$bundle" >/dev/null 2>&1 || fail "$bundle could not be ad-hoc signed"
	codesign --verify --deep --strict "$bundle" >/dev/null 2>&1 || fail "$bundle's ad-hoc signature does not verify"
	echo "ok   ad-hoc signed and verified"

	# What a host reads: id, name, type. The FFGL name field is char[16] and
	# not null-terminated, so only something reading it back as a host does
	# notices a truncation.
	local OXBOW="${OXBOW:-$HOME/Projects/resolume/oxbow/build/oxbow}"
	if [[ -x "$OXBOW" ]]; then
		local probe
		probe="$( "$OXBOW" probe "$bundle" 2>&1 || true )"
		printf '%s\n' "$probe" | sed -n '1,3p' | sed 's/^/   /'
		grep -q "id:          $id" <<<"$probe" || fail "oxbow did not read the id $id"
		grep -q "name:        $name\$" <<<"$probe" || fail "oxbow did not read the name '$name'"
		grep -q "type:        $kind" <<<"$probe" || fail "oxbow did not read the type as $kind"
		local selftest
		selftest="$( "$OXBOW" selftest "$bundle" 2>&1 || true )"
		grep -q "selftest:    PASS" <<<"$selftest" || { printf '%s\n' "$selftest" | tail -20; fail "oxbow selftest failed on $bundle"; }
		echo "ok   a host reads $name $id $kind, and oxbow selftest renders 120 frames through it"
	else
		echo "   skipped: no oxbow at $OXBOW (set OXBOW=...)"
	fi
}

check_bundle "$BUILD/Knockout.bundle" "Knockout" "com.stoatworks.ffgl.knockout" KO01 "SW Knockout" effect

#---------------------------------------------------------------------------
step "Offline (no GL)"
#---------------------------------------------------------------------------
"$KOTEST" --offline >/dev/null || { "$KOTEST" --offline | grep FAIL; fail "kotest --offline"; }
echo "ok   kotest --offline: $( "$KOTEST" --offline | grep -c '^  ok' ) checks"

#---------------------------------------------------------------------------
step "Checks (this Mac's GPU)"
#---------------------------------------------------------------------------
# Every claim the README makes, in the order the README makes them. They run
# on the test fonts (tools/fixtures), whose every glyph is known exactly.
CHECKS="coverage zoom edge overlap winding layout fit transform reveal composite state resize fonts text stb controls"
for check in $CHECKS; do
	if out=$( "$KOTEST" --$check 2>&1 ); then
		printf '   ok   kotest --%s: %s checks\n' "$check" "$( printf '%s\n' "$out" | grep -c '^  ok' )"
	else
		printf '%s\n' "$out" | grep -E 'FAIL' | sed 's/^/      /'
		fail "kotest --$check"
	fi
done

#---------------------------------------------------------------------------
step "Real fonts (this Mac's)"
#---------------------------------------------------------------------------
# The same measurements on faces nobody here drew: TrueType and CFF, upright
# and italic, a variable font's overlapping strokes, against stb_truetype's
# rasteriser. Which faces it finds is this Mac's business, so it reports
# what it ran on.
if out=$( "$KOTEST" --system-fonts --real 2>&1 ); then
	printf '   ok   kotest --system-fonts --real: %s checks\n' "$( printf '%s\n' "$out" | grep -c '^  ok' )"
else
	printf '%s\n' "$out" | grep -E 'FAIL' | sed 's/^/      /'
	fail "kotest --system-fonts --real"
fi

#---------------------------------------------------------------------------
step "Software renderer"
#---------------------------------------------------------------------------
# Every check again on Apple's software renderer, which is what a GPU-less
# runner falls back to: a check that only holds on this GPU is found here.
for check in $CHECKS; do
	if out=$( KOTEST_RENDERER=software "$KOTEST" --$check 2>&1 ); then
		printf '   ok   kotest --%s (software): %s checks\n' "$check" "$( printf '%s\n' "$out" | grep -c '^  ok' )"
	else
		printf '%s\n' "$out" | grep -E 'FAIL' | sed 's/^/      /'
		fail "kotest --$check on the software renderer -- run: KOTEST_RENDERER=software $KOTEST --$check"
	fi
done

#---------------------------------------------------------------------------
step "Pipe"
#---------------------------------------------------------------------------
# The fleet's --pipe frame format, which the video renders through. Two and a
# half frames in must be exactly two out and a clean exit -- a partial frame is
# the end of the stream, never a frame -- a cue naming no parameter must be
# refused, and a reader that hangs up must end the run with exit 1, not
# SIGPIPE's silent 141.
frame=$(( 64 * 36 * 4 ))
raw=$( mktemp ); cues=$( mktemp ); out=$( mktemp )
head -c $(( frame * 5 / 2 )) /dev/zero > "$raw"
status=0
"$KOTEST" --pipe --size 64x36 < "$raw" > "$out" 2>/dev/null || status=$?
got=$( wc -c < "$out" | tr -d ' ' )
[[ "$status" -eq 0 && "$got" == "$(( frame * 2 ))" ]] \
	|| fail "2.5 frames in gave $got bytes out (want $(( frame * 2 ))), exit $status"
echo "ok   2.5 frames in, exactly 2 frames out, clean exit"
printf '0 No Such Control 0.5\n' > "$cues"
status=0
"$KOTEST" --pipe --size 64x36 --script "$cues" < "$raw" >/dev/null 2>&1 || status=$?
[[ "$status" -eq 2 ]] || fail "a cue naming no parameter gave exit $status, not 2"
echo "ok   a cue naming no parameter is refused (exit 2)"
head -c $(( frame * 20 )) /dev/zero > "$raw"
set +e
"$KOTEST" --pipe --size 64x36 < "$raw" 2>/dev/null | head -c 1 >/dev/null
s1=${PIPESTATUS[0]}
set -e
[[ "$s1" -eq 1 ]] || fail "a closed stdout gave exit $s1, not 1"
echo "ok   | head -c 1: exit 1, not SIGPIPE's 141"
rm -f "$raw" "$cues" "$out"

#---------------------------------------------------------------------------
step "Negative controls"
#---------------------------------------------------------------------------
# Every wrong model must fail a check of its own. A check that cannot fail is
# not a check.
if out=$( "$KOTEST" --negative 2>&1 ); then
	printf '%s\n' "$out" | grep -E 'wrong models caught' | sed 's/^/   /'
else
	printf '%s\n' "$out" | grep -E 'MISS' | sed 's/^/      /'
	fail "a negative control went undetected"
fi

#---------------------------------------------------------------------------
step "Mutants"
#---------------------------------------------------------------------------
if out=$( tools/mutate.sh 2>&1 ); then
	printf '%s\n' "$out" | grep -E '^mutants' | sed 's/^/   /'
else
	printf '%s\n' "$out" | grep -E 'FAIL|^mutants' | sed 's/^/   /'
	fail "a mutant survived -- run tools/mutate.sh"
fi

#---------------------------------------------------------------------------
step "Cost"
#---------------------------------------------------------------------------
"$KOTEST" --system-fonts --bench

printf '\n\033[32mall green\033[0m\n'

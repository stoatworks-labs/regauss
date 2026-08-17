#!/usr/bin/env bash
#
# Everything, in the order that fails fastest.
#
# Each check answers a question none of the others can:
#
#   --field       does the GLSL field match Field.cpp -- the one thing the
#                 OpenFX build and the browser demo both copy
#   --purity      identity at rest, a clean rotation at one phosphor, and no
#                 purity error on a mask that cannot have one
#   --coil        does the coil really demagnetise the mask, and does the
#                 magnetisation really come back afterwards
#   sweep.py      does every control actually reach the picture
#   gains         are the measured mask gains still the right ones
#   registration  does the bundle contain its own plugin and nobody else's
#   OFX plist     does the OpenFX bundle sign -- the release step, run early
#   lipo          is the macOS build really universal
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build}"
failures=0

step() {
	printf '\n\033[1m== %s\033[0m\n' "$1"
}

check() {
	if "$@"; then
		return 0
	fi
	printf '\033[31mFAILED: %s\033[0m\n' "$*"
	failures=$((failures + 1))
}

if [ ! -x "$BUILD/rgtest" ]; then
	echo "$BUILD/rgtest not found."
	echo "Configure with -DREGAUSS_BUILD_TOOLS=ON and build first:"
	echo "  cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD"
	exit 1
fi

step "the GLSL field against Field.cpp"
check "$BUILD/rgtest" --field

step "the purity model"
check "$BUILD/rgtest" --purity

step "the degauss coil"
check "$BUILD/rgtest" --coil

step "no dead controls"
check python3 tools/sweep.py

# ---------------------------------------------------------------------------
# The browser demo's shaders.
#
# The rest of the fleet's demos carry their GLSL as strings "copied unedited
# from source/shaders/", and nothing checks that claim. A copy is a copy: the
# plugin's shader gets a fix, the demo's does not, and the page quietly stops
# being a demo of the plugin while continuing to look like one.
#
# demo/shaders.js is generated instead, and committed — the demo is served as
# it is, with no build step, and that has to stay true. This is what keeps the
# committed copy honest.
# ---------------------------------------------------------------------------
if [ -f demo/shaders.js ] && command -v node >/dev/null 2>&1; then
	step "the demo runs the plugin's shaders"
	check node demo/extract-shaders.mjs --check
fi

# ---------------------------------------------------------------------------
# The mask gains.
#
# MaskSpec::gain compensates for the light the mask blocks, and it is measured
# rather than derived -- edge shaping and anti-aliasing both eat into the duty
# cycle, so the analytic answer is wrong by a few per cent. That means it can
# go stale silently: change a mask shape and every pattern quietly gets
# brighter or darker than the one beside it, which reads as "that mask looks a
# bit dim" and never as a bug.
#
# Measure at 0.30, not at a low level: see the note in source/Masks.cpp. At
# 0.05 the 8-bit readback's rounding is larger than the quantity being
# measured.
# ---------------------------------------------------------------------------
step "the mask gains still hold"
gain_at() {
	"$BUILD/rgtest" --flat 0.30 --measure --frames 4 --width 1280 --height 960 --out /tmp/rg-gain.png \
		--set "Magnetisation=0" --set "Interference=0" --set "Deflection=0" --set "Purity=0" \
		--set "Scanlines=0" --set "Curvature=0" --set "Vignette=0" --set "Corner Radius=0" \
		--set "Halation=0" --set "Persistence=0" --set "Mask Strength=1.0" \
		--set "Mask Pattern=$1" 2>/dev/null | awk '/mean/ {print ($3+$4+$5)/3}'
}

reference=$(gain_at 0)
if [ -z "$reference" ]; then
	printf '\033[31mFAILED: could not measure the unmasked reference\033[0m\n'
	failures=$((failures + 1))
else
	for pattern in 1 2 3 4; do
		measured=$(gain_at "$pattern")
		verdict=$(python3 -c "
import sys
r, m = float('$reference'), float('$measured')
off = abs(m - r) / r * 100.0
print(f'{m:.4f} {off:.2f} {\"ok\" if off < 2.0 else \"OFF\"}')
")
		set -- $verdict
		if [ "$3" = "ok" ]; then
			printf 'ok   pattern %s: %s (%.2f%% off the reference %s)\n' "$pattern" "$1" "$2" "$reference"
		else
			printf '\033[31mFAILED: pattern %s measures %s, %s%% off the reference %s -- re-measure the gain\033[0m\n' \
				"$pattern" "$1" "$2" "$reference"
			failures=$((failures + 1))
		fi
	done
fi

# ---------------------------------------------------------------------------
# Registration.
#
# The failure this catches is specific and silent: `CFFGLPluginInfo` registers
# itself from a file-scope constructor and nothing references it by name, so a
# linker that drops the translation unit gives a bundle which loads, exports
# plugMain, and reports that it contains no plugins. Resolume shows an empty
# effects list and no error at all.
# ---------------------------------------------------------------------------
step "the bundle contains its own plugin"
binary="$BUILD/Regauss.bundle/Contents/MacOS/Regauss"
if [ ! -f "$binary" ]; then
	printf '\033[31mFAILED: %s not built\033[0m\n' "$binary"
	failures=$((failures + 1))
else
	# Read once into variables rather than piping into `grep -q`.
	#
	# `grep -q` exits the instant it matches, which closes the pipe under the
	# still-running `nm` or `strings`; they take SIGPIPE and exit 141, and with
	# `set -o pipefail` the *pipeline* is then a failure however well the grep
	# went. It is only intermittent from the shell -- a short output fits the
	# pipe buffer and the writer finishes before the reader leaves.
	symbols=$(nm -gU "$binary" 2>/dev/null)
	literals=$(strings "$binary" 2>/dev/null)

	if ! grep -q plugMain <<<"$symbols"; then
		printf '\033[31mFAILED: the bundle exports no plugMain\033[0m\n'
		failures=$((failures + 1))
	elif ! grep -qx "RG01" <<<"$literals"; then
		printf '\033[31mFAILED: the bundle does not carry its own id RG01\033[0m\n'
		failures=$((failures + 1))
	else
		printf 'ok   exports plugMain and carries RG01\n'
	fi
fi

# ---------------------------------------------------------------------------
# The OpenFX bundle's plist.
#
# This check exists because it went wrong in flipbook. cmake/InfoOFX.plist.in
# gets copied from repo to repo, and the version it was copied from had the
# PREVIOUS plugin's name hardcoded into CFBundleExecutable. NOTHING caught it:
# the bundle assembles, the binary is universal, the OFX entry point exports,
# ofxprobe loads it and renders through it. It fails only at release time, in
# codesign, with a message that names a "subcomponent" and never mentions the
# plist.
#
# So the check is the release step itself, run here where it is cheap -- on a
# COPY of the bundle, so a verify run never leaves a signature on the build
# tree that the release job did not put there.
# ---------------------------------------------------------------------------
if [ -d "$BUILD/Regauss.ofx.bundle" ]; then
	step "the OpenFX bundle signs"

	plist="$BUILD/Regauss.ofx.bundle/Contents/Info.plist"
	named=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$plist" 2>/dev/null)
	if [ ! -f "$BUILD/Regauss.ofx.bundle/Contents/MacOS/$named" ]; then
		printf '\033[31mFAILED: Info.plist names "%s", which is not in Contents/MacOS\033[0m\n' "$named"
		failures=$((failures + 1))
	else
		scratch="${TMPDIR:-/tmp}/regauss-signcheck.ofx.bundle"
		rm -rf "$scratch"
		cp -R "$BUILD/Regauss.ofx.bundle" "$scratch"
		if codesign --force --sign - --timestamp=none "$scratch" >/dev/null 2>&1; then
			printf 'ok   CFBundleExecutable is %s, and the bundle ad-hoc signs\n' "$named"
		else
			printf '\033[31mFAILED: the OpenFX bundle will not codesign\033[0m\n'
			codesign --force --sign - --timestamp=none "$scratch" 2>&1 | sed 's/^/       /'
			failures=$((failures + 1))
		fi
		rm -rf "$scratch"
	fi
fi

# ---------------------------------------------------------------------------
# Universal.
#
# CMake latches CMAKE_OSX_ARCHITECTURES when the first target is created, so
# setting it late is silently ignored and the build log still says success. The
# only honest answer comes from lipo. Skipped when the developer asked for a
# single-architecture build on purpose.
# ---------------------------------------------------------------------------
step "the macOS build is universal"
if grep -q "CMAKE_OSX_ARCHITECTURES:.*arm64;x86_64" "$BUILD/CMakeCache.txt" 2>/dev/null; then
	arches=$(lipo -archs "$binary" 2>/dev/null)
	case "$arches" in
		*arm64*x86_64* | *x86_64*arm64*)
			printf 'ok   Regauss: %s\n' "$arches" ;;
		*)
			printf '\033[31mFAILED: Regauss is %s, not universal\033[0m\n' "${arches:-missing}"
			failures=$((failures + 1)) ;;
	esac
else
	echo "skipped: this build was configured for one architecture"
fi

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit "$failures"

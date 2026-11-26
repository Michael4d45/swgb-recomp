#!/bin/sh
# Configure and build the native macOS host: the lifted C on pcrecomp's
# runtime/win32hle, a 64-bit program (arm64 or x86-64) with no Wine. The
# guest's 4 GB is one block of the host's memory (cmake/native.cmake,
# docs/linux-macos.md).
#
#   ./build-macos.sh                       # build-macos/swgbrecomp
#   build-macos/swgbrecomp --run --game game
#
# Needs a lift (run_lift.py), Xcode's command line tools, and Homebrew's
# cmake, ninja, pkgconf, sdl2 and sdl2_ttf. BUILD_DIR, PCRECOMP and CMAKE_ARGS
# override. HLE_BASED=1 builds the same way on 64-bit Linux.
set -e
cd "$(dirname "$0")"
BUILD_DIR=${BUILD_DIR:-build-macos}
PCRECOMP=${PCRECOMP:-$(cd .. && pwd)/pcrecomp}
# The pcrecomp it needs (tools/pcrecomp_pin.sh): cloned when there is none,
# checked when there is. A configured build keeps the one it was set up with.
# SWGB_PCRECOMP_ANY=1 builds against whatever is there.
. ./tools/pcrecomp_pin.sh
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
  cached=$(sed -n 's/^PCRECOMP:[A-Z]*=//p' "$BUILD_DIR/CMakeCache.txt")
  [ -z "$cached" ] || PCRECOMP=$cached
fi
if [ ! -d "$PCRECOMP" ]; then
  echo "pcrecomp is not at $PCRECOMP: cloning $PCRECOMP_PIN_BRANCH of $PCRECOMP_PIN_URL"
  pcrecomp_pin_clone "$PCRECOMP"
fi
why=$(pcrecomp_pin_why "$PCRECOMP")
if [ -n "$why" ] && [ "${SWGB_PCRECOMP_ANY:-0}" = 0 ]; then
  echo "$(basename "$0"): $why." >&2
  echo "  sh tools/pcrecomp_pin.sh fetch \"$PCRECOMP\" checks that commit out there (./setup.sh asks to);" >&2
  echo "  SWGB_PCRECOMP_ANY=1 builds against it as it is." >&2
  exit 1
fi
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  # shellcheck disable=SC2086  # CMAKE_ARGS is a list of flags
  cmake -S . -B "$BUILD_DIR" -G Ninja -DPCRECOMP="$PCRECOMP" $CMAKE_ARGS
fi
cmake --build "$BUILD_DIR" "$@"

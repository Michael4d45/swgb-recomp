#!/bin/sh
# Configure and build the native Linux host: the lifted C on pcrecomp's
# runtime/win32hle, a 32-bit Linux program with no Wine (src/linux,
# cmake/native.cmake, docs/linux-macos.md).
#
#   ./build-linux.sh                       # build-linux/swgbrecomp
#   CMAKE_ARGS=-DWITH_FFMPEG=ON ./build-linux.sh
#   build-linux/swgbrecomp --run --game game
#
# Needs a lift (run_lift.py), clang or gcc with -m32 (gcc-multilib), cmake,
# ninja, and SDL2 and SDL2_ttf for i386 (./setup.sh installs them and does
# the lift). Prefers clang when CC is unset (musttail for THIS_COD ITAILs;
# gcc 15+ also works with -O1). BUILD_DIR, PCRECOMP and CMAKE_ARGS override.
# pcrecomp is the fork's branch at the commit tools/pcrecomp_pin.sh names.
set -e
cd "$(dirname "$0")"
BUILD_DIR=${BUILD_DIR:-build-linux}
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
# clang: musttail turns RECOMP_ITAIL into a jmp (gcc grows the stack on
# THIS_COD blitters). Override with CC=gcc if you must.
if [ -z "${CC:-}" ] && command -v clang >/dev/null 2>&1; then
  CC=clang
  export CC
fi
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  # shellcheck disable=SC2086  # CMAKE_ARGS is a list of flags
  cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/linux-i386.cmake -DPCRECOMP="$PCRECOMP" \
    ${CC:+-DCMAKE_C_COMPILER="$CC"} $CMAKE_ARGS
fi
cmake --build "$BUILD_DIR" "$@"

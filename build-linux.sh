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
# ninja, and SDL2 and SDL2_ttf for i386. Prefers clang when CC is unset
# (musttail for THIS_COD ITAILs; gcc 15+ also works with -O1). BUILD_DIR,
# PCRECOMP and CMAKE_ARGS override.
set -e
cd "$(dirname "$0")"
BUILD_DIR=${BUILD_DIR:-build-linux}
PCRECOMP=${PCRECOMP:-$(cd .. && pwd)/pcrecomp}
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

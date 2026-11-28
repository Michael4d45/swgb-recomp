#!/bin/sh
# Compiles every object of both hosts, swgbrecomp and swgbrecomp_cc, with the
# build's own flags, and links nothing: no lift is needed, so CI runs it on
# every push (.github/workflows/unit.yml), and a merge can run it before it
# is pushed. A bad merge of a host source fails here.
#
#   sh tools/compile_hosts.sh BUILD_DIR [cmake args...]
#
#   Linux:   sh tools/compile_hosts.sh out/hosts -DCMAKE_TOOLCHAIN_FILE=cmake/linux-i386.cmake
#   Windows: the same, without the toolchain, from an x86 MSVC prompt
#            (vcvarsall amd64_x86, as build.cmd sets up)
#
# BUILD_DIR (relative to the checkout, or absolute) is a directory of its own:
# the configure points SWGB_GEN_LIB and SWGB_CC_GEN_LIB at an empty file,
# which only the link would read. Give -DPCRECOMP=DIR unless pcrecomp is
# beside this checkout. The build type is build.cmd's: MSVC's default, Debug,
# does not build (miniz's /O2 against /RTC1).
set -e
[ $# -ge 1 ] || { echo "usage: sh tools/compile_hosts.sh BUILD_DIR [cmake args...]" >&2; exit 2; }
build=$1
shift
cd "$(dirname "$0")/.."
mkdir -p "$build"
: > "$build/no-lift"
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSWGB_GEN_LIB="$build/no-lift" -DSWGB_CC_GEN_LIB="$build/no-lift" "$@"
# Every compile edge of the two host targets (rule C_COMPILER__swgb_...).
objs=$(ninja -C "$build" -t targets all | tr -d '\r' | sed -n 's/: C_COMPILER__swgb.*//p')
for t in swgb swgb_cc; do
  printf '%s\n' "$objs" | grep -q "$t[.]dir" || { echo "$0: $build has no objects of $t" >&2; exit 1; }
done
# shellcheck disable=SC2086  # a list of targets
ninja -C "$build" $objs

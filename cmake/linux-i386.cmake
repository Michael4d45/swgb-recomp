# The native Linux host (src/linux, on pcrecomp's runtime/win32hle): a 32-bit
# program, so the guest's addresses are host addresses. clang preferred (its
# musttail makes RECOMP_ITAIL a real jmp; gcc grows the C stack on THIS_COD
# blitters and SIGSEGVs entering Standard Game). gcc -m32 still configures.
# SDL2 and SDL2_ttf for i386 (docs/linux-macos.md lists the packages).
#
#   cmake -S . -B build-linux -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/linux-i386.cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)
# Prefer clang when the user did not name a compiler: musttail for ITAIL.
# A plain variable, not a FORCEd cache entry: a FORCEd compiler differs from
# the cached one when cmake regenerates the build, and CMake then deletes the
# cache and configures again without this file (an x86-64 build, which fails
# to link the i386 SDL2).
if(NOT CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
  find_program(_SWGB_CLANG clang)
  if(_SWGB_CLANG)
    set(CMAKE_C_COMPILER "${_SWGB_CLANG}")
  endif()
endif()
# SSE2 for doubles, as MSVC's x86 builds compute them: i386 gcc and clang use the
# x87 by default, whose 80-bit registers round the lifted float code differently,
# and a multiplayer game against a Windows host would go out of sync
# (docs/multiplayer.md, "Windows and Linux"). No contraction into FMAs either.
set(CMAKE_C_FLAGS_INIT "-m32 -msse2 -mfpmath=sse -ffp-contract=off")
# Not position-independent: i386 PIC spends a register on the GOT, and the
# host sits at 0x08048000, clear of the guest's 0x00400000..0x00A5A000.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-m32 -no-pie")
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
# The 32-bit .pc files first: Debian and Ubuntu, Arch (lib32), Fedora (/usr/lib).
set(ENV{PKG_CONFIG_PATH} "/usr/lib/i386-linux-gnu/pkgconfig:/usr/lib32/pkgconfig:/usr/lib/pkgconfig")

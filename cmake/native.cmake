# =============================================================================
# The native Linux and macOS host: the lifted C on pcrecomp's runtime/win32hle
# (src/linux), with no Wine. Included by CMakeLists.txt on those systems;
# docs/linux-macos.md has the design, the gaps and the build steps.
#
#   ./build-linux.sh     # 32-bit Linux (cmake/linux-i386.cmake): flat memory
#   ./build-macos.sh     # 64-bit macOS: the guest's 4 GB is one block of the host's
#
# Modelled on Red Alert 2's and Civilization III's native builds.
# =============================================================================
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(PCRECOMP "${CMAKE_SOURCE_DIR}/../pcrecomp" CACHE PATH "pcrecomp checkout")
set(GEN_DIR "${CMAKE_SOURCE_DIR}/src/recomp/gen" CACHE PATH "the lifted C (run_lift.py's output)")
# As on Windows, a worktree may link a lift built elsewhere (libswgb_gen.a of
# a build with the same memory model) instead of compiling GEN_DIR again.
set(SWGB_GEN_LIB "" CACHE FILEPATH "a prebuilt libswgb_gen.a to link instead of compiling GEN_DIR")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(SDL2_TTF REQUIRED IMPORTED_TARGET SDL2_ttf)
# ffmpeg is for movies (win32hle's bink.c, and Track 6's player when it
# comes): the game runs without it.
option(WITH_FFMPEG "movies through ffmpeg (needs its libraries for the target)" OFF)
if(WITH_FFMPEG)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswresample)
endif()

set(HLE ${PCRECOMP}/runtime/win32hle)
if(NOT EXISTS "${HLE}/win32hle.h")
  message(FATAL_ERROR "${HLE} is missing: PCRECOMP must be a pcrecomp checkout with runtime/win32hle")
endif()
file(GLOB HLE_SOURCES CONFIGURE_DEPENDS "${HLE}/*.c")
list(FILTER HLE_SOURCES EXCLUDE REGEX "_selftest\\.c$|/[.]_[^/]*$")

set(SWGB_NATIVE_DEFS RECOMP_RETADDR=0u RECOMP_LOCAL_REGS SWGB_WIN32HLE)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  list(APPEND SWGB_NATIVE_DEFS RECOMP_FLAT_MEMORY)       # a guest address is a host address
else()
  # 64-bit: where the guest's 4 GB goes. A fixed address lets the lifted code
  # add a constant, not a global it reloads after every store; on a Mac 16 TB
  # up is free on arm64 and x86-64 alike. Empty: wherever mmap puts it.
  if(APPLE)
    set(SWGB_MEM_BASE "0x100000000000ull" CACHE STRING "host address of the guest's 4 GB (empty: anywhere)")
  else()
    set(SWGB_MEM_BASE "" CACHE STRING "host address of the guest's 4 GB (empty: anywhere)")
  endif()
  if(SWGB_MEM_BASE)
    list(APPEND SWGB_NATIVE_DEFS RECOMP_MEM_BASE=${SWGB_MEM_BASE})
  endif()
endif()
# win32hle runs one guest thread at a time (its machine lock), so indirect
# calls can cache their targets; the lift writes the x87 stack as _ST(i), so
# it can be a ring with a function's own pushes in locals. Red Alert 2 went
# from 35 to 40-45 fps with these. Off: the indirect-call ring for a crash report.
option(SWGB_FAST_ICALLS "cache indirect-call targets, x87 ring, no icall trace ring" ON)
if(SWGB_FAST_ICALLS)
  list(APPEND SWGB_NATIVE_DEFS RECOMP_RESOLVE_CACHE=16384 RECOMP_ICALL_SITE_CACHE RECOMP_NO_ICALL_TRACE
                               RECOMP_X87_RING RECOMP_X87_LOCAL)
endif()
set(SWGB_NATIVE_INCS src/linux src/runtime ${GEN_DIR} third_party/miniz ${HLE} ${PCRECOMP}/runtime/recomp32)
# gcc and clang take the lifted C as it is (docs/linux-macos.md, "The lifted
# C under gcc"); these keep its integer and aliasing semantics x86's.
set(GEN_OPT "-O1" CACHE STRING "optimization for the generated C")
set(SWGB_GEN_FLAGS -w ${GEN_OPT} -fno-strict-aliasing -fwrapv)
# The few generated files the game spends its time in, at GEN_HOT_OPT
# (docs/performance.md: tools/profile_report.py --hot picks them; a re-lift
# moves functions between files, so pick them again). Windows compiles the
# whole lift at /O2 and has no such list.
set(GEN_HOT_FILES "recomp_0022.c;recomp_0040.c;recomp_0050.c;recomp_0051.c" CACHE STRING
    "generated files (names, ;-separated) compiled with GEN_HOT_OPT")
set(GEN_HOT_OPT "-O2" CACHE STRING "optimization for GEN_HOT_FILES")
set(CC_GEN_HOT_FILES "recomp_0022.c;recomp_0023.c;recomp_0040.c;recomp_0041.c;recomp_0050.c;recomp_0051.c;recomp_0052.c"
    CACHE STRING "Clone Campaigns' generated files compiled with GEN_HOT_OPT")

if(SWGB_GEN_LIB)
  if(NOT EXISTS "${SWGB_GEN_LIB}")
    message(FATAL_ERROR "SWGB_GEN_LIB ${SWGB_GEN_LIB} does not exist")
  endif()
  add_library(swgb_gen STATIC IMPORTED)
  set_target_properties(swgb_gen PROPERTIES IMPORTED_LOCATION "${SWGB_GEN_LIB}")
  set(SWGB_GEN_FILE "${SWGB_GEN_LIB}")
else()
  file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${GEN_DIR}/*.c")
  list(FILTER GEN_SOURCES EXCLUDE REGEX "/[.]_[^/]*$")   # macOS ._* files on some external drives
  if(NOT GEN_SOURCES)
    message(FATAL_ERROR "${GEN_DIR} is empty -- run `python3 run_lift.py --all` first")
  endif()
  add_library(swgb_gen STATIC ${GEN_SOURCES})
  target_include_directories(swgb_gen PRIVATE ${SWGB_NATIVE_INCS})
  target_compile_definitions(swgb_gen PRIVATE ${SWGB_NATIVE_DEFS})
  target_compile_options(swgb_gen PRIVATE ${SWGB_GEN_FLAGS})
  set(SWGB_GEN_FILE "$<TARGET_FILE:swgb_gen>")
  foreach(f IN LISTS GEN_HOT_FILES)
    set_source_files_properties("${GEN_DIR}/${f}" PROPERTIES COMPILE_OPTIONS "${GEN_HOT_OPT}")
  endforeach()
endif()

add_executable(swgb
  src/linux/main.c
  src/linux/shims.c
  src/linux/eh.c
  src/linux/fields.c
  src/linux/framerate.c
  src/linux/profile.c
  src/linux/movie.c
  src/runtime/keyscript.c
  src/runtime/profile_core.c
  src/runtime/movie_core.c
  src/runtime/coverage.c
  src/runtime/widescreen.c
  src/runtime/smooth.c
  src/runtime/dplay.c
  src/runtime/inflate.c
  src/runtime/cdaudio.c
  third_party/miniz/miniz.c
  ${HLE_SOURCES}
)
target_include_directories(swgb PRIVATE ${SWGB_NATIVE_INCS})
target_compile_definitions(swgb PRIVATE ${SWGB_NATIVE_DEFS})
target_compile_options(swgb PRIVATE -fno-strict-aliasing -fwrapv)
# The host's own C (src/) with -Wall, read from the target so a new file gets
# it, as on Windows; win32hle's and miniz's keep their own. swgbrecomp_cc
# compiles the same files, so it gets this too.
get_target_property(_host_sources swgb SOURCES)
list(FILTER _host_sources INCLUDE REGEX "^src/")
set_source_files_properties(${_host_sources} PROPERTIES COMPILE_OPTIONS "-Wall;-Wno-unused-parameter")
set_source_files_properties(third_party/miniz/miniz.c PROPERTIES COMPILE_OPTIONS "-w;-O2")
# Every object of the lift, referenced or not: the dispatch table is the only
# way in, and nothing may be dropped from behind it.
if(APPLE)
  target_link_libraries(swgb PRIVATE "-Wl,-force_load,${SWGB_GEN_FILE}")
else()
  target_link_libraries(swgb PRIVATE "-Wl,--whole-archive" "${SWGB_GEN_FILE}" "-Wl,--no-whole-archive")
endif()
if(NOT SWGB_GEN_LIB)
  add_dependencies(swgb swgb_gen)
endif()
target_link_libraries(swgb PRIVATE PkgConfig::SDL2 PkgConfig::SDL2_TTF pthread m)
if(WITH_FFMPEG)
  target_compile_definitions(swgb PRIVATE HLE_WITH_FFMPEG)
  target_link_libraries(swgb PRIVATE PkgConfig::FFMPEG)
endif()

set_target_properties(swgb PROPERTIES OUTPUT_NAME swgbrecomp)

# Clone Campaigns' host, swgbrecomp_cc, as on Windows (cmake/clone_campaigns.cmake):
# only when its lift is there (python3 run_lift.py --build cc --all -> src/recomp/gen_cc),
# or SWGB_CC_GEN_LIB names a prebuilt libswgb_cc_gen.a. The host is the base
# target's sources with SWGB_CC (main.c's exe, registry key and disc;
# guest_addrs.h's engine addresses).
set(CC_GEN_DIR "${CMAKE_SOURCE_DIR}/src/recomp/gen_cc" CACHE PATH "the lifted Clone Campaigns C (run_lift.py --build cc)")
set(SWGB_CC_GEN_LIB "" CACHE FILEPATH "a prebuilt libswgb_cc_gen.a to link instead of compiling CC_GEN_DIR")
file(GLOB CC_GEN_SOURCES CONFIGURE_DEPENDS "${CC_GEN_DIR}/*.c")
list(FILTER CC_GEN_SOURCES EXCLUDE REGEX "/[.]_[^/]*$")
if(SWGB_CC_GEN_LIB OR CC_GEN_SOURCES)
  set(SWGB_CC_INCS ${SWGB_NATIVE_INCS})
  list(TRANSFORM SWGB_CC_INCS REPLACE "^${GEN_DIR}$" "${CC_GEN_DIR}")
  if(SWGB_CC_GEN_LIB)
    if(NOT EXISTS "${SWGB_CC_GEN_LIB}")
      message(FATAL_ERROR "SWGB_CC_GEN_LIB ${SWGB_CC_GEN_LIB} does not exist")
    endif()
    add_library(swgb_cc_gen STATIC IMPORTED)
    set_target_properties(swgb_cc_gen PROPERTIES IMPORTED_LOCATION "${SWGB_CC_GEN_LIB}")
    set(SWGB_CC_GEN_FILE "${SWGB_CC_GEN_LIB}")
  else()
    add_library(swgb_cc_gen STATIC ${CC_GEN_SOURCES})
    target_include_directories(swgb_cc_gen PRIVATE ${SWGB_CC_INCS})
    target_compile_definitions(swgb_cc_gen PRIVATE ${SWGB_NATIVE_DEFS})
    target_compile_options(swgb_cc_gen PRIVATE ${SWGB_GEN_FLAGS})
    set(SWGB_CC_GEN_FILE "$<TARGET_FILE:swgb_cc_gen>")
    foreach(f IN LISTS CC_GEN_HOT_FILES)
      set_source_files_properties("${CC_GEN_DIR}/${f}" PROPERTIES COMPILE_OPTIONS "${GEN_HOT_OPT}")
    endforeach()
  endif()
  get_target_property(_cc_sources swgb SOURCES)
  add_executable(swgb_cc ${_cc_sources})
  target_include_directories(swgb_cc PRIVATE ${SWGB_CC_INCS})
  target_compile_definitions(swgb_cc PRIVATE ${SWGB_NATIVE_DEFS} SWGB_CC=1)
  target_compile_options(swgb_cc PRIVATE -fno-strict-aliasing -fwrapv)
  if(APPLE)
    target_link_libraries(swgb_cc PRIVATE "-Wl,-force_load,${SWGB_CC_GEN_FILE}")
  else()
    target_link_libraries(swgb_cc PRIVATE "-Wl,--whole-archive" "${SWGB_CC_GEN_FILE}" "-Wl,--no-whole-archive")
  endif()
  if(NOT SWGB_CC_GEN_LIB)
    add_dependencies(swgb_cc swgb_cc_gen)
  endif()
  target_link_libraries(swgb_cc PRIVATE PkgConfig::SDL2 PkgConfig::SDL2_TTF pthread m)
  if(WITH_FFMPEG)
    target_compile_definitions(swgb_cc PRIVATE HLE_WITH_FFMPEG)
    target_link_libraries(swgb_cc PRIVATE PkgConfig::FFMPEG)
  endif()
  set_target_properties(swgb_cc PROPERTIES OUTPUT_NAME swgbrecomp_cc)
endif()

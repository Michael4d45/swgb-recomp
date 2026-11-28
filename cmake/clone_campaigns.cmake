# =============================================================================
# Clone Campaigns: a second host, swgbrecomp_cc.exe, for the expansion's exe
# (Steam's battlegrounds_x1.exe, unwrapped; docs/lifting.md).
#
#   py -3 run_lift.py --build cc --all   # -> src/recomp/gen_cc/ (needs work/functions_x1.json)
#   build.cmd                            # builds swgbrecomp_cc.exe beside swgbrecomp.exe
#
# The lifted program is linked into the host (the dispatch table is its only
# way in), so one host runs one exe: the expansion is its own target, built
# only when CC_GEN_DIR has a lift in it (or SWGB_CC_GEN_LIB names a prebuilt
# swgb_cc_gen.lib). Without one, the build is the base game's alone.
#
# The host is the base target's: its sources, definitions, libraries and link
# options are read from `swgb`, so every runtime change reaches both. SWGB_CC
# picks the exe's name, registry key and disc in host.c, and the engine's
# addresses in guest_addrs.h. The generated C gets the base game's
# optimization, GEN_OPT.
# =============================================================================
set(CC_GEN_DIR "${CMAKE_SOURCE_DIR}/src/recomp/gen_cc" CACHE PATH "the lifted Clone Campaigns C (run_lift.py --build cc)")
set(SWGB_CC_GEN_LIB "" CACHE FILEPATH "a prebuilt swgb_cc_gen.lib to link instead of compiling CC_GEN_DIR")
file(GLOB CC_GEN_SOURCES CONFIGURE_DEPENDS "${CC_GEN_DIR}/*.c")
if(NOT SWGB_CC_GEN_LIB AND NOT CC_GEN_SOURCES)
  return()
endif()

set(SWGB_CC_INCS src/runtime ${CC_GEN_DIR} third_party/miniz ${PCRECOMP}/runtime/native32 ${PCRECOMP}/runtime/recomp32)
if(SWGB_CC_GEN_LIB)
  if(NOT EXISTS "${SWGB_CC_GEN_LIB}")
    message(FATAL_ERROR "SWGB_CC_GEN_LIB ${SWGB_CC_GEN_LIB} does not exist")
  endif()
  add_library(swgb_cc_gen STATIC IMPORTED)
  set_target_properties(swgb_cc_gen PROPERTIES IMPORTED_LOCATION "${SWGB_CC_GEN_LIB}")
else()
  add_library(swgb_cc_gen STATIC ${CC_GEN_SOURCES})
  target_include_directories(swgb_cc_gen PRIVATE ${SWGB_CC_INCS})
  target_compile_definitions(swgb_cc_gen PRIVATE ${SWGB_DEFS})
  set_source_files_properties(${CC_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/w;${GEN_OPT}")
  if(SWGB_TRACE)
    target_compile_definitions(swgb_cc_gen PRIVATE RECOMP_TRACE=1)
  endif()
endif()

# The base host's own: everything but its lifted library.
get_target_property(_cc_sources swgb SOURCES)
get_target_property(_cc_defs swgb COMPILE_DEFINITIONS)
get_target_property(_cc_libs swgb LINK_LIBRARIES)
get_target_property(_cc_link swgb LINK_OPTIONS)
list(REMOVE_ITEM _cc_libs swgb_gen)
list(FILTER _cc_link EXCLUDE REGEX "WHOLEARCHIVE")

add_executable(swgb_cc ${_cc_sources})
target_include_directories(swgb_cc PRIVATE ${SWGB_CC_INCS})
target_compile_definitions(swgb_cc PRIVATE ${_cc_defs} SWGB_CC=1)
target_link_libraries(swgb_cc PRIVATE swgb_cc_gen ${_cc_libs})
target_link_options(swgb_cc PRIVATE ${_cc_link}
                    "/WHOLEARCHIVE:$<IF:$<BOOL:${SWGB_CC_GEN_LIB}>,${SWGB_CC_GEN_LIB},$<TARGET_FILE:swgb_cc_gen>>")
set_target_properties(swgb_cc PROPERTIES OUTPUT_NAME swgbrecomp_cc)

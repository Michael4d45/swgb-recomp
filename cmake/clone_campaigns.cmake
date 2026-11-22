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
# swgb_cc_gen.lib). Without one, the build is the base game's alone, as before.
# The runtime is the same sources; SWGB_CC picks the exe's name and its
# registry key in host.c.
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
endif()

add_executable(swgb_cc
  src/runtime/host.c
  src/runtime/softdd.c
  src/runtime/inflate.c
  third_party/miniz/miniz.c
  ${PCRECOMP}/runtime/native32/native32.c
  ${PCRECOMP}/runtime/recomp32/image_loader.c
  ${PCRECOMP}/runtime/recomp32/recomp_trace.c
)
target_include_directories(swgb_cc PRIVATE ${SWGB_CC_INCS})
target_compile_definitions(swgb_cc PRIVATE ${SWGB_DEFS} SWGB_CC=1)
target_link_libraries(swgb_cc PRIVATE swgb_cc_gen)
target_link_options(swgb_cc PRIVATE "/WHOLEARCHIVE:$<IF:$<BOOL:${SWGB_CC_GEN_LIB}>,${SWGB_CC_GEN_LIB},$<TARGET_FILE:swgb_cc_gen>>")
# The same layout as the base host: the guest's 0x00400000..0x00A5A000 free
# (battlegrounds_x1.exe's image is 0x00400000..0x007B1000).
target_link_options(swgb_cc PRIVATE /BASE:0x60000000 /FIXED:NO /DYNAMICBASE:NO
                                          /NXCOMPAT /LARGEADDRESSAWARE /STACK:0x1000000
                                          /MANIFESTUAC:NO /MANIFEST:NO)
target_link_libraries(swgb_cc PRIVATE user32 gdi32 kernel32 ole32 uuid dxguid winmm ws2_32 shlwapi advapi32)
if(SWGB_TRACE)
  target_compile_definitions(swgb_cc PRIVATE RECOMP_TRACE=1)
  if(NOT SWGB_CC_GEN_LIB)
    target_compile_definitions(swgb_cc_gen PRIVATE RECOMP_TRACE=1)
  endif()
endif()
set_target_properties(swgb_cc PROPERTIES OUTPUT_NAME swgbrecomp_cc)

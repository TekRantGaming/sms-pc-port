# -DSMS_ECLIPSE=ON builds Super Mario Eclipse into the port (docs/ECLIPSE.md).
#
# Eclipse, BetterSunshineEngine and BetterSunshineMoveset (and the SunshineHeaderInterface headers
# they are written against) are fetched at configure time at pinned
# revisions, never kept in this repository, fixed up mechanically
# (platform/mods/eclipse/fixup_sources.py) and built with clang into one
# library (platform/mods/eclipse/lib). Their patches register with the port's
# code-mod registry (platform/mods/modhooks.cpp) and reach the game through
# hooks in the decomp source (decomp-patches/modhook-*.patch).
#
# Their headers describe the GameCube's layout; platform/mods/eclipse/shi-layout.patch
# (tools/mods/shi_layout) re-lays their classes out as the port lays out the
# game's, on 32- and 64-bit hosts.
find_program(SMS_CLANGXX NAMES clang++)
find_program(SMS_CLANG NAMES clang)
if(NOT SMS_CLANGXX OR NOT SMS_CLANG)
  message(FATAL_ERROR "SMS_ECLIPSE needs clang/clang++ (Eclipse's sources are written for clang)")
endif()
find_package(Git REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(SMS_ECLIPSE_SRC_DIR "${CMAKE_BINARY_DIR}/eclipse-src" CACHE PATH
  "Where the Eclipse, BetterSunshineEngine, BetterSunshineMoveset and SunshineHeaderInterface sources are fetched to")

# name  url  revision
set(_eclipse_repos
  "eclipse|https://github.com/JoshuaMKW/super-mario-eclipse|52749795113f415b97d02392c45385982daa70bb"
  "bse|https://github.com/JoshuaMKW/BetterSunshineEngine|fd6273014545ac0174fa54fada02edd9212f63d8"
  "moveset|https://github.com/JoshuaMKW/BetterSunshineMoveset|2eb6f136cce4c0c7816808ad9dc3d1d95ef52b83"
  "shi|https://github.com/JoshuaMKW/SunshineHeaderInterface|a0d858951e7fb22dce5304aa5c50287ecb0d6862")

foreach(r ${_eclipse_repos})
  string(REPLACE "|" ";" r "${r}")
  list(GET r 0 _name)
  list(GET r 1 _url)
  list(GET r 2 _rev)
  set(_dir "${SMS_ECLIPSE_SRC_DIR}/${_name}")
  set(_have "")
  if(EXISTS "${_dir}/.git")
    execute_process(COMMAND ${GIT_EXECUTABLE} -C "${_dir}" rev-parse HEAD
      OUTPUT_VARIABLE _have OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  endif()
  if(NOT _have STREQUAL _rev)
    message(STATUS "SMS_ECLIPSE: fetching ${_name} ${_rev}")
    file(MAKE_DIRECTORY "${_dir}")
    execute_process(COMMAND ${GIT_EXECUTABLE} init -q "${_dir}")
    execute_process(COMMAND ${GIT_EXECUTABLE} -C "${_dir}" fetch -q --depth 1 "${_url}" "${_rev}"
      RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "SMS_ECLIPSE: could not fetch ${_url} at ${_rev}")
    endif()
    execute_process(COMMAND ${GIT_EXECUTABLE} -C "${_dir}" -c advice.detachedHead=false checkout -q -f FETCH_HEAD
      RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "SMS_ECLIPSE: could not check out ${_name}")
    endif()
  endif()
endforeach()

execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/platform/mods/eclipse/fixup_sources.py
  "${SMS_ECLIPSE_SRC_DIR}/eclipse" "${SMS_ECLIPSE_SRC_DIR}/bse" "${SMS_ECLIPSE_SRC_DIR}/shi"
  "${SMS_ECLIPSE_SRC_DIR}/moveset"
  RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "SMS_ECLIPSE: fixup_sources.py failed")
endif()

include(ExternalProject)
set(_eclipse_lib "${CMAKE_BINARY_DIR}/eclipse-build/libsms_eclipse.a")
ExternalProject_Add(sms_eclipse_build
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/platform/mods/eclipse/lib
  BINARY_DIR ${CMAKE_BINARY_DIR}/eclipse-build
  CMAKE_ARGS -DCMAKE_BUILD_TYPE=RelWithDebInfo
    -DCMAKE_C_COMPILER=${SMS_CLANG} -DCMAKE_CXX_COMPILER=${SMS_CLANGXX}
    -DECLIPSE_SRC=${SMS_ECLIPSE_SRC_DIR}/eclipse -DBSE_SRC=${SMS_ECLIPSE_SRC_DIR}/bse
    -DMOVESET_SRC=${SMS_ECLIPSE_SRC_DIR}/moveset
    -DSHI_SRC=${SMS_ECLIPSE_SRC_DIR}/shi -DPORT_MODS=${CMAKE_CURRENT_SOURCE_DIR}/platform/mods
    -DSMS_ARCH=${SMS_ARCH}
  BUILD_ALWAYS ON
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS ${_eclipse_lib})
# The mods' plain new/delete go to the game's heaps, like the game's own
# (see the sms_game rename above). Their static constructors run when the
# modules load (sms_mod_start), not at process start (platform/mods/eclipse/lib
# moves them to their own section): Kuribo runs a module's constructors when
# it loads it, and BetterSunshineEngine's start threads and allocate from the
# game's heaps.
ExternalProject_Add_Step(sms_eclipse_build rename_new
  COMMAND ${SMS_OBJCOPY} --redefine-syms=${CMAKE_BINARY_DIR}/game_new_syms.txt ${_eclipse_lib}
  DEPENDEES build)

# Functions the mods call out of line that the decomp only has inline, and
# SDK functions the retail game never needed: built with the game's flags.
target_sources(sms_game PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/mods/eclipse/port_shims.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/mods/eclipse/sdk_extras.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/mods/eclipse/rawfn_trampolines.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/mods/eclipse/rawdata.cpp)
# The functions the mods call through SunshineHeaderInterface's raw_fn.hxx
# (tools/mods/gen_rawfn.py): C++11 for their return conversion, and members
# called as the retail code calls them whatever their access.
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/platform/mods/eclipse/rawfn_trampolines.cpp
  PROPERTIES COMPILE_OPTIONS "-std=gnu++11;-fno-access-control")

add_dependencies(sms sms_eclipse_build)
# After each link, check that the game and the mods agree on the widths and
# signedness of the integer and float results and arguments where they call
# each other: by name, through game virtual functions, and at the patch
# targets (tools/mods/abi_check.py, from the binary's debug info: about half
# a minute). A failed check is redone on the next build.
option(SMS_ECLIPSE_ABI_CHECK "Check the mods' result and argument types against the game's after linking" ON)
if(SMS_ECLIPSE_ABI_CHECK)
  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/sms_abi_check.stamp
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/mods/abi_check.py $<TARGET_FILE:sms> ${_eclipse_lib}
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/sms_abi_check.stamp
    DEPENDS sms ${CMAKE_CURRENT_SOURCE_DIR}/tools/mods/abi_check.py
    COMMENT "Checking the mods' result and argument types against the game's" VERBATIM)
  add_custom_target(sms_abi_check ALL DEPENDS ${CMAKE_BINARY_DIR}/sms_abi_check.stamp)
endif()
target_compile_definitions(sms PRIVATE SMS_ECLIPSE=1)
target_link_libraries(sms PRIVATE -Wl,--whole-archive ${_eclipse_lib} -Wl,--no-whole-archive)
set_property(TARGET sms APPEND PROPERTY LINK_DEPENDS ${_eclipse_lib})
# Names the mods use for things the decomp spells otherwise: retail globals
# under their map names, and functions whose u32 is unsigned int there and
# unsigned long here (the same type on the 32-bit port).
target_link_options(sms PRIVATE
  -Wl,--defsym=gStageBGM=_ZN10MSMainProc11MSStageInfo8stageBgmE
  -Wl,--defsym=gAudioVolume=_ZN5MSBgm12smMainVolumeE
  -Wl,--defsym=waterColor=gModelWaterManagerWaterColor)
# TMarDirector::fireStartDemoCamera's callback argument (and the callback's
# first parameter) is uintptr_t in the decomp and u32 in the mods' headers;
# the mods pass their own callbacks and 0 through it, and the game only hands
# the argument back to the callback.
if(SMS_ARCH STREQUAL "32")
  target_link_options(sms PRIVATE
    -Wl,--defsym=_ZN7JKRHeap5allocEjiPS_=_ZN7JKRHeap5allocEmiPS_
    -Wl,--defsym=_ZN13JKRMemArchiveC1EPvj15JKRMemBreakFlag=_ZN13JKRMemArchiveC1EPvm15JKRMemBreakFlag
    -Wl,--defsym=_ZN12TMarDirector19fireStartDemoCameraEPKcPKN9JGeometry5TVec3IfEElfbPFlmmEmPN6JDrama6TActorENS9_6TFlagTItEE=_ZN12TMarDirector19fireStartDemoCameraEPKcPKN9JGeometry5TVec3IfEElfbPFljmEjPN6JDrama6TActorENS9_6TFlagTItEE)
else()
  # On LP64 hosts it is the other way round: the game's u32 is unsigned int,
  # and these declarations (size_t, unsigned long) say unsigned long.
  target_link_options(sms PRIVATE
    -Wl,--defsym=_ZN7JKRHeap5allocEmiPS_=_ZN7JKRHeap5allocEjiPS_
    -Wl,--defsym=_ZN13JKRMemArchiveC1EPvm15JKRMemBreakFlag=_ZN13JKRMemArchiveC1EPvj15JKRMemBreakFlag
    -Wl,--defsym=_ZN6JStage6TActor11JSGSetShapeEm=_ZN6JStage6TActor11JSGSetShapeEj
    -Wl,--defsym=_ZN6JStage6TActor15JSGSetAnimationEm=_ZN6JStage6TActor15JSGSetAnimationEj
    -Wl,--defsym=_ZN6JStage7TSystem16JSGGetSystemDataEm=_ZN6JStage7TSystem16JSGGetSystemDataEj
    -Wl,--defsym=_ZN6JStage7TSystem16JSGSetSystemDataEmm=_ZN6JStage7TSystem16JSGSetSystemDataEjj
    -Wl,--defsym=_ZN12TMarDirector19fireStartDemoCameraEPKcPKN9JGeometry5TVec3IfEEifbPFijjEjPN6JDrama6TActorENS9_6TFlagTItEE=_ZN12TMarDirector19fireStartDemoCameraEPKcPKN9JGeometry5TVec3IfEEifbPFimjEmPN6JDrama6TActorENS9_6TFlagTItEE)
endif()
message(STATUS "SMS port: Super Mario Eclipse built in (sources in ${SMS_ECLIPSE_SRC_DIR})")

# Builds BetterSunshineEngine, BetterSunshineMoveset and Super Mario Eclipse (fetched sources, see
# cmake/eclipse.cmake) into one static library for the 32-bit port, with
# clang (their sources are written for it) and the port's shim headers in
# place of the Kuribo/Kamek SDKs and PowerPC-only macros.
cmake_minimum_required(VERSION 3.16)
project(sms_eclipse C CXX)

foreach(v ECLIPSE_SRC BSE_SRC MOVESET_SRC SHI_SRC PORT_MODS)
  if(NOT ${v})
    message(FATAL_ERROR "${v} is required")
  endif()
endforeach()

# The same file sets their own CMake builds glob (Eclipse: src/ one level
# deep; BetterSunshineEngine: its listed directories), minus what the port
# provides natively: BSE's Dolphin SDK reimplementations (libs/dolphin), its
# C runtime (cstd, except the two string helpers) and PowerPC assembly.
set(_bse_dirs src src/camera src/collision src/debug src/level_select src/libs src/moveset
  src/objects src/patches src/patches/collision src/physics src/shine src/sunscript src/toolbox src/yoshi)
set(BSE_SOURCES ${BSE_SRC}/src/cstd/stdlib.c)
foreach(d ${_bse_dirs})
  file(GLOB _f ${BSE_SRC}/${d}/*.c ${BSE_SRC}/${d}/*.cpp)
  list(APPEND BSE_SOURCES ${_f})
endforeach()
file(GLOB MOVESET_SOURCES ${MOVESET_SRC}/src/*.cpp ${MOVESET_SRC}/src/*/*.cpp)
file(GLOB ECLIPSE_SOURCES ${ECLIPSE_SRC}/src/*.c ${ECLIPSE_SRC}/src/*.cpp ${ECLIPSE_SRC}/src/*/*.c ${ECLIPSE_SRC}/src/*/*.cpp)

set(_shi ${SHI_SRC}/include ${SHI_SRC}/include/JSystem ${SHI_SRC}/include/Dolphin ${SHI_SRC}/include/SMS
  ${SHI_SRC}/include/Kamek ${SHI_SRC}/include/Kuribo)
set(_defs NTSCU KURIBO_NO_TYPES "BETTER_SMS_VERSION=\"v2.0\"" BETTER_SMS_CUSTOM_MUSIC=1 BETTER_SMS_EXTRA_SHINES=1
  BETTER_SMS_EXTRA_OBJECTS=1 BETTER_SMS_EXTRA_COLLISION=1 BETTER_SMS_EXTENDED_RENDER_DISTANCE=1
  BETTER_SMS_SLOT_B_SUPPORT=1 BETTER_SMS_SHADOW_MARIO_HEALTHBAR=1 "SUPER_MARIO_ECLIPSE_VERSION=\"v1.1\"")
# -fno-use-init-array: static constructors go to .ctors, which the port
# renames (cmake/eclipse.cmake) and runs when the modules load, as Kuribo does.
# -malign-double (32-bit): 8-byte members aligned to 8 inside structs, as on the
# GameCube, so SunshineHeaderInterface's classes match retail (and the port's
# types.h, which does the same for s64/u64/f64).
if(NOT SMS_ARCH)
  set(SMS_ARCH 32)
endif()
set(_arch -m${SMS_ARCH})
if(SMS_ARCH STREQUAL "32")
  # SSE float math, as the game is built (x87 would round differently)
  list(APPEND _arch -malign-double -msse2 -mfpmath=sse)
endif()
if(WIN32)
  # GNU ld does not know clang's .llvm_addrsig and maps it at 4 GiB, which
  # Windows refuses to load.
  # -mno-ms-bitfields: MinGW lays out bitfields as MSVC does, starting a new
  # unit whenever the declared type changes, so SunshineHeaderInterface's
  # bool/u32 flag words (TLiveActor::mStateFlags, TMario's, ...) grow and every
  # member after them moves (TMapObjBase::mRegisterName 0x30 past the game's
  # unkF4). The decomp has no such bitfields; GCC's layout is the one
  # shi-layout.patch was made for, as on Linux and macOS.
  list(APPEND _arch -fno-addrsig -mno-ms-bitfields)
endif()
# -fnew-alignment=4: the game's operator new (JKRHeap) guarantees 4-byte
# alignment, not the 16 clang would otherwise assume for aligned SSE stores;
# -fno-aligned-new keeps every allocation on that operator (the game's heaps).
# -fno-sized-deallocation: the same for every delete. Clang 19 and later call
# operator delete(void*, size_t) by default, which the game does not define and
# the rename (game_new_syms.txt) does not cover, so it was the host's: free()
# on a game-heap block aborted the game (saving BSE's settings, loading a stage).
# -fstandalone-debug: every game class the mods use is described in their
# debug info, which tools/mods/abi_check.py compares with the game's.
# -funsigned-char: built for the PowerPC (powerpc-gecko-ibm-kuribo-eabi), the
# mods' char is unsigned, and so is SunshineHeaderInterface's s8, a char
# (the game's is signed: CodeWarrior's -char signed).
set(_opts ${_arch} -funsigned-char -fnew-alignment=4 -fno-aligned-new -fno-sized-deallocation -O2 -g -fstandalone-debug -w -fms-extensions -fno-exceptions -fno-threadsafe-statics -fno-strict-aliasing
  -fno-use-init-array
  -include ${PORT_MODS}/eclipse/shim/sms_mod_prelude.h
  $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20> $<$<COMPILE_LANGUAGE:C>:-std=gnu11>)

add_library(bse_objs OBJECT ${BSE_SOURCES})
target_include_directories(bse_objs PRIVATE ${PORT_MODS}/eclipse/shim ${PORT_MODS}/include
  ${BSE_SRC}/include/BetterSMS ${BSE_SRC}/src ${BSE_SRC}/include ${_shi})
target_compile_definitions(bse_objs PRIVATE ${_defs})
target_compile_options(bse_objs PRIVATE ${_opts})

add_library(moveset_objs OBJECT ${MOVESET_SOURCES})
target_include_directories(moveset_objs PRIVATE ${PORT_MODS}/eclipse/shim ${PORT_MODS}/include
  ${MOVESET_SRC}/include ${BSE_SRC}/include ${_shi})
target_compile_definitions(moveset_objs PRIVATE ${_defs})
target_compile_options(moveset_objs PRIVATE ${_opts})

add_library(eclipse_objs OBJECT ${ECLIPSE_SOURCES})
target_include_directories(eclipse_objs PRIVATE ${PORT_MODS}/eclipse/shim ${PORT_MODS}/include
  ${ECLIPSE_SRC}/include ${BSE_SRC}/include ${_shi})
target_compile_definitions(eclipse_objs PRIVATE ${_defs})
target_compile_options(eclipse_objs PRIVATE ${_opts})

# The partial links below run the compiler driver as a linker, for this target.
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
  set(_ld_arch -arch ${CMAKE_OSX_ARCHITECTURES})
else()
  set(_ld_arch -m${SMS_ARCH})
endif()
if(WIN32)
  # lld (MinGW clang's linker in some environments) has no -r; GNU ld does.
  list(APPEND _ld_arch -fuse-ld=bfd)
  include(${CMAKE_CURRENT_SOURCE_DIR}/object_response.cmake)
endif()

# Kuribo loads each module on its own, so their private names never meet;
# linked into one program they would (Eclipse and the moveset both define
# gSettingsGroup, gSaveBnr...). The modules built on BetterSunshineEngine are
# each linked into one object whose own definitions are made local
# (tools/mods/localize_module.py); what they use of the engine and the game
# stays an undefined reference, resolved by name as Kuribo resolves it.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_tools ${PORT_MODS}/../../tools/mods)
set(_modules)
foreach(m moveset eclipse)
  target_compile_options(${m}_objs PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
  set(_o ${CMAKE_CURRENT_BINARY_DIR}/${m}_module.o)
  set(_partial_objects $<TARGET_OBJECTS:${m}_objs>)
  set(_response_dependency)
  if(WIN32)
    set(_response_dependency ${CMAKE_CURRENT_BINARY_DIR}/${m}_module.rsp)
    sms_object_response(_partial_objects "${_response_dependency}" $<TARGET_OBJECTS:${m}_objs>)
  endif()
  if(APPLE)
    # ld64 -r already turns hidden (private extern) definitions into local ones.
    set(_localize)
  else()
    set(_localize COMMAND ${Python3_EXECUTABLE} ${_tools}/localize_module.py ${CMAKE_OBJCOPY} ${_o})
  endif()
  add_custom_command(OUTPUT ${_o}
    COMMAND ${CMAKE_CXX_COMPILER} ${_ld_arch} -r -nostdlib -o ${_o} ${_partial_objects}
    ${_localize}
    DEPENDS ${m}_objs $<TARGET_OBJECTS:${m}_objs> ${_response_dependency} ${_tools}/localize_module.py COMMAND_EXPAND_LISTS VERBATIM)
  list(APPEND _modules ${_o})
endforeach()

# Then everything into one object: the projects share many file names
# (player.cpp, generic.cpp...), which an archive of their objects would not
# keep apart. This partial link also moves their static constructors out of
# the ones the process runs at start, to where platform/mods/modhooks.cpp finds
# them: on ELF the section sms_mod_ctors (mod_ctors.ld), on Mach-O
# __DATA,__sms_mod_ctors (tools/mods/macho_ctors.py), on PE .data$sms_mod_ctors_m.
set(_obj ${CMAKE_CURRENT_BINARY_DIR}/sms_eclipse_all.o)
set(_partial_objects $<TARGET_OBJECTS:bse_objs> ${_modules})
set(_response_dependency)
if(WIN32)
  set(_response_dependency ${CMAKE_CURRENT_BINARY_DIR}/sms_eclipse_all.rsp)
  sms_object_response(_partial_objects "${_response_dependency}" $<TARGET_OBJECTS:bse_objs> ${_modules})
endif()
if(APPLE)
  set(_ctors COMMAND ${Python3_EXECUTABLE} ${_tools}/macho_ctors.py ${_obj})
elseif(WIN32)
  set(_ctors COMMAND ${CMAKE_OBJCOPY} --rename-section .ctors=.data$sms_mod_ctors_m ${_obj})
else()
  set(_ctors)
  list(APPEND _ld_arch -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/mod_ctors.ld)
endif()
add_custom_command(OUTPUT ${_obj}
  COMMAND ${CMAKE_CXX_COMPILER} ${_ld_arch} -r -nostdlib -o ${_obj}
    ${_partial_objects}
  ${_ctors}
  DEPENDS bse_objs $<TARGET_OBJECTS:bse_objs> ${_modules} ${_response_dependency} ${CMAKE_CURRENT_SOURCE_DIR}/mod_ctors.ld
  COMMAND_EXPAND_LISTS VERBATIM)
add_library(sms_eclipse STATIC ${_obj})
set_target_properties(sms_eclipse PROPERTIES LINKER_LANGUAGE CXX)

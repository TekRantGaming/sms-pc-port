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
# -fnew-alignment=4: the game's operator new (JKRHeap) guarantees 4-byte
# alignment, not the 16 clang would otherwise assume for aligned SSE stores;
# -fno-aligned-new keeps every allocation on that operator (the game's heaps).
# -fstandalone-debug: every game class the mods use is described in their
# debug info, which tools/mods/abi_check.py compares with the game's.
# -funsigned-char: built for the PowerPC (powerpc-gecko-ibm-kuribo-eabi), the
# mods' char is unsigned, and so is SunshineHeaderInterface's s8, a char
# (the game's is signed: CodeWarrior's -char signed).
set(_opts ${_arch} -funsigned-char -fnew-alignment=4 -fno-aligned-new -O2 -g -fstandalone-debug -w -fms-extensions -fno-exceptions -fno-threadsafe-statics -fno-strict-aliasing
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

# Kuribo loads each module on its own, so their private names never meet;
# linked into one program they would (Eclipse and the moveset both define
# gSettingsGroup, gSaveBnr...). The modules built on BetterSunshineEngine are
# each linked into one object whose own definitions are made local; what they
# use of the engine and the game stays an undefined reference, resolved by
# name as Kuribo resolves it.
set(_modules)
foreach(m moveset eclipse)
  target_compile_options(${m}_objs PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
  set(_o ${CMAKE_CURRENT_BINARY_DIR}/${m}_module.o)
  add_custom_command(OUTPUT ${_o}
    COMMAND ${CMAKE_CXX_COMPILER} -m${SMS_ARCH} -r -nostdlib -o ${_o} $<TARGET_OBJECTS:${m}_objs>
    COMMAND ${CMAKE_OBJCOPY} --localize-hidden ${_o}
    DEPENDS ${m}_objs $<TARGET_OBJECTS:${m}_objs> COMMAND_EXPAND_LISTS VERBATIM)
  list(APPEND _modules ${_o})
endforeach()

# Then everything into one object: the projects share many file names
# (player.cpp, generic.cpp...), which an archive of their objects would not
# keep apart. This partial link also moves their static constructors out of
# .ctors (mod_ctors.ld).
set(_obj ${CMAKE_CURRENT_BINARY_DIR}/sms_eclipse_all.o)
add_custom_command(OUTPUT ${_obj}
  COMMAND ${CMAKE_CXX_COMPILER} -m${SMS_ARCH} -r -nostdlib -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/mod_ctors.ld -o ${_obj}
    $<TARGET_OBJECTS:bse_objs> ${_modules}
  DEPENDS bse_objs $<TARGET_OBJECTS:bse_objs> ${_modules} ${CMAKE_CURRENT_SOURCE_DIR}/mod_ctors.ld
  COMMAND_EXPAND_LISTS VERBATIM)
add_library(sms_eclipse STATIC ${_obj})
set_target_properties(sms_eclipse PROPERTIES LINKER_LANGUAGE CXX)

cmake_minimum_required(VERSION 3.20)
project(sms_port C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(SMS_DECOMP ${CMAKE_CURRENT_SOURCE_DIR}/decomp)
set(SMS_PORT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# The decomp's header roots, in configure.py's order: the game's include/,
# then each library's libs/<name>/include. (MSL's C and C++ headers,
# libs/PowerPC_EABI_Support/.../MSL_Common, are not used: the port builds
# against the host C and C++ libraries.)
set(SMS_DECOMP_INCLUDE_SUBDIRS include
  libs/dolphin/include libs/JSystem/include libs/THPPlayer/include
  libs/PowerPC_EABI_Support/include libs/TRK_MINNOW_DOLPHIN/include
  libs/OdemuExi2/include)
set(SMS_DECOMP_INCLUDE_DIRS "")
foreach(d ${SMS_DECOMP_INCLUDE_SUBDIRS})
  list(APPEND SMS_DECOMP_INCLUDE_DIRS ${SMS_DECOMP}/${d})
endforeach()

# Default: optimised with debug info. -DNDEBUG is left to the decomp's own
# NDEBUG=1 definition, so the stock RelWithDebInfo flags are replaced.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g" CACHE STRING "" FORCE)

# --- Target word size -------------------------------------------------------
# The game assumes 32-bit pointers (pointer-in-u32 slots, in-place relocated
# resource structs). Build -m32 when the host has multilib; otherwise fall back
# to -m64 so compile work can proceed.
set(SMS_ARCH "auto" CACHE STRING "auto, 32 or 64")
if(SMS_ARCH STREQUAL "auto")
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS "-m32")
  set(CMAKE_REQUIRED_LINK_OPTIONS "-m32")
  check_cxx_source_compiles("#include <cstdio>\nint main(){std::puts(\"\");return 0;}" SMS_HAVE_M32)
  unset(CMAKE_REQUIRED_FLAGS)
  unset(CMAKE_REQUIRED_LINK_OPTIONS)
  if(SMS_HAVE_M32)
    set(SMS_ARCH 32)
  else()
    set(SMS_ARCH 64)
    message(WARNING "-m32 does not link here (install gcc-multilib g++-multilib); building -m64")
  endif()
endif()
add_compile_options(-m${SMS_ARCH})
add_link_options(-m${SMS_ARCH})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # RUNPATH, not RPATH: a toolchain's library directory (the launcher's conda
  # tools add one to every link) then finds only the game's own libraries.
  # An RPATH would also be searched for the system GL driver's libraries, and
  # an older libdrm there stops a newer Mesa loading: no window and no EGL.
  add_link_options("LINKER:--enable-new-dtags")
endif()
message(STATUS "SMS port: building -m${SMS_ARCH}")

# --- Decomp translation units -----------------------------------------------
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(SMS_COMPILER_CACHE "$ENV{SMS_COMPILER_CACHE}" CACHE FILEPATH
    "Optional compiler cache executable (for example sccache)")
if(SMS_COMPILER_CACHE)
  set(CMAKE_C_COMPILER_LAUNCHER "${SMS_COMPILER_CACHE}")
  set(CMAKE_CXX_COMPILER_LAUNCHER "${SMS_COMPILER_CACHE}")
endif()
if(WIN32 AND SMS_ARCH STREQUAL "32" AND "$ENV{SMS_WINDOWS_32_CROSS}" STREQUAL "1")
  set(_msys_launcher "${Python3_EXECUTABLE};${CMAKE_CURRENT_SOURCE_DIR}/tools/msys_cross_compile.py")
  set(_msys_compile_launcher "${_msys_launcher}")
  if(SMS_COMPILER_CACHE)
    list(APPEND _msys_compile_launcher --cache "${SMS_COMPILER_CACHE}")
  endif()
  set(CMAKE_C_COMPILER_LAUNCHER "${_msys_compile_launcher}")
  set(CMAKE_CXX_COMPILER_LAUNCHER "${_msys_compile_launcher}")
  set(CMAKE_C_LINKER_LAUNCHER "${_msys_launcher}")
  set(CMAKE_CXX_LINKER_LAUNCHER "${_msys_launcher}")
endif()
execute_process(
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_sources.py
          ${SMS_DECOMP} ${CMAKE_BINARY_DIR}/decomp_sources.cmake
  RESULT_VARIABLE _gen_rc)
if(NOT _gen_rc EQUAL 0)
  message(FATAL_ERROR "tools/gen_sources.py failed")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SMS_DECOMP}/configure.py)
include(${CMAKE_BINARY_DIR}/decomp_sources.cmake)

# Port patches (decomp-patches/*.patch) are applied to copies of the touched
# files under the build tree; those copies replace the originals in the
# source lists.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches.cmake)

set(SMS_COMMON_DEFS GEKKO TARGET_PC VERSION_GMSE01 BUILD_VERSION=2 NDEBUG=1)
set(SMS_GAME_FLAGS
  -w -fno-strict-aliasing -fwrapv -fno-omit-frame-pointer
  # Retail MSL has no _FORTIFY_SOURCE checks (Ubuntu g++ enables them at -O1+;
  # TMapObjTree::initMapObj passes snprintf a 0x100 size for a 64-byte buffer)
  # nor stack protectors.
  -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fno-stack-protector
  # Float math without contraction (Gekko-like). SSE is x86-only; arm64 uses NEON.
  -ffp-contract=off
  $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++03 -fno-gnu-keywords -fpermissive -fno-threadsafe-statics>
  $<$<COMPILE_LANGUAGE:C>:-std=gnu99 -fcommon>
  -include ${SMS_PORT_SRC}/port_compat.h)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i.86)$" OR CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
  # Closer to Gekko single/double results than x87 extended precision
  # (measured on the play5 lockstep trace).
  list(APPEND SMS_GAME_FLAGS -msse2 -mfpmath=sse)
endif()
# String literals are Shift-JIS on disc and in the MWCC build (sjiswrap).
# GCC normally uses -fexec-charset=CP932. Some portable compiler builds lack
# iconv, and Apple Clang rejects CP932; both use the source mirror below.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-fexec-charset=CP932" SMS_HAVE_CP932)
if(SMS_HAVE_CP932)
  list(APPEND SMS_GAME_FLAGS -finput-charset=UTF-8 -fexec-charset=CP932)
endif()
check_cxx_compiler_flag("-fno-unreachable-traps" SMS_HAVE_NO_UNREACHABLE_TRAPS)
if(SMS_HAVE_NO_UNREACHABLE_TRAPS)
  list(APPEND SMS_GAME_FLAGS -fno-unreachable-traps)
endif()
if(SMS_ARCH STREQUAL "64")
  list(APPEND SMS_GAME_FLAGS -fno-pie)
endif()
# Clang rejects the game's pointer-to-int truncations ((int)ptr, (u32)ptr) as
# hard errors in C++ where g++ -fpermissive only warns; -fms-extensions turns
# them back into (silenced) warnings with the same truncating result. Game
# memory stays below 4 GiB (port_low_alloc), so nothing is lost.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  list(APPEND SMS_GAME_FLAGS -fms-extensions)
endif()

# macOS: native arm64 reserves the low 4 GiB as PAGEZERO, so port_low_alloc
# cannot satisfy pointer-in-u32. Build x86_64 (Rosetta) with a 4 KiB pagezero.
if(APPLE)
  if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
    message(FATAL_ERROR
      "macOS builds must use CMAKE_OSX_ARCHITECTURES=x86_64 (Rosetta). "
      "Native arm64 cannot mmap below 4 GiB. Use ./build.sh.")
  endif()
endif()

set(SMS_GAME_INCLUDE_DIRS ${SMS_DECOMP_INCLUDE_DIRS})
set(SMS_GAME_PCH_DIR ${SMS_DECOMP}/include)
if(NOT SMS_HAVE_CP932)
  set(SMS_CP932_ROOT ${CMAKE_BINARY_DIR}/cp932)
  execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/darwin_cp932_mirror.py
            --decomp ${SMS_DECOMP}
            --patched ${SMS_PATCH_ROOT}
            --out ${SMS_CP932_ROOT}
    RESULT_VARIABLE _cp932_rc)
  if(NOT _cp932_rc EQUAL 0)
    message(FATAL_ERROR "tools/darwin_cp932_mirror.py failed")
  endif()
  set(SMS_GAME_INCLUDE_DIRS "")
  foreach(d ${SMS_DECOMP_INCLUDE_SUBDIRS})
    list(APPEND SMS_GAME_INCLUDE_DIRS ${SMS_CP932_ROOT}/${d})
  endforeach()
  set(SMS_GAME_PCH_DIR ${SMS_CP932_ROOT}/include)
  list(APPEND SMS_GAME_FLAGS -Wno-invalid-source-encoding)
  foreach(lst SMS_DECOMP_CXX_SOURCES SMS_DECOMP_C_SOURCES SMS_DECOMP_PCH_SOURCES)
    set(_retargeted "")
    foreach(f ${${lst}})
      file(RELATIVE_PATH _rel ${SMS_DECOMP} "${f}")
      if(_rel MATCHES "^\\.\\.")
        file(RELATIVE_PATH _rel ${SMS_PATCH_ROOT} "${f}")
      endif()
      if(EXISTS ${SMS_CP932_ROOT}/${_rel})
        list(APPEND _retargeted ${SMS_CP932_ROOT}/${_rel})
      else()
        list(APPEND _retargeted ${f})
      endif()
    endforeach()
    set(${lst} ${_retargeted})
  endforeach()
  message(STATUS "SMS port: compiling game sources from CP932 mirror")
endif()

set_source_files_properties(${SMS_DECOMP_CXX_SOURCES} PROPERTIES LANGUAGE CXX)
set_source_files_properties(${SMS_DECOMP_CXX_SOURCES} ${SMS_DECOMP_C_SOURCES} PROPERTIES
  OBJECT_DEPENDS ${SMS_PATCH_HEADER_STAMP})
if(EXISTS "${SMS_GAME_PCH_DIR}/SMS.pch")
  set(_sms_pch ${SMS_GAME_PCH_DIR}/SMS.pch)
else()
  set(_sms_pch ${SMS_DECOMP}/include/SMS.pch)
endif()
set_source_files_properties(${SMS_DECOMP_PCH_SOURCES} PROPERTIES
  COMPILE_OPTIONS "-include;${_sms_pch}")

option(SMS_WITH_GX "Build and link platform/gx (the graphics layer)" ON)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND SMS_WITH_GX AND SMS_ARCH STREQUAL "32" AND DEFINED ENV{SMS_LINUX32_ROOT})
  set(_graphics "$ENV{SMS_LINUX32_ROOT}/graphics/usr")
  add_library(SDL2::SDL2 SHARED IMPORTED GLOBAL)
  set_target_properties(SDL2::SDL2 PROPERTIES
    IMPORTED_LOCATION "${_graphics}/lib/i386-linux-gnu/libSDL2-2.0.so.0"
    INTERFACE_INCLUDE_DIRECTORIES "${_graphics}/include;${_graphics}/include/SDL2;${_graphics}/include/i386-linux-gnu")
  # The cross linker also needs SDL2's private X11/audio/Wayland dependencies.
  # RUNPATH lets the private loader prefer this SDK's newer libc/C++ runtime.
  add_link_options("LINKER:--enable-new-dtags"
    "LINKER:-rpath-link,${_graphics}/lib/i386-linux-gnu"
    "LINKER:-rpath-link,$ENV{SMS_LINUX32_ROOT}/graphics/lib/i386-linux-gnu"
    "LINKER:-rpath-link,${_graphics}/lib/i386-linux-gnu/pulseaudio")
  set(CMAKE_DISABLE_FIND_PACKAGE_SDL2 ON)
  set(SMS_GX_EGL_LIB "${_graphics}/lib/i386-linux-gnu/libEGL.so.1" CACHE FILEPATH "" FORCE)
  set(SMS_GX_EGL_INCLUDE "${_graphics}/include" CACHE PATH "" FORCE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND SMS_WITH_GX AND SMS_ARCH STREQUAL "32")
  # Only the i386 runtime libraries are installed (no :i386 -dev packages):
  # compile against the amd64 SDL2/EGL headers and link the .so files directly.
  set(_i386 /usr/lib/i386-linux-gnu)
  if(EXISTS ${_i386}/libSDL2-2.0.so.0 AND NOT TARGET SDL2::SDL2)
    set(_shim ${CMAKE_BINARY_DIR}/sdl2-m32)
    set(_shim_config
      "/* 32-bit build against the amd64 SDL2 headers. */\n"
      "#include \"/usr/include/x86_64-linux-gnu/SDL2/_real_SDL_config.h\"\n"
      "#undef SIZEOF_VOIDP\n#define SIZEOF_VOIDP 4\n")
    string(CONCAT _shim_config ${_shim_config})
    set(_old_shim_config "")
    if(EXISTS ${_shim}/SDL2/_real_SDL_config.h)
      file(READ ${_shim}/SDL2/_real_SDL_config.h _old_shim_config)
    endif()
    if(NOT _shim_config STREQUAL _old_shim_config)
      file(WRITE ${_shim}/SDL2/_real_SDL_config.h "${_shim_config}")
    endif()
    add_library(SDL2::SDL2 SHARED IMPORTED GLOBAL)
    set_target_properties(SDL2::SDL2 PROPERTIES
      IMPORTED_LOCATION ${_i386}/libSDL2-2.0.so.0
      INTERFACE_INCLUDE_DIRECTORIES "${_shim};/usr/include/SDL2")
    set(CMAKE_DISABLE_FIND_PACKAGE_SDL2 ON)
  endif()
  if(EXISTS ${_i386}/libEGL.so.1)
    set(SMS_GX_EGL_LIB ${_i386}/libEGL.so.1 CACHE FILEPATH "" FORCE)
    set(SMS_GX_EGL_INCLUDE /usr/include CACHE PATH "" FORCE)
  endif()
endif()
if(SMS_WITH_GX AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/platform/gx/CMakeLists.txt)
  add_subdirectory(platform/gx)
endif()

add_library(sms_game STATIC ${SMS_DECOMP_CXX_SOURCES} ${SMS_DECOMP_C_SOURCES})
# Host THP decoders (the decomp's SDK decoder, patched; platform/thp).
include(${CMAKE_CURRENT_SOURCE_DIR}/platform/thp/thp.cmake)
# The SDK's PADClamp (dead zone / octagon), built from the decomp as C.
target_sources(sms_game PRIVATE ${SMS_DECOMP}/libs/dolphin/src/pad/Padclamp.c)
# port_include must stay ahead of the game headers: its dolphin/types.h
# overrides the decomp's (LP64 integer widths, weak AT_ADDRESS globals).
if(DEFINED SMS_CP932_ROOT)
  # CP932 mirror already merges patched headers.
  target_include_directories(sms_game BEFORE PRIVATE
    ${SMS_PORT_SRC}/port_include ${SMS_GAME_INCLUDE_DIRS})
else()
  target_include_directories(sms_game BEFORE PRIVATE
    ${SMS_PORT_SRC}/port_include ${SMS_PATCHED_INCLUDE_DIRS} ${SMS_GAME_INCLUDE_DIRS})
endif()
if(TARGET sms_gx AND SMS_GX_OVERRIDE_INCLUDE_DIR)
  # GXVert.h vertex writers become functions implemented by sms_gx.
  target_include_directories(sms_game BEFORE PRIVATE ${SMS_GX_OVERRIDE_INCLUDE_DIR})
endif()
target_compile_definitions(sms_game PRIVATE ${SMS_COMMON_DEFS})
target_compile_options(sms_game PRIVATE ${SMS_GAME_FLAGS})

# The game replaces the global operator new/delete with JKRHeap allocation,
# which would also capture libstdc++ and the platform layer (and crash before
# the first JKRHeap exists). Rename the game's definitions and every game
# reference to them, so only game code allocates from JKR heaps.
if(WIN32 AND SMS_ARCH STREQUAL "64")
  # MinGW x64 is LLP64: size_t is unsigned long long, mangled as y.
  set(_sz y)
elseif(SMS_ARCH STREQUAL "32")
  set(_sz j)
elseif(WIN32)
  # LLP64: size_t is unsigned long long.
  set(_sz y)
else()
  set(_sz m)
endif()
if(APPLE)
  # Mach-O adds an extra leading underscore; GNU binutils objcopy does not
  # rewrite Mach-O archives — use llvm-objcopy (Homebrew llvm).
  set(_object_prefix "_")
  find_program(SMS_OBJCOPY NAMES llvm-objcopy
    PATHS "$ENV{SMS_LLVM_BIN}" /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin
    NO_DEFAULT_PATH)
  if(NOT SMS_OBJCOPY)
    find_program(SMS_OBJCOPY NAMES llvm-objcopy)
  endif()
  if(NOT SMS_OBJCOPY)
    message(FATAL_ERROR "llvm-objcopy required on macOS (brew install llvm)")
  endif()
elseif(WIN32)
  # COFF adds a leading underscore to each 32-bit C++ symbol.
  if(SMS_ARCH STREQUAL "32")
    set(_object_prefix "_")
  else()
    set(_object_prefix "")
  endif()
  set(SMS_OBJCOPY objcopy)
else()
  set(_object_prefix "")
  set(SMS_OBJCOPY objcopy)
endif()
file(WRITE ${CMAKE_BINARY_DIR}/game_new_syms.txt
  "${_object_prefix}_Znw${_sz} __sms_game_new\n${_object_prefix}_Zna${_sz} __sms_game_new_array\n${_object_prefix}_ZdlPv __sms_game_delete\n${_object_prefix}_ZdaPv __sms_game_delete_array\n")
add_custom_command(TARGET sms_game POST_BUILD
  COMMAND ${SMS_OBJCOPY} --redefine-syms=${CMAKE_BINARY_DIR}/game_new_syms.txt $<TARGET_FILE:sms_game>
  COMMENT "Renaming the game's global operator new/delete")

# --- Platform layer ---------------------------------------------------------
file(GLOB_RECURSE SMS_PLATFORM_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/*.c
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/*.cpp)
# platform/gx is owned by the graphics layer and builds itself: if it provides
# platform/gx/CMakeLists.txt defining a library target `sms_gx`, it is linked
# in and its strong GX* definitions replace the weak stubs.
list(FILTER SMS_PLATFORM_SOURCES EXCLUDE REGEX "/platform/gx/")
# platform/mods/eclipse is built by cmake/eclipse.cmake, and only with SMS_ECLIPSE.
list(FILTER SMS_PLATFORM_SOURCES EXCLUDE REGEX "/platform/mods/eclipse/")
list(FILTER SMS_PLATFORM_SOURCES EXCLUDE REGEX "/tests/")
if(WIN32 OR APPLE)
  list(FILTER SMS_PLATFORM_SOURCES EXCLUDE REGEX "/platform/trace/trace\\.cpp$")
endif()
add_executable(sms ${SMS_PORT_SRC}/port_main.cpp ${SMS_PLATFORM_SOURCES})
# MSL's maths and the MTX/VEC library round every operation to single or
# double as the Gekko does: SSE on x86 (never x87 extended precision) and no
# contraction (their fused multiply-adds are explicit), on every host.
foreach(gekko_src platform/misc/msl_math.c platform/mtx/mtx.cpp)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i.86)$" OR CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/${gekko_src} PROPERTIES
      COMPILE_OPTIONS "-msse2;-mfpmath=sse;-ffp-contract=off")
  else()
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/${gekko_src} PROPERTIES
      COMPILE_OPTIONS "-ffp-contract=off")
  endif()
endforeach()
# The rest of the platform layer, the graphics layer (sms_gx) included, does
# its float maths in SSE on x86 as well: the 32-bit build would otherwise
# evaluate it in x87 extended precision, and its results would differ in the
# last bit from the 64-bit build's (GXInitSpecularDir's half-angle vector
# moved a specular highlight by one level; docs/64-BIT.md, item 17). These
# are the x86-64 compiler's defaults, so the 64-bit build does not change.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i.86)$" OR CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
  target_compile_options(sms PRIVATE -msse2 -mfpmath=sse)
  if(TARGET sms_gx)
    target_compile_options(sms_gx PRIVATE -msse2 -mfpmath=sse)
  endif()
endif()
if(APPLE AND SMS_SDL2_FRAMEWORK_DIR)
  # @rpath/SDL2.framework: beside the executable (build.sh copies it
  # there) or in SMS.app/Contents/Frameworks. No absolute path, so a copy
  # that lacks the framework fails here as it would on another Mac.
  set_target_properties(sms PROPERTIES BUILD_RPATH "@executable_path;@executable_path/../Frameworks")
endif()
target_include_directories(sms PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/platform ${CMAKE_CURRENT_SOURCE_DIR}/platform/mods/include ${SMS_PORT_SRC}
  ${SMS_PORT_SRC}/port_include ${SMS_DECOMP_INCLUDE_DIRS})
target_compile_definitions(sms PRIVATE ${SMS_COMMON_DEFS} SMS_AUDIO_DSP_HLE=1)
target_compile_options(sms PRIVATE -fno-strict-aliasing -Wno-unused-parameter -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0
  $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++11>)
# dsptask.c points a DSP task at the fixed low-memory address 0x80004600.
if(WIN32)
  target_link_options(sms PRIVATE "-Wl,--defsym=${_object_prefix}lbl_80004600=0x80004600")
  if(SMS_ARCH STREQUAL "32")
    target_link_options(sms PRIVATE -Wl,--large-address-aware)
  endif()
  if(SMS_ARCH STREQUAL "64")
    # Game pointers pass through u32 slots, including addresses of static
    # data. Keep the image below 4 GiB, as on the Linux non-PIE build.
    target_link_options(sms PRIVATE -Wl,--image-base,0x400000 -Wl,--disable-dynamicbase -Wl,--disable-high-entropy-va)
  endif()
elseif(APPLE)
  # Apple ld has no --defsym; provide a normal symbol (DSP HLE does not need the GC address).
  target_sources(sms PRIVATE ${SMS_PORT_SRC}/lbl_80004600_darwin.c)
else()
  target_link_options(sms PRIVATE -Wl,--defsym=lbl_80004600=0x80004600)
endif()
if(SMS_ARCH STREQUAL "64")
  target_compile_options(sms PRIVATE -fno-pie)
  if(NOT APPLE AND NOT WIN32)
    target_link_options(sms PRIVATE -no-pie)
  endif()
endif()
if(APPLE)
  # Shrink PAGEZERO so MAP_32BIT / low stacks work (x86_64 / Rosetta only).
  target_link_options(sms PRIVATE "LINKER:-pagezero_size,0x1000")
endif()
find_package(Threads REQUIRED)
if(WIN32 AND SMS_ARCH STREQUAL "64")
  add_executable(sms_windows_stack_test EXCLUDE_FROM_ALL
    platform/os/tests/windows_stack_test.cpp platform/os/windows_stack.cpp)
  target_include_directories(sms_windows_stack_test PRIVATE ${SMS_PORT_SRC})
  target_compile_features(sms_windows_stack_test PRIVATE cxx_std_11)
  target_link_libraries(sms_windows_stack_test PRIVATE Threads::Threads)
  target_link_options(sms_windows_stack_test PRIVATE -Wl,--image-base,0x400000
    -Wl,--disable-dynamicbase -Wl,--disable-high-entropy-va)
endif()
if(APPLE)
  # Apple ld: -force_load pulls every member (like --whole-archive).
  # force_load does not pull INTERFACE deps, so also link sms_gx normally for SDL2.
  # Darwin requires even weak imports to be resolved unless explicitly allowed.
  # These three code-mod symbols are intentionally absent from standard Sunshine.
  foreach(_optional_mod_symbol IN ITEMS _sPlayerAnimeDatas _sPlayerAnimeInfos _sPlayerAnimeInfosSize)
    target_link_options(sms PRIVATE "LINKER:-U,${_optional_mod_symbol}")
  endforeach()
  target_link_libraries(sms PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  target_link_options(sms PRIVATE "LINKER:-force_load,$<TARGET_FILE:sms_game>")
  add_dependencies(sms sms_game)
  # A link option carries no file dependency: relink when the archives change.
  set_property(TARGET sms APPEND PROPERTY LINK_DEPENDS $<TARGET_FILE:sms_game>)
  if(TARGET sms_gx)
    target_link_options(sms PRIVATE "LINKER:-force_load,$<TARGET_FILE:sms_gx>")
    target_link_libraries(sms PRIVATE sms_gx)
    add_dependencies(sms sms_gx)
  endif()
else()
  target_link_libraries(sms PRIVATE -Wl,--whole-archive sms_game -Wl,--no-whole-archive Threads::Threads m ${CMAKE_DL_LIBS})
  if(TARGET sms_gx)
    target_link_libraries(sms PRIVATE -Wl,--whole-archive sms_gx -Wl,--no-whole-archive)
  endif()
endif()

option(SMS_ECLIPSE "Build Super Mario Eclipse in (fetches its sources, see docs/ECLIPSE.md)" OFF)
if(SMS_ECLIPSE)
  include(cmake/eclipse.cmake)
elseif(WIN32)
  # COFF x64 does not resolve these optional weak data imports to null.
  # Only standard Sunshine uses null imports; Eclipse must supply real ones.
  foreach(_optional_mod_symbol IN ITEMS sPlayerAnimeDatas sPlayerAnimeInfos sPlayerAnimeInfosSize)
    target_link_options(sms PRIVATE "-Wl,--defsym=${_object_prefix}${_optional_mod_symbol}=0")
  endforeach()
endif()

# --- Standalone executable ------------------------------------------------------
# -DSMS_BUNDLE_DISC=/path/GMSE01.iso adds the target sms_standalone: a copy of
# sms with the disc's files packed after it (tools/bundle_disc.py), which runs
# with no disc image or extracted folder beside it (platform/disc reads the
# trailer at the end of its own executable). On macOS it is SMS.app instead,
# with the packed image and an icon in Contents/Resources and SDL2.framework
# inside.
set(SMS_BUNDLE_DISC "" CACHE FILEPATH "GMSE01 disc image (.iso/.gcm/.ciso) to bundle into sms-standalone")
if(SMS_BUNDLE_DISC)
  if(NOT EXISTS "${SMS_BUNDLE_DISC}")
    message(FATAL_ERROR "SMS_BUNDLE_DISC: ${SMS_BUNDLE_DISC} does not exist")
  endif()
endif()
if(SMS_BUNDLE_DISC AND APPLE)
  if(NOT SMS_SDL2_FRAMEWORK_DIR)
    message(FATAL_ERROR "SMS.app needs SDL2.framework (pass -DSMS_SDL2_FRAMEWORK=...)")
  endif()
  set(_app ${CMAKE_BINARY_DIR}/SMS.app)
  set(_app_disc ${_app}/Contents/Resources/disc.gcm)
  set(_app_icon ${_app}/Contents/Resources/SMS.icns)
  add_custom_command(OUTPUT ${_app_disc}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_app}/Contents/Resources
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/bundle_disc.py
            --image-only --disc "${SMS_BUNDLE_DISC}" --out ${_app_disc}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/bundle_disc.py "${SMS_BUNDLE_DISC}"
    COMMENT "Packing ${SMS_BUNDLE_DISC} into SMS.app"
    VERBATIM)
  # The app icon is the game's memory-card icon, taken from the disc.
  add_custom_command(OUTPUT ${_app_icon}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_app}/Contents/Resources
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_icon.py
            --disc "${SMS_BUNDLE_DISC}" --icns ${_app_icon}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_icon.py
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/bundle_disc.py "${SMS_BUNDLE_DISC}"
    COMMENT "Extracting the SMS.app icon"
    VERBATIM)
  add_custom_command(OUTPUT ${_app}/Contents/MacOS/sms
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_mac_app.sh
            ${_app} $<TARGET_FILE:sms> ${SMS_SDL2_FRAMEWORK_DIR}/SDL2.framework
    DEPENDS sms ${_app_disc} ${_app_icon} ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_mac_app.sh
            ${SMS_SDL2_FRAMEWORK_DIR}/SDL2.framework/SDL2
    COMMENT "Assembling SMS.app"
    VERBATIM)
  add_custom_target(sms_standalone ALL DEPENDS ${_app}/Contents/MacOS/sms)
elseif(SMS_BUNDLE_DISC)
  if(WIN32)
    # Explorer's icon for sms.exe (and so sms-standalone.exe): the game's
    # memory-card icon, taken from the disc into an icon resource.
    enable_language(RC)
    set(_ico ${CMAKE_BINARY_DIR}/sms.ico)
    set(_rc ${CMAKE_BINARY_DIR}/sms_icon.rc)
    add_custom_command(OUTPUT ${_ico}
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_icon.py
              --disc "${SMS_BUNDLE_DISC}" --ico ${_ico}
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_icon.py
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/bundle_disc.py "${SMS_BUNDLE_DISC}"
      COMMENT "Extracting the sms.exe icon"
      VERBATIM)
    set(_res ${CMAKE_BINARY_DIR}/sms_icon.res.obj)
    file(CONFIGURE OUTPUT ${_rc} CONTENT "1 ICON \"${_ico}\"\n")
    # Not a source of sms: windres runs gcc to preprocess the .rc, and cannot
    # when a path has a space in it (C:\Users\First Last\...). Through popen,
    # cmd.exe drops the quotes around gcc's path; with --use-temp-file windres
    # runs gcc itself but splits quoted -I"..." options at their spaces, and
    # misreads a command that ends in a quoted .rc path. So windres runs here,
    # with --use-temp-file, without sms's include directories, and from the
    # build directory so the .rc path has no spaces.
    add_custom_command(OUTPUT ${_res}
      COMMAND ${CMAKE_RC_COMPILER} --use-temp-file -O coff -i sms_icon.rc -o ${_res}
      DEPENDS ${_rc} ${_ico}
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      COMMENT "Compiling the sms.exe icon resource"
      VERBATIM)
    set_source_files_properties(${_res} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    target_sources(sms PRIVATE ${_res})
  endif()
  set(_standalone ${CMAKE_BINARY_DIR}/sms-standalone${CMAKE_EXECUTABLE_SUFFIX})
  add_custom_command(OUTPUT ${_standalone}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/bundle_disc.py
            --exe $<TARGET_FILE:sms> --disc "${SMS_BUNDLE_DISC}" --out ${_standalone}
    DEPENDS sms ${CMAKE_CURRENT_SOURCE_DIR}/tools/bundle_disc.py "${SMS_BUNDLE_DISC}"
    COMMENT "Bundling ${SMS_BUNDLE_DISC} into sms-standalone"
    VERBATIM)
  add_custom_target(sms_standalone ALL DEPENDS ${_standalone})
endif()

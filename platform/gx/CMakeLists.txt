cmake_minimum_required(VERSION 3.16)
project(sms_gx C CXX)

# GX/GD for the Super Mario Sunshine PC port: OpenGL 3.3 core backend.
set(SMS_DECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../decomp" CACHE PATH "Path to the decompilation")
# The decomp's game headers and the SDK's (libs/dolphin/include).
set(SMS_GX_DECOMP_INCLUDE ${SMS_DECOMP_DIR}/include ${SMS_DECOMP_DIR}/libs/dolphin/include)
option(SMS_GX_BUILD_TESTS "Build the headless self-test" ON)

set(SMS_GX_SOURCES
    src/gl_loader.cpp
    src/gx_api.cpp
    src/gx_fifo.cpp
    src/gx_hires.cpp
    src/gx_overlay.cpp
    src/gx_platform.cpp
    src/gx_render.cpp
    src/gx_shader.cpp
    src/gx_texture.cpp
    src/gx_trace.cpp
    src/gx_vert.cpp)

# GD is compiled from the decompiled SDK sources: it only emits command bytes.
set(SMS_GX_GD_SOURCES
    ${SMS_DECOMP_DIR}/libs/dolphin/src/gd/GDBase.c
    ${SMS_DECOMP_DIR}/libs/dolphin/src/gd/GDGeometry.c
    ${SMS_DECOMP_DIR}/libs/dolphin/src/gd/GDLight.c
    ${SMS_DECOMP_DIR}/libs/dolphin/src/gd/GDPixel.c
    ${SMS_DECOMP_DIR}/libs/dolphin/src/gd/GDTev.c
    ${SMS_DECOMP_DIR}/libs/dolphin/src/gd/GDTransform.c)
set_source_files_properties(${SMS_GX_GD_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/src/gd_host_prefix.h;-w;-fcommon")

# Game sources must see port_include/dolphin/gx/GXVert.h before the decomp's.
set(SMS_GX_OVERRIDE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/port_include" "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(SMS_GX_OVERRIDE_INCLUDE_DIR "${SMS_GX_OVERRIDE_INCLUDE_DIR}" PARENT_SCOPE)
endif()

# The port's dolphin/types.h override (u32 stays 32 bits on LP64 hosts), so the
# GX objects this library lays over GXTexObj etc. match the game's layout.
if(DEFINED SMS_PORT_SRC)
    set(SMS_GX_TYPES_INCLUDE ${SMS_PORT_SRC}/port_include)
endif()

add_library(sms_gx STATIC ${SMS_GX_SOURCES} ${SMS_GX_GD_SOURCES})
target_include_directories(sms_gx
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/port_include ${SMS_GX_TYPES_INCLUDE} ${SMS_GX_DECOMP_INCLUDE}
    # port_fpu.h (the SDK's float-to-unsigned conversion, port_cvt_fp2unsigned)
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
# macOS has no system GL/glcorearb.h; ship Khronos headers under third_party/.
if(APPLE)
    target_include_directories(sms_gx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
endif()
target_compile_features(sms_gx PRIVATE cxx_std_17)
# The SDK functions here that compute in floats round every operation as the
# DOL's objects do, which MWCC built without contraction: no host compiler may
# fuse a multiply-add (GCC and Clang do on arm64 by default; x86 builds
# without -mfma never fuse, so their code does not change).
target_compile_options(sms_gx PRIVATE -ffp-contract=off)
set_target_properties(sms_gx PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Window (SDL2) and headless (EGL) context creation; both optional so the
# library still builds where only a host-supplied context is used.
# On macOS x86_64, prefer a universal SDL2.framework — Homebrew's bottle is
# often arm64-only and will not link into an x86_64 binary.
set(SMS_SDL2_FRAMEWORK "" CACHE PATH "Path to SDL2.framework (universal)")
if(APPLE)
  set(_sms_sdl2_fw "")
  foreach(_cand
      "${SMS_SDL2_FRAMEWORK}"
      "${CMAKE_BINARY_DIR}/SDL2.framework"
      "${CMAKE_SOURCE_DIR}/build/deps/SDL2.framework"
      "/Library/Frameworks/SDL2.framework")
    if(_cand AND EXISTS "${_cand}/SDL2")
      set(_sms_sdl2_fw "${_cand}")
      break()
    endif()
  endforeach()
  if(_sms_sdl2_fw)
    get_filename_component(_sms_sdl2_fw_dir "${_sms_sdl2_fw}" DIRECTORY)
    add_library(SDL2::SDL2 INTERFACE IMPORTED GLOBAL)
    # SDL.h includes <SDL2/SDL_main.h>, which resolves through -F.
    target_include_directories(SDL2::SDL2 INTERFACE "${_sms_sdl2_fw}/Headers")
    target_compile_options(SDL2::SDL2 INTERFACE "-F${_sms_sdl2_fw_dir}")
    target_link_options(SDL2::SDL2 INTERFACE "LINKER:-F${_sms_sdl2_fw_dir}" "LINKER:-framework,SDL2")
    message(STATUS "sms_gx: using SDL2.framework at ${_sms_sdl2_fw}")
    set(SMS_SDL2_FRAMEWORK_DIR "${_sms_sdl2_fw_dir}" PARENT_SCOPE)
  endif()
endif()
if(NOT TARGET SDL2::SDL2)
  find_package(SDL2 CONFIG QUIET)
endif()
find_library(SMS_GX_EGL_LIB EGL)
find_path(SMS_GX_EGL_INCLUDE EGL/egl.h)
if(TARGET SDL2::SDL2)
    target_compile_definitions(sms_gx PRIVATE SMS_GX_HAVE_SDL2=1)
    target_link_libraries(sms_gx PUBLIC SDL2::SDL2)
    message(STATUS "sms_gx: SDL2 window support")
else()
    message(STATUS "sms_gx: SDL2 not found - no window support")
endif()
if(SMS_GX_EGL_LIB AND SMS_GX_EGL_INCLUDE)
    target_compile_definitions(sms_gx PRIVATE SMS_GX_HAVE_EGL=1)
    target_link_libraries(sms_gx PUBLIC ${SMS_GX_EGL_LIB})
    message(STATUS "sms_gx: EGL headless support")
else()
    message(STATUS "sms_gx: EGL not found - no headless context")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(sms_gx PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wno-unused-function -fno-strict-aliasing>)
endif()

if(SMS_GX_BUILD_TESTS)
    # GXDrawSphere calls MSL's sinf and cosf, which the port links from
    # platform/misc/msl_math.c; the standalone tests link it themselves.
    # Built as the port builds it (the top-level CMakeLists.txt): SSE maths on
    # x86, never x87 extended precision, and no contraction. Source file
    # properties are per directory, so the tests' copy needs its own.
    set(SMS_GX_MSL_MATH ${CMAKE_CURRENT_SOURCE_DIR}/../misc/msl_math.c)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i.86)$" OR CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
        set(_sms_gx_msl_opts "-msse2;-mfpmath=sse;-ffp-contract=off")
    else()
        set(_sms_gx_msl_opts "-ffp-contract=off")
    endif()
    set_source_files_properties(${SMS_GX_MSL_MATH} PROPERTIES COMPILE_OPTIONS "${_sms_gx_msl_opts}"
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
    add_executable(gx_selftest tests/gx_selftest.cpp ${SMS_GX_MSL_MATH})
    target_include_directories(gx_selftest PRIVATE ${SMS_GX_DECOMP_INCLUDE} ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(APPLE)
        target_include_directories(gx_selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
    endif()
    target_link_libraries(gx_selftest PRIVATE sms_gx)
    target_compile_features(gx_selftest PRIVATE cxx_std_17)
    enable_testing()
    add_test(NAME gx_selftest COMMAND gx_selftest --headless)
    set_tests_properties(gx_selftest PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(gx_hires_test tests/gx_hires_test.cpp ${SMS_GX_MSL_MATH})
    target_include_directories(gx_hires_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(gx_hires_test PRIVATE sms_gx)
    target_compile_features(gx_hires_test PRIVATE cxx_std_17)
    add_test(NAME gx_hires_test COMMAND gx_hires_test)
    add_test(NAME gx_hires_fallback_test COMMAND gx_hires_test --fallback)
    add_test(NAME gx_hires_sync_test COMMAND gx_hires_test --sync)
    add_test(NAME gx_hires_no_preload_test COMMAND gx_hires_test --no-preload)
    set_tests_properties(gx_hires_test gx_hires_fallback_test gx_hires_sync_test gx_hires_no_preload_test
        PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(gx_windowtest tests/gx_windowtest.cpp)
    target_include_directories(gx_windowtest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(gx_windowtest PRIVATE cxx_std_17)
    add_test(NAME gx_windowtest COMMAND gx_windowtest)
    if(TARGET SDL2::SDL2)
        add_executable(gx_windowsmoke tests/gx_windowtest.cpp ${SMS_GX_MSL_MATH})
        target_include_directories(gx_windowsmoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_features(gx_windowsmoke PRIVATE cxx_std_17)
        target_compile_definitions(gx_windowsmoke PRIVATE SMS_GX_HAVE_SDL2=1)
        target_link_libraries(gx_windowsmoke PRIVATE sms_gx)
    endif()
endif()

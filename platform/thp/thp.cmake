# Host THP decoders: the decomp's own SDK decoder (libs/dolphin/src/thp/THPDec.c,
# THPAudio.c) with decomp-patches/thp-0*.patch applied, compiled into
# sms_game with the game's flags. Their strong THPInit/THPVideoDecode/
# THPAudioDecode replace the weak stubs in platform/sdk_stubs.cpp.
#
# Use either:
#   include(${CMAKE_CURRENT_SOURCE_DIR}/platform/thp/thp.cmake)   # after add_library(sms_game ...)
# or, without editing CMakeLists.txt:
#   cmake -DCMAKE_PROJECT_INCLUDE=<repo>/platform/thp/thp.cmake ...
function(sms_thp_add_decoders)
  if(NOT TARGET sms_game)
    message(FATAL_ERROR "platform/thp/thp.cmake: sms_game does not exist yet")
  endif()
  set(_thp ${SMS_PATCH_ROOT}/libs/dolphin/src/thp/THPDec.c ${SMS_PATCH_ROOT}/libs/dolphin/src/thp/THPAudio.c)
  foreach(f ${_thp})
    if(NOT EXISTS ${f})
      message(FATAL_ERROR "platform/thp: ${f} missing (are decomp-patches/thp-*.patch applied?)")
    endif()
  endforeach()
  target_sources(sms_game PRIVATE ${_thp})
  message(STATUS "SMS port: host THP decoders linked")
endfunction()

if(TARGET sms_game)
  sms_thp_add_decoders()
else()
  cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL sms_thp_add_decoders)
endif()

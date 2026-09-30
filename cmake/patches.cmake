# Applies decomp-patches/*.patch (unified diffs against decomp/, -p1) to copies
# of the touched files in ${CMAKE_BINARY_DIR}/patched, at configure time.
# Patched sources replace their originals in the source lists; patched headers
# keep their decomp path under patched/ (patched/include,
# patched/libs/<name>/include), and those roots are searched before the
# decomp's own (SMS_PATCHED_INCLUDE_DIRS, SMS_DECOMP_INCLUDE_DIRS).
#
# Patches are applied in a scratch tree and copied over only when the result
# differs, so reconfiguring does not touch unchanged files (and does not force
# a rebuild of everything that includes a patched header).
set(SMS_PATCH_ROOT ${CMAKE_BINARY_DIR}/patched)
set(SMS_PATCHED_INCLUDE_DIRS "")
foreach(d ${SMS_DECOMP_INCLUDE_SUBDIRS})
  list(APPEND SMS_PATCHED_INCLUDE_DIRS ${SMS_PATCH_ROOT}/${d})
endforeach()
set(_scratch ${CMAKE_BINARY_DIR}/patched.new)
file(GLOB SMS_PATCHES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/decomp-patches/*.patch)
list(SORT SMS_PATCHES)
# MWCC's fused multiply-adds as explicit calls (src/port_fmac.h), generated
# by tools/fmacontract/fmarewrite.py against the tree the patches above make:
# applied after them. Its first line records a hash of what it was made
# from (the decomp's src/, include/ and libs/ as they are on disk, and
# decomp-patches/*.patch), which tools/fmacontract/fmastamp.py --check
# compares. A stale patch is not applied, and the build fails (the
# sms_fma_check target in CMakeLists.txt, which checks again at every build,
# so that an edit to the decomp is caught too) until it is regenerated or
# SMS_FMA_CONTRACT is turned off. Configuring still succeeds, so that the
# compile_commands.json the rewriter reads are written.
if(SMS_FMA_CONTRACT)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               ${CMAKE_CURRENT_SOURCE_DIR}/tools/fmacontract/fmastamp.py)
  # configure again when the decomp moves to another commit
  execute_process(COMMAND git -C ${SMS_DECOMP} rev-parse --path-format=absolute --git-path HEAD
                  OUTPUT_VARIABLE _decomp_head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_decomp_head AND EXISTS "${_decomp_head}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_decomp_head})
  endif()
  execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/fmacontract/fmastamp.py
                          --check --decomp ${SMS_DECOMP}
                  RESULT_VARIABLE _fma_rc ERROR_VARIABLE _fma_err)
  file(GLOB _fma_patches CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/decomp-patches/fma/*.patch)
  # a regenerated patch configures again, stale or not
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_fma_patches})
  if(_fma_rc EQUAL 0)
    list(SORT _fma_patches)
    list(APPEND SMS_PATCHES ${_fma_patches})
  else()
    message(WARNING "${_fma_err}The build fails until then.")
  endif()
endif()
file(REMOVE_RECURSE ${_scratch})
file(MAKE_DIRECTORY ${_scratch} ${SMS_PATCHED_INCLUDE_DIRS})
set(_touched "")
foreach(p ${SMS_PATCHES})
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${p})
  file(STRINGS ${p} _hdrs REGEX "^\\+\\+\\+ b/")
  foreach(h ${_hdrs})
    string(REGEX REPLACE "^\\+\\+\\+ b/([^\t ]+).*" "\\1" f "${h}")
    if(NOT EXISTS ${_scratch}/${f})
      get_filename_component(d ${_scratch}/${f} DIRECTORY)
      file(MAKE_DIRECTORY ${d})
      file(COPY_FILE ${SMS_DECOMP}/${f} ${_scratch}/${f})
      list(APPEND _touched ${f})
    endif()
  endforeach()
  execute_process(COMMAND patch -p1 --quiet -d ${_scratch} -i ${p}
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "port patch failed to apply: ${p}")
  endif()
endforeach()

# Sync the scratch tree into patched/: copy changed files, drop stale ones.
file(GLOB_RECURSE _old RELATIVE ${SMS_PATCH_ROOT} ${SMS_PATCH_ROOT}/*)
foreach(f ${_old})
  # Managed below; dropping it here would rebuild every game unit.
  if(f STREQUAL "headers.stamp")
    continue()
  endif()
  list(FIND _touched ${f} idx)
  if(idx EQUAL -1)
    file(REMOVE ${SMS_PATCH_ROOT}/${f})
  endif()
endforeach()
set(_headers "")
foreach(f ${_touched})
  get_filename_component(d ${SMS_PATCH_ROOT}/${f} DIRECTORY)
  file(MAKE_DIRECTORY ${d})
  file(COPY_FILE ${_scratch}/${f} ${SMS_PATCH_ROOT}/${f} ONLY_IF_DIFFERENT)
  if(f MATCHES "^(include|libs/[^/]+/include)/")
    list(APPEND _headers ${f})
  endif()
endforeach()
file(REMOVE_RECURSE ${_scratch})

# A header that becomes patched shadows the original, but existing objects'
# dependency files still name the original: record the set of patched headers
# in a stamp every game object depends on, rewritten only when the set changes.
set(SMS_PATCH_HEADER_STAMP ${SMS_PATCH_ROOT}/headers.stamp)
set(_stamp_text "${_headers}")
if(EXISTS ${SMS_PATCH_HEADER_STAMP})
  file(READ ${SMS_PATCH_HEADER_STAMP} _old_stamp)
else()
  set(_old_stamp "<none>")
endif()
if(NOT _old_stamp STREQUAL _stamp_text)
  file(WRITE ${SMS_PATCH_HEADER_STAMP} "${_stamp_text}")
endif()

foreach(f ${_touched})
  if(f MATCHES "^(src|libs/[^/]+/src)/")
    foreach(lst SMS_DECOMP_CXX_SOURCES SMS_DECOMP_C_SOURCES SMS_DECOMP_PCH_SOURCES)
      list(FIND ${lst} ${SMS_DECOMP}/${f} idx)
      if(NOT idx EQUAL -1)
        list(REMOVE_AT ${lst} ${idx})
        list(INSERT ${lst} ${idx} ${SMS_PATCH_ROOT}/${f})
      endif()
    endforeach()
  endif()
endforeach()
list(LENGTH SMS_PATCHES _n)
message(STATUS "SMS port: ${_n} decomp patch(es) applied")

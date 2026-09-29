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

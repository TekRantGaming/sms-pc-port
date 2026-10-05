# Keep custom partial links below cmd.exe's command-length limit. Generator
# expressions resolve the object libraries after their output paths are known.
function(sms_object_response output filename)
  file(GENERATE OUTPUT "${filename}"
    CONTENT "\"$<JOIN:${ARGN},\"\n\">\"\n")
  set(${output} "@${filename}" PARENT_SCOPE)
endfunction()

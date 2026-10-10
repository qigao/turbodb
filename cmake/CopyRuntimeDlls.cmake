cmake_minimum_required(VERSION 3.21)

list(REMOVE_DUPLICATES runtime_dlls)
file(MAKE_DIRECTORY "${runtime_directory}")
set(copied_names "")
foreach(runtime_dll IN LISTS runtime_dlls)
  if(runtime_dll STREQUAL "")
    continue()
  endif()
  if(runtime_dll IN_LIST project_dlls)
    continue()
  endif()
  cmake_path(GET runtime_dll FILENAME dll_name)
  # Windows names are case insensitive. Different sources with the same name
  # cannot both be deployed to this bin directory.
  string(TOLOWER "${dll_name}" dll_key)
  if(dll_key IN_LIST copied_names)
    message(FATAL_ERROR "Conflicting runtime DLL sources for ${dll_name}")
  endif()
  list(APPEND copied_names "${dll_key}")
  file(COPY_FILE "${runtime_dll}" "${runtime_directory}/${dll_name}"
    ONLY_IF_DIFFERENT)
endforeach()

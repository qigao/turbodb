# Include after all modules: one writer owns dependency DLLs in the shared bin.
# Query the configured graph so optional modules and tests need no copy rules.
set(_turbodb_runtime_directories "${PROJECT_SOURCE_DIR}")
set(_turbodb_runtime_targets "")
set(_turbodb_runtime_dlls "")
set(_turbodb_project_dlls "")
while(_turbodb_runtime_directories)
  list(POP_FRONT _turbodb_runtime_directories _directory)
  get_property(_subdirectories DIRECTORY "${_directory}" PROPERTY SUBDIRECTORIES)
  list(APPEND _turbodb_runtime_directories ${_subdirectories})
  get_property(_targets DIRECTORY "${_directory}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(_target IN LISTS _targets)
    get_target_property(_type "${_target}" TYPE)
    if(_type STREQUAL "EXECUTABLE" OR _type STREQUAL "SHARED_LIBRARY" OR
       _type STREQUAL "MODULE_LIBRARY")
      list(APPEND _turbodb_runtime_targets "${_target}")
      list(APPEND _turbodb_runtime_dlls "$<TARGET_RUNTIME_DLLS:${_target}>")
      if(NOT _type STREQUAL "EXECUTABLE")
        list(APPEND _turbodb_project_dlls "$<TARGET_FILE:${_target}>")
      endif()
    endif()
  endforeach()
endwhile()

if(_turbodb_runtime_targets)
  # file(GENERATE) resolves paths without making the copy task depend on the
  # consumers. In-tree DLLs remain owned by their build targets.
  file(GENERATE
    OUTPUT "${PROJECT_BINARY_DIR}/runtime-dlls-$<CONFIG>.cmake"
    CONTENT
"set(runtime_dlls [==[${_turbodb_runtime_dlls}]==])
set(project_dlls [==[${_turbodb_project_dlls}]==])
set(runtime_directory [==[${CMAKE_RUNTIME_OUTPUT_DIRECTORY}$<$<BOOL:${CMAKE_CONFIGURATION_TYPES}>:/$<CONFIG>>]==])
include([==[${CMAKE_CURRENT_LIST_DIR}/CopyRuntimeDlls.cmake]==])
")
  add_custom_target(turbodb_runtime_dependencies ALL
    COMMAND "${CMAKE_COMMAND}" -P
      "${PROJECT_BINARY_DIR}/runtime-dlls-$<CONFIG>.cmake"
    COMMENT "Copying project runtime dependencies"
    VERBATIM)
  set_target_properties(turbodb_runtime_dependencies PROPERTIES FOLDER "cmake")
  foreach(_target IN LISTS _turbodb_runtime_targets)
    add_dependencies("${_target}" turbodb_runtime_dependencies)
  endforeach()
endif()

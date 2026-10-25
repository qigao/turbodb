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

# CMake's FindSQLite3/FindPostgreSQL modules model the Windows import libraries
# as UNKNOWN imported targets. TARGET_RUNTIME_DLLS therefore cannot infer the
# matching DLLs. Keep them beside the driver modules: Salts intentionally loads
# explicit Plugin paths with DLL_LOAD_DIR | DEFAULT_DIRS and does not broaden
# dependency lookup to PATH.
if(WIN32 AND TARGET orm_sqlite_driver)
  get_filename_component(_turbodb_sqlite_lib_dir "${SQLite3_LIBRARY}" DIRECTORY)
  get_filename_component(_turbodb_sqlite_prefix "${_turbodb_sqlite_lib_dir}"
                         DIRECTORY)
  set(_turbodb_sqlite_runtime "${_turbodb_sqlite_prefix}/bin/sqlite3.dll")
  if(NOT EXISTS "${_turbodb_sqlite_runtime}")
    message(FATAL_ERROR
            "SQLite driver runtime DLL is missing: ${_turbodb_sqlite_runtime}")
  endif()
  list(APPEND _turbodb_runtime_dlls "${_turbodb_sqlite_runtime}")
endif()

if(WIN32 AND TARGET orm_postgresql_driver)
  foreach(_config IN ITEMS DEBUG RELEASE)
    if(PostgreSQL_LIBRARY_${_config})
      get_filename_component(_turbodb_postgresql_lib_dir_${_config}
                             "${PostgreSQL_LIBRARY_${_config}}" DIRECTORY)
      get_filename_component(_turbodb_postgresql_prefix_${_config}
                             "${_turbodb_postgresql_lib_dir_${_config}}"
                             DIRECTORY)
      set(_turbodb_postgresql_runtime_${_config}
          "${_turbodb_postgresql_prefix_${_config}}/bin/libpq.dll")
      if(NOT EXISTS "${_turbodb_postgresql_runtime_${_config}}")
        message(
          FATAL_ERROR
            "PostgreSQL driver runtime DLL is missing: ${_turbodb_postgresql_runtime_${_config}}"
        )
      endif()
    endif()
  endforeach()
  if(_turbodb_postgresql_runtime_DEBUG AND
     _turbodb_postgresql_runtime_RELEASE)
    list(
      APPEND _turbodb_runtime_dlls
      "$<$<CONFIG:Debug>:${_turbodb_postgresql_runtime_DEBUG}>$<$<NOT:$<CONFIG:Debug>>:${_turbodb_postgresql_runtime_RELEASE}>"
    )
  elseif(_turbodb_postgresql_runtime_RELEASE)
    list(APPEND _turbodb_runtime_dlls
         "${_turbodb_postgresql_runtime_RELEASE}")
  elseif(_turbodb_postgresql_runtime_DEBUG)
    list(APPEND _turbodb_runtime_dlls "${_turbodb_postgresql_runtime_DEBUG}")
  endif()
endif()

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

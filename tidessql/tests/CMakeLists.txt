# Engine tests do not load the ORM runtime or its driver plugin.
if(WIN32)
  set(TIDESSQL_TEST_RUNTIME_PATH
    "PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::Core>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TidesDB::tidesdb>")
  if(TARGET zstd::libzstd_shared)
    string(APPEND TIDESSQL_TEST_RUNTIME_PATH
      ";PATH=path_list_prepend:$<TARGET_FILE_DIR:zstd::libzstd_shared>")
  endif()
endif()
add_subdirectory(api)
add_subdirectory(protocol)
if(TURBODB_BUILD_TIDESSQL_SERVER)
  add_subdirectory(daemon)
endif()
include_directories(${PROJECT_SOURCE_DIR}/tidessql/src)
link_libraries(TurboDB::SqlParser Salts::CSTL Salts::CFlow)
add_subdirectory(unit)
add_subdirectory(integration)

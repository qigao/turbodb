if(WIN32)
  set(ORM_TEST_RUNTIME_PATH
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::Core>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::DataBind>"
  )

  string(APPEND ORM_TEST_RUNTIME_PATH
         ";PATH=path_list_prepend:$<TARGET_FILE_DIR:SQLite::SQLite3>")
  if(SQLite3_LIBRARY)
    get_filename_component(ORM_TEST_SQLITE_LIBRARY_DIR "${SQLite3_LIBRARY}"
                           DIRECTORY)
    get_filename_component(ORM_TEST_SQLITE_PREFIX_DIR
                           "${ORM_TEST_SQLITE_LIBRARY_DIR}" DIRECTORY)
    if(EXISTS "${ORM_TEST_SQLITE_PREFIX_DIR}/bin")
      string(APPEND ORM_TEST_RUNTIME_PATH
             ";PATH=path_list_prepend:${ORM_TEST_SQLITE_PREFIX_DIR}/bin")
    endif()
  endif()
  string(APPEND ORM_TEST_RUNTIME_PATH
         ";PATH=path_list_prepend:$<TARGET_FILE_DIR:PostgreSQL::PostgreSQL>")
  string(APPEND ORM_TEST_RUNTIME_PATH
         ";PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboDB::Redis>")
  string(APPEND ORM_TEST_RUNTIME_PATH
         ";PATH=path_list_prepend:$<TARGET_FILE_DIR:TidesDB::tidesdb>")
  if(TARGET zstd::libzstd_shared)
    string(APPEND ORM_TEST_RUNTIME_PATH
           ";PATH=path_list_prepend:$<TARGET_FILE_DIR:zstd::libzstd_shared>")
  endif()
  if(TARGET Snappy::snappy)
    string(APPEND ORM_TEST_RUNTIME_PATH
           ";PATH=path_list_prepend:$<TARGET_FILE_DIR:Snappy::snappy>")
  endif()
  if(TARGET LZ4::lz4_shared)
    string(APPEND ORM_TEST_RUNTIME_PATH
           ";PATH=path_list_prepend:$<TARGET_FILE_DIR:LZ4::lz4_shared>")
  endif()

  if(EXISTS "${CMAKE_C_COMPILER}")
    get_filename_component(ORM_TEST_VC_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
    if(EXISTS "${ORM_TEST_VC_BIN_DIR}")
      string(APPEND ORM_TEST_RUNTIME_PATH
             ";PATH=path_list_prepend:${ORM_TEST_VC_BIN_DIR}")
    endif()
  endif()
endif()

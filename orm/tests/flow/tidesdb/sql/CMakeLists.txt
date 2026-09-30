cmake_add_test(
  NAME orm_tidesdb_sql_parser_test TEST_NAME orm_tidesdb_sql_parser
  FOLDER "orm/tests/flow/tidesdb/sql" SOURCES parser_test.c
  LIBS orm_tidesdb_sql ${ORM_INTERNAL_TEST_LIBRARY} Salts::TinyTest Salts::TinyMock
  INCLUDES ${ORM_SOURCE_DIR}/../drivers/tidesdb/sql ${ORM_SOURCE_DIR}/src/abi
    ${ORM_SOURCE_DIR}/src/flow)
target_compile_features(orm_tidesdb_sql_parser_test PRIVATE c_std_11)
set_target_properties(orm_tidesdb_sql_parser_test PROPERTIES C_STANDARD_REQUIRED ON LINKER_LANGUAGE CXX)
target_compile_options(orm_tidesdb_sql_parser_test PRIVATE ${ORM_TEST_WARNING_OPTIONS})
if(WIN32)
  set_tests_properties(orm_tidesdb_sql_parser PROPERTIES
    ENVIRONMENT_MODIFICATION "${ORM_TEST_RUNTIME_PATH}")
endif()

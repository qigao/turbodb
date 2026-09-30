cmake_add_test(
  NAME orm_tidesdb_sql_parser_test TEST_NAME orm_tidesdb_sql_parser
  FOLDER "orm/tests/flow/tidesdb/sql" SOURCES parser_test.c
  LIBS orm_tidesdb_sql ${ORM_INTERNAL_TEST_LIBRARY} Salts::TinyTest Salts::TinyMock
  INCLUDES ${ORM_SOURCE_DIR}/../drivers/tidesdb/sql ${ORM_SOURCE_DIR}/src/abi
    ${ORM_SOURCE_DIR}/src/flow)
set_target_properties(orm_tidesdb_sql_parser_test PROPERTIES LINKER_LANGUAGE CXX)
if(WIN32)
  set_tests_properties(orm_tidesdb_sql_parser PROPERTIES
    ENVIRONMENT_MODIFICATION "${ORM_TEST_RUNTIME_PATH}")
endif()

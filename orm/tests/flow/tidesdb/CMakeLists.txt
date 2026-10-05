cmake_add_test(
  NAME orm_tidesdb_row_test
  FOLDER "orm/tests/flow/tidesdb"
  TEST_NAME orm_tidesdb_row
  SOURCES orm_tidesdb_row_test.c ${ORM_SOURCE_DIR}/../drivers/tidesdb/row.c
  LIBS ${ORM_INTERNAL_TEST_LIBRARY} Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/abi
    ${ORM_SOURCE_DIR}/src/flow ${ORM_SOURCE_DIR}/../drivers/tidesdb)

set_property(TARGET orm_tidesdb_row_test PROPERTY LINKER_LANGUAGE CXX)

cmake_add_test(
  NAME orm_tidesdb_flow_test
  FOLDER "orm/tests/flow/tidesdb"
  TEST_NAME orm_tidesdb_flow
  SOURCES orm_tidesdb_flow_test.c
    ${ORM_SOURCE_DIR}/../drivers/tidesdb/orm_tidesdb_cursor.c
  LIBS ${ORM_INTERNAL_TEST_LIBRARY} Salts::DataBind Salts::CFlow Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/flow
    ${ORM_SOURCE_DIR}/../drivers/tidesdb)

set_property(TARGET orm_tidesdb_flow_test PROPERTY LINKER_LANGUAGE CXX)

if(WIN32)
  set_tests_properties(orm_tidesdb_flow PROPERTIES ENVIRONMENT_MODIFICATION
                                                   "${ORM_TEST_RUNTIME_PATH}")
endif()

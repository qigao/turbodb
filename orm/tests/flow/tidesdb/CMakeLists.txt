add_subdirectory(sql)

cmake_add_test(
  NAME orm_tidesdb_row_test
  FOLDER "orm/tests/flow/tidesdb"
  TEST_NAME orm_tidesdb_row
  SOURCES orm_tidesdb_row_test.c ${ORM_SOURCE_DIR}/../drivers/tidesdb/row.c
  LIBS ${ORM_INTERNAL_TEST_LIBRARY} Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/abi
    ${ORM_SOURCE_DIR}/src/flow ${ORM_SOURCE_DIR}/../drivers/tidesdb)
target_compile_features(orm_tidesdb_row_test PRIVATE c_std_11)

set_property(TARGET orm_tidesdb_row_test PROPERTY LINKER_LANGUAGE CXX)
set_target_properties(orm_tidesdb_row_test PROPERTIES C_STANDARD_REQUIRED ON)
target_compile_options(orm_tidesdb_row_test PRIVATE ${ORM_TEST_WARNING_OPTIONS})
if(MSVC)
  target_compile_options(orm_tidesdb_row_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()

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
target_compile_features(orm_tidesdb_flow_test PRIVATE c_std_11)

set_property(TARGET orm_tidesdb_flow_test PROPERTY LINKER_LANGUAGE CXX)
set_target_properties(orm_tidesdb_flow_test PROPERTIES C_STANDARD_REQUIRED ON)
target_compile_options(orm_tidesdb_flow_test
                       PRIVATE ${ORM_TEST_WARNING_OPTIONS})
if(MSVC)
  target_compile_options(orm_tidesdb_flow_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()

if(WIN32)
  set_tests_properties(orm_tidesdb_flow PROPERTIES ENVIRONMENT_MODIFICATION
                                                   "${ORM_TEST_RUNTIME_PATH}")
endif()

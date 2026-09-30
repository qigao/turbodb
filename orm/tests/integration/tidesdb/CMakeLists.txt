add_subdirectory(sql)

cmake_add_test(
  NAME orm_tidesdb_public_flow_test
  FOLDER "orm/tests/integration/tidesdb"
  TEST_NAME orm_tidesdb_public_flow
  SOURCES tidesdb_public_flow_test.c
  LIBS turbo_orm Salts::CFlow Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)

add_dependencies(orm_tidesdb_public_flow_test orm_tidesdb_driver)

if(WIN32)
  set_tests_properties(
    orm_tidesdb_public_flow PROPERTIES ENVIRONMENT_MODIFICATION
                                       "${ORM_TEST_RUNTIME_PATH}")
endif()

set_tests_properties(
  orm_tidesdb_public_flow
  PROPERTIES ENVIRONMENT
             "ORM_TIDESDB_PLUGIN=$<TARGET_FILE:orm_tidesdb_driver>")

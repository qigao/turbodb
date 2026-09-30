add_subdirectory(sql)

cmake_add_test(
  NAME orm_tidesdb_public_flow_test
  FOLDER "orm/tests/integration/tidesdb"
  TEST_NAME orm_tidesdb_public_flow
  SOURCES tidesdb_public_flow_test.c
  LIBS turbo_orm Salts::CFlow Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)
target_compile_features(orm_tidesdb_public_flow_test PRIVATE c_std_11)

add_dependencies(orm_tidesdb_public_flow_test orm_tidesdb_driver)
set_target_properties(orm_tidesdb_public_flow_test
                      PROPERTIES C_STANDARD_REQUIRED ON)
target_compile_options(orm_tidesdb_public_flow_test
                       PRIVATE ${ORM_TEST_WARNING_OPTIONS})
if(MSVC)
  target_compile_options(orm_tidesdb_public_flow_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()

if(WIN32)
  set_tests_properties(
    orm_tidesdb_public_flow PROPERTIES ENVIRONMENT_MODIFICATION
                                       "${ORM_TEST_RUNTIME_PATH}")
endif()

set_tests_properties(
  orm_tidesdb_public_flow
  PROPERTIES ENVIRONMENT
             "ORM_TIDESDB_PLUGIN=$<TARGET_FILE:orm_tidesdb_driver>")

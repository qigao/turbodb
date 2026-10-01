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

cmake_add_test(
  NAME orm_tidesdb_generation_maintenance_test
  FOLDER "orm/tests/integration/tidesdb"
  TEST_NAME orm_tidesdb_generation_maintenance
  SOURCES tidesdb_generation_maintenance_test.c
  LIBS turbo_orm Salts::CFlow Salts::Core Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)

add_dependencies(orm_tidesdb_generation_maintenance_test orm_tidesdb_driver)
target_compile_features(orm_tidesdb_generation_maintenance_test PRIVATE c_std_11)
if(MSVC)
  target_compile_options(orm_tidesdb_generation_maintenance_test PRIVATE /WX)
else()
  target_compile_options(orm_tidesdb_generation_maintenance_test PRIVATE -Werror)
endif()

if(WIN32)
  set_tests_properties(
    orm_tidesdb_generation_maintenance
    PROPERTIES ENVIRONMENT_MODIFICATION "${ORM_TEST_RUNTIME_PATH}")
endif()

set_tests_properties(
  orm_tidesdb_generation_maintenance
  PROPERTIES ENVIRONMENT
             "ORM_TIDESDB_PLUGIN=$<TARGET_FILE:orm_tidesdb_driver>")

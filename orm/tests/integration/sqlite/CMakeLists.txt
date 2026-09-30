cmake_add_test(
  NAME orm_sqlite_provider_atomicity_test
  FOLDER "orm/tests/integration/sqlite"
  TEST_NAME orm_sqlite_provider_atomicity
  SOURCES sqlite_provider_atomicity_test.c
  LIBS orm_c_test_static Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)

if(WIN32)
  set_tests_properties(
    orm_sqlite_provider_atomicity PROPERTIES ENVIRONMENT_MODIFICATION
                                             "${ORM_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME orm_sqlite_plugin_maintenance_test
  FOLDER "orm/tests/integration/sqlite"
  TEST_NAME orm_sqlite_plugin_maintenance
  SOURCES sqlite_plugin_maintenance_test.c
  LIBS turbo_orm Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)

add_dependencies(orm_sqlite_plugin_maintenance_test orm_sqlite_driver)
if(MSVC)
  target_compile_options(orm_sqlite_plugin_maintenance_test PRIVATE /WX
                                                                   )
else()
  target_compile_options(orm_sqlite_plugin_maintenance_test PRIVATE -Werror)
endif()

if(WIN32)
  set_tests_properties(
    orm_sqlite_plugin_maintenance PROPERTIES ENVIRONMENT_MODIFICATION
                                             "${ORM_TEST_RUNTIME_PATH}")
endif()

set_tests_properties(
  orm_sqlite_plugin_maintenance
  PROPERTIES ENVIRONMENT "ORM_SQLITE_PLUGIN=$<TARGET_FILE:orm_sqlite_driver>")

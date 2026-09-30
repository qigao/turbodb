cmake_add_test(
  NAME orm_tidesdb_sql_test TEST_NAME orm_tidesdb_sql
  FOLDER "orm/tests/integration/tidesdb/sql" SOURCES sql_test.c
  LIBS turbo_orm Salts::CFlow Salts::TinyTest
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)
add_dependencies(orm_tidesdb_sql_test orm_tidesdb_driver)
set_tests_properties(orm_tidesdb_sql PROPERTIES TIMEOUT 60
  ENVIRONMENT "ORM_TIDESDB_PLUGIN=$<TARGET_FILE:orm_tidesdb_driver>")
if(WIN32)
  set_tests_properties(orm_tidesdb_sql PROPERTIES ENVIRONMENT_MODIFICATION "${ORM_TEST_RUNTIME_PATH}")
endif()

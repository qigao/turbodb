cmake_add_test(
  NAME orm_postgresql_plugin_test
  FOLDER "orm/tests/driver/postgresql"
  TEST_NAME orm_postgresql_plugin
  SOURCES orm_postgresql_plugin_test.c
  LIBS turbo_orm Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)

add_dependencies(orm_postgresql_plugin_test orm_postgresql_driver)
if(MSVC)
  target_compile_options(orm_postgresql_plugin_test PRIVATE /WX)
else()
  target_compile_options(orm_postgresql_plugin_test PRIVATE -Werror)
endif()

set_tests_properties(
  orm_postgresql_plugin
  PROPERTIES ENVIRONMENT
             "ORM_POSTGRESQL_PLUGIN=$<TARGET_FILE:orm_postgresql_driver>")

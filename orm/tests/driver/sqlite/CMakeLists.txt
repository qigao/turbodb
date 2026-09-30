cmake_add_test(
  NAME orm_sqlite_plugin_test
  FOLDER "orm/tests/driver/sqlite"
  TEST_NAME orm_sqlite_plugin
  SOURCES orm_sqlite_plugin_test.c
  LIBS turbo_orm Salts::TinyTest Salts::CFlow Salts::Platform
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)

add_dependencies(orm_sqlite_plugin_test orm_sqlite_driver)
if(MSVC)
  target_compile_options(orm_sqlite_plugin_test PRIVATE /WX)
else()
  target_compile_options(orm_sqlite_plugin_test PRIVATE -Werror)
endif()

if(WIN32)
  set_tests_properties(orm_sqlite_plugin PROPERTIES ENVIRONMENT_MODIFICATION
                                                    "${ORM_TEST_RUNTIME_PATH}")
endif()

set_tests_properties(
  orm_sqlite_plugin
  PROPERTIES ENVIRONMENT "ORM_SQLITE_PLUGIN=$<TARGET_FILE:orm_sqlite_driver>")

cmake_add_test(
  NAME orm_mongodb_plugin_test
  FOLDER "orm/tests/driver/mongodb"
  TEST_NAME orm_mongodb_plugin
  SOURCES orm_mongodb_plugin_test.c
  LIBS turbo_orm
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)

add_dependencies(orm_mongodb_plugin_test orm_mongodb_driver)

if(WIN32)
  set_tests_properties(orm_mongodb_plugin PROPERTIES ENVIRONMENT_MODIFICATION
                                                     "${ORM_TEST_RUNTIME_PATH}")
endif()

set_tests_properties(
  orm_mongodb_plugin
  PROPERTIES ENVIRONMENT "ORM_MONGODB_PLUGIN=$<TARGET_FILE:orm_mongodb_driver>")

cmake_add_test(
  NAME orm_mongodb_live_driver_test
  FOLDER "orm/tests/e2e/mongodb"
  TEST_NAME orm_mongodb_live_driver
  SOURCES mongodb_live_driver_test.c
  LIBS turbo_orm Salts::CFlow
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)
target_compile_features(orm_mongodb_live_driver_test PRIVATE c_std_11)

add_dependencies(orm_mongodb_live_driver_test orm_mongodb_driver)
set_target_properties(orm_mongodb_live_driver_test
                      PROPERTIES C_STANDARD_REQUIRED ON)
target_compile_options(orm_mongodb_live_driver_test
                       PRIVATE ${ORM_TEST_WARNING_OPTIONS})

set_tests_properties(orm_mongodb_live_driver PROPERTIES LABELS e2e RUN_SERIAL
                                                        TRUE)
if(WIN32)
  set_tests_properties(
    orm_mongodb_live_driver
    PROPERTIES
      ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::Core>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::DataBind>"
  )
endif()

set_tests_properties(
  orm_mongodb_live_driver
  PROPERTIES ENVIRONMENT "ORM_MONGODB_PLUGIN=$<TARGET_FILE:orm_mongodb_driver>")

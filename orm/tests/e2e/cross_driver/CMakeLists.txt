cmake_add_test(
  NAME orm_sql_validation_parity_test
  FOLDER "orm/tests/e2e/cross_driver"
  TEST_NAME orm_sql_validation_parity
  SOURCES sql_validation_parity_test.c
  LIBS turbo_orm Salts::DataBind Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)
target_compile_features(orm_sql_validation_parity_test PRIVATE c_std_11)

add_dependencies(orm_sql_validation_parity_test orm_sqlite_driver
                 orm_postgresql_driver)
set_target_properties(orm_sql_validation_parity_test
                      PROPERTIES C_STANDARD_REQUIRED ON)
target_compile_options(orm_sql_validation_parity_test
                       PRIVATE ${ORM_TEST_WARNING_OPTIONS})
if(MSVC)
  target_compile_options(orm_sql_validation_parity_test PRIVATE /WX)
else()
  target_compile_options(orm_sql_validation_parity_test PRIVATE -Werror)
endif()

set_tests_properties(orm_sql_validation_parity PROPERTIES LABELS e2e
                                                          RUN_SERIAL TRUE)
if(WIN32)
  set_tests_properties(
    orm_sql_validation_parity
    PROPERTIES
      ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::Core>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::DataBind>"
  )
endif()

set_tests_properties(
  orm_sql_validation_parity
  PROPERTIES
    ENVIRONMENT
    "ORM_SQLITE_PLUGIN=$<TARGET_FILE:orm_sqlite_driver>;ORM_POSTGRESQL_PLUGIN=$<TARGET_FILE:orm_postgresql_driver>"
)

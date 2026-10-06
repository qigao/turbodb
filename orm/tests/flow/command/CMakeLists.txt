cmake_add_test(
  NAME orm_command_publisher_mock_test
  TEST_NAME orm_command_publisher_mock
  SOURCES orm_command_publisher_mock_test.c
  LIBS ${ORM_INTERNAL_TEST_LIBRARY} Salts::CFlow Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/flow
  FOLDER "orm/tests/flow/command")
set_target_properties(orm_command_publisher_mock_test PROPERTIES
   LINKER_LANGUAGE CXX)
set_tests_properties(orm_command_publisher_mock PROPERTIES TIMEOUT 30)
if(WIN32)
  set_tests_properties(orm_command_publisher_mock PROPERTIES
    ENVIRONMENT_MODIFICATION "${ORM_TEST_RUNTIME_PATH}")
endif()

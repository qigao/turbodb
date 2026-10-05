cmake_add_test(
  NAME tidessql_sdk_cpp_test TEST_NAME tidessql_sdk_cpp
  FOLDER "tidessql/tests/api" SOURCES sdk_cpp_test.cpp
  LIBS TurboDB::TidesSQL Salts::TinyTest)
set_tests_properties(tidessql_sdk_cpp PROPERTIES TIMEOUT 30)
if(WIN32)
  set_tests_properties(tidessql_sdk_cpp PROPERTIES ENVIRONMENT_MODIFICATION "${TIDESSQL_TEST_RUNTIME_PATH}")
endif()

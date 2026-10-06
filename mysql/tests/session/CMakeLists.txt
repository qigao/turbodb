cmake_add_test(
  NAME mysql_client_test
  FOLDER "mysql/tests/session"
  TEST_NAME mysql_client
  SOURCES mysql_client_test.c
  LIBS TurboDB::MySQL Salts::TinyTest)

cmake_add_test(
  NAME mysql_client_cpp_test
  FOLDER "mysql/tests/session"
  TEST_NAME mysql_client_cpp
  SOURCES mysql_client_cpp_test.cpp
  LIBS TurboDB::MySQL Salts::TinyTest)

cmake_add_test(
  NAME mysql_session_mock_test
  FOLDER "mysql/tests/session"
  TEST_NAME mysql_session_mock
  SOURCES mysql_session_mock_test.c
  LIBS TurboDB::MySQL Salts::CNet Salts::TinyTest)

cmake_add_test(
  NAME mysql_script_test
  FOLDER "mysql/tests/session"
  TEST_NAME mysql_script
  SOURCES mysql_script_test.c
  LIBS TurboDB::MySQL Salts::CNet Salts::TinyTest)

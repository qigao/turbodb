cmake_add_test(
  NAME mysql_auth_boundary_test
  FOLDER "drivers/mysql/tests/auth"
  TEST_NAME mysql_auth_boundary
  SOURCES mysql_auth_boundary_test.c
  LIBS TurboDB::MySQL Salts::TinyTest)

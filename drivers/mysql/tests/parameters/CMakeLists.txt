cmake_add_test(
  NAME mysql_parameter_boundary_test
  FOLDER "drivers/mysql/tests/parameters"
  TEST_NAME mysql_parameter_boundary
  SOURCES mysql_parameter_boundary_test.c
  LIBS TurboDB::MySQL Salts::TinyTest)

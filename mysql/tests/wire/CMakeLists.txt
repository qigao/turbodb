cmake_add_test(
  NAME mysql_packet_boundary_test
  FOLDER "mysql/tests/wire"
  TEST_NAME mysql_packet_boundary
  SOURCES mysql_packet_boundary_test.c
  LIBS TurboDB::MySQL Salts::TinyTest)

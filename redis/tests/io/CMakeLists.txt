cmake_add_test(
  NAME test_redis_io_mock
  SOURCES test_redis_io_mock.c
  LIBS Salts::CFlow Salts::Core Salts::TinyTest Salts::TinyMock
  DEFS REDIS_STATIC
  FOLDER "redis/tests/io")
set_tests_properties(test_redis_io_mock PROPERTIES TIMEOUT 30)

cmake_minimum_required(VERSION 3.15)

set(REDIS_TEST_LIBS tedis TurboUtils::TinyTest)
set(REDIS_TEST_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/..)

cmake_add_test(
  NAME test_redis_io
  SOURCES test_redis_io.c ../redis_io.c
  LIBS TurboUtils::CFlow TurboUtils::Core TurboUtils::TinyTest
  INCLUDES ${REDIS_TEST_INCLUDES}
  FOLDER "redis/tests")
target_compile_definitions(test_redis_io PRIVATE REDIS_BUILD)
cmake_add_test(
  NAME test_redis_socket
  SOURCES test_redis_socket.c ../redis_socket.c
  LIBS TurboUtils::Core TurboUtils::TinyTest
  INCLUDES ${REDIS_TEST_INCLUDES}
  FOLDER "redis/tests")
if(WIN32)
  target_link_libraries(test_redis_socket PRIVATE ws2_32)
endif()

cmake_add_test(
  NAME test_redis_resp_buffer
  SOURCES test_redis_resp_buffer.c
  LIBS ${REDIS_TEST_LIBS}
  INCLUDES ${REDIS_TEST_INCLUDES}
  FOLDER "redis/tests")

cmake_add_test(
  NAME test_redis_cflow
  SOURCES test_redis_cflow.c
  LIBS ${REDIS_TEST_LIBS} TurboUtils::CFlow
  INCLUDES ${REDIS_TEST_INCLUDES}
  FOLDER "redis/tests")
target_compile_definitions(test_redis_cflow PRIVATE REDIS_BUILD)
if(WIN32)
  target_link_libraries(test_redis_cflow PRIVATE ws2_32)
endif()

cmake_add_test(
  NAME test_redis_pool
  SOURCES test_redis_pool.c
  LIBS ${REDIS_TEST_LIBS}
  DEFS REDIS_TESTING
  INCLUDES ${REDIS_TEST_INCLUDES}
  FOLDER "redis/tests")
cmake_add_test(
  NAME test_redis_cluster
  SOURCES test_redis_cluster.c
  LIBS ${REDIS_TEST_LIBS}
  INCLUDES ${REDIS_TEST_INCLUDES}
  FOLDER "redis/tests")
cmake_add_test(
  NAME test_redis_sentinel
  SOURCES test_redis_sentinel.c
  LIBS ${REDIS_TEST_LIBS}
  INCLUDES ${REDIS_TEST_INCLUDES}
  FOLDER "redis/tests")

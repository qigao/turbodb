cmake_minimum_required(VERSION 3.15)

set(REDIS_TEST_LIBS tedis TurboUtils::TinyTest)
set(REDIS_TEST_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/..)

cmake_add_test(
  NAME test_redis_client
  SOURCES test_redis_client.c
  LIBS ${REDIS_TEST_LIBS}
  INCLUDES ${REDIS_TEST_INCLUDES}
  FOLDER "redis/tests")
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

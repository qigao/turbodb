cmake_minimum_required(VERSION 3.15)

if(BUILD_TESTS)
add_subdirectory(io)

cmake_add_test(
  NAME test_redis_io
  SOURCES test_redis_io.c ../redis_io.c
  LIBS CMeta::CFlow Salts::Core Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/..
  FOLDER "redis/tests")
target_compile_definitions(test_redis_io PRIVATE REDIS_BUILD)
cmake_add_test(
  NAME test_redis_socket
  SOURCES test_redis_socket.c ../redis_socket.c
  LIBS Salts::Core Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/..
  FOLDER "redis/tests")
if(WIN32)
  target_link_libraries(test_redis_socket PRIVATE ws2_32)
endif()

cmake_add_test(
  NAME test_redis_resp_buffer
  SOURCES test_redis_resp_buffer.c
  LIBS tedis Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/..
  FOLDER "redis/tests")

cmake_add_test(
  NAME test_redis_cflow
  SOURCES test_redis_cflow.c
  LIBS tedis Salts::TinyTest CMeta::CFlow
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/..
  FOLDER "redis/tests")
target_compile_definitions(test_redis_cflow PRIVATE REDIS_BUILD)
if(WIN32)
  target_link_libraries(test_redis_cflow PRIVATE ws2_32)
endif()

cmake_add_test(
  NAME test_redis_lua_apply
  SOURCES test_redis_lua_apply.c
  LIBS tedis Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/..
  FOLDER "redis/tests")
if(WIN32)
  target_link_libraries(test_redis_lua_apply PRIVATE ws2_32)
endif()

cmake_add_test(
  NAME test_redis_lua_apply_batch
  SOURCES test_redis_lua_apply_batch.c
  LIBS tedis Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/..
  FOLDER "redis/tests")
if(WIN32)
  target_link_libraries(test_redis_lua_apply_batch PRIVATE ws2_32)
endif()

cmake_add_test(
  NAME test_redis_pool
  SOURCES test_redis_pool.c
  LIBS tedis Salts::TinyTest
  DEFS REDIS_TESTING
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/..
  FOLDER "redis/tests")
cmake_add_test(
  NAME test_redis_cluster
  SOURCES test_redis_cluster.c
  LIBS tedis Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/..
  FOLDER "redis/tests")
cmake_add_test(
  NAME test_redis_sentinel
  SOURCES test_redis_sentinel.c
  LIBS tedis Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/..
  FOLDER "redis/tests")

cmake_add_test(
  NAME test_redis_stream_group
  SOURCES test_redis_stream_group.c
  LIBS tedis Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/..
  FOLDER "redis/tests")
if(WIN32)
  target_link_libraries(test_redis_stream_group PRIVATE ws2_32)
endif()
endif()

if(BUILD_BENCHMARK)
  cmake_add_benchmark(NAME redis_protocol_benchmark
    SOURCES protocol_benchmark.c LIBS tedis Salts::TinyTest
    FOLDER "redis/tests")
  add_test(NAME redis_protocol_benchmark COMMAND redis_protocol_benchmark)
  set_tests_properties(redis_protocol_benchmark PROPERTIES LABELS benchmark)
endif()

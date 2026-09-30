cmake_add_test(
  NAME orm_redis_query_test
  FOLDER "orm/tests/abi/redis"
  TEST_NAME orm_redis_query
  SOURCES redis_query_test.c ${ORM_SOURCE_DIR}/../drivers/redis/query.c
  LIBS ${ORM_INTERNAL_TEST_LIBRARY} Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/abi
    ${ORM_SOURCE_DIR}/src/flow ${ORM_SOURCE_DIR}/../drivers/redis)

set_property(TARGET orm_redis_query_test PROPERTY LINKER_LANGUAGE CXX)

if(WIN32)
  set_tests_properties(orm_redis_query PROPERTIES ENVIRONMENT_MODIFICATION
                                                  "${ORM_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME orm_redis_backend_contract_test
  FOLDER "orm/tests/abi/redis"
  TEST_NAME orm_redis_backend_contract
  SOURCES redis_backend_contract_test.c
  LIBS turbo_orm Salts::Core
  DEFS "ORM_REDIS_TEST_PLUGIN_PATH=\"$<TARGET_FILE:orm_redis_driver>\""
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)

add_dependencies(orm_redis_backend_contract_test orm_redis_driver)
if(WIN32)
  target_link_libraries(orm_redis_backend_contract_test PRIVATE ws2_32)
endif()
if(MSVC)
  target_compile_options(orm_redis_backend_contract_test
                         PRIVATE /WX /wd5072)
else()
  target_compile_options(orm_redis_backend_contract_test
                         PRIVATE -Werror)
endif()

if(WIN32)
  set_tests_properties(
    orm_redis_backend_contract PROPERTIES ENVIRONMENT_MODIFICATION
                                          "${ORM_TEST_RUNTIME_PATH}")
endif()

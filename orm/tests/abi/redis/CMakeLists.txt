cmake_add_test(
  NAME orm_redis_query_test
  FOLDER "orm/tests/abi/redis"
  TEST_NAME orm_redis_query
  SOURCES redis_query_test.c ${ORM_SOURCE_DIR}/../drivers/redis/query.c
  LIBS ${ORM_INTERNAL_TEST_LIBRARY} Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/abi
    ${ORM_SOURCE_DIR}/src/flow ${ORM_SOURCE_DIR}/../drivers/redis)
target_compile_features(orm_redis_query_test PRIVATE c_std_11)

set_property(TARGET orm_redis_query_test PROPERTY LINKER_LANGUAGE CXX)
target_compile_options(orm_redis_query_test
                       PRIVATE ${ORM_TEST_WARNING_OPTIONS})
if(MSVC)
  target_compile_options(orm_redis_query_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()

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
target_compile_features(orm_redis_backend_contract_test PRIVATE c_std_11)

add_dependencies(orm_redis_backend_contract_test orm_redis_driver)
if(WIN32)
  target_link_libraries(orm_redis_backend_contract_test PRIVATE ws2_32)
endif()
if(MSVC)
  target_compile_options(orm_redis_backend_contract_test
                         PRIVATE /W4 /WX /std:c11 /wd5072)
else()
  target_compile_options(orm_redis_backend_contract_test
                         PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(WIN32)
  set_tests_properties(
    orm_redis_backend_contract PROPERTIES ENVIRONMENT_MODIFICATION
                                          "${ORM_TEST_RUNTIME_PATH}")
endif()

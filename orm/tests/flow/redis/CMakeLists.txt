cmake_add_test(
  NAME orm_redis_flow_test
  FOLDER "orm/tests/flow/redis"
  TEST_NAME orm_redis_flow
  SOURCES orm_redis_flow_test.c ${ORM_SOURCE_DIR}/../drivers/redis/orm_redis_cursor.c
  LIBS ${ORM_INTERNAL_TEST_LIBRARY} Salts::DataBind Salts::CFlow Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/flow
    ${ORM_SOURCE_DIR}/../drivers/redis)
target_compile_features(orm_redis_flow_test PRIVATE c_std_11)

set_property(TARGET orm_redis_flow_test PROPERTY LINKER_LANGUAGE CXX)
set_target_properties(orm_redis_flow_test PROPERTIES C_STANDARD_REQUIRED ON)
target_compile_options(orm_redis_flow_test PRIVATE ${ORM_TEST_WARNING_OPTIONS})
if(MSVC)
  target_compile_options(orm_redis_flow_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()

if(WIN32)
  set_tests_properties(orm_redis_flow PROPERTIES ENVIRONMENT_MODIFICATION
                                                 "${ORM_TEST_RUNTIME_PATH}")
endif()

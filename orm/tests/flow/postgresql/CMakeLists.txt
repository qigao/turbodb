cmake_add_test(
  NAME orm_postgres_async_test
  FOLDER "orm/tests/flow/postgresql"
  TEST_NAME orm_postgres_async
  SOURCES orm_postgres_async_test.c
    ${ORM_SOURCE_DIR}/../drivers/postgresql/postgres_transport_policy.c
    ${ORM_SOURCE_DIR}/../drivers/postgresql/orm_postgres_cursor.c
    ${ORM_SOURCE_DIR}/../drivers/postgresql/orm_postgres_libpq.c
  LIBS orm_c_test_static PostgreSQL::PostgreSQL Salts::CFlow Salts::TinyTest Salts::TinyMock
  INCLUDES ${ORM_SOURCE_DIR}/src/abi ${ORM_SOURCE_DIR}/src/flow
    ${ORM_SOURCE_DIR}/src/sql ${ORM_SOURCE_DIR}/../drivers/postgresql)
set_target_properties(orm_postgres_async_test PROPERTIES LINKER_LANGUAGE CXX)
if(WIN32)
  set_tests_properties(orm_postgres_async PROPERTIES ENVIRONMENT_MODIFICATION "${ORM_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME orm_postgres_libpq_test
  FOLDER "orm/tests/flow/postgresql"
  TEST_NAME orm_postgres_libpq
  SOURCES orm_postgres_libpq_test.c ../../abi/c_api_fake_libpq.cpp
    ${ORM_SOURCE_DIR}/../drivers/postgresql/orm_postgres_libpq.c
  LIBS orm_test_support Salts::Core Salts::CFlow Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/flow
    ${ORM_SOURCE_DIR}/../drivers/postgresql ${ORM_ABI_TEST_DIR}
    ${PostgreSQL_INCLUDE_DIRS})
target_compile_features(orm_postgres_libpq_test PRIVATE cxx_std_17)

set_target_properties(
  orm_postgres_libpq_test
  PROPERTIES CXX_STANDARD_REQUIRED ON)

if(WIN32)
  set_tests_properties(
    orm_postgres_libpq PROPERTIES ENVIRONMENT_MODIFICATION
                                  "${ORM_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME orm_postgres_flow_test
  FOLDER "orm/tests/flow/postgresql"
  TEST_NAME orm_postgres_flow
  SOURCES orm_postgres_flow_test.c
    ${ORM_SOURCE_DIR}/../drivers/postgresql/orm_postgres_cursor.c
  LIBS ${ORM_INTERNAL_TEST_LIBRARY} Salts::CFlow Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/flow
    ${ORM_SOURCE_DIR}/../drivers/postgresql)

set_property(TARGET orm_postgres_flow_test PROPERTY LINKER_LANGUAGE CXX)

if(WIN32)
  set_tests_properties(orm_postgres_flow PROPERTIES ENVIRONMENT_MODIFICATION
                                                    "${ORM_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME orm_postgres_transport_policy_test
  FOLDER "orm/tests/flow/postgresql"
  TEST_NAME orm_postgres_transport_policy
  SOURCES orm_postgres_transport_policy_test.c
    ${ORM_SOURCE_DIR}/../drivers/postgresql/postgres_transport_policy.c
  LIBS orm_c_test_static Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/abi
    ${ORM_SOURCE_DIR}/../drivers/postgresql)
set_property(TARGET orm_postgres_transport_policy_test PROPERTY LINKER_LANGUAGE CXX)

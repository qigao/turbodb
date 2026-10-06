cmake_add_test(
  NAME tidessql_prepared_owner_test TEST_NAME tidessql_prepared_owner
  FOLDER "tidessql/tests/integration"
  SOURCES prepared_owner_test.c
  LIBS TurboDB::TidesSQL Salts::TinyTest)
set_target_properties(tidessql_prepared_owner_test PROPERTIES LINKER_LANGUAGE CXX)
set_tests_properties(tidessql_prepared_owner PROPERTIES TIMEOUT 60)
if(WIN32)
  set_tests_properties(tidessql_prepared_owner PROPERTIES ENVIRONMENT_MODIFICATION "${TIDESSQL_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME tidessql_prepared_test TEST_NAME tidessql_prepared
  FOLDER "tidessql/tests/integration"
  SOURCES prepared_test.c
  LIBS TurboDB::TidesSQL Salts::TinyTest)
set_target_properties(tidessql_prepared_test PROPERTIES LINKER_LANGUAGE CXX)
set_tests_properties(tidessql_prepared PROPERTIES TIMEOUT 60)
if(WIN32)
  set_tests_properties(tidessql_prepared PROPERTIES ENVIRONMENT_MODIFICATION "${TIDESSQL_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME tidessql_connection_test TEST_NAME tidessql_connection
  FOLDER "tidessql/tests/integration"
  SOURCES connection_test.c
  LIBS TurboDB::TidesSQL Salts::TinyTest)
set_target_properties(tidessql_connection_test PROPERTIES LINKER_LANGUAGE CXX)
set_tests_properties(tidessql_connection PROPERTIES TIMEOUT 60)
if(WIN32)
  set_tests_properties(tidessql_connection PROPERTIES ENVIRONMENT_MODIFICATION "${TIDESSQL_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME tidessql_database_test TEST_NAME tidessql_database
  FOLDER "tidessql/tests/integration"
  SOURCES database_test.c
  LIBS TurboDB::TidesSQL Salts::TinyTest)
set_target_properties(tidessql_database_test PROPERTIES LINKER_LANGUAGE CXX)
set_tests_properties(tidessql_database PROPERTIES TIMEOUT 60)
if(WIN32)
  set_tests_properties(tidessql_database PROPERTIES ENVIRONMENT_MODIFICATION "${TIDESSQL_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME tidessql_dispatch_test TEST_NAME tidessql_dispatch
  FOLDER "tidessql/tests/integration"
  SOURCES dispatch_test.c
  LIBS TurboDB::TidesSQL Salts::TinyTest)
set_target_properties(tidessql_dispatch_test PROPERTIES LINKER_LANGUAGE CXX)
set_tests_properties(tidessql_dispatch PROPERTIES TIMEOUT 60)
if(WIN32)
  set_tests_properties(tidessql_dispatch PROPERTIES ENVIRONMENT_MODIFICATION "${TIDESSQL_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME orm_tidesdb_sql_catalog_store_test TEST_NAME orm_tidesdb_sql_catalog_store
  FOLDER "tidessql/tests/integration"
  SOURCES catalog_store_test.c
  LIBS TidesDB::tidesdb TurboDB::TidesSQL
    Salts::CSTL Salts::CFlow Salts::TinyTest)
set_target_properties(orm_tidesdb_sql_catalog_store_test PROPERTIES LINKER_LANGUAGE CXX)
set_tests_properties(orm_tidesdb_sql_catalog_store PROPERTIES TIMEOUT 60)
if(WIN32)
  set_tests_properties(orm_tidesdb_sql_catalog_store PROPERTIES ENVIRONMENT_MODIFICATION "${TIDESSQL_TEST_RUNTIME_PATH}")
endif()
cmake_add_test(
  NAME orm_tidesdb_sql_relation_test TEST_NAME orm_tidesdb_sql_relation
  FOLDER "tidessql/tests/integration"
  SOURCES relation_test.c
  LIBS TidesDB::tidesdb TurboDB::TidesSQL
    Salts::CSTL Salts::CFlow Salts::TinyTest)
set_target_properties(orm_tidesdb_sql_relation_test PROPERTIES LINKER_LANGUAGE CXX)
set_tests_properties(orm_tidesdb_sql_relation PROPERTIES TIMEOUT 60)
if(WIN32)
  set_tests_properties(orm_tidesdb_sql_relation PROPERTIES ENVIRONMENT_MODIFICATION "${TIDESSQL_TEST_RUNTIME_PATH}")
endif()
cmake_add_test(
  NAME orm_tidesdb_sql_runtime_test TEST_NAME orm_tidesdb_sql_runtime
  FOLDER "tidessql/tests/integration"
  SOURCES runtime_test.c
  LIBS TidesDB::tidesdb TurboDB::TidesSQL
    Salts::CSTL Salts::CFlow Salts::TinyTest)
set_target_properties(orm_tidesdb_sql_runtime_test PROPERTIES LINKER_LANGUAGE CXX)
# Per-case native database close and fault matrices need headroom on macOS runners.
set_tests_properties(orm_tidesdb_sql_runtime PROPERTIES TIMEOUT 180)
if(WIN32)
  set_tests_properties(orm_tidesdb_sql_runtime PROPERTIES ENVIRONMENT_MODIFICATION "${TIDESSQL_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME orm_tidesdb_sql_cte_plan_test TEST_NAME orm_tidesdb_sql_cte_plan
  FOLDER "tidessql/tests/integration"
  SOURCES cte_plan_test.c
  LIBS TidesDB::tidesdb TurboDB::TidesSQL
    Salts::CSTL Salts::CFlow Salts::TinyTest)
set_target_properties(orm_tidesdb_sql_cte_plan_test PROPERTIES LINKER_LANGUAGE CXX)
# The complete native dependency suite includes per-case close and fault matrices.
set_tests_properties(orm_tidesdb_sql_cte_plan PROPERTIES TIMEOUT 90)
if(WIN32)
  set_tests_properties(orm_tidesdb_sql_cte_plan PROPERTIES ENVIRONMENT_MODIFICATION "${TIDESSQL_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME orm_tidesdb_sql_index_store_test TEST_NAME orm_tidesdb_sql_index_store
  FOLDER "tidessql/tests/integration"
  SOURCES index_store_test.c
  LIBS TidesDB::tidesdb TurboDB::TidesSQL
    Salts::CSTL Salts::CFlow Salts::TinyTest)
set_target_properties(orm_tidesdb_sql_index_store_test PROPERTIES LINKER_LANGUAGE CXX)
# This suite repeatedly closes and reopens durable stores during fault sweeps.
set_tests_properties(orm_tidesdb_sql_index_store PROPERTIES TIMEOUT 120)
if(WIN32)
  set_tests_properties(orm_tidesdb_sql_index_store PROPERTIES ENVIRONMENT_MODIFICATION "${TIDESSQL_TEST_RUNTIME_PATH}")
endif()

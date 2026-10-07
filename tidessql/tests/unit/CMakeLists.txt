cmake_add_test(
  NAME orm_tidesdb_sql_budget_test TEST_NAME orm_tidesdb_sql_budget
  FOLDER "tidessql/tests/unit" SOURCES budget_test.c
  LIBS TurboDB::TidesSQL Salts::TinyTest)
set_target_properties(orm_tidesdb_sql_budget_test PROPERTIES LINKER_LANGUAGE CXX)
cmake_add_test(
  NAME orm_tidesdb_sql_value_test TEST_NAME orm_tidesdb_sql_value
  FOLDER "tidessql/tests/unit" SOURCES value_test.c
  LIBS TurboDB::TidesSQL Salts::TinyTest)
set_target_properties(orm_tidesdb_sql_value_test PROPERTIES LINKER_LANGUAGE CXX)
cmake_add_test(
  NAME orm_tidesdb_sql_expr_test TEST_NAME orm_tidesdb_sql_expr
  FOLDER "tidessql/tests/unit" SOURCES expr_test.c
  LIBS TurboDB::TidesSQL Salts::TinyTest)
set_target_properties(orm_tidesdb_sql_expr_test PROPERTIES LINKER_LANGUAGE CXX)
cmake_add_test(
  NAME tidessql_parameters_test TEST_NAME tidessql_parameters
  FOLDER "tidessql/tests/unit" SOURCES parameters_test.c
  LIBS TurboDB::TidesSQL Salts::TinyTest)
set_target_properties(tidessql_parameters_test PROPERTIES LINKER_LANGUAGE CXX)
cmake_add_test(
  NAME orm_tidesdb_sql_scan_test TEST_NAME orm_tidesdb_sql_scan
  FOLDER "tidessql/tests/unit" SOURCES scan_test.c
  LIBS TurboDB::TidesSQL Salts::TinyTest)
set_target_properties(orm_tidesdb_sql_scan_test PROPERTIES LINKER_LANGUAGE CXX)
cmake_add_test(
  NAME orm_tidesdb_sql_aggregate_test TEST_NAME orm_tidesdb_sql_aggregate
  FOLDER "tidessql/tests/unit" SOURCES aggregate_test.c
  LIBS TurboDB::TidesSQL Salts::TinyTest)
set_target_properties(orm_tidesdb_sql_aggregate_test PROPERTIES LINKER_LANGUAGE CXX)
cmake_add_test(
  NAME orm_tidesdb_sql_window_test TEST_NAME orm_tidesdb_sql_window
  FOLDER "tidessql/tests/unit" SOURCES window_test.c
  LIBS TurboDB::TidesSQL Salts::TinyTest)
set_target_properties(orm_tidesdb_sql_window_test PROPERTIES LINKER_LANGUAGE CXX)
cmake_add_test(
  NAME orm_tidesdb_sql_select_test TEST_NAME orm_tidesdb_sql_select
  FOLDER "tidessql/tests/unit" SOURCES select_test.c
  LIBS TurboDB::TidesSQL Salts::TinyTest)
set_target_properties(orm_tidesdb_sql_select_test PROPERTIES LINKER_LANGUAGE CXX)
cmake_add_test(
  NAME orm_tidesdb_sql_catalog_test TEST_NAME orm_tidesdb_sql_catalog
  FOLDER "tidessql/tests/unit" SOURCES catalog_test.c
  LIBS TurboDB::TidesSQL Salts::TinyTest)
set_target_properties(orm_tidesdb_sql_catalog_test PROPERTIES LINKER_LANGUAGE CXX)
cmake_add_test(
  NAME orm_tidesdb_sql_index_test TEST_NAME orm_tidesdb_sql_index
  FOLDER "tidessql/tests/unit" SOURCES index_test.c
  LIBS TurboDB::TidesSQL Salts::TinyTest)
set_target_properties(orm_tidesdb_sql_index_test PROPERTIES LINKER_LANGUAGE CXX)
cmake_add_test(
  NAME orm_tidesdb_sql_index_key_test TEST_NAME orm_tidesdb_sql_index_key
  FOLDER "tidessql/tests/unit" SOURCES index_key_test.c
  LIBS TurboDB::TidesSQL Salts::TinyTest)
set_target_properties(orm_tidesdb_sql_index_key_test PROPERTIES LINKER_LANGUAGE CXX)
cmake_add_test(
  NAME orm_tidesdb_sql_index_record_test TEST_NAME orm_tidesdb_sql_index_record
  FOLDER "tidessql/tests/unit" SOURCES index_record_test.c
  LIBS TurboDB::TidesSQL Salts::TinyTest)
set_target_properties(orm_tidesdb_sql_index_record_test PROPERTIES LINKER_LANGUAGE CXX)
cmake_add_test(
  NAME orm_tidesdb_sql_join_test TEST_NAME orm_tidesdb_sql_join
  FOLDER "tidessql/tests/unit" SOURCES join_test.c
  LIBS TurboDB::TidesSQL Salts::TinyTest)
set_target_properties(orm_tidesdb_sql_join_test PROPERTIES LINKER_LANGUAGE CXX)
cmake_add_test(
  NAME orm_tidesdb_sql_from_test TEST_NAME orm_tidesdb_sql_from
  FOLDER "tidessql/tests/unit" SOURCES from_test.c
  LIBS TurboDB::TidesSQL Salts::TinyTest)
set_target_properties(orm_tidesdb_sql_from_test PROPERTIES LINKER_LANGUAGE CXX)
cmake_add_test(
  NAME orm_tidesdb_sql_union_test TEST_NAME orm_tidesdb_sql_union
  FOLDER "tidessql/tests/unit" SOURCES union_test.c
  LIBS TurboDB::TidesSQL Salts::TinyTest)
set_target_properties(orm_tidesdb_sql_union_test PROPERTIES LINKER_LANGUAGE CXX)
cmake_add_test(
  NAME orm_tidesdb_sql_subquery_test TEST_NAME orm_tidesdb_sql_subquery
  FOLDER "tidessql/tests/unit" SOURCES subquery_test.c
  LIBS TurboDB::TidesSQL Salts::TinyTest)
set_target_properties(orm_tidesdb_sql_subquery_test PROPERTIES LINKER_LANGUAGE CXX)
cmake_add_test(
  NAME orm_tidesdb_sql_cte_store_test TEST_NAME orm_tidesdb_sql_cte_store
  FOLDER "tidessql/tests/unit" SOURCES cte_store_test.c
  LIBS TurboDB::TidesSQL Salts::TinyTest)
set_target_properties(orm_tidesdb_sql_cte_store_test PROPERTIES LINKER_LANGUAGE CXX)
cmake_add_test(
  NAME orm_tidesdb_sql_cte_recursive_test TEST_NAME orm_tidesdb_sql_cte_recursive
  FOLDER "tidessql/tests/unit" SOURCES cte_recursive_test.c
  LIBS TurboDB::TidesSQL Salts::TinyTest)
set_target_properties(orm_tidesdb_sql_cte_recursive_test PROPERTIES LINKER_LANGUAGE CXX)
cmake_add_test(
  NAME orm_tidesdb_sql_cte_bind_test TEST_NAME orm_tidesdb_sql_cte_bind
  FOLDER "tidessql/tests/unit" SOURCES cte_bind_test.c
  LIBS TurboDB::TidesSQL Salts::TinyTest)
set_target_properties(orm_tidesdb_sql_cte_bind_test PROPERTIES LINKER_LANGUAGE CXX)
if(WIN32)
  set_tests_properties(tidessql_parameters orm_tidesdb_sql_budget orm_tidesdb_sql_value
    orm_tidesdb_sql_expr orm_tidesdb_sql_scan orm_tidesdb_sql_aggregate orm_tidesdb_sql_window orm_tidesdb_sql_join orm_tidesdb_sql_union orm_tidesdb_sql_subquery orm_tidesdb_sql_cte_store orm_tidesdb_sql_cte_recursive orm_tidesdb_sql_cte_bind orm_tidesdb_sql_from orm_tidesdb_sql_select orm_tidesdb_sql_catalog PROPERTIES
    ENVIRONMENT_MODIFICATION "${TIDESSQL_TEST_RUNTIME_PATH}")
endif()

get_property(tidessql_tests DIRECTORY PROPERTY TESTS)
set_property(TEST ${tidessql_tests} APPEND PROPERTY LABELS tidessql)

find_package(SQLite3 REQUIRED)

add_library(dbtool_schema_fixture MODULE schema_fixture.c
  ../../../drivers/sqlite/schema.c)
target_link_libraries(dbtool_schema_fixture PRIVATE turbodb_schema_support SQLite::SQLite3)
set_target_properties(dbtool_schema_fixture PROPERTIES
  PREFIX "" C_VISIBILITY_PRESET hidden FOLDER "dbtools/tests/plugin")

add_library(dbtool_schema_missing_fixture MODULE invalid_fixture.c)
target_link_libraries(dbtool_schema_missing_fixture PRIVATE TurboDB::SchemaABI)
target_compile_definitions(dbtool_schema_missing_fixture PRIVATE DBTOOL_FIXTURE_MISSING)
set_target_properties(dbtool_schema_missing_fixture PROPERTIES
  PREFIX "" C_VISIBILITY_PRESET hidden FOLDER "dbtools/tests/plugin")

add_library(dbtool_schema_abi_fixture MODULE invalid_fixture.c)
target_link_libraries(dbtool_schema_abi_fixture PRIVATE TurboDB::SchemaABI)
target_compile_definitions(dbtool_schema_abi_fixture PRIVATE DBTOOL_FIXTURE_ABI)
set_target_properties(dbtool_schema_abi_fixture PROPERTIES
  PREFIX "" C_VISIBILITY_PRESET hidden FOLDER "dbtools/tests/plugin")

add_library(dbtool_schema_contract_fixture MODULE invalid_fixture.c)
target_link_libraries(dbtool_schema_contract_fixture PRIVATE TurboDB::SchemaABI)
target_compile_definitions(dbtool_schema_contract_fixture PRIVATE DBTOOL_FIXTURE_CONTRACT)
set_target_properties(dbtool_schema_contract_fixture PROPERTIES
  PREFIX "" C_VISIBILITY_PRESET hidden FOLDER "dbtools/tests/plugin")

add_library(dbtool_schema_ops_fixture MODULE invalid_fixture.c)
target_link_libraries(dbtool_schema_ops_fixture PRIVATE TurboDB::SchemaABI)
set_target_properties(dbtool_schema_ops_fixture PROPERTIES
  PREFIX "" C_VISIBILITY_PRESET hidden FOLDER "dbtools/tests/plugin")

cmake_add_test(
  NAME dbtool_plugin_test
  SOURCES dbtool_plugin_test.c
  LIBS turbodb_dbtool_core Salts::TinyTest
  DEFS
    "DBTOOL_SCHEMA_FIXTURE=\"$<TARGET_FILE:dbtool_schema_fixture>\""
    "DBTOOL_MISSING_FIXTURE=\"$<TARGET_FILE:dbtool_schema_missing_fixture>\""
    "DBTOOL_ABI_FIXTURE=\"$<TARGET_FILE:dbtool_schema_abi_fixture>\""
    "DBTOOL_CONTRACT_FIXTURE=\"$<TARGET_FILE:dbtool_schema_contract_fixture>\""
    "DBTOOL_OPS_FIXTURE=\"$<TARGET_FILE:dbtool_schema_ops_fixture>\""
  FOLDER "dbtools/tests/plugin")
add_dependencies(dbtool_plugin_test dbtool_schema_fixture dbtool_schema_missing_fixture
  dbtool_schema_abi_fixture dbtool_schema_contract_fixture dbtool_schema_ops_fixture)

if(TARGET orm_sqlite_driver AND TARGET orm_postgresql_driver AND TARGET orm_mysql_driver)
  cmake_add_test(
    NAME dbtool_orm_plugins_test
    SOURCES dbtool_orm_plugins_test.c
    LIBS turbodb_dbtool_core Salts::TinyTest
    DEFS
      "DBTOOL_SQLITE_PLUGIN=\"$<TARGET_FILE:orm_sqlite_driver>\""
      "DBTOOL_POSTGRESQL_PLUGIN=\"$<TARGET_FILE:orm_postgresql_driver>\""
      "DBTOOL_MYSQL_PLUGIN=\"$<TARGET_FILE:orm_mysql_driver>\""
    FOLDER "dbtools/tests/plugin")
  add_dependencies(dbtool_orm_plugins_test orm_sqlite_driver orm_postgresql_driver orm_mysql_driver)
endif()

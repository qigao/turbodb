if(NOT TARGET TurboDB::SqlParser OR NOT TARGET TidesDB::tidesdb)
  message(FATAL_ERROR "TidesSQL requires SqlParser and TidesDB")
endif()

add_library(turbodb_tidessql STATIC
  src/connection.c src/bridge.c src/budget.c src/diagnostics.c src/value.c src/expr.c
  src/work.c src/rows.c src/scan.c src/aggregate.c src/join.c src/union.c
  src/subquery.c src/cte_store.c src/cte_bind.c src/cte_query.c src/from.c
  src/from_run.c src/from_dependent.c src/select.c src/table_clear.c
  src/table_alter.c src/name.c src/catalog.c src/index.c src/index_key.c
  src/index_record.c src/index_directory.c src/index_change.c src/index_store.c
  src/index_drop.c src/index_lookup.c src/catalog_store.c src/relation.c
  src/insert.c src/binding.c src/parameters.c src/parameters_statement.c src/prepared.c
  src/change.c src/show.c src/explain.c
  src/compound.c src/dependencies.c src/runtime.c src/window.c src/session.c)

target_include_directories(turbodb_tidessql PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE src)
target_link_libraries(turbodb_tidessql PUBLIC TurboDB::Types
  PRIVATE TurboDB::SqlParser Salts::CSTL Salts::CFlow TidesDB::tidesdb)
if(UNIX AND NOT APPLE)
  target_link_libraries(turbodb_tidessql PRIVATE m)
endif()
set_target_properties(turbodb_tidessql PROPERTIES C_VISIBILITY_PRESET hidden)
cmake_config_target(turbodb_tidessql ALIAS TurboDB::TidesSQL
  FOLDER tidessql EXPORT_SET TidesSQLTargets EXPORT_NAME TidesSQL VERSION 1.3.0)
cmake_install_headers(FILES include/tidessql/tidessql.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/tidessql)
cmake_config_package(PACKAGE TidesSQL EXPORT_SET TidesSQLTargets NAMESPACE TurboDB::
  CONFIG_TEMPLATE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TidesSQLConfig.cmake.in
  VERSION 1.3.0 COMPATIBILITY SameMajorVersion)

find_package(GmSSL CONFIG REQUIRED)
add_subdirectory(server/mysql)
if(TURBODB_BUILD_TIDESSQL_SERVER)
  add_subdirectory(server/daemon)
endif()
if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
if(BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

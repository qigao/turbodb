cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(
    Orm
    VERSION 2.1.0
    DESCRIPTION "Typed reactive ORM for C11 with C++17 ownership wrappers"
    LANGUAGES C)
  set(ORM_BUILD_TESTS_DEFAULT ON)
else()
  set(ORM_BUILD_TESTS_DEFAULT OFF)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
  include(CmakeUtils)
endif()

option(ORM_BUILD_TESTS "Build the Orm tests" ${ORM_BUILD_TESTS_DEFAULT})
option(ORM_WARNINGS_AS_ERRORS "Treat ORM compiler warnings as errors"
       ${ORM_BUILD_TESTS_DEFAULT})
option(ORM_BUILD_SHARED
       "Build ORM as a shared library so backend clients stay behind TurboDB"
       ON)

# Keep the installed TurboDB::ORM contract identical whether ORM is built on
# its own or as a TurboDB subdirectory. Optional backends remain opt-in so a
# static ORM consumer does not inherit unrelated backend libraries.
if(NOT DEFINED ORM_WITH_SQLITE_DEFAULT)
  set(ORM_WITH_SQLITE_DEFAULT ON)
endif()

if(NOT DEFINED ORM_WITH_PGSQL_DEFAULT)
  set(ORM_WITH_PGSQL_DEFAULT OFF)
endif()
if(NOT DEFINED ORM_WITH_REDIS_DEFAULT)
  set(ORM_WITH_REDIS_DEFAULT OFF)
endif()
if(NOT DEFINED ORM_WITH_MONGODB_DEFAULT)
  set(ORM_WITH_MONGODB_DEFAULT OFF)
endif()
if(NOT DEFINED ORM_WITH_TIDESDB_DEFAULT)
  set(ORM_WITH_TIDESDB_DEFAULT OFF)
endif()

option(ORM_WITH_SQLITE "Enable SQLite backend" ${ORM_WITH_SQLITE_DEFAULT})
option(ORM_WITH_PGSQL "Enable PostgreSQL backend" ${ORM_WITH_PGSQL_DEFAULT})
option(ORM_WITH_REDIS "Enable Redis backend" ${ORM_WITH_REDIS_DEFAULT})
option(ORM_WITH_MONGODB "Enable MongoDB backend" ${ORM_WITH_MONGODB_DEFAULT})
option(ORM_WITH_TIDESDB "Enable TidesDB embedded backend" ${ORM_WITH_TIDESDB_DEFAULT})

if((ORM_BUILD_TESTS OR (ORM_BUILD_SHARED AND ORM_WITH_PGSQL)) AND
   NOT CMAKE_CXX_COMPILER_LOADED)
  enable_language(CXX)
endif()

# Find required base dependencies
find_package(TurboUtils CONFIG REQUIRED)

# Find backend-specific dependencies based on enabled options
if(ORM_WITH_SQLITE)
  find_package(SQLite3 REQUIRED)
endif()

if(ORM_WITH_PGSQL)
  find_package(PostgreSQL REQUIRED)
endif()

if(ORM_WITH_REDIS AND NOT TARGET TurboDB::Redis)
  find_package(TurboDB CONFIG REQUIRED)
endif()

if(ORM_WITH_TIDESDB AND NOT TARGET TidesDB::tidesdb)
  find_package(TidesDB CONFIG REQUIRED)
endif()

if(ORM_WITH_MONGODB)
  find_package(mongoc-1.0 CONFIG REQUIRED)
endif()

set(ORM_VERSION 2.1.0)
set(ORM_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include/orm)
set(ORM_ABI_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/abi)
set(ORM_FLOW_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/flow)
set(ORM_SQL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/sql)
set(ORM_ABI_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/abi)
set(ORM_TEST_SUPPORT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)

set(ORM_C_SOURCES src/abi/orm_core.c src/abi/orm_plan.c src/abi/orm_result.c
                  src/dbs/sql/orm_sql_render.c)
set(ORM_FLOW_SOURCES src/flow/orm_cbind_source.c
                     src/flow/orm_command_source.c
                     src/flow/orm_text_token.c
                     src/dbs/redis/orm_redis_cursor.c
                     src/dbs/mongo/orm_mongo_cursor.c
                     src/dbs/tidesdb/orm_tidesdb_cursor.c)
if(ORM_WITH_SQLITE)
  list(APPEND ORM_C_SOURCES src/dbs/sqlite/backend.c)
  list(APPEND ORM_FLOW_SOURCES src/dbs/sqlite/orm_sqlite_cursor.c)
endif()
if(ORM_WITH_REDIS)
  list(APPEND ORM_C_SOURCES src/dbs/redis/backend.c
                             src/dbs/redis/query.c
                             src/dbs/redis/orm_redis_lib.c)
endif()
if(ORM_WITH_TIDESDB)
  list(APPEND ORM_C_SOURCES src/dbs/tidesdb/bridge.c
                             src/dbs/tidesdb/backend.c
                             src/dbs/tidesdb/row.c)
endif()
if(ORM_WITH_MONGODB)
  list(APPEND ORM_C_SOURCES src/dbs/mongo/backend.c
                             src/dbs/mongo/query.c
                             src/dbs/mongo/orm_mongo_lib.c)
endif()

foreach(ORM_C_SOURCE IN LISTS ORM_C_SOURCES ORM_FLOW_SOURCES)
  if(NOT ORM_C_SOURCE MATCHES "\\.c$")
    message(FATAL_ERROR
            "orm_c accepts only C implementation sources: ${ORM_C_SOURCE}")
  endif()
endforeach()

if(ORM_BUILD_SHARED)
  add_library(orm_c SHARED ${ORM_C_SOURCES} ${ORM_FLOW_SOURCES})
else()
  add_library(orm_c STATIC ${ORM_C_SOURCES} ${ORM_FLOW_SOURCES})
endif()
add_library(orm_cpp INTERFACE)
add_library(TurboDB::ORM ALIAS orm_c)
target_compile_features(orm_c PRIVATE c_std_11)
target_compile_definitions(orm_c PRIVATE ORM_C_BUILD)

if(NOT ORM_BUILD_SHARED)
  target_compile_definitions(orm_c PUBLIC ORM_C_STATIC)
endif()

target_include_directories(
  orm_c
  PUBLIC $<BUILD_INTERFACE:${ORM_INCLUDE_DIR}>
         $<INSTALL_INTERFACE:include/shared/orm>
  PRIVATE ${ORM_ABI_SOURCE_DIR} ${ORM_FLOW_SOURCE_DIR} ${ORM_SQL_SOURCE_DIR})
target_link_libraries(
  orm_c PUBLIC TurboUtils::Core
  PRIVATE TurboUtils::CSerde TurboUtils::CBind TurboUtils::CFlow
          TurboUtils::STL TurboUtils::Platform)

if(ORM_WITH_SQLITE)
  target_compile_definitions(orm_c PRIVATE ORM_WITH_SQLITE)
  target_link_libraries(orm_c PRIVATE SQLite::SQLite3)
endif()

if(ORM_WITH_REDIS)
  target_compile_definitions(orm_c PRIVATE ORM_WITH_REDIS)
  target_link_libraries(orm_c PRIVATE TurboDB::Redis)
endif()

if(ORM_WITH_TIDESDB)
  target_compile_definitions(orm_c PRIVATE ORM_WITH_TIDESDB)
  target_link_libraries(orm_c PRIVATE TidesDB::tidesdb)
endif()

if(ORM_WITH_MONGODB)
  target_compile_definitions(orm_c PRIVATE ORM_WITH_MONGO)
  target_link_libraries(orm_c PRIVATE mongo::mongoc_shared)
endif()

set_target_properties(
  orm_c
  PROPERTIES C_STANDARD_REQUIRED ON
             LINKER_LANGUAGE C
             C_VISIBILITY_PRESET hidden)
target_compile_features(orm_cpp INTERFACE cxx_std_17)
target_include_directories(
  orm_cpp INTERFACE $<BUILD_INTERFACE:${ORM_INCLUDE_DIR}>
                    $<INSTALL_INTERFACE:include/shared/orm>)
target_link_libraries(orm_cpp INTERFACE orm_c)

set(ORM_RUNTIME_DEPENDENCY_ARGS)
if(ORM_BUILD_SHARED)
  list(APPEND ORM_RUNTIME_DEPENDENCY_ARGS
       RUNTIME_DEPENDENCY_SET OrmRuntimeDependencies)
endif()
cmake_config_target(
  orm_c
  ALIAS
  Orm::C
  EXPORT_NAME
  C
  EXPORT_SET
  OrmTargets
  FOLDER
  "orm"
  ${ORM_RUNTIME_DEPENDENCY_ARGS}
  NO_VERSION)
cmake_config_target(
  orm_cpp
  ALIAS
  Orm::Cpp
  EXPORT_NAME
  Cpp
  EXPORT_SET
  OrmTargets
  FOLDER
  "orm"
  NO_VERSION)

if(ORM_WITH_PGSQL)
  set(ORM_POSTGRESQL_SOURCES
      src/dbs/postgres/component.c
      src/dbs/postgres/backend.c
      src/dbs/postgres/orm_postgres_libpq.c
      src/dbs/postgres/orm_postgres_cursor.c
      src/flow/orm_text_token.c)
  foreach(ORM_POSTGRESQL_SOURCE IN LISTS ORM_POSTGRESQL_SOURCES)
    if(NOT ORM_POSTGRESQL_SOURCE MATCHES "\\.c$")
      message(FATAL_ERROR
              "orm_postgresql accepts only C implementation sources: ${ORM_POSTGRESQL_SOURCE}")
    endif()
  endforeach()
  if(ORM_BUILD_SHARED)
    add_library(orm_postgresql SHARED ${ORM_POSTGRESQL_SOURCES})
  else()
    add_library(orm_postgresql STATIC ${ORM_POSTGRESQL_SOURCES})
  endif()
  target_compile_features(orm_postgresql PRIVATE c_std_11)
  target_compile_definitions(orm_postgresql PRIVATE ORM_WITH_PGSQL
                                                     ORM_POSTGRESQL_BUILD)
  if(NOT ORM_BUILD_SHARED)
    target_compile_definitions(orm_postgresql PUBLIC ORM_POSTGRESQL_STATIC)
  endif()
  target_include_directories(
    orm_postgresql
    PUBLIC $<BUILD_INTERFACE:${ORM_INCLUDE_DIR}>
           $<INSTALL_INTERFACE:include/shared/orm>
    PRIVATE ${ORM_ABI_SOURCE_DIR} ${ORM_FLOW_SOURCE_DIR} ${ORM_SQL_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/postgres)
  target_link_libraries(
    orm_postgresql
    PUBLIC orm_c
    PRIVATE PostgreSQL::PostgreSQL TurboUtils::CSerde TurboUtils::CBind
            TurboUtils::CFlow TurboUtils::STL)
  set(ORM_POSTGRESQL_LINKER_LANGUAGE C)
  if(ORM_BUILD_SHARED)
    # Some libpq packages close static BoringSSL archives through the C++
    # runtime. This changes only final linkage; every implementation source
    # in the component remains C11.
    set(ORM_POSTGRESQL_LINKER_LANGUAGE CXX)
  endif()
  set_target_properties(
    orm_postgresql
    PROPERTIES C_STANDARD_REQUIRED ON
               LINKER_LANGUAGE ${ORM_POSTGRESQL_LINKER_LANGUAGE}
               C_VISIBILITY_PRESET hidden)
  set(ORM_POSTGRESQL_RUNTIME_DEPENDENCY_ARGS)
  if(ORM_BUILD_SHARED)
    list(APPEND ORM_POSTGRESQL_RUNTIME_DEPENDENCY_ARGS
         RUNTIME_DEPENDENCY_SET OrmPostgreSQLRuntimeDependencies)
  endif()
  cmake_config_target(
    orm_postgresql
    ALIAS
    Orm::PostgreSQL
    EXPORT_NAME
    PostgreSQL
    EXPORT_SET
    OrmPostgreSQLTargets
    FOLDER
    "orm"
    ${ORM_POSTGRESQL_RUNTIME_DEPENDENCY_ARGS}
    NO_VERSION)
endif()

if(ORM_BUILD_SHARED)
  set(ORM_RUNTIME_SEARCH_DIRECTORIES)
  foreach(ORM_BACKEND_LIBRARY IN ITEMS "${SQLite3_LIBRARY}")
    if(ORM_BACKEND_LIBRARY)
      get_filename_component(ORM_BACKEND_LIBRARY_DIR "${ORM_BACKEND_LIBRARY}" DIRECTORY)
      get_filename_component(ORM_BACKEND_PREFIX_DIR "${ORM_BACKEND_LIBRARY_DIR}" DIRECTORY)
      list(APPEND ORM_RUNTIME_SEARCH_DIRECTORIES "${ORM_BACKEND_PREFIX_DIR}/bin")
    endif()
  endforeach()
  if(WIN32)
    install(
      RUNTIME_DEPENDENCY_SET OrmRuntimeDependencies
      DIRECTORIES ${ORM_RUNTIME_SEARCH_DIRECTORIES}
      PRE_EXCLUDE_REGEXES "api-ms-win-.*" "ext-ms-win-.*"
                          "clang_rt.asan_dynamic-.*" "turbo_utils\\.dll"
      POST_EXCLUDE_REGEXES ".*[\\/]system32[\\/].*"
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  else()
    install(
      RUNTIME_DEPENDENCY_SET OrmRuntimeDependencies
      DIRECTORIES ${ORM_RUNTIME_SEARCH_DIRECTORIES}
      PRE_EXCLUDE_REGEXES "libturbo_.*"
      POST_EXCLUDE_REGEXES "^/lib/.*" "^/usr/lib/.*"
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
  endif()
endif()

if(ORM_BUILD_SHARED AND ORM_WITH_PGSQL)
  get_filename_component(ORM_POSTGRESQL_LIBRARY_DIR "${PostgreSQL_LIBRARY}"
                         DIRECTORY)
  get_filename_component(ORM_POSTGRESQL_PREFIX_DIR
                         "${ORM_POSTGRESQL_LIBRARY_DIR}" DIRECTORY)
  if(WIN32)
    install(
      RUNTIME_DEPENDENCY_SET OrmPostgreSQLRuntimeDependencies
      DIRECTORIES "${ORM_POSTGRESQL_PREFIX_DIR}/bin"
      PRE_EXCLUDE_REGEXES "api-ms-win-.*" "ext-ms-win-.*"
                          "clang_rt.asan_dynamic-.*" "orm_c\\.dll"
                          "turbo_utils\\.dll"
      POST_EXCLUDE_REGEXES ".*[\\/]system32[\\/].*"
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  else()
    install(
      RUNTIME_DEPENDENCY_SET OrmPostgreSQLRuntimeDependencies
      DIRECTORIES "${ORM_POSTGRESQL_PREFIX_DIR}/bin"
      PRE_EXCLUDE_REGEXES "liborm_c\\..*" "libturbo_.*"
      POST_EXCLUDE_REGEXES "^/lib/.*" "^/usr/lib/.*"
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
  endif()
endif()

if(MSVC)
  set(ORM_MSVC_WARNING_OPTIONS /W4 /permissive-)
  if(ORM_WARNINGS_AS_ERRORS)
    list(APPEND ORM_MSVC_WARNING_OPTIONS /WX)
  endif()
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    list(APPEND ORM_MSVC_WARNING_OPTIONS /Zc:preprocessor)
  endif()
endif()

function(orm_enable_strict_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE ${ORM_MSVC_WARNING_OPTIONS})
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(ORM_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

orm_enable_strict_warnings(orm_c)
if(ORM_WITH_PGSQL)
  orm_enable_strict_warnings(orm_postgresql)
endif()

set(ORM_INTERNAL_TEST_LIBRARY orm_c)
if(ORM_BUILD_SHARED AND ORM_BUILD_TESTS)
  add_library(orm_c_test_static STATIC ${ORM_C_SOURCES} ${ORM_FLOW_SOURCES})
  target_compile_features(orm_c_test_static PRIVATE c_std_11)
  target_compile_definitions(orm_c_test_static PRIVATE ORM_C_STATIC)
  target_include_directories(
    orm_c_test_static
    PUBLIC ${ORM_INCLUDE_DIR}
    PRIVATE ${ORM_ABI_SOURCE_DIR} ${ORM_FLOW_SOURCE_DIR} ${ORM_SQL_SOURCE_DIR})
  target_link_libraries(
    orm_c_test_static PUBLIC TurboUtils::Core
    PRIVATE TurboUtils::CSerde TurboUtils::CBind TurboUtils::CFlow
            TurboUtils::STL TurboUtils::Platform)
  if(ORM_WITH_SQLITE)
    target_compile_definitions(orm_c_test_static PRIVATE ORM_WITH_SQLITE)
    target_link_libraries(orm_c_test_static PRIVATE SQLite::SQLite3)
  endif()
  if(ORM_WITH_REDIS)
    target_compile_definitions(orm_c_test_static PRIVATE ORM_WITH_REDIS)
    target_link_libraries(orm_c_test_static PRIVATE TurboDB::Redis)
  endif()
  if(ORM_WITH_TIDESDB)
    target_compile_definitions(orm_c_test_static PRIVATE ORM_WITH_TIDESDB)
    target_link_libraries(orm_c_test_static PRIVATE TidesDB::tidesdb)
  endif()
  if(ORM_WITH_MONGODB)
    target_compile_definitions(orm_c_test_static PRIVATE ORM_WITH_MONGO)
    target_link_libraries(orm_c_test_static PRIVATE mongo::mongoc_shared)
  endif()
  set_target_properties(orm_c_test_static PROPERTIES C_STANDARD_REQUIRED ON)
  orm_enable_strict_warnings(orm_c_test_static)
  set(ORM_INTERNAL_TEST_LIBRARY orm_c_test_static)
endif()

if(ORM_BUILD_TESTS)
  enable_testing()
endif()

if(ORM_BUILD_TESTS)
set(ORM_TEST_PRIVATE_LIBS TurboUtils::Core)

function(orm_link_internal_test TARGET_NAME)
  target_link_libraries(${TARGET_NAME} PRIVATE ${ORM_INTERNAL_TEST_LIBRARY}
                                               ${ARGN})
  if(ORM_BUILD_SHARED)
    # The static test clone expands backend archives into the executable, so
    # its final link must close the same C++ runtime dependencies as orm_c.
    set_property(TARGET ${TARGET_NAME} PROPERTY LINKER_LANGUAGE CXX)
  endif()
endfunction()

if(ORM_WITH_SQLITE)
  list(APPEND ORM_TEST_PRIVATE_LIBS SQLite::SQLite3 TurboUtils::CSerde
                                    TurboUtils::CFlow)
endif()
if(ORM_WITH_REDIS)
  list(APPEND ORM_TEST_PRIVATE_LIBS TurboDB::Redis)
endif()
if(ORM_WITH_TIDESDB)
  list(APPEND ORM_TEST_PRIVATE_LIBS TidesDB::tidesdb)
endif()
if(ORM_WITH_MONGODB)
  list(APPEND ORM_TEST_PRIVATE_LIBS mongo::mongoc_shared)
endif()
set(ORM_TEST_DEFINITIONS ORM_C_STATIC)
if(ORM_WITH_SQLITE)
  list(APPEND ORM_TEST_DEFINITIONS ORM_WITH_SQLITE)
endif()
if(ORM_WITH_REDIS)
  list(APPEND ORM_TEST_DEFINITIONS ORM_WITH_REDIS)
endif()
if(ORM_WITH_TIDESDB)
  list(APPEND ORM_TEST_DEFINITIONS ORM_WITH_TIDESDB)
endif()
if(ORM_WITH_MONGODB)
  list(APPEND ORM_TEST_DEFINITIONS ORM_WITH_MONGO)
endif()

if(ORM_WITH_MONGODB)
add_executable(orm_mongo_query_test tests/flow/orm_mongo_query_test.c)
target_compile_features(orm_mongo_query_test PRIVATE c_std_11)
target_compile_definitions(orm_mongo_query_test PRIVATE ORM_C_STATIC
                                                        ORM_WITH_MONGO)
target_include_directories(
  orm_mongo_query_test PRIVATE ${ORM_INCLUDE_DIR} ${ORM_ABI_SOURCE_DIR}
                               ${ORM_FLOW_SOURCE_DIR}
                               ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/mongo)
orm_link_internal_test(orm_mongo_query_test TurboUtils::TinyTest
                       mongo::bson_shared)
set_target_properties(orm_mongo_query_test
                      PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_mongo_query_test)
if(MSVC)
  target_compile_options(orm_mongo_query_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_mongo_query COMMAND orm_mongo_query_test)
endif()

if(ORM_WITH_PGSQL)
add_executable(
  orm_postgres_libpq_test tests/flow/orm_postgres_libpq_test.c
                           tests/abi/c_api_fake_libpq.cpp
                           src/dbs/postgres/orm_postgres_libpq.c)
target_compile_features(orm_postgres_libpq_test PRIVATE c_std_11 cxx_std_17)
target_compile_definitions(orm_postgres_libpq_test PRIVATE ORM_C_STATIC)
target_include_directories(
  orm_postgres_libpq_test
  PRIVATE ${ORM_INCLUDE_DIR} ${ORM_FLOW_SOURCE_DIR}
          ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/postgres ${ORM_ABI_TEST_DIR}
          ${ORM_TEST_SUPPORT_DIR} ${PostgreSQL_INCLUDE_DIRS})
target_link_libraries(orm_postgres_libpq_test PRIVATE TurboUtils::Core
                                                  TurboUtils::TinyTest)
set_target_properties(
  orm_postgres_libpq_test PROPERTIES CXX_STANDARD_REQUIRED ON
                                     C_STANDARD_REQUIRED ON
                                     FOLDER "orm/tests")
orm_enable_strict_warnings(orm_postgres_libpq_test)
if(MSVC)
  target_compile_options(orm_postgres_libpq_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_postgres_libpq COMMAND orm_postgres_libpq_test)

endif()

add_executable(orm_cbind_source_test tests/flow/orm_cbind_source_test.c)
target_compile_features(orm_cbind_source_test PRIVATE c_std_11)
target_compile_definitions(orm_cbind_source_test PRIVATE ORM_C_STATIC)
target_include_directories(
  orm_cbind_source_test PRIVATE ${ORM_INCLUDE_DIR}
                                 ${ORM_FLOW_SOURCE_DIR})
orm_link_internal_test(orm_cbind_source_test TurboUtils::CBind
                       TurboUtils::CFlow TurboUtils::TinyTest)
set_target_properties(orm_cbind_source_test PROPERTIES C_STANDARD_REQUIRED ON
                                                        FOLDER "orm/tests")
orm_enable_strict_warnings(orm_cbind_source_test)
if(MSVC)
  target_compile_options(orm_cbind_source_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_cbind_source COMMAND orm_cbind_source_test)

add_executable(orm_pure_c_build_test tests/flow/orm_pure_c_build_test.c)
target_compile_features(orm_pure_c_build_test PRIVATE c_std_11)
target_compile_definitions(orm_pure_c_build_test PRIVATE ORM_C_STATIC)
target_include_directories(orm_pure_c_build_test PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(orm_pure_c_build_test PRIVATE orm_c
                                                    TurboUtils::TinyTest)
set_target_properties(orm_pure_c_build_test
                      PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_pure_c_build_test)
if(MSVC)
  target_compile_options(orm_pure_c_build_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_pure_c_build COMMAND orm_pure_c_build_test)

add_executable(orm_sql_render_test tests/flow/orm_sql_render_test.c)
target_compile_features(orm_sql_render_test PRIVATE c_std_11)
target_compile_definitions(orm_sql_render_test PRIVATE ORM_C_STATIC)
target_include_directories(
  orm_sql_render_test PRIVATE ${ORM_INCLUDE_DIR} ${ORM_ABI_SOURCE_DIR}
                              ${ORM_FLOW_SOURCE_DIR}
                              ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/sql)
orm_link_internal_test(orm_sql_render_test TurboUtils::TinyTest)
set_target_properties(orm_sql_render_test
                      PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_sql_render_test)
if(MSVC)
  target_compile_options(orm_sql_render_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_sql_render COMMAND orm_sql_render_test)

set(ORM_POSTGRES_FLOW_TEST_SOURCES tests/flow/orm_postgres_flow_test.c
                                  src/dbs/postgres/orm_postgres_cursor.c)
add_executable(orm_postgres_flow_test ${ORM_POSTGRES_FLOW_TEST_SOURCES})
target_compile_features(orm_postgres_flow_test PRIVATE c_std_11)
target_compile_definitions(orm_postgres_flow_test PRIVATE ORM_C_STATIC)
target_include_directories(
  orm_postgres_flow_test PRIVATE ${ORM_INCLUDE_DIR} ${ORM_FLOW_SOURCE_DIR}
                                 ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/postgres)
orm_link_internal_test(orm_postgres_flow_test TurboUtils::CFlow
                       TurboUtils::TinyTest)
set_target_properties(orm_postgres_flow_test
                      PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_postgres_flow_test)
if(MSVC)
  target_compile_options(orm_postgres_flow_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_postgres_flow COMMAND orm_postgres_flow_test)

add_executable(orm_redis_flow_test tests/flow/orm_redis_flow_test.c)
target_compile_features(orm_redis_flow_test PRIVATE c_std_11)
target_compile_definitions(orm_redis_flow_test PRIVATE ORM_C_STATIC)
target_include_directories(
  orm_redis_flow_test PRIVATE ${ORM_INCLUDE_DIR} ${ORM_FLOW_SOURCE_DIR}
                              ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/redis)
orm_link_internal_test(orm_redis_flow_test TurboUtils::CBind
                       TurboUtils::CFlow TurboUtils::TinyTest)
set_target_properties(orm_redis_flow_test
                      PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_redis_flow_test)
if(MSVC)
  target_compile_options(orm_redis_flow_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_redis_flow COMMAND orm_redis_flow_test)

add_executable(orm_mongo_flow_test tests/flow/orm_mongo_flow_test.c)
target_compile_features(orm_mongo_flow_test PRIVATE c_std_11)
target_compile_definitions(orm_mongo_flow_test PRIVATE ORM_C_STATIC)
target_include_directories(
  orm_mongo_flow_test PRIVATE ${ORM_INCLUDE_DIR} ${ORM_FLOW_SOURCE_DIR}
                              ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/mongo)
orm_link_internal_test(orm_mongo_flow_test TurboUtils::CBind
                       TurboUtils::CFlow TurboUtils::TinyTest)
set_target_properties(orm_mongo_flow_test
                      PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_mongo_flow_test)
if(MSVC)
  target_compile_options(orm_mongo_flow_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_mongo_flow COMMAND orm_mongo_flow_test)

add_executable(orm_tidesdb_row_test tests/flow/orm_tidesdb_row_test.c
                                     src/dbs/tidesdb/row.c)
target_compile_features(orm_tidesdb_row_test PRIVATE c_std_11)
target_compile_definitions(orm_tidesdb_row_test PRIVATE ORM_C_STATIC)
target_include_directories(
  orm_tidesdb_row_test PRIVATE ${ORM_INCLUDE_DIR} ${ORM_ABI_SOURCE_DIR}
                                ${ORM_FLOW_SOURCE_DIR}
                                ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/tidesdb)
orm_link_internal_test(orm_tidesdb_row_test TurboUtils::TinyTest)
set_target_properties(orm_tidesdb_row_test
                      PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_tidesdb_row_test)
if(MSVC)
  target_compile_options(orm_tidesdb_row_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_tidesdb_row COMMAND orm_tidesdb_row_test)

add_executable(orm_tidesdb_flow_test tests/flow/orm_tidesdb_flow_test.c)
target_compile_features(orm_tidesdb_flow_test PRIVATE c_std_11)
target_compile_definitions(orm_tidesdb_flow_test PRIVATE ORM_C_STATIC)
target_include_directories(
  orm_tidesdb_flow_test PRIVATE ${ORM_INCLUDE_DIR} ${ORM_FLOW_SOURCE_DIR}
                                ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/tidesdb)
orm_link_internal_test(orm_tidesdb_flow_test TurboUtils::CBind
                       TurboUtils::CFlow TurboUtils::TinyTest)
set_target_properties(orm_tidesdb_flow_test
                      PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_tidesdb_flow_test)
if(MSVC)
  target_compile_options(orm_tidesdb_flow_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_tidesdb_flow COMMAND orm_tidesdb_flow_test)

if(ORM_WITH_TIDESDB)
add_executable(orm_tidesdb_public_flow_test
               tests/integration/tidesdb_public_flow_test.c)
target_compile_features(orm_tidesdb_public_flow_test PRIVATE c_std_11)
target_compile_definitions(orm_tidesdb_public_flow_test PRIVATE ORM_C_STATIC)
target_include_directories(orm_tidesdb_public_flow_test PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(
  orm_tidesdb_public_flow_test PRIVATE orm_c TurboUtils::CBind
                                       TurboUtils::CFlow TurboUtils::TinyTest)
set_target_properties(orm_tidesdb_public_flow_test
                      PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_tidesdb_public_flow_test)
if(MSVC)
  target_compile_options(orm_tidesdb_public_flow_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_tidesdb_public_flow COMMAND orm_tidesdb_public_flow_test)
endif()

if(ORM_WITH_SQLITE)
add_executable(orm_sqlite_flow_test tests/flow/orm_sqlite_flow_test.c)
target_compile_features(orm_sqlite_flow_test PRIVATE c_std_11)
target_compile_definitions(orm_sqlite_flow_test PRIVATE ORM_C_STATIC)
target_include_directories(
  orm_sqlite_flow_test PRIVATE ${ORM_INCLUDE_DIR}
                               ${ORM_FLOW_SOURCE_DIR}
                               ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/sqlite)
orm_link_internal_test(orm_sqlite_flow_test SQLite::SQLite3 TurboUtils::CBind
                       TurboUtils::CFlow TurboUtils::TinyTest)
set_target_properties(orm_sqlite_flow_test PROPERTIES C_STANDARD_REQUIRED ON
                                                       FOLDER "orm/tests")
orm_enable_strict_warnings(orm_sqlite_flow_test)
if(MSVC)
  target_compile_options(orm_sqlite_flow_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_sqlite_flow COMMAND orm_sqlite_flow_test)

add_executable(orm_public_flow_test tests/flow/orm_public_flow_test.c)
target_compile_features(orm_public_flow_test PRIVATE c_std_11)
target_compile_definitions(orm_public_flow_test PRIVATE ORM_C_STATIC)
target_include_directories(orm_public_flow_test PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(orm_public_flow_test PRIVATE orm_c TurboUtils::CBind
                                                   TurboUtils::CFlow
                                                   TurboUtils::TinyTest)
set_target_properties(orm_public_flow_test
                      PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_public_flow_test)
if(MSVC)
  target_compile_options(orm_public_flow_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_public_flow COMMAND orm_public_flow_test)

add_executable(orm_cpp_flow_test tests/flow/orm_cpp_flow_test.cpp)
target_compile_features(orm_cpp_flow_test PRIVATE cxx_std_17)
target_compile_definitions(orm_cpp_flow_test PRIVATE ORM_C_STATIC)
target_include_directories(orm_cpp_flow_test PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(orm_cpp_flow_test PRIVATE orm_c TurboUtils::CBind
                                                TurboUtils::CFlow
                                                TurboUtils::TinyTest)
set_target_properties(orm_cpp_flow_test
                      PROPERTIES CXX_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_cpp_flow_test)
add_test(NAME orm_cpp_flow COMMAND orm_cpp_flow_test)

endif()

if(ORM_WITH_REDIS)
add_executable(orm_redis_query_test tests/abi/redis_query_test.c)
target_compile_features(orm_redis_query_test PRIVATE c_std_11)
target_compile_definitions(orm_redis_query_test PRIVATE ORM_C_STATIC)
target_include_directories(
  orm_redis_query_test PRIVATE ${ORM_INCLUDE_DIR} ${ORM_ABI_SOURCE_DIR}
                               ${ORM_FLOW_SOURCE_DIR}
                               ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/redis)
orm_link_internal_test(orm_redis_query_test TurboUtils::TinyTest)
orm_enable_strict_warnings(orm_redis_query_test)
if(MSVC)
  target_compile_options(orm_redis_query_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
set_target_properties(orm_redis_query_test PROPERTIES FOLDER "orm/tests")
add_test(NAME orm_redis_query COMMAND orm_redis_query_test)

add_executable(orm_redis_backend_contract_test
               tests/abi/redis_backend_contract_test.c)
target_compile_features(orm_redis_backend_contract_test PRIVATE c_std_11)
orm_link_internal_test(orm_redis_backend_contract_test)
if(WIN32)
  target_link_libraries(orm_redis_backend_contract_test PRIVATE ws2_32)
endif()
if(MSVC)
  target_compile_options(orm_redis_backend_contract_test PRIVATE /W4 /WX
                                                                 /std:c11
                                                                 /wd5072)
else()
  target_compile_options(orm_redis_backend_contract_test
                         PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
set_target_properties(orm_redis_backend_contract_test PROPERTIES FOLDER "orm/tests")
add_test(NAME orm_redis_backend_contract
         COMMAND orm_redis_backend_contract_test)

endif()

if(WIN32)
  set(ORM_TEST_RUNTIME_PATH
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboUtils::Core>")
  set(ORM_RUNTIME_TESTS orm_redis_flow orm_mongo_flow orm_tidesdb_flow
                        orm_cbind_source orm_pure_c_build orm_sql_render
                        orm_postgres_flow)
  if(ORM_WITH_SQLITE)
    string(APPEND ORM_TEST_RUNTIME_PATH
           ";PATH=path_list_prepend:$<TARGET_FILE_DIR:SQLite::SQLite3>")
    list(APPEND ORM_RUNTIME_TESTS orm_sqlite_flow orm_public_flow orm_cpp_flow)
  endif()
  if(ORM_WITH_PGSQL)
    string(APPEND ORM_TEST_RUNTIME_PATH
           ";PATH=path_list_prepend:$<TARGET_FILE_DIR:PostgreSQL::PostgreSQL>")
    list(APPEND ORM_RUNTIME_TESTS orm_postgres_libpq)
  endif()
  if(ORM_WITH_REDIS)
    string(
      APPEND
      ORM_TEST_RUNTIME_PATH
      ";PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboDB::Redis>"
    )
    list(APPEND ORM_RUNTIME_TESTS orm_redis_query orm_redis_backend_contract)
  endif()
  if(ORM_WITH_TIDESDB)
    string(APPEND ORM_TEST_RUNTIME_PATH
           ";PATH=path_list_prepend:$<TARGET_FILE_DIR:TidesDB::tidesdb>")
    if(TARGET zstd::libzstd_shared)
      string(APPEND ORM_TEST_RUNTIME_PATH
             ";PATH=path_list_prepend:$<TARGET_FILE_DIR:zstd::libzstd_shared>")
    endif()
    if(TARGET Snappy::snappy)
      string(APPEND ORM_TEST_RUNTIME_PATH
             ";PATH=path_list_prepend:$<TARGET_FILE_DIR:Snappy::snappy>")
    endif()
    if(TARGET LZ4::lz4_shared)
      string(APPEND ORM_TEST_RUNTIME_PATH
             ";PATH=path_list_prepend:$<TARGET_FILE_DIR:LZ4::lz4_shared>")
    endif()
    list(APPEND ORM_RUNTIME_TESTS orm_tidesdb_public_flow)
  endif()
  if(ORM_WITH_MONGODB)
    string(APPEND ORM_TEST_RUNTIME_PATH
           ";PATH=path_list_prepend:$<TARGET_FILE_DIR:mongo::mongoc_shared>;PATH=path_list_prepend:$<TARGET_FILE_DIR:mongo::bson_shared>")
    list(APPEND ORM_RUNTIME_TESTS orm_mongo_query)
  endif()

  if(EXISTS "${CMAKE_C_COMPILER}")
    get_filename_component(ORM_TEST_VC_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
    if(EXISTS "${ORM_TEST_VC_BIN_DIR}")
      string(APPEND ORM_TEST_RUNTIME_PATH
             ";PATH=path_list_prepend:${ORM_TEST_VC_BIN_DIR}")
    endif()
  endif()

  set_tests_properties(${ORM_RUNTIME_TESTS}
                       PROPERTIES ENVIRONMENT_MODIFICATION
                                  "${ORM_TEST_RUNTIME_PATH}")
endif()

endif()

include(GNUInstallDirs)

cmake_install_headers(
  FILES include/orm/orm.h include/orm/orm.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/shared/orm)
if(ORM_WITH_PGSQL)
  cmake_install_headers(
    FILES include/orm/orm_postgresql.h include/orm/orm_postgresql.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/shared/orm)
endif()

cmake_config_package(PACKAGE Orm VERSION "${ORM_VERSION}")
if(ORM_WITH_PGSQL)
  cmake_config_package(PACKAGE OrmPostgreSQL NAMESPACE Orm::
                       VERSION "${ORM_VERSION}")
endif()

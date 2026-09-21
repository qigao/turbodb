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

# Keep the installed Orm::C contract identical whether ORM is built on its own
# or as a TurboDB subdirectory. The public target is always shared so backend
# development packages never enter the consumer's CMake package boundary.
if(NOT DEFINED ORM_WITH_SQLITE_DEFAULT)
  set(ORM_WITH_SQLITE_DEFAULT ON)
endif()

if(NOT DEFINED ORM_WITH_PGSQL_DEFAULT)
  set(ORM_WITH_PGSQL_DEFAULT ON)
endif()
if(NOT DEFINED ORM_WITH_REDIS_DEFAULT)
  set(ORM_WITH_REDIS_DEFAULT OFF)
endif()
if(NOT DEFINED ORM_WITH_MONGODB_DEFAULT)
  set(ORM_WITH_MONGODB_DEFAULT OFF)
endif()
option(ORM_WITH_SQLITE "Enable SQLite backend" ${ORM_WITH_SQLITE_DEFAULT})
option(ORM_WITH_PGSQL "Enable PostgreSQL backend" ${ORM_WITH_PGSQL_DEFAULT})
option(ORM_WITH_REDIS "Enable Redis backend" ${ORM_WITH_REDIS_DEFAULT})
option(ORM_WITH_MONGODB "Enable MongoDB backend" ${ORM_WITH_MONGODB_DEFAULT})
option(ORM_POSTGRES_LIVE_TESTS
       "Run tests against TURBODB_ORM_PGSQL_TEST_CONNINFO" OFF)

if(ORM_POSTGRES_LIVE_TESTS)
  if(NOT ORM_BUILD_TESTS)
    message(FATAL_ERROR "ORM_POSTGRES_LIVE_TESTS requires ORM_BUILD_TESTS=ON")
  endif()
  if(NOT ORM_WITH_PGSQL)
    message(FATAL_ERROR "ORM_POSTGRES_LIVE_TESTS requires ORM_WITH_PGSQL=ON")
  endif()
  if(NOT DEFINED ENV{TURBODB_ORM_PGSQL_TEST_CONNINFO} OR
     "$ENV{TURBODB_ORM_PGSQL_TEST_CONNINFO}" STREQUAL "")
    message(FATAL_ERROR
            "ORM_POSTGRES_LIVE_TESTS requires TURBODB_ORM_PGSQL_TEST_CONNINFO")
  endif()
endif()

if((ORM_BUILD_TESTS OR ORM_WITH_PGSQL) AND
   NOT CMAKE_CXX_COMPILER_LOADED)
  enable_language(CXX)
endif()

# Find required base dependencies
if(NOT TARGET Salts::Core)
  find_package(Salts CONFIG REQUIRED PATHS "$ENV{SALTS_ROOT}"
               NO_DEFAULT_PATH)
endif()

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

if(NOT TARGET TidesDB::tidesdb)
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
set(ORM_FLOW_SOURCES src/flow/orm_cbind_publisher.c
                     src/flow/orm_command_publisher.c
                     src/flow/orm_text_token.c
                     src/dbs/redis/orm_redis_cursor.c
                     src/dbs/mongo/orm_mongo_cursor.c
                     src/dbs/tidesdb/orm_tidesdb_cursor.c)
if(ORM_WITH_SQLITE)
  list(APPEND ORM_C_SOURCES src/dbs/sqlite/backend.c
                            src/dbs/sqlite/maintenance.c)
  list(APPEND ORM_FLOW_SOURCES src/dbs/sqlite/orm_sqlite_cursor.c)
endif()
if(ORM_WITH_REDIS)
  list(APPEND ORM_C_SOURCES src/dbs/redis/backend.c
                             src/dbs/redis/query.c
                             src/dbs/redis/orm_redis_lib.c)
endif()
list(APPEND ORM_C_SOURCES src/dbs/tidesdb/bridge.c
                           src/dbs/tidesdb/backend.c
                           src/dbs/tidesdb/row.c)
if(ORM_WITH_MONGODB)
  list(APPEND ORM_C_SOURCES src/dbs/mongo/backend.c
                             src/dbs/mongo/query.c
                             src/dbs/mongo/orm_mongo_lib.c)
endif()

foreach(ORM_C_SOURCE IN LISTS ORM_C_SOURCES ORM_FLOW_SOURCES)
  if(NOT ORM_C_SOURCE MATCHES "\\.c$")
    message(FATAL_ERROR
            "turbo_orm accepts only C implementation sources: ${ORM_C_SOURCE}")
  endif()
endforeach()

add_library(turbo_orm SHARED ${ORM_C_SOURCES} ${ORM_FLOW_SOURCES})
add_library(orm_cpp INTERFACE)
target_compile_features(turbo_orm PRIVATE c_std_11)
target_compile_definitions(turbo_orm PRIVATE ORM_C_BUILD)
target_include_directories(
  turbo_orm
  PUBLIC $<BUILD_INTERFACE:${ORM_INCLUDE_DIR}>
         $<INSTALL_INTERFACE:include/shared/orm>
  PRIVATE ${ORM_ABI_SOURCE_DIR} ${ORM_FLOW_SOURCE_DIR} ${ORM_SQL_SOURCE_DIR})
target_link_libraries(
  turbo_orm PUBLIC Salts::Core
  PRIVATE Salts::CSerde Salts::CBind Salts::CFlow
          Salts::CSTL Salts::Platform)

if(ORM_WITH_SQLITE)
  target_compile_definitions(turbo_orm PRIVATE ORM_WITH_SQLITE)
  target_link_libraries(turbo_orm PRIVATE SQLite::SQLite3)
endif()

if(ORM_WITH_REDIS)
  target_compile_definitions(turbo_orm PRIVATE ORM_WITH_REDIS)
  target_link_libraries(turbo_orm PRIVATE TurboDB::Redis)
endif()

target_compile_definitions(turbo_orm PRIVATE ORM_WITH_TIDESDB)
target_link_libraries(turbo_orm PRIVATE TidesDB::tidesdb)

if(ORM_WITH_MONGODB)
  target_compile_definitions(turbo_orm PRIVATE ORM_WITH_MONGO)
  target_link_libraries(turbo_orm PRIVATE mongo::mongoc_shared)
endif()

set_target_properties(
  turbo_orm
  PROPERTIES C_STANDARD_REQUIRED ON
             LINKER_LANGUAGE C
             C_VISIBILITY_PRESET hidden)
target_compile_features(orm_cpp INTERFACE cxx_std_17)
target_include_directories(
  orm_cpp INTERFACE $<BUILD_INTERFACE:${ORM_INCLUDE_DIR}>
                    $<INSTALL_INTERFACE:include/shared/orm>)
target_link_libraries(orm_cpp INTERFACE turbo_orm)

cmake_config_target(
  turbo_orm
  ALIAS
  Orm::C
  EXPORT_NAME
  C
  EXPORT_SET
  OrmTargets
  FOLDER
  "orm"
  RUNTIME_DEPENDENCY_SET
  OrmRuntimeDependencies
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
  add_library(orm_postgresql SHARED ${ORM_POSTGRESQL_SOURCES})
  target_compile_features(orm_postgresql PRIVATE c_std_11)
  target_compile_definitions(orm_postgresql PRIVATE ORM_WITH_PGSQL
                                                     ORM_POSTGRESQL_BUILD)
  target_include_directories(
    orm_postgresql
    PUBLIC $<BUILD_INTERFACE:${ORM_INCLUDE_DIR}>
           $<INSTALL_INTERFACE:include/shared/orm>
    PRIVATE ${ORM_ABI_SOURCE_DIR} ${ORM_FLOW_SOURCE_DIR} ${ORM_SQL_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/postgres)
  target_link_libraries(
    orm_postgresql
    PUBLIC turbo_orm
    PRIVATE PostgreSQL::PostgreSQL Salts::CSerde Salts::CBind
            Salts::CFlow Salts::CSTL)
  # Some libpq packages close static BoringSSL archives through the C++
  # runtime. This changes only final linkage; every implementation source in
  # the component remains C11.
  set_target_properties(
    orm_postgresql
    PROPERTIES C_STANDARD_REQUIRED ON
               LINKER_LANGUAGE CXX
               C_VISIBILITY_PRESET hidden)
  cmake_config_target(
    orm_postgresql
    ALIAS
    Orm::PostgreSQL
    EXPORT_NAME
    PostgreSQL
    EXPORT_SET
    OrmTargets
    FOLDER
    "orm"
    RUNTIME_DEPENDENCY_SET
    OrmPostgreSQLRuntimeDependencies
    NO_VERSION)
endif()

set(ORM_RUNTIME_DEPENDENCY_TARGETS)
if(ORM_WITH_SQLITE)
  list(APPEND ORM_RUNTIME_DEPENDENCY_TARGETS SQLite::SQLite3)
endif()
if(ORM_WITH_REDIS)
  list(APPEND ORM_RUNTIME_DEPENDENCY_TARGETS TurboDB::Redis)
endif()
list(APPEND ORM_RUNTIME_DEPENDENCY_TARGETS TidesDB::tidesdb)
if(ORM_WITH_MONGODB)
  list(APPEND ORM_RUNTIME_DEPENDENCY_TARGETS mongo::mongoc_shared
                                              mongo::bson_shared)
endif()

# Runtime dependency scanning is independent from the consumer CMake contract.
# Include enabled backend and codec target directories so standalone installs
# can resolve their DLL/shared-library closure without development packages.
foreach(ORM_OPTIONAL_RUNTIME_TARGET IN ITEMS zstd::libzstd_shared
                                             Snappy::snappy
                                             LZ4::lz4_shared)
  if(TARGET ${ORM_OPTIONAL_RUNTIME_TARGET})
    list(APPEND ORM_RUNTIME_DEPENDENCY_TARGETS
         ${ORM_OPTIONAL_RUNTIME_TARGET})
  endif()
endforeach()

function(orm_collect_runtime_search_directories TARGET_NAME OUTPUT_VARIABLE)
  if(NOT TARGET ${TARGET_NAME})
    message(FATAL_ERROR
            "missing runtime dependency target: ${TARGET_NAME}")
  endif()

  set(ORM_COLLECTED_RUNTIME_DIRECTORIES "${${OUTPUT_VARIABLE}}")
  list(APPEND ORM_COLLECTED_RUNTIME_DIRECTORIES
       "$<TARGET_FILE_DIR:${TARGET_NAME}>")

  get_target_property(ORM_RUNTIME_TARGET_IMPORTED ${TARGET_NAME} IMPORTED)
  if(WIN32 AND ORM_RUNTIME_TARGET_IMPORTED)
    set(ORM_RUNTIME_ARTIFACT_PROPERTIES IMPORTED_LOCATION IMPORTED_IMPLIB)
    get_target_property(ORM_RUNTIME_TARGET_CONFIGURATIONS ${TARGET_NAME}
                        IMPORTED_CONFIGURATIONS)
    foreach(ORM_RUNTIME_TARGET_CONFIGURATION IN LISTS
            ORM_RUNTIME_TARGET_CONFIGURATIONS)
      string(TOUPPER "${ORM_RUNTIME_TARGET_CONFIGURATION}"
             ORM_RUNTIME_TARGET_CONFIGURATION_UPPER)
      list(APPEND ORM_RUNTIME_ARTIFACT_PROPERTIES
           "IMPORTED_LOCATION_${ORM_RUNTIME_TARGET_CONFIGURATION_UPPER}"
           "IMPORTED_IMPLIB_${ORM_RUNTIME_TARGET_CONFIGURATION_UPPER}")
    endforeach()

    foreach(ORM_RUNTIME_ARTIFACT_PROPERTY IN LISTS
            ORM_RUNTIME_ARTIFACT_PROPERTIES)
      get_target_property(ORM_RUNTIME_ARTIFACT ${TARGET_NAME}
                          ${ORM_RUNTIME_ARTIFACT_PROPERTY})
      if(ORM_RUNTIME_ARTIFACT AND
         NOT ORM_RUNTIME_ARTIFACT MATCHES "-NOTFOUND$" AND
         NOT ORM_RUNTIME_ARTIFACT MATCHES "\\$<")
        get_filename_component(ORM_RUNTIME_ARTIFACT_DIRECTORY
                               "${ORM_RUNTIME_ARTIFACT}" DIRECTORY)
        get_filename_component(ORM_RUNTIME_ARTIFACT_PREFIX
                               "${ORM_RUNTIME_ARTIFACT_DIRECTORY}" DIRECTORY)
        list(APPEND ORM_COLLECTED_RUNTIME_DIRECTORIES
             "${ORM_RUNTIME_ARTIFACT_DIRECTORY}"
             "${ORM_RUNTIME_ARTIFACT_PREFIX}/bin")
      endif()
    endforeach()
  endif()

  list(REMOVE_DUPLICATES ORM_COLLECTED_RUNTIME_DIRECTORIES)
  set(${OUTPUT_VARIABLE} "${ORM_COLLECTED_RUNTIME_DIRECTORIES}" PARENT_SCOPE)
endfunction()

set(ORM_RUNTIME_SEARCH_DIRECTORIES)
foreach(ORM_RUNTIME_DEPENDENCY_TARGET IN LISTS ORM_RUNTIME_DEPENDENCY_TARGETS)
  orm_collect_runtime_search_directories(
    ${ORM_RUNTIME_DEPENDENCY_TARGET} ORM_RUNTIME_SEARCH_DIRECTORIES)
endforeach()

# Find modules may expose only an import-library path rather than the runtime
# DLL location. Preserve the explicit SQL prefix-to-bin derivation as a
# fallback for those targets.
set(ORM_PACKAGE_TEST_EXPECTED_RUNTIME_DIR)
foreach(ORM_BACKEND_LIBRARY IN ITEMS "${SQLite3_LIBRARY}")
  if(WIN32 AND ORM_BACKEND_LIBRARY)
    get_filename_component(ORM_BACKEND_LIBRARY_DIR "${ORM_BACKEND_LIBRARY}"
                           DIRECTORY)
    get_filename_component(ORM_BACKEND_PREFIX_DIR "${ORM_BACKEND_LIBRARY_DIR}"
                           DIRECTORY)
    list(APPEND ORM_RUNTIME_SEARCH_DIRECTORIES "${ORM_BACKEND_PREFIX_DIR}/bin")
    if("${ORM_BACKEND_LIBRARY}" STREQUAL "${SQLite3_LIBRARY}")
      set(ORM_PACKAGE_TEST_EXPECTED_RUNTIME_DIR
          "${ORM_BACKEND_PREFIX_DIR}/bin")
    endif()
  endif()
endforeach()
list(REMOVE_DUPLICATES ORM_RUNTIME_SEARCH_DIRECTORIES)
if(WIN32)
  install(
    RUNTIME_DEPENDENCY_SET OrmRuntimeDependencies
    DIRECTORIES ${ORM_RUNTIME_SEARCH_DIRECTORIES}
    PRE_EXCLUDE_REGEXES "api-ms-win-.*" "ext-ms-win-.*"
                        "clang_rt.asan_dynamic-.*" "salts_utils\\.dll"
    POST_EXCLUDE_REGEXES ".*[\\/]system32[\\/].*"
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
  install(
    RUNTIME_DEPENDENCY_SET OrmRuntimeDependencies
    DIRECTORIES ${ORM_RUNTIME_SEARCH_DIRECTORIES}
    PRE_EXCLUDE_REGEXES "libsalts_.*"
    POST_EXCLUDE_REGEXES "^/lib/.*" "^/usr/lib/.*"
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

if(ORM_WITH_PGSQL)
  set(ORM_POSTGRESQL_RUNTIME_SEARCH_DIRECTORIES)
  orm_collect_runtime_search_directories(
    PostgreSQL::PostgreSQL ORM_POSTGRESQL_RUNTIME_SEARCH_DIRECTORIES)
  if(WIN32 AND PostgreSQL_LIBRARY)
    get_filename_component(ORM_POSTGRESQL_LIBRARY_DIR "${PostgreSQL_LIBRARY}"
                           DIRECTORY)
    get_filename_component(ORM_POSTGRESQL_PREFIX_DIR
                           "${ORM_POSTGRESQL_LIBRARY_DIR}" DIRECTORY)
    list(APPEND ORM_POSTGRESQL_RUNTIME_SEARCH_DIRECTORIES
         "${ORM_POSTGRESQL_PREFIX_DIR}/bin")
  endif()
  list(REMOVE_DUPLICATES ORM_POSTGRESQL_RUNTIME_SEARCH_DIRECTORIES)
  if(WIN32)
    install(
      RUNTIME_DEPENDENCY_SET OrmPostgreSQLRuntimeDependencies
      DIRECTORIES ${ORM_POSTGRESQL_RUNTIME_SEARCH_DIRECTORIES}
      PRE_EXCLUDE_REGEXES "api-ms-win-.*" "ext-ms-win-.*"
                          "clang_rt.asan_dynamic-.*" "turbo_orm\\.dll"
                          "salts_utils\\.dll"
      POST_EXCLUDE_REGEXES ".*[\\/]system32[\\/].*"
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  else()
    install(
      RUNTIME_DEPENDENCY_SET OrmPostgreSQLRuntimeDependencies
      DIRECTORIES ${ORM_POSTGRESQL_RUNTIME_SEARCH_DIRECTORIES}
      PRE_EXCLUDE_REGEXES "libturbo_orm\\..*" "libsalts_.*"
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

orm_enable_strict_warnings(turbo_orm)
if(ORM_WITH_PGSQL)
  orm_enable_strict_warnings(orm_postgresql)
endif()

set(ORM_INTERNAL_TEST_LIBRARY turbo_orm)
if(ORM_BUILD_TESTS)
  add_library(orm_c_test_static STATIC ${ORM_C_SOURCES} ${ORM_FLOW_SOURCES})
  target_compile_features(orm_c_test_static PRIVATE c_std_11)
  target_compile_definitions(orm_c_test_static PRIVATE ORM_C_STATIC)
  target_include_directories(
    orm_c_test_static
    PUBLIC ${ORM_INCLUDE_DIR}
    PRIVATE ${ORM_ABI_SOURCE_DIR} ${ORM_FLOW_SOURCE_DIR} ${ORM_SQL_SOURCE_DIR})
  target_link_libraries(
    orm_c_test_static PUBLIC Salts::Core
    PRIVATE Salts::CSerde Salts::CBind Salts::CFlow
            Salts::CSTL Salts::Platform)
  if(ORM_WITH_SQLITE)
    target_compile_definitions(orm_c_test_static PRIVATE ORM_WITH_SQLITE)
    target_link_libraries(orm_c_test_static PRIVATE SQLite::SQLite3)
  endif()
  if(ORM_WITH_REDIS)
    target_compile_definitions(orm_c_test_static PRIVATE ORM_WITH_REDIS)
    target_link_libraries(orm_c_test_static PRIVATE TurboDB::Redis)
  endif()
  target_compile_definitions(orm_c_test_static PRIVATE ORM_WITH_TIDESDB)
  target_link_libraries(orm_c_test_static PRIVATE TidesDB::tidesdb)
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
set(ORM_TEST_PRIVATE_LIBS Salts::Core)

add_test(
  NAME orm_shared_package_contract
  COMMAND
    ${CMAKE_COMMAND}
    "-DORM_PACKAGE_TEST_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/package/shared"
    "-DORM_PACKAGE_TEST_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/package"
    "-DORM_PACKAGE_TEST_BUILD_DIR=${CMAKE_BINARY_DIR}"
    "-DORM_PACKAGE_TEST_INSTALL_SCRIPT=${CMAKE_CURRENT_BINARY_DIR}/cmake_install.cmake"
    "-DORM_PACKAGE_TEST_SALTS_ROOT=$ENV{SALTS_ROOT}"
    "-DORM_PACKAGE_TEST_VCPKG_INSTALLED_DIR=${VCPKG_INSTALLED_DIR}"
    "-DORM_PACKAGE_TEST_EXPECTED_RUNTIME_DIR=${ORM_PACKAGE_TEST_EXPECTED_RUNTIME_DIR}"
    "-DORM_PACKAGE_TEST_EXPECT_SQLITE=${ORM_WITH_SQLITE}"
    "-DORM_PACKAGE_TEST_EXPECT_POSTGRESQL=${ORM_WITH_PGSQL}"
    "-DORM_PACKAGE_TEST_CONFIG=$<CONFIG>"
    "-DORM_PACKAGE_TEST_GENERATOR=${CMAKE_GENERATOR}"
    "-DORM_PACKAGE_TEST_C_COMPILER=${CMAKE_C_COMPILER}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/package/run_package_contract.cmake")

function(orm_link_internal_test TARGET_NAME)
  target_link_libraries(${TARGET_NAME} PRIVATE ${ORM_INTERNAL_TEST_LIBRARY}
                                               ${ARGN})
  # The static test clone expands backend archives into the executable, so its
  # final link must close the same C++ runtime dependencies as turbo_orm.
  set_property(TARGET ${TARGET_NAME} PROPERTY LINKER_LANGUAGE CXX)
endfunction()

if(ORM_WITH_SQLITE)
  list(APPEND ORM_TEST_PRIVATE_LIBS SQLite::SQLite3 Salts::CSerde
                                    Salts::CFlow)
endif()
if(ORM_WITH_REDIS)
  list(APPEND ORM_TEST_PRIVATE_LIBS TurboDB::Redis)
endif()
list(APPEND ORM_TEST_PRIVATE_LIBS TidesDB::tidesdb)
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
list(APPEND ORM_TEST_DEFINITIONS ORM_WITH_TIDESDB)
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
orm_link_internal_test(orm_mongo_query_test Salts::TinyTest
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
target_link_libraries(orm_postgres_libpq_test PRIVATE Salts::Core
                                                  Salts::TinyTest)
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

add_executable(orm_postgresql_component_test
               tests/flow/orm_postgresql_component_test.c)
target_compile_features(orm_postgresql_component_test PRIVATE c_std_11)
target_link_libraries(orm_postgresql_component_test PRIVATE orm_postgresql
                                                            Salts::TinyTest)
set_target_properties(orm_postgresql_component_test
                      PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_postgresql_component_test)
if(MSVC)
  target_compile_options(orm_postgresql_component_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_postgresql_component COMMAND orm_postgresql_component_test)

if(ORM_POSTGRES_LIVE_TESTS)
  add_executable(orm_postgres_live_test tests/integration/postgres_live_test.c)
  target_compile_features(orm_postgres_live_test PRIVATE c_std_11)
  target_link_libraries(orm_postgres_live_test PRIVATE orm_postgresql
                                                         Salts::TinyTest)
  set_target_properties(orm_postgres_live_test
                        PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
  orm_enable_strict_warnings(orm_postgres_live_test)
  if(MSVC)
    target_compile_options(orm_postgres_live_test
                           PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
  endif()
  add_test(NAME orm_postgres_live COMMAND orm_postgres_live_test)
endif()

endif()

add_executable(orm_cbind_publisher_test tests/flow/orm_cbind_publisher_test.c)
target_compile_features(orm_cbind_publisher_test PRIVATE c_std_11)
target_compile_definitions(orm_cbind_publisher_test PRIVATE ORM_C_STATIC)
target_include_directories(
  orm_cbind_publisher_test PRIVATE ${ORM_INCLUDE_DIR}
                                 ${ORM_FLOW_SOURCE_DIR})
orm_link_internal_test(orm_cbind_publisher_test Salts::CBind
                       Salts::CFlow Salts::TinyTest)
set_target_properties(orm_cbind_publisher_test PROPERTIES C_STANDARD_REQUIRED ON
                                                        FOLDER "orm/tests")
orm_enable_strict_warnings(orm_cbind_publisher_test)
if(MSVC)
  target_compile_options(orm_cbind_publisher_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_cbind_publisher COMMAND orm_cbind_publisher_test)

add_executable(orm_pure_c_build_test tests/flow/orm_pure_c_build_test.c)
target_compile_features(orm_pure_c_build_test PRIVATE c_std_11)
target_compile_definitions(orm_pure_c_build_test PRIVATE ORM_C_STATIC)
target_include_directories(orm_pure_c_build_test PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(orm_pure_c_build_test PRIVATE turbo_orm
                                                    Salts::TinyTest)
set_target_properties(orm_pure_c_build_test
                      PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_pure_c_build_test)
if(MSVC)
  target_compile_options(orm_pure_c_build_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_pure_c_build COMMAND orm_pure_c_build_test)

add_executable(orm_backend_profile_test tests/abi/backend_profile_test.c)
target_compile_features(orm_backend_profile_test PRIVATE c_std_11)
target_compile_definitions(
  orm_backend_profile_test
  PRIVATE ORM_C_STATIC ORM_EXPECT_SQLITE=$<BOOL:${ORM_WITH_SQLITE}>)
orm_link_internal_test(orm_backend_profile_test Salts::TinyTest)
set_target_properties(orm_backend_profile_test
                      PROPERTIES FOLDER "orm/tests")
orm_enable_strict_warnings(orm_backend_profile_test)
if(MSVC)
  target_compile_options(orm_backend_profile_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_backend_profile COMMAND orm_backend_profile_test)

add_executable(orm_sql_render_test tests/flow/orm_sql_render_test.c)
target_compile_features(orm_sql_render_test PRIVATE c_std_11)
target_compile_definitions(orm_sql_render_test PRIVATE ORM_C_STATIC)
target_include_directories(
  orm_sql_render_test PRIVATE ${ORM_INCLUDE_DIR} ${ORM_ABI_SOURCE_DIR}
                              ${ORM_FLOW_SOURCE_DIR}
                              ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/sql)
orm_link_internal_test(orm_sql_render_test Salts::TinyTest)
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
orm_link_internal_test(orm_postgres_flow_test Salts::CFlow
                       Salts::TinyTest)
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
orm_link_internal_test(orm_redis_flow_test Salts::CBind
                       Salts::CFlow Salts::TinyTest)
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
orm_link_internal_test(orm_mongo_flow_test Salts::CBind
                       Salts::CFlow Salts::TinyTest)
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
orm_link_internal_test(orm_tidesdb_row_test Salts::TinyTest)
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
orm_link_internal_test(orm_tidesdb_flow_test Salts::CBind
                       Salts::CFlow Salts::TinyTest)
set_target_properties(orm_tidesdb_flow_test
                      PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_tidesdb_flow_test)
if(MSVC)
  target_compile_options(orm_tidesdb_flow_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_tidesdb_flow COMMAND orm_tidesdb_flow_test)

add_executable(orm_tidesdb_public_flow_test
               tests/integration/tidesdb_public_flow_test.c)
target_compile_features(orm_tidesdb_public_flow_test PRIVATE c_std_11)
target_compile_definitions(orm_tidesdb_public_flow_test PRIVATE ORM_C_STATIC)
target_include_directories(orm_tidesdb_public_flow_test PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(
  orm_tidesdb_public_flow_test PRIVATE turbo_orm Salts::CBind
                                       Salts::CFlow Salts::TinyTest)
set_target_properties(orm_tidesdb_public_flow_test
                      PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_tidesdb_public_flow_test)
if(MSVC)
  target_compile_options(orm_tidesdb_public_flow_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_tidesdb_public_flow COMMAND orm_tidesdb_public_flow_test)

if(ORM_WITH_SQLITE)
add_executable(orm_sqlite_flow_test tests/flow/orm_sqlite_flow_test.c)
target_compile_features(orm_sqlite_flow_test PRIVATE c_std_11)
target_compile_definitions(orm_sqlite_flow_test PRIVATE ORM_C_STATIC)
target_include_directories(
  orm_sqlite_flow_test PRIVATE ${ORM_INCLUDE_DIR}
                               ${ORM_FLOW_SOURCE_DIR}
                               ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/sqlite)
orm_link_internal_test(orm_sqlite_flow_test SQLite::SQLite3 Salts::CBind
                       Salts::CFlow Salts::TinyTest)
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
target_link_libraries(orm_public_flow_test PRIVATE turbo_orm Salts::CBind
                                                   Salts::CFlow
                                                   Salts::TinyTest)
set_target_properties(orm_public_flow_test
                      PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_public_flow_test)
if(MSVC)
  target_compile_options(orm_public_flow_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_public_flow COMMAND orm_public_flow_test)

add_executable(orm_sqlite_provider_atomicity_test
               tests/integration/sqlite_provider_atomicity_test.c)
target_compile_features(orm_sqlite_provider_atomicity_test PRIVATE c_std_11)
target_compile_definitions(orm_sqlite_provider_atomicity_test PRIVATE ORM_C_STATIC)
target_include_directories(orm_sqlite_provider_atomicity_test PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(orm_sqlite_provider_atomicity_test
                      PRIVATE turbo_orm Salts::TinyTest)
set_target_properties(
  orm_sqlite_provider_atomicity_test
  PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_sqlite_provider_atomicity_test)
if(MSVC)
  target_compile_options(orm_sqlite_provider_atomicity_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_sqlite_provider_atomicity
         COMMAND orm_sqlite_provider_atomicity_test)

add_executable(orm_sqlite_provider_checkpoint_test
               tests/integration/sqlite_provider_checkpoint_test.c)
target_compile_features(orm_sqlite_provider_checkpoint_test PRIVATE c_std_11)
target_compile_definitions(orm_sqlite_provider_checkpoint_test PRIVATE ORM_C_STATIC)
target_include_directories(orm_sqlite_provider_checkpoint_test PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(orm_sqlite_provider_checkpoint_test
                      PRIVATE turbo_orm Salts::TinyTest)
set_target_properties(
  orm_sqlite_provider_checkpoint_test
  PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_sqlite_provider_checkpoint_test)
if(MSVC)
  target_compile_options(orm_sqlite_provider_checkpoint_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_sqlite_provider_checkpoint
         COMMAND orm_sqlite_provider_checkpoint_test)

add_executable(orm_cpp_flow_test tests/flow/orm_cpp_flow_test.cpp)
target_compile_features(orm_cpp_flow_test PRIVATE cxx_std_17)
target_compile_definitions(orm_cpp_flow_test PRIVATE ORM_C_STATIC)
target_include_directories(orm_cpp_flow_test PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(orm_cpp_flow_test PRIVATE turbo_orm Salts::CBind
                                                Salts::CFlow
                                                Salts::TinyTest)
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
orm_link_internal_test(orm_redis_query_test Salts::TinyTest)
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
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::Core>")
  set(ORM_RUNTIME_TESTS orm_redis_flow orm_mongo_flow orm_tidesdb_flow
                        orm_cbind_publisher orm_pure_c_build orm_sql_render
                        orm_postgres_flow orm_backend_profile)
  if(ORM_WITH_SQLITE)
    string(APPEND ORM_TEST_RUNTIME_PATH
           ";PATH=path_list_prepend:$<TARGET_FILE_DIR:SQLite::SQLite3>")
    list(APPEND ORM_RUNTIME_TESTS orm_sqlite_flow orm_public_flow\n                                  orm_sqlite_provider_atomicity orm_cpp_flow)
  endif()
  if(ORM_WITH_PGSQL)
    string(APPEND ORM_TEST_RUNTIME_PATH
           ";PATH=path_list_prepend:$<TARGET_FILE_DIR:PostgreSQL::PostgreSQL>")
    list(APPEND ORM_RUNTIME_TESTS orm_postgres_libpq)
    if(ORM_POSTGRES_LIVE_TESTS)
      list(APPEND ORM_RUNTIME_TESTS orm_postgres_live)
    endif()
  endif()
  if(ORM_WITH_REDIS)
    string(
      APPEND
      ORM_TEST_RUNTIME_PATH
      ";PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboDB::Redis>"
    )
    list(APPEND ORM_RUNTIME_TESTS orm_redis_query orm_redis_backend_contract)
  endif()
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
if(ORM_WITH_SQLITE)
  cmake_install_headers(
    FILES include/orm/orm_sqlite.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/shared/orm)
endif()
if(ORM_WITH_PGSQL)
  cmake_install_headers(
    FILES include/orm/orm_postgresql.h include/orm/orm_postgresql.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/shared/orm)
endif()

cmake_config_package(PACKAGE Orm VERSION "${ORM_VERSION}")

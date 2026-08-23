cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(
    Orm
    VERSION 2.1.0
    DESCRIPTION "QueryDSL-inspired ORM for C11 and C++17"
    LANGUAGES C CXX)
  set(ORM_BUILD_TESTS_DEFAULT ON)
else()
  set(ORM_BUILD_TESTS_DEFAULT OFF)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
  include(CmakeUtils)
endif()

option(ORM_BUILD_TESTS "Build the Orm tests" ${ORM_BUILD_TESTS_DEFAULT})
option(ORM_BUILD_EXAMPLES "Build the Orm usage examples"
       ${ORM_BUILD_TESTS_DEFAULT})
option(ORM_INSTALL_EXAMPLES "Install Orm example source code" OFF)
option(ORM_BUILD_SCHEMA_TOOLS "Build ORM schema validation tools"
       ${ORM_BUILD_TESTS_DEFAULT})

# Keep the installed TurboDB::ORM contract identical whether ORM is built on
# its own or as a TurboDB subdirectory. Optional backends remain opt-in so a
# static ORM consumer does not inherit unrelated backend libraries.
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
if(NOT DEFINED ORM_WITH_TIDESDB_DEFAULT)
  set(ORM_WITH_TIDESDB_DEFAULT OFF)
endif()

option(ORM_WITH_SQLITE "Enable SQLite backend" ${ORM_WITH_SQLITE_DEFAULT})
option(ORM_WITH_PGSQL "Enable PostgreSQL backend" ${ORM_WITH_PGSQL_DEFAULT})
option(ORM_WITH_REDIS "Enable Redis backend" ${ORM_WITH_REDIS_DEFAULT})
option(ORM_WITH_MONGODB "Enable MongoDB backend" ${ORM_WITH_MONGODB_DEFAULT})
option(ORM_WITH_TIDESDB "Enable TidesDB embedded backend" ${ORM_WITH_TIDESDB_DEFAULT})

# Find required base dependencies
find_package(TurboUtils CONFIG REQUIRED)
find_package(TurboParser CONFIG REQUIRED)

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

if(ORM_WITH_REDIS AND WIN32 AND NOT TARGET TurboNet::CoroNet)
  find_package(TurboNet CONFIG REQUIRED)
endif()

set(ORM_VERSION 2.1.0)
set(ORM_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include/orm)
set(ORM_DB_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include/orm/dbs/postgres)
set(ORM_ABI_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/abi)
set(ORM_ABI_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/abi)
set(ORM_TEST_SUPPORT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)

set(ORM_C_SOURCES src/abi/orm_c.cpp)
if(ORM_WITH_PGSQL)
  list(APPEND ORM_C_SOURCES src/dbs/postgres/backend.cpp)
endif()
if(ORM_WITH_SQLITE)
  list(APPEND ORM_C_SOURCES src/dbs/sqlite/backend.cpp)
endif()
if(ORM_WITH_REDIS)
  list(APPEND ORM_C_SOURCES src/dbs/redis/backend.cpp
                             src/dbs/redis/query.cpp)
endif()
if(ORM_WITH_TIDESDB)
  list(APPEND ORM_C_SOURCES src/dbs/tidesdb/bridge.c
                             src/dbs/tidesdb/backend.cpp
                             src/dbs/tidesdb/row.cpp)
endif()
if(ORM_WITH_MONGODB)
  list(APPEND ORM_C_SOURCES src/dbs/mongo/backend.cpp
                             src/dbs/mongo/query.cpp)
endif()

add_library(orm_c ${ORM_C_SOURCES})
add_library(orm_cpp INTERFACE)
add_library(TurboDB::ORM ALIAS orm_c)
target_compile_features(orm_c PRIVATE cxx_std_17)
target_compile_definitions(orm_c PRIVATE ORM_C_BUILD)

target_compile_definitions(orm_c PUBLIC ORM_C_STATIC)

target_include_directories(
  orm_c
  PUBLIC $<BUILD_INTERFACE:${ORM_INCLUDE_DIR}>
         $<INSTALL_INTERFACE:include/shared/orm>
  PRIVATE ${ORM_ABI_SOURCE_DIR} ${ORM_DB_INCLUDE_DIR})
target_link_libraries(orm_c PUBLIC TurboUtils::Core PRIVATE TurboParser::Mustache)

if(ORM_WITH_SQLITE)
  target_compile_definitions(orm_c PRIVATE ORM_WITH_SQLITE)
  target_link_libraries(orm_c PRIVATE SQLite::SQLite3)
endif()

if(ORM_WITH_PGSQL)
  target_compile_definitions(orm_c PRIVATE ORM_WITH_PGSQL)
  target_link_libraries(orm_c PRIVATE PostgreSQL::PostgreSQL)
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
  PROPERTIES CXX_STANDARD_REQUIRED ON
             C_STANDARD_REQUIRED ON
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(orm_cpp INTERFACE cxx_std_17)
target_include_directories(
  orm_cpp INTERFACE $<BUILD_INTERFACE:${ORM_INCLUDE_DIR}>
                    $<INSTALL_INTERFACE:include/shared/orm>)
target_link_libraries(orm_cpp INTERFACE orm_c)

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

if(MSVC)
  set(ORM_MSVC_WARNING_OPTIONS /W4 /WX /permissive-)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    list(APPEND ORM_MSVC_WARNING_OPTIONS /Zc:preprocessor)
  endif()
endif()

function(orm_enable_strict_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE ${ORM_MSVC_WARNING_OPTIONS})
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endfunction()

orm_enable_strict_warnings(orm_c)

if(ORM_BUILD_TESTS)
  enable_testing()
endif()

if(ORM_BUILD_SCHEMA_TOOLS)
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/OrmSchemaValidation.cmake)

  set(ORM_CPP_METADATA_TEMPLATE_PATH
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/templates/orm_cpp_metadata.mustache)
  set(ORM_C_FACADE_TEMPLATE_PATH
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/templates/orm_c_facade.mustache)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                                         ${ORM_CPP_METADATA_TEMPLATE_PATH}
                                         ${ORM_C_FACADE_TEMPLATE_PATH})
  file(READ ${ORM_CPP_METADATA_TEMPLATE_PATH} ORM_CPP_METADATA_TEMPLATE)
  file(READ ${ORM_C_FACADE_TEMPLATE_PATH} ORM_C_FACADE_TEMPLATE)
  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/orm_schema_template.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/orm_schema_template.hpp @ONLY)

  add_library(orm_schema_validator STATIC tools/orm_schema_model.cpp
                                          tools/orm_schema_validator.cpp)
  target_compile_features(orm_schema_validator PRIVATE cxx_std_17)
  target_include_directories(orm_schema_validator
                             PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tools)
  target_link_libraries(orm_schema_validator PUBLIC TurboParser::TbeSchema)
  set_target_properties(orm_schema_validator PROPERTIES CXX_STANDARD_REQUIRED ON
                                                        FOLDER "orm/tools")
  orm_enable_strict_warnings(orm_schema_validator)

  add_library(orm_schema_generator STATIC tools/orm_schema_generator.cpp)
  target_compile_features(orm_schema_generator PRIVATE cxx_std_17)
  target_include_directories(orm_schema_generator
                             PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tools
                             PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
  target_link_libraries(orm_schema_generator PUBLIC orm_schema_validator
                                             PRIVATE TurboParser::Mustache)
  set_target_properties(orm_schema_generator PROPERTIES CXX_STANDARD_REQUIRED ON
                                                        FOLDER "orm/tools")
  orm_enable_strict_warnings(orm_schema_generator)

  add_executable(orm_schema_validate tools/orm_schema_validate_main.cpp)
  target_compile_features(orm_schema_validate PRIVATE cxx_std_17)
  target_link_libraries(orm_schema_validate PRIVATE orm_schema_validator
                                                     TurboParser::TbeSchema
                                                     TurboUtils::Core)
  set_target_properties(orm_schema_validate PROPERTIES CXX_STANDARD_REQUIRED ON
                                                        FOLDER "orm/tools")
  orm_enable_strict_warnings(orm_schema_validate)
  if(WIN32)
    add_custom_command(
      TARGET orm_schema_validate
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "$<TARGET_FILE:TurboUtils::Core>"
              "$<TARGET_FILE_DIR:orm_schema_validate>")
  endif()

  add_executable(orm_schema_generate tools/orm_schema_generate_main.cpp)
  target_compile_features(orm_schema_generate PRIVATE cxx_std_17)
  target_link_libraries(orm_schema_generate PRIVATE orm_schema_generator
                                                     TurboParser::TbeSchema
                                                     TurboUtils::Core)
  set_target_properties(orm_schema_generate PROPERTIES CXX_STANDARD_REQUIRED ON
                                                        FOLDER "orm/tools")
  orm_enable_strict_warnings(orm_schema_generate)
  if(WIN32)
    add_custom_command(
      TARGET orm_schema_generate
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "$<TARGET_FILE:TurboUtils::Core>"
              "$<TARGET_FILE:TurboParser::Mustache>"
              "$<TARGET_FILE_DIR:orm_schema_generate>")
  endif()

  if(ORM_BUILD_TESTS)
    add_executable(orm_schema_validator_test
                   tests/abi/orm_schema_validator_test.cpp)
    target_compile_features(orm_schema_validator_test PRIVATE cxx_std_17)
    target_link_libraries(orm_schema_validator_test PRIVATE orm_schema_generator
                                                              TurboParser::TbeSchema
                                                              TurboUtils::TinyTest)
    set_target_properties(orm_schema_validator_test
                          PROPERTIES CXX_STANDARD_REQUIRED ON FOLDER "orm/tests")
    orm_enable_strict_warnings(orm_schema_validator_test)
    add_test(NAME orm_schema_validator COMMAND orm_schema_validator_test)

    orm_add_schema_validation(
      orm_schema_validation_invalid_fixture
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/schema/invalid_validator_fixture.schema)
    add_test(
      NAME orm_schema_validation_rejects_invalid
      COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target
              orm_schema_validation_invalid_fixture)
    set_tests_properties(orm_schema_validation_rejects_invalid
                         PROPERTIES WILL_FAIL TRUE)

    set(ORM_SCHEMA_GENERATED_TEST_HEADER
        ${CMAKE_CURRENT_BINARY_DIR}/orm_schema_generated/validator_fixture.orm.hpp)
    orm_add_schema_generation(
      ${ORM_SCHEMA_GENERATED_TEST_HEADER}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/schema/validator_fixture.schema)
    add_executable(
      orm_schema_generated_metadata_test
      tests/abi/orm_schema_generated_metadata_test.cpp
      ${ORM_SCHEMA_GENERATED_TEST_HEADER})
    target_compile_features(orm_schema_generated_metadata_test PRIVATE cxx_std_17)
    target_include_directories(orm_schema_generated_metadata_test
                               PRIVATE ${ORM_INCLUDE_DIR}
                                       ${CMAKE_CURRENT_BINARY_DIR}/orm_schema_generated)
    target_link_libraries(orm_schema_generated_metadata_test PRIVATE orm_cpp
                                                                      TurboUtils::TinyTest)
    set_target_properties(orm_schema_generated_metadata_test
                          PROPERTIES CXX_STANDARD_REQUIRED ON FOLDER "orm/tests")
    orm_enable_strict_warnings(orm_schema_generated_metadata_test)
    add_test(NAME orm_schema_generated_metadata
             COMMAND orm_schema_generated_metadata_test)

    if(ORM_WITH_SQLITE)
      set(ORM_SCHEMA_GENERATED_C_DIRECTORY
          ${CMAKE_CURRENT_BINARY_DIR}/orm_schema_generated_c)
      set(ORM_SCHEMA_GENERATED_C_FACADE
          ${ORM_SCHEMA_GENERATED_C_DIRECTORY}/c_facade_fixture.orm.h)
      set(ORM_SCHEMA_GENERATED_C_MODEL_HEADER
          ${ORM_SCHEMA_GENERATED_C_DIRECTORY}/c_facade_fixture.tbe.h)
      set(ORM_SCHEMA_GENERATED_C_MODEL_SOURCE
          ${ORM_SCHEMA_GENERATED_C_DIRECTORY}/c_facade_fixture.tbe.c)
      orm_add_c_schema_generation(
        ${ORM_SCHEMA_GENERATED_C_FACADE}
        ${ORM_SCHEMA_GENERATED_C_MODEL_HEADER}
        ${ORM_SCHEMA_GENERATED_C_MODEL_SOURCE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/schema/c_facade_fixture.schema)
      add_executable(
        orm_schema_generated_c_test
        tests/abi/orm_schema_generated_c_test.c
        ${ORM_SCHEMA_GENERATED_C_MODEL_SOURCE}
        ${ORM_SCHEMA_GENERATED_C_MODEL_HEADER}
        ${ORM_SCHEMA_GENERATED_C_FACADE})
      target_compile_features(orm_schema_generated_c_test PRIVATE c_std_11)
      target_include_directories(
        orm_schema_generated_c_test
        PRIVATE ${ORM_INCLUDE_DIR} ${ORM_SCHEMA_GENERATED_C_DIRECTORY})
      target_link_libraries(
        orm_schema_generated_c_test
        PRIVATE orm_c TurboParser::DataBind TurboUtils::TinyTest)
      set_target_properties(
        orm_schema_generated_c_test
        PROPERTIES C_STANDARD_REQUIRED ON LINKER_LANGUAGE CXX FOLDER "orm/tests")
      orm_enable_strict_warnings(orm_schema_generated_c_test)
      if(MSVC)
        target_compile_options(
          orm_schema_generated_c_test PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
      endif()
      add_test(NAME orm_schema_generated_c COMMAND orm_schema_generated_c_test)
    endif()
  endif()

  orm_add_schema_validation(
    orm_schema_validation_fixture
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/schema/validator_fixture.schema)
endif()

if(ORM_BUILD_TESTS)
set(ORM_TEST_PRIVATE_LIBS TurboUtils::Core TurboParser::Mustache)
if(ORM_WITH_SQLITE)
  list(APPEND ORM_TEST_PRIVATE_LIBS SQLite::SQLite3)
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
if(ORM_WITH_PGSQL)
  list(APPEND ORM_TEST_DEFINITIONS ORM_WITH_PGSQL)
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

if(ORM_WITH_PGSQL)
add_executable(
  orm_postgres_c_api_test tests/abi/c_api_test.c tests/abi/c_api_fake_libpq.cpp
                          ${ORM_C_SOURCES})
target_compile_features(orm_postgres_c_api_test PRIVATE c_std_11 cxx_std_17)
target_compile_definitions(orm_postgres_c_api_test
                           PRIVATE ${ORM_TEST_DEFINITIONS})
target_include_directories(
  orm_postgres_c_api_test
  PRIVATE ${ORM_INCLUDE_DIR} ${ORM_DB_INCLUDE_DIR} ${ORM_ABI_SOURCE_DIR}
          ${ORM_ABI_TEST_DIR} ${ORM_TEST_SUPPORT_DIR} ${PostgreSQL_INCLUDE_DIRS})
target_link_libraries(orm_postgres_c_api_test PRIVATE ${ORM_TEST_PRIVATE_LIBS})
set_target_properties(
  orm_postgres_c_api_test PROPERTIES CXX_STANDARD_REQUIRED ON
                                     C_STANDARD_REQUIRED ON
                                     FOLDER "orm/tests")
orm_enable_strict_warnings(orm_postgres_c_api_test)
if(MSVC)
  target_compile_options(orm_postgres_c_api_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_postgres_c_api COMMAND orm_postgres_c_api_test)
endif()

if(ORM_WITH_SQLITE)
add_executable(
  orm_sqlite_c_api_test tests/abi/sqlite_api_test.c
                        tests/abi/c_api_fake_libpq.cpp ${ORM_C_SOURCES})
target_compile_features(orm_sqlite_c_api_test PRIVATE c_std_11 cxx_std_17)
target_compile_definitions(orm_sqlite_c_api_test
                           PRIVATE ${ORM_TEST_DEFINITIONS})
target_include_directories(
  orm_sqlite_c_api_test
  PRIVATE ${ORM_INCLUDE_DIR} ${ORM_DB_INCLUDE_DIR} ${ORM_ABI_SOURCE_DIR}
          ${ORM_ABI_TEST_DIR} ${ORM_TEST_SUPPORT_DIR} ${PostgreSQL_INCLUDE_DIRS})
target_link_libraries(orm_sqlite_c_api_test PRIVATE ${ORM_TEST_PRIVATE_LIBS})
set_target_properties(orm_sqlite_c_api_test PROPERTIES CXX_STANDARD_REQUIRED ON
                                                       C_STANDARD_REQUIRED ON
                                                       FOLDER "orm/tests")
orm_enable_strict_warnings(orm_sqlite_c_api_test)
if(MSVC)
  target_compile_options(orm_sqlite_c_api_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_sqlite_c_api COMMAND orm_sqlite_c_api_test)

add_executable(orm_cpp_api_test tests/abi/cpp_api_test.cpp src/abi/orm_c.cpp
                                src/dbs/sqlite/backend.cpp)
target_compile_features(orm_cpp_api_test PRIVATE cxx_std_17)
target_compile_definitions(orm_cpp_api_test PRIVATE ORM_WITH_SQLITE)
target_include_directories(
  orm_cpp_api_test
  PRIVATE ${ORM_INCLUDE_DIR} ${ORM_ABI_SOURCE_DIR})
target_link_libraries(
  orm_cpp_api_test
  PRIVATE TurboUtils::Core TurboUtils::TinyTest TurboParser::Mustache
          SQLite::SQLite3)
set_target_properties(orm_cpp_api_test PROPERTIES CXX_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_cpp_api_test)
add_test(NAME orm_cpp_api COMMAND orm_cpp_api_test)

add_executable(orm_jpa_api_test tests/abi/jpa_api_test.cpp src/abi/orm_c.cpp
                                src/dbs/sqlite/backend.cpp)
target_compile_features(orm_jpa_api_test PRIVATE cxx_std_17)
target_compile_definitions(orm_jpa_api_test PRIVATE ORM_WITH_SQLITE)
target_include_directories(
  orm_jpa_api_test
  PRIVATE ${ORM_INCLUDE_DIR} ${ORM_ABI_SOURCE_DIR})
target_link_libraries(
  orm_jpa_api_test
  PRIVATE TurboUtils::Core TurboParser::Mustache TurboUtils::TinyTest
          SQLite::SQLite3)
set_target_properties(orm_jpa_api_test PROPERTIES CXX_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_jpa_api_test)
add_test(NAME orm_jpa_api COMMAND orm_jpa_api_test)

add_executable(orm_typed_fetch_test tests/abi/typed_fetch_test.cpp
                                    tests/abi/c_api_fake_libpq.cpp ${ORM_C_SOURCES})
target_compile_features(orm_typed_fetch_test PRIVATE cxx_std_17)
target_compile_definitions(orm_typed_fetch_test PRIVATE ${ORM_TEST_DEFINITIONS})
target_include_directories(
  orm_typed_fetch_test
  PRIVATE ${ORM_INCLUDE_DIR} ${ORM_DB_INCLUDE_DIR} ${ORM_ABI_SOURCE_DIR}
          ${ORM_ABI_TEST_DIR} ${ORM_TEST_SUPPORT_DIR} ${PostgreSQL_INCLUDE_DIRS})
target_link_libraries(orm_typed_fetch_test PRIVATE ${ORM_TEST_PRIVATE_LIBS}
                                                   TurboUtils::TinyTest)
set_target_properties(orm_typed_fetch_test PROPERTIES CXX_STANDARD_REQUIRED ON
                                                       FOLDER "orm/tests")
orm_enable_strict_warnings(orm_typed_fetch_test)
add_test(NAME orm_typed_fetch COMMAND orm_typed_fetch_test)
endif()

if(ORM_WITH_PGSQL)
add_executable(orm_cpp17_test tests/dbs/postgres/test.cpp)
target_compile_features(orm_cpp17_test PRIVATE cxx_std_17)
target_include_directories(orm_cpp17_test PRIVATE ${ORM_DB_INCLUDE_DIR}
                                                  ${ORM_TEST_SUPPORT_DIR}
                                                  ${PostgreSQL_INCLUDE_DIRS})
set_target_properties(orm_cpp17_test PROPERTIES CXX_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_cpp17_test)
add_test(NAME orm_cpp17 COMMAND orm_cpp17_test)
endif()

add_executable(orm_redis_query_test tests/abi/redis_query_test.cpp
                                    src/dbs/redis/query.cpp)
target_compile_features(orm_redis_query_test PRIVATE cxx_std_17)
target_include_directories(
  orm_redis_query_test PRIVATE ${ORM_INCLUDE_DIR} ${ORM_ABI_SOURCE_DIR}
                               ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/redis)
target_link_libraries(orm_redis_query_test PRIVATE TurboUtils::Core
                                                   TurboUtils::TinyTest
                                                   TurboParser::Mustache)
set_target_properties(orm_redis_query_test PROPERTIES CXX_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_redis_query_test)
add_test(NAME orm_redis_query COMMAND orm_redis_query_test)

add_executable(orm_tidesdb_row_test tests/abi/tidesdb_row_test.cpp
                                    src/dbs/tidesdb/row.cpp)
target_compile_features(orm_tidesdb_row_test PRIVATE cxx_std_17)
target_include_directories(
  orm_tidesdb_row_test PRIVATE ${ORM_INCLUDE_DIR} ${ORM_ABI_SOURCE_DIR}
                               ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/tidesdb)
target_link_libraries(orm_tidesdb_row_test PRIVATE TurboUtils::Core
                                                   TurboUtils::TinyTest)
set_target_properties(orm_tidesdb_row_test PROPERTIES CXX_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_tidesdb_row_test)
add_test(NAME orm_tidesdb_row COMMAND orm_tidesdb_row_test)

if(ORM_WITH_MONGODB)
  add_executable(orm_mongo_query_test tests/abi/mongo_query_test.cpp
                                      src/dbs/mongo/query.cpp)
  target_compile_features(orm_mongo_query_test PRIVATE cxx_std_17)
  target_compile_definitions(orm_mongo_query_test PRIVATE ORM_C_STATIC)
  target_include_directories(
    orm_mongo_query_test PRIVATE ${ORM_INCLUDE_DIR} ${ORM_ABI_SOURCE_DIR}
                                 ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/mongo)
  target_link_libraries(orm_mongo_query_test PRIVATE TurboUtils::Core
                                                     TurboUtils::TinyTest
                                                     mongo::mongoc_shared)
  set_target_properties(orm_mongo_query_test
                        PROPERTIES CXX_STANDARD_REQUIRED ON FOLDER "orm/tests")
  orm_enable_strict_warnings(orm_mongo_query_test)
  add_test(NAME orm_mongo_query COMMAND orm_mongo_query_test)

  add_executable(orm_mongo_live_bdd_test
                 tests/integration/mongo_live_bdd_test.cpp)
  target_compile_features(orm_mongo_live_bdd_test PRIVATE cxx_std_17)
  target_link_libraries(orm_mongo_live_bdd_test PRIVATE orm_c
                                                         mongo::mongoc_shared
                                                         TurboUtils::TinyTest)
  set_target_properties(orm_mongo_live_bdd_test
                        PROPERTIES CXX_STANDARD_REQUIRED ON FOLDER "orm/tests")
  orm_enable_strict_warnings(orm_mongo_live_bdd_test)
  add_test(NAME orm_mongo_live_bdd COMMAND orm_mongo_live_bdd_test)
  set_tests_properties(orm_mongo_live_bdd PROPERTIES LABELS "integration;mongo")
endif()

add_executable(orm_mustache_render_test tests/abi/mustache_render_test.cpp)
target_compile_features(orm_mustache_render_test PRIVATE cxx_std_17)
target_include_directories(
  orm_mustache_render_test PRIVATE ${ORM_INCLUDE_DIR} ${ORM_ABI_SOURCE_DIR})
target_link_libraries(orm_mustache_render_test PRIVATE TurboUtils::Core
                                                       TurboParser::Mustache)
set_target_properties(orm_mustache_render_test PROPERTIES CXX_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_mustache_render_test)
add_test(NAME orm_mustache_render COMMAND orm_mustache_render_test)

add_executable(orm_mock_orm_backend_test
               tests/abi/mock_orm_backend_test.cpp)
target_compile_features(orm_mock_orm_backend_test PRIVATE cxx_std_17)
target_include_directories(
  orm_mock_orm_backend_test
  PRIVATE ${ORM_INCLUDE_DIR} ${ORM_ABI_SOURCE_DIR} ${ORM_TEST_SUPPORT_DIR})
# tinymock.hpp (C++) is not part of the installed TurboUtils package yet, so it
# must come from the tinytest source tree.  TinyTest is still linked as the
# framework target for the suite runner and C tinymock.h.

target_include_directories(
   orm_mock_orm_backend_test
   PRIVATE ${ORM_INCLUDE_DIR} ${ORM_ABI_SOURCE_DIR} ${ORM_TEST_SUPPORT_DIR})
target_link_libraries(orm_mock_orm_backend_test
                      PRIVATE TurboUtils::Core TurboUtils::TinyTest)
target_link_libraries(orm_mock_orm_backend_test
                      PRIVATE TurboUtils::Core TurboUtils::TinyTest)
set_target_properties(orm_mock_orm_backend_test
                      PROPERTIES CXX_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_mock_orm_backend_test)
add_test(NAME orm_mock_orm_backend COMMAND orm_mock_orm_backend_test)

if(ORM_WITH_REDIS)
add_executable(
  orm_redis_transaction_test
  tests/abi/redis_transaction_test.cpp
  tests/abi/redis_transaction_fake.cpp
  tests/abi/c_api_fake_libpq.cpp
  src/abi/orm_c.cpp
  src/dbs/postgres/backend.cpp
  src/dbs/sqlite/backend.cpp
  src/dbs/redis/backend.cpp
  src/dbs/redis/query.cpp)
target_compile_features(orm_redis_transaction_test PRIVATE cxx_std_17)
target_compile_definitions(orm_redis_transaction_test PRIVATE ORM_C_STATIC
                                                              ORM_WITH_REDIS)
target_include_directories(
  orm_redis_transaction_test
  PRIVATE ${ORM_INCLUDE_DIR} ${ORM_DB_INCLUDE_DIR} ${ORM_ABI_SOURCE_DIR}
          ${ORM_ABI_TEST_DIR} ${ORM_TEST_SUPPORT_DIR} ${PostgreSQL_INCLUDE_DIRS})
target_include_directories(
  orm_redis_transaction_test SYSTEM
  PRIVATE $<TARGET_PROPERTY:TurboDB::Redis,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(
  orm_redis_transaction_test PRIVATE SQLite::SQLite3 TurboUtils::Core
                                     TurboParser::Mustache)
set_target_properties(orm_redis_transaction_test
                      PROPERTIES CXX_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_redis_transaction_test)
add_test(NAME orm_redis_transaction COMMAND orm_redis_transaction_test)

add_executable(
  orm_driver_error_mapping_test
  tests/abi/driver_error_mapping_test.cpp
  tests/abi/redis_transaction_fake.cpp
  tests/abi/c_api_fake_libpq.cpp
  src/abi/orm_c.cpp
  src/dbs/postgres/backend.cpp
  src/dbs/sqlite/backend.cpp
  src/dbs/redis/backend.cpp
  src/dbs/redis/query.cpp)
target_compile_features(orm_driver_error_mapping_test PRIVATE cxx_std_17)
target_compile_definitions(orm_driver_error_mapping_test PRIVATE ORM_C_STATIC
                                                                 ORM_WITH_REDIS)
target_include_directories(
  orm_driver_error_mapping_test
  PRIVATE ${ORM_INCLUDE_DIR} ${ORM_DB_INCLUDE_DIR} ${ORM_ABI_SOURCE_DIR}
          ${ORM_ABI_TEST_DIR} ${ORM_TEST_SUPPORT_DIR} ${PostgreSQL_INCLUDE_DIRS})
target_include_directories(
  orm_driver_error_mapping_test SYSTEM
  PRIVATE $<TARGET_PROPERTY:TurboDB::Redis,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(
  orm_driver_error_mapping_test PRIVATE SQLite::SQLite3 TurboUtils::Core
                                          TurboParser::Mustache)
set_target_properties(orm_driver_error_mapping_test
                      PROPERTIES CXX_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_driver_error_mapping_test)
add_test(NAME orm_driver_error_mapping COMMAND orm_driver_error_mapping_test)

add_executable(orm_redis_backend_contract_test
               tests/abi/redis_backend_contract_test.c)
target_compile_features(orm_redis_backend_contract_test PRIVATE c_std_11)
target_link_libraries(orm_redis_backend_contract_test PRIVATE orm_c)
if(MSVC)
  target_compile_options(orm_redis_backend_contract_test PRIVATE /W4 /WX
                                                                 /std:c11)
else()
  target_compile_options(orm_redis_backend_contract_test
                         PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
set_target_properties(orm_redis_backend_contract_test PROPERTIES FOLDER "orm/tests")
add_test(NAME orm_redis_backend_contract
         COMMAND orm_redis_backend_contract_test)

add_executable(orm_redis_live_bdd_test
               tests/integration/redis_live_bdd_test.cpp)
target_compile_features(orm_redis_live_bdd_test PRIVATE cxx_std_17)
target_link_libraries(orm_redis_live_bdd_test PRIVATE orm_c TurboDB::Redis
                                                       TurboUtils::TinyTest)
set_target_properties(orm_redis_live_bdd_test PROPERTIES CXX_STANDARD_REQUIRED ON
                                                         FOLDER "orm/tests")
orm_enable_strict_warnings(orm_redis_live_bdd_test)
add_test(NAME orm_redis_live_bdd COMMAND orm_redis_live_bdd_test)
set_tests_properties(orm_redis_live_bdd PROPERTIES LABELS "integration;redis")
endif()

if(ORM_WITH_TIDESDB)
add_executable(orm_tidesdb_live_bdd_test
               tests/integration/tidesdb_live_bdd_test.cpp)
target_compile_features(orm_tidesdb_live_bdd_test PRIVATE cxx_std_17)
target_link_libraries(orm_tidesdb_live_bdd_test PRIVATE orm_c TidesDB::tidesdb
                                                         TurboUtils::TinyTest)
set_target_properties(orm_tidesdb_live_bdd_test PROPERTIES CXX_STANDARD_REQUIRED ON
                                                           FOLDER "orm/tests")
orm_enable_strict_warnings(orm_tidesdb_live_bdd_test)
add_test(NAME orm_tidesdb_live_bdd COMMAND orm_tidesdb_live_bdd_test)
set_tests_properties(orm_tidesdb_live_bdd PROPERTIES LABELS
                                                     "integration;tidesdb")
endif()

if(WIN32)
  set(ORM_TEST_RUNTIME_PATH
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboUtils::Core>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboParser::Mustache>")
  set(ORM_RUNTIME_TESTS orm_redis_query orm_tidesdb_row
                        orm_mustache_render orm_mock_orm_backend orm_typed_fetch)
  if(ORM_WITH_SQLITE)
    string(APPEND ORM_TEST_RUNTIME_PATH
           ";PATH=path_list_prepend:$<TARGET_FILE_DIR:SQLite::SQLite3>")
    list(APPEND ORM_RUNTIME_TESTS orm_sqlite_c_api orm_cpp_api orm_jpa_api)
  endif()
  if(ORM_WITH_PGSQL)
    list(APPEND ORM_RUNTIME_TESTS orm_postgres_c_api orm_cpp17)
  endif()
  if(ORM_WITH_REDIS)
    string(
      APPEND
      ORM_TEST_RUNTIME_PATH
      ";PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboNet::CoroNet>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboDB::Redis>"
    )
    list(APPEND ORM_RUNTIME_TESTS orm_redis_transaction
         orm_driver_error_mapping orm_redis_backend_contract
         orm_redis_live_bdd)
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
    list(APPEND ORM_RUNTIME_TESTS orm_tidesdb_live_bdd)
  endif()
  if(ORM_WITH_MONGODB)
    string(APPEND ORM_TEST_RUNTIME_PATH
           ";PATH=path_list_prepend:$<TARGET_FILE_DIR:mongo::mongoc_shared>;PATH=path_list_prepend:$<TARGET_FILE_DIR:mongo::bson_shared>")
    list(APPEND ORM_RUNTIME_TESTS orm_mongo_query orm_mongo_live_bdd)
  endif()
  if(ORM_BUILD_SCHEMA_TOOLS)
    list(APPEND ORM_RUNTIME_TESTS orm_schema_generated_metadata)
    if(ORM_WITH_SQLITE)
      string(APPEND ORM_TEST_RUNTIME_PATH
             ";PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboParser::DataBind>")
      list(APPEND ORM_RUNTIME_TESTS orm_schema_generated_c)
    endif()
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

cmake_install_headers(FILES include/orm/orm.h include/orm/orm.hpp include/orm/jpa.hpp
                      include/orm/model.hpp DESTINATION
                      ${CMAKE_INSTALL_INCLUDEDIR}/shared/orm)
cmake_install_headers(FILES include/orm/dbs/postgres/model_detail.hpp DESTINATION
                      ${CMAKE_INSTALL_INCLUDEDIR}/shared/orm/dbs/postgres)

cmake_config_package(PACKAGE Orm VERSION "${ORM_VERSION}")

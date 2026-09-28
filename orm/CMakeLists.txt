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
# or as a TurboDB subdirectory. Database Drivers are deployment/build products;
# they are not capabilities of the generic core package.
if(NOT DEFINED ORM_BUILD_SQLITE_DRIVER_DEFAULT)
  set(ORM_BUILD_SQLITE_DRIVER_DEFAULT ON)
endif()
if(NOT DEFINED ORM_BUILD_POSTGRESQL_DRIVER_DEFAULT)
  set(ORM_BUILD_POSTGRESQL_DRIVER_DEFAULT ON)
endif()
if(NOT DEFINED ORM_BUILD_LEGACY_POSTGRESQL_COMPONENT_DEFAULT)
  set(ORM_BUILD_LEGACY_POSTGRESQL_COMPONENT_DEFAULT ON)
endif()
if(NOT DEFINED ORM_BUILD_REDIS_DRIVER_DEFAULT)
  set(ORM_BUILD_REDIS_DRIVER_DEFAULT OFF)
endif()
if(NOT DEFINED ORM_BUILD_MONGODB_DRIVER_DEFAULT)
  set(ORM_BUILD_MONGODB_DRIVER_DEFAULT OFF)
endif()
option(ORM_BUILD_SQLITE_DRIVER
       "Build the independent SQLite TurboDb.Driver module"
       ${ORM_BUILD_SQLITE_DRIVER_DEFAULT})
option(ORM_BUILD_POSTGRESQL_DRIVER
       "Build the independent PostgreSQL TurboDb.Driver module"
       ${ORM_BUILD_POSTGRESQL_DRIVER_DEFAULT})
option(ORM_BUILD_LEGACY_POSTGRESQL_COMPONENT
       "Build the legacy Orm::PostgreSQL direct connector compatibility component"
       ${ORM_BUILD_LEGACY_POSTGRESQL_COMPONENT_DEFAULT})
option(ORM_BUILD_REDIS_DRIVER
       "Build the independent Redis TurboDb.Driver module"
       ${ORM_BUILD_REDIS_DRIVER_DEFAULT})
option(ORM_BUILD_MONGODB_DRIVER
       "Build the independent MongoDB TurboDb.Driver module"
       ${ORM_BUILD_MONGODB_DRIVER_DEFAULT})
if(NOT DEFINED ORM_BUILD_TIDESDB_DRIVER_DEFAULT)
  if(TARGET TidesDB::tidesdb)
    set(ORM_BUILD_TIDESDB_DRIVER_DEFAULT ON)
  else()
    set(ORM_BUILD_TIDESDB_DRIVER_DEFAULT OFF)
  endif()
endif()
option(ORM_BUILD_TIDESDB_DRIVER
       "Build the independent TidesDB TurboDb.Driver module"
       ${ORM_BUILD_TIDESDB_DRIVER_DEFAULT})
option(ORM_MONGODB_LIVE_TESTS
       "Run the MongoDB Driver CRUD test against ORM_MONGODB_URI" OFF)
option(ORM_REDIS_LIVE_TESTS
       "Run the Redis Driver CRUD test against ORM_REDIS_PORT" OFF)
option(ORM_POSTGRES_LIVE_TESTS
       "Run tests against TURBODB_ORM_PGSQL_TEST_CONNINFO" OFF)
option(ORM_MYSQL_LIVE_TESTS
       "Qualify MySQL HandshakeV10/CNet/TLS/auth/ping against a real service" OFF)

if(ORM_MONGODB_LIVE_TESTS AND NOT ORM_BUILD_MONGODB_DRIVER)
  message(FATAL_ERROR "ORM_MONGODB_LIVE_TESTS requires ORM_BUILD_MONGODB_DRIVER=ON")
endif()
if(ORM_REDIS_LIVE_TESTS AND NOT ORM_BUILD_REDIS_DRIVER)
  message(FATAL_ERROR "ORM_REDIS_LIVE_TESTS requires ORM_BUILD_REDIS_DRIVER=ON")
endif()

if(ORM_MYSQL_LIVE_TESTS)
  if(NOT ORM_BUILD_TESTS)
    message(FATAL_ERROR "ORM_MYSQL_LIVE_TESTS requires ORM_BUILD_TESTS=ON")
  endif()
  foreach(ORM_MYSQL_REQUIRED_ENV IN ITEMS
          ORM_MYSQL_HOST ORM_MYSQL_PORT ORM_MYSQL_USER ORM_MYSQL_PASSWORD
          ORM_MYSQL_CA_FILE ORM_MYSQL_SERVER_NAME)
    if(NOT DEFINED ENV{${ORM_MYSQL_REQUIRED_ENV}} OR
       "$ENV{${ORM_MYSQL_REQUIRED_ENV}}" STREQUAL "")
      message(FATAL_ERROR
              "ORM_MYSQL_LIVE_TESTS requires ${ORM_MYSQL_REQUIRED_ENV}")
    endif()
  endforeach()
endif()

if(ORM_POSTGRES_LIVE_TESTS)
  if(NOT ORM_BUILD_TESTS)
    message(FATAL_ERROR "ORM_POSTGRES_LIVE_TESTS requires ORM_BUILD_TESTS=ON")
  endif()
  if(NOT ORM_BUILD_POSTGRESQL_DRIVER)
    message(FATAL_ERROR
            "ORM_POSTGRES_LIVE_TESTS requires ORM_BUILD_POSTGRESQL_DRIVER=ON")
  endif()
  if(NOT DEFINED ENV{TURBODB_ORM_PGSQL_TEST_CONNINFO} OR
     "$ENV{TURBODB_ORM_PGSQL_TEST_CONNINFO}" STREQUAL "")
    message(FATAL_ERROR
            "ORM_POSTGRES_LIVE_TESTS requires TURBODB_ORM_PGSQL_TEST_CONNINFO")
  endif()
endif()

if((ORM_BUILD_TESTS OR ORM_BUILD_POSTGRESQL_DRIVER OR
    ORM_BUILD_LEGACY_POSTGRESQL_COMPONENT) AND
   NOT CMAKE_CXX_COMPILER_LOADED)
  enable_language(CXX)
endif()

# Find required base dependencies
if(NOT TARGET Salts::Core)
  find_package(Salts 1.8.0 EXACT CONFIG REQUIRED PATHS "$ENV{SALTS_ROOT}"
               NO_DEFAULT_PATH)
endif()

foreach(required_salts_target IN ITEMS Salts::Core Salts::PluginABI Salts::Plugin)
  if(NOT TARGET ${required_salts_target})
    message(FATAL_ERROR
            "The selected Salts 1.8.0 installation must export ${required_salts_target}")
  endif()
endforeach()

if(NOT TARGET Salts::DataBind)
  if(NOT DEFINED ENV{SALTS_UTILS_ROOT} OR "$ENV{SALTS_UTILS_ROOT}" STREQUAL "")
    message(FATAL_ERROR "ORM requires an explicit SALTS_UTILS_ROOT")
  endif()
  if(NOT IS_DIRECTORY "$ENV{SALTS_UTILS_ROOT}")
    message(FATAL_ERROR "SALTS_UTILS_ROOT is not a directory: $ENV{SALTS_UTILS_ROOT}")
  endif()
  unset(SaltsUtils_DIR CACHE)
  unset(SaltsUtils_DIR)
  find_package(SaltsUtils 4.1.0 EXACT CONFIG REQUIRED PATHS "$ENV{SALTS_UTILS_ROOT}"
               NO_DEFAULT_PATH)
endif()
if(NOT TARGET Salts::DataBind)
  message(FATAL_ERROR
          "The selected SaltsUtils 4.1.0 installation must export Salts::DataBind")
endif()

if(ORM_MYSQL_LIVE_TESTS)
  if(NOT TARGET Salts::CNet)
    message(FATAL_ERROR
            "The selected Salts 1.8.0 installation must export Salts::CNet for MySQL M2")
  endif()
  find_package(OpenSSL REQUIRED)
endif()

# Driver/component-native dependencies remain outside the generic core.
if(ORM_BUILD_SQLITE_DRIVER)
  find_package(SQLite3 REQUIRED)
endif()

if(ORM_BUILD_POSTGRESQL_DRIVER OR ORM_BUILD_LEGACY_POSTGRESQL_COMPONENT)
  find_package(PostgreSQL REQUIRED)
endif()

if(ORM_BUILD_REDIS_DRIVER AND NOT TARGET TurboDB::Redis)
  find_package(TurboDB CONFIG REQUIRED)
endif()

if(ORM_BUILD_TIDESDB_DRIVER AND NOT TARGET TidesDB::tidesdb)
  find_package(TidesDB CONFIG REQUIRED)
endif()

if(ORM_BUILD_MONGODB_DRIVER)
  find_package(mongoc CONFIG REQUIRED)
  find_package(bson CONFIG REQUIRED)
  if(TARGET mongoc::shared)
    set(ORM_MONGODB_MONGOC_TARGET mongoc::shared)
  elseif(TARGET mongoc::static)
    set(ORM_MONGODB_MONGOC_TARGET mongoc::static)
  else()
    message(FATAL_ERROR "MongoDB Driver requires a canonical mongoc target")
  endif()
  if(TARGET bson::shared)
    set(ORM_MONGODB_BSON_TARGET bson::shared)
  elseif(TARGET bson::static)
    set(ORM_MONGODB_BSON_TARGET bson::static)
  else()
    message(FATAL_ERROR "MongoDB Driver requires a canonical bson target")
  endif()
endif()

set(ORM_VERSION 2.1.0)
set(ORM_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include/orm)
set(ORM_ABI_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/abi)
set(ORM_FLOW_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/flow)
set(ORM_SQL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/sql)
set(ORM_DRIVER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/driver)
set(ORM_RUNTIME_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime)
set(ORM_ABI_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/abi)
set(ORM_TEST_SUPPORT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)

set(ORM_C_SOURCES src/abi/orm_core.c src/abi/orm_owner.c src/abi/orm_plan.c
                  src/abi/orm_result.c src/driver/orm_driver_contract.c
                  src/driver/orm_driver_owner_bridge.c
                  src/driver/orm_driver_plan_view.c
                  src/runtime/orm_runtime.c
                  src/dbs/sql/orm_sql_render.c)
set(ORM_FLOW_SOURCES src/flow/orm_row_publisher.c
                     src/flow/orm_command_publisher.c
                     src/flow/orm_text_token.c)

foreach(ORM_C_SOURCE IN LISTS ORM_C_SOURCES ORM_FLOW_SOURCES)
  if(NOT ORM_C_SOURCE MATCHES "\\.c$")
    message(FATAL_ERROR
            "turbo_orm accepts only C implementation sources: ${ORM_C_SOURCE}")
  endif()
endforeach()

add_library(turbo_orm SHARED ${ORM_C_SOURCES} ${ORM_FLOW_SOURCES})
add_library(orm_cpp INTERFACE)
target_compile_features(turbo_orm PRIVATE c_std_11)
target_compile_definitions(turbo_orm PRIVATE ORM_C_BUILD
                                             ORM_NATIVE_OWNER_CANDIDATE)
target_include_directories(
  turbo_orm
  PUBLIC $<BUILD_INTERFACE:${ORM_INCLUDE_DIR}>
         $<INSTALL_INTERFACE:include/shared/orm>
  PRIVATE ${ORM_ABI_SOURCE_DIR} ${ORM_FLOW_SOURCE_DIR} ${ORM_SQL_SOURCE_DIR}
          ${ORM_DRIVER_SOURCE_DIR} ${ORM_RUNTIME_SOURCE_DIR})
target_link_libraries(
  turbo_orm PUBLIC Salts::Core Salts::CFlow
  PRIVATE Salts::CSerde Salts::DataBind Salts::Plugin
          Salts::CSTL Salts::Platform ${CMAKE_DL_LIBS})

get_target_property(ORM_CORE_LINK_CONTRACT turbo_orm LINK_LIBRARIES)
foreach(ORM_FORBIDDEN_CORE_LINK IN ITEMS SQLite::SQLite3
                                         PostgreSQL::PostgreSQL
                                         TidesDB::tidesdb
                                         TurboDB::Redis
                                         ${ORM_MONGODB_MONGOC_TARGET}
                                         ${ORM_MONGODB_BSON_TARGET})
  list(FIND ORM_CORE_LINK_CONTRACT "${ORM_FORBIDDEN_CORE_LINK}"
       ORM_FORBIDDEN_CORE_LINK_INDEX)
  if(NOT ORM_FORBIDDEN_CORE_LINK_INDEX EQUAL -1)
    message(FATAL_ERROR
            "generic turbo_orm must not link ${ORM_FORBIDDEN_CORE_LINK}")
  endif()
endforeach()

if(ORM_BUILD_SQLITE_DRIVER)
  add_library(orm_sqlite_driver MODULE
    src/dbs/sqlite/plugin.c
    src/driver/orm_driver_backend_bridge.c
    src/dbs/sqlite/backend.c
    src/dbs/sqlite/orm_sqlite_cursor.c
    src/dbs/sql/orm_sql_render.c)
  target_compile_features(orm_sqlite_driver PRIVATE c_std_11)
  target_compile_definitions(orm_sqlite_driver PRIVATE ORM_C_STATIC)
  target_include_directories(
    orm_sqlite_driver PRIVATE
      ${ORM_INCLUDE_DIR}
      ${ORM_ABI_SOURCE_DIR}
      ${ORM_FLOW_SOURCE_DIR}
      ${ORM_SQL_SOURCE_DIR}
      ${ORM_DRIVER_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/sqlite)
  target_link_libraries(
    orm_sqlite_driver PRIVATE
      turbo_orm
      SQLite::SQLite3
      Salts::PluginABI
      Salts::Core
      Salts::CSerde
      Salts::CFlow
      Salts::CSTL)
  # Runtime MODULE owns the native SQLite dependency. Generic Orm::C must
  # remain independent from SQLite headers, objects and link items.
  get_target_property(ORM_SQLITE_DRIVER_LINK_CONTRACT
                      orm_sqlite_driver LINK_LIBRARIES)
  list(FIND ORM_SQLITE_DRIVER_LINK_CONTRACT "SQLite::SQLite3"
       ORM_SQLITE_DRIVER_SQLITE_LINK_INDEX)
  if(ORM_SQLITE_DRIVER_SQLITE_LINK_INDEX EQUAL -1)
    message(FATAL_ERROR
            "orm_sqlite_driver must directly link SQLite::SQLite3")
  endif()
  set_target_properties(
    orm_sqlite_driver PROPERTIES
      PREFIX ""
      OUTPUT_NAME "turbodb_driver_sqlite"
      C_STANDARD_REQUIRED ON
      C_VISIBILITY_PRESET hidden
      WINDOWS_EXPORT_ALL_SYMBOLS OFF
      FOLDER "orm/drivers")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Keep statically linked SQLite/Salts archive symbols private to the
    # Driver MODULE. salts_plugin_query is defined by the MODULE object itself
    # and remains the only intentional generic loader entry.
    target_link_options(orm_sqlite_driver PRIVATE "LINKER:--exclude-libs,ALL")
  endif()
endif()

if(ORM_BUILD_TIDESDB_DRIVER)
  add_library(orm_tidesdb_driver MODULE
    src/dbs/tidesdb/plugin.c
    src/driver/orm_driver_backend_bridge.c
    src/dbs/tidesdb/backend.c
    src/dbs/tidesdb/bridge.c
    src/dbs/tidesdb/row.c
    src/dbs/tidesdb/orm_tidesdb_cursor.c)
  target_compile_features(orm_tidesdb_driver PRIVATE c_std_11)
  target_compile_definitions(
    orm_tidesdb_driver PRIVATE ORM_C_STATIC ORM_NATIVE_OWNER_CANDIDATE)
  target_include_directories(
    orm_tidesdb_driver PRIVATE
      ${ORM_INCLUDE_DIR}
      ${ORM_ABI_SOURCE_DIR}
      ${ORM_FLOW_SOURCE_DIR}
      ${ORM_DRIVER_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/tidesdb)
  target_link_libraries(
    orm_tidesdb_driver PRIVATE
      turbo_orm
      TidesDB::tidesdb
      Salts::PluginABI
      Salts::Core
      Salts::CSerde
      Salts::CFlow
      Salts::DataBind
      Salts::CSTL)
  set_target_properties(
    orm_tidesdb_driver PROPERTIES
      PREFIX ""
      OUTPUT_NAME "turbodb_driver_tidesdb"
      C_STANDARD_REQUIRED ON
      C_VISIBILITY_PRESET hidden
      WINDOWS_EXPORT_ALL_SYMBOLS OFF
      FOLDER "orm/drivers")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(orm_tidesdb_driver PRIVATE
      "LINKER:--exclude-libs,ALL" "LINKER:-z,defs")
  endif()
endif()

if(ORM_BUILD_REDIS_DRIVER)
  add_library(orm_redis_driver MODULE
    src/dbs/redis/plugin.c
    src/driver/orm_driver_backend_bridge.c
    src/dbs/redis/backend.c
    src/dbs/redis/query.c
    src/dbs/redis/orm_redis_lib.c
    src/dbs/redis/orm_redis_cursor.c
    src/flow/orm_text_token.c)
  target_compile_features(orm_redis_driver PRIVATE c_std_11)
  target_compile_definitions(
    orm_redis_driver PRIVATE ORM_C_STATIC ORM_NATIVE_OWNER_CANDIDATE)
  target_include_directories(
    orm_redis_driver PRIVATE
      ${ORM_INCLUDE_DIR}
      ${ORM_ABI_SOURCE_DIR}
      ${ORM_FLOW_SOURCE_DIR}
      ${ORM_DRIVER_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/redis)
  target_link_libraries(
    orm_redis_driver PRIVATE
      turbo_orm
      TurboDB::Redis
      Salts::PluginABI
      Salts::Core
      Salts::CSerde
      Salts::CFlow
      Salts::DataBind
      Salts::CSTL
      Salts::Platform)
  set_target_properties(
    orm_redis_driver PROPERTIES
      PREFIX ""
      OUTPUT_NAME "turbodb_driver_redis"
      C_STANDARD_REQUIRED ON
      C_VISIBILITY_PRESET hidden
      WINDOWS_EXPORT_ALL_SYMBOLS OFF
      FOLDER "orm/drivers")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(orm_redis_driver PRIVATE
      "LINKER:--exclude-libs,ALL" "LINKER:-z,defs")
  endif()
endif()

if(ORM_BUILD_MONGODB_DRIVER)
  add_library(orm_mongodb_driver MODULE
    src/dbs/mongo/plugin.c
    src/driver/orm_driver_backend_bridge.c
    src/dbs/mongo/backend.c
    src/dbs/mongo/query.c
    src/dbs/mongo/orm_mongo_lib.c
    src/dbs/mongo/orm_mongo_cursor.c
    src/flow/orm_text_token.c)
  target_compile_features(orm_mongodb_driver PRIVATE c_std_11)
  target_compile_definitions(
    orm_mongodb_driver PRIVATE ORM_C_STATIC ORM_NATIVE_OWNER_CANDIDATE)
  target_include_directories(
    orm_mongodb_driver PRIVATE
      ${ORM_INCLUDE_DIR}
      ${ORM_ABI_SOURCE_DIR}
      ${ORM_FLOW_SOURCE_DIR}
      ${ORM_DRIVER_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/mongo)
  target_link_libraries(
    orm_mongodb_driver PRIVATE
      turbo_orm
      ${ORM_MONGODB_MONGOC_TARGET}
      ${ORM_MONGODB_BSON_TARGET}
      Salts::PluginABI
      Salts::Core
      Salts::CSerde
      Salts::CFlow
      Salts::DataBind
      Salts::CSTL)
  set_target_properties(
    orm_mongodb_driver PROPERTIES
      PREFIX ""
      OUTPUT_NAME "turbodb_driver_mongodb"
      C_STANDARD_REQUIRED ON
      C_VISIBILITY_PRESET hidden
      WINDOWS_EXPORT_ALL_SYMBOLS OFF
      FOLDER "orm/drivers")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(orm_mongodb_driver PRIVATE
      "LINKER:--exclude-libs,ALL" "LINKER:-z,defs")
  endif()
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

set(ORM_POSTGRESQL_BACKEND_SOURCES
    src/dbs/postgres/backend.c
    src/dbs/postgres/orm_postgres_libpq.c
    src/dbs/postgres/orm_postgres_cursor.c
    src/flow/orm_text_token.c)
foreach(ORM_POSTGRESQL_SOURCE IN LISTS ORM_POSTGRESQL_BACKEND_SOURCES)
  if(NOT ORM_POSTGRESQL_SOURCE MATCHES "\\.c$")
    message(FATAL_ERROR
            "PostgreSQL Driver/component accepts only C implementation sources: ${ORM_POSTGRESQL_SOURCE}")
  endif()
endforeach()

if(ORM_BUILD_LEGACY_POSTGRESQL_COMPONENT)
  add_library(orm_postgresql SHARED
    src/dbs/postgres/component.c
    ${ORM_POSTGRESQL_BACKEND_SOURCES})
  target_compile_features(orm_postgresql PRIVATE c_std_11)
  target_compile_definitions(orm_postgresql PRIVATE
    ORM_POSTGRESQL_BUILD ORM_NATIVE_OWNER_CANDIDATE)
  target_include_directories(
    orm_postgresql
    PUBLIC $<BUILD_INTERFACE:${ORM_INCLUDE_DIR}>
           $<INSTALL_INTERFACE:include/shared/orm>
    PRIVATE ${ORM_ABI_SOURCE_DIR} ${ORM_FLOW_SOURCE_DIR} ${ORM_SQL_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/postgres)
  target_link_libraries(
    orm_postgresql
    PUBLIC turbo_orm
    PRIVATE PostgreSQL::PostgreSQL Salts::CSerde
            Salts::CFlow Salts::CSTL)
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
    "orm/legacy"
    RUNTIME_DEPENDENCY_SET
    OrmPostgreSQLRuntimeDependencies
    NO_VERSION)
endif()

if(ORM_BUILD_POSTGRESQL_DRIVER)
  add_library(orm_postgresql_driver MODULE
    src/dbs/postgres/plugin.c
    src/driver/orm_driver_backend_bridge.c
    ${ORM_POSTGRESQL_BACKEND_SOURCES}
    src/dbs/sql/orm_sql_render.c)
  target_compile_features(orm_postgresql_driver PRIVATE c_std_11)
  target_compile_definitions(
    orm_postgresql_driver PRIVATE ORM_C_STATIC ORM_NATIVE_OWNER_CANDIDATE)
  target_include_directories(
    orm_postgresql_driver PRIVATE
      ${ORM_INCLUDE_DIR}
      ${ORM_ABI_SOURCE_DIR}
      ${ORM_FLOW_SOURCE_DIR}
      ${ORM_SQL_SOURCE_DIR}
      ${ORM_DRIVER_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/postgres)
  target_link_libraries(
    orm_postgresql_driver PRIVATE
      turbo_orm
      PostgreSQL::PostgreSQL
      Salts::PluginABI
      Salts::Core
      Salts::CSerde
      Salts::CFlow
      Salts::CSTL)
  set_target_properties(
    orm_postgresql_driver PROPERTIES
      PREFIX ""
      OUTPUT_NAME "turbodb_driver_postgresql"
      C_STANDARD_REQUIRED ON
      LINKER_LANGUAGE CXX
      C_VISIBILITY_PRESET hidden
      WINDOWS_EXPORT_ALL_SYMBOLS OFF
      FOLDER "orm/drivers")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(orm_postgresql_driver PRIVATE
      "LINKER:--exclude-libs,ALL")
  endif()
  get_target_property(ORM_POSTGRESQL_DRIVER_LINK_CONTRACT
                      orm_postgresql_driver LINK_LIBRARIES)
  list(FIND ORM_POSTGRESQL_DRIVER_LINK_CONTRACT "PostgreSQL::PostgreSQL"
       ORM_POSTGRESQL_DRIVER_NATIVE_LINK_INDEX)
  if(ORM_POSTGRESQL_DRIVER_NATIVE_LINK_INDEX EQUAL -1)
    message(FATAL_ERROR
            "orm_postgresql_driver must directly link PostgreSQL::PostgreSQL")
  endif()
endif()

set(ORM_RUNTIME_DEPENDENCY_TARGETS Salts::DataBind)

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
list(REMOVE_DUPLICATES ORM_RUNTIME_SEARCH_DIRECTORIES)
if(WIN32)
  install(
    RUNTIME_DEPENDENCY_SET OrmRuntimeDependencies
    DIRECTORIES ${ORM_RUNTIME_SEARCH_DIRECTORIES}
    PRE_EXCLUDE_REGEXES "api-ms-win-.*" "ext-ms-win-.*"
                        "clang_rt.asan_dynamic-.*" "salts\\.dll"
                        "salts_utils\\.dll"
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

if(TARGET orm_postgresql)
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

if(TARGET orm_sqlite_driver)
  orm_enable_strict_warnings(orm_sqlite_driver)
endif()
if(TARGET orm_postgresql_driver)
  orm_enable_strict_warnings(orm_postgresql_driver)
endif()
if(TARGET orm_tidesdb_driver)
  orm_enable_strict_warnings(orm_tidesdb_driver)
endif()
if(TARGET orm_redis_driver)
  orm_enable_strict_warnings(orm_redis_driver)
endif()
if(TARGET orm_mongodb_driver)
  orm_enable_strict_warnings(orm_mongodb_driver)
endif()

orm_enable_strict_warnings(turbo_orm)
if(TARGET orm_postgresql)
  orm_enable_strict_warnings(orm_postgresql)
endif()

set(ORM_INTERNAL_TEST_LIBRARY turbo_orm)
if(ORM_BUILD_TESTS)
  set(ORM_TEST_STATIC_SOURCES ${ORM_C_SOURCES} ${ORM_FLOW_SOURCES})
  if(ORM_BUILD_SQLITE_DRIVER)
    list(APPEND ORM_TEST_STATIC_SOURCES
         src/dbs/sqlite/backend.c
         src/dbs/sqlite/orm_sqlite_cursor.c)
  endif()
  add_library(orm_c_test_static STATIC ${ORM_TEST_STATIC_SOURCES})
  target_compile_features(orm_c_test_static PRIVATE c_std_11)
  target_compile_definitions(orm_c_test_static PRIVATE ORM_C_STATIC)
  # Internal tests that compile cursor/backend sources directly must see the
  # same opaque-handle layout as the static core. This definition is not part
  # of the installed Orm::C/Orm::Cpp interface.
  target_compile_definitions(orm_c_test_static
                             PUBLIC ORM_NATIVE_OWNER_CANDIDATE)
  target_include_directories(
    orm_c_test_static
    PUBLIC ${ORM_INCLUDE_DIR}
    PRIVATE ${ORM_ABI_SOURCE_DIR} ${ORM_FLOW_SOURCE_DIR} ${ORM_SQL_SOURCE_DIR}
            ${ORM_DRIVER_SOURCE_DIR} ${ORM_RUNTIME_SOURCE_DIR})
  target_link_libraries(
    orm_c_test_static PUBLIC Salts::Core
    PRIVATE Salts::CSerde Salts::DataBind Salts::Plugin Salts::CFlow
            Salts::CSTL Salts::Platform ${CMAKE_DL_LIBS})
  if(ORM_BUILD_SQLITE_DRIVER)
    target_compile_definitions(orm_c_test_static PRIVATE ORM_LEGACY_SQLITE_FACTORY)
    target_link_libraries(orm_c_test_static PRIVATE SQLite::SQLite3)
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
    "-DORM_PACKAGE_TEST_EXPECT_LEGACY_POSTGRESQL=${ORM_BUILD_LEGACY_POSTGRESQL_COMPONENT}"
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

if(ORM_BUILD_SQLITE_DRIVER)
  list(APPEND ORM_TEST_PRIVATE_LIBS SQLite::SQLite3 Salts::CSerde
                                    Salts::CFlow)
endif()
list(APPEND ORM_TEST_PRIVATE_LIBS TidesDB::tidesdb)
set(ORM_TEST_DEFINITIONS ORM_C_STATIC)
if(ORM_BUILD_SQLITE_DRIVER)
  list(APPEND ORM_TEST_DEFINITIONS ORM_LEGACY_SQLITE_FACTORY)
endif()
list(APPEND ORM_TEST_DEFINITIONS ORM_WITH_TIDESDB)

if(ORM_BUILD_MONGODB_DRIVER)
add_executable(orm_mongo_query_test
               tests/flow/orm_mongo_query_test.c
               src/dbs/mongo/query.c)
target_compile_features(orm_mongo_query_test PRIVATE c_std_11)
target_compile_definitions(orm_mongo_query_test PRIVATE ORM_C_STATIC)
target_include_directories(
  orm_mongo_query_test PRIVATE ${ORM_INCLUDE_DIR} ${ORM_ABI_SOURCE_DIR}
                               ${ORM_FLOW_SOURCE_DIR}
                               ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/mongo)
orm_link_internal_test(orm_mongo_query_test Salts::TinyTest
                       ${ORM_MONGODB_BSON_TARGET})
set_target_properties(orm_mongo_query_test
                      PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_mongo_query_test)
if(MSVC)
  target_compile_options(orm_mongo_query_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_mongo_query COMMAND orm_mongo_query_test)

add_executable(orm_mongodb_plugin_test
               tests/driver/orm_mongodb_plugin_test.c)
target_compile_features(orm_mongodb_plugin_test PRIVATE c_std_11)
target_compile_definitions(orm_mongodb_plugin_test PRIVATE ORM_C_STATIC)
target_include_directories(orm_mongodb_plugin_test PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(orm_mongodb_plugin_test PRIVATE turbo_orm)
add_dependencies(orm_mongodb_plugin_test orm_mongodb_driver)
set_target_properties(orm_mongodb_plugin_test PROPERTIES
                      C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_mongodb_plugin_test)
add_test(NAME orm_mongodb_plugin COMMAND orm_mongodb_plugin_test)
set_tests_properties(orm_mongodb_plugin PROPERTIES
  ENVIRONMENT "ORM_MONGODB_PLUGIN=$<TARGET_FILE:orm_mongodb_driver>")

add_executable(orm_mongodb_live_driver_test
               tests/integration/mongodb_live_driver_test.c)
target_compile_features(orm_mongodb_live_driver_test PRIVATE c_std_11)
target_compile_definitions(orm_mongodb_live_driver_test PRIVATE ORM_C_STATIC)
target_include_directories(orm_mongodb_live_driver_test PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(orm_mongodb_live_driver_test PRIVATE
                      turbo_orm Salts::CFlow)
add_dependencies(orm_mongodb_live_driver_test orm_mongodb_driver)
set_target_properties(orm_mongodb_live_driver_test PROPERTIES
                      C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_mongodb_live_driver_test)
if(ORM_MONGODB_LIVE_TESTS)
  add_test(NAME orm_mongodb_live_driver COMMAND orm_mongodb_live_driver_test)
  set_tests_properties(orm_mongodb_live_driver PROPERTIES
    ENVIRONMENT "ORM_MONGODB_PLUGIN=$<TARGET_FILE:orm_mongodb_driver>;ORM_MONGODB_URI=$ENV{ORM_MONGODB_URI}")
endif()
endif()

if(ORM_BUILD_POSTGRESQL_DRIVER OR ORM_BUILD_LEGACY_POSTGRESQL_COMPONENT)
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
endif()

if(ORM_BUILD_LEGACY_POSTGRESQL_COMPONENT)
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
endif()

if(ORM_BUILD_POSTGRESQL_DRIVER)
add_executable(orm_postgresql_plugin_test
  tests/driver/orm_postgresql_plugin_test.c)
target_compile_features(orm_postgresql_plugin_test PRIVATE c_std_11)
target_compile_definitions(orm_postgresql_plugin_test PRIVATE ORM_C_STATIC)
target_include_directories(orm_postgresql_plugin_test PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(orm_postgresql_plugin_test PRIVATE
  turbo_orm Salts::TinyTest)
add_dependencies(orm_postgresql_plugin_test orm_postgresql_driver)
set_target_properties(orm_postgresql_plugin_test PROPERTIES
  C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_postgresql_plugin_test)
if(MSVC)
  target_compile_options(orm_postgresql_plugin_test PRIVATE /WX)
else()
  target_compile_options(orm_postgresql_plugin_test PRIVATE -Werror)
endif()
add_test(NAME orm_postgresql_plugin COMMAND orm_postgresql_plugin_test)
set_tests_properties(orm_postgresql_plugin PROPERTIES
  ENVIRONMENT "ORM_POSTGRESQL_PLUGIN=$<TARGET_FILE:orm_postgresql_driver>")
endif()

if(ORM_BUILD_SQLITE_DRIVER AND ORM_BUILD_POSTGRESQL_DRIVER)
  add_executable(orm_sql_plugin_matrix_test
    tests/driver/orm_sql_plugin_matrix.c)
  target_compile_features(orm_sql_plugin_matrix_test PRIVATE c_std_11)
  target_include_directories(orm_sql_plugin_matrix_test PRIVATE ${ORM_INCLUDE_DIR})
  target_link_libraries(orm_sql_plugin_matrix_test PRIVATE turbo_orm)
  add_dependencies(orm_sql_plugin_matrix_test
    orm_sqlite_driver orm_postgresql_driver)
  set_target_properties(orm_sql_plugin_matrix_test PROPERTIES
    C_STANDARD_REQUIRED ON FOLDER "orm/tests")
  orm_enable_strict_warnings(orm_sql_plugin_matrix_test)
  if(MSVC)
    target_compile_options(orm_sql_plugin_matrix_test PRIVATE /WX)
  else()
    target_compile_options(orm_sql_plugin_matrix_test PRIVATE -Werror)
  endif()
  get_target_property(ORM_SQL_PLUGIN_MATRIX_LINK_CONTRACT
                      orm_sql_plugin_matrix_test LINK_LIBRARIES)
  foreach(ORM_FORBIDDEN_MATRIX_LINK IN ITEMS SQLite::SQLite3
                                           PostgreSQL::PostgreSQL)
    list(FIND ORM_SQL_PLUGIN_MATRIX_LINK_CONTRACT
         "${ORM_FORBIDDEN_MATRIX_LINK}" ORM_FORBIDDEN_MATRIX_LINK_INDEX)
    if(NOT ORM_FORBIDDEN_MATRIX_LINK_INDEX EQUAL -1)
      message(FATAL_ERROR
              "SQL Plugin matrix app must not link ${ORM_FORBIDDEN_MATRIX_LINK}")
    endif()
  endforeach()
  foreach(ORM_SQL_PLUGIN_MATRIX_SCENARIO IN ITEMS sqlite postgresql both)
    add_test(
      NAME orm_sql_plugin_matrix_${ORM_SQL_PLUGIN_MATRIX_SCENARIO}
      COMMAND orm_sql_plugin_matrix_test
              ${ORM_SQL_PLUGIN_MATRIX_SCENARIO}
              "$<TARGET_FILE:orm_sqlite_driver>"
              "$<TARGET_FILE:orm_postgresql_driver>")
  endforeach()
endif()

if(ORM_POSTGRES_LIVE_TESTS)
  add_executable(orm_postgres_live_test tests/integration/postgres_live_test.c)
  target_compile_features(orm_postgres_live_test PRIVATE c_std_11)
  target_compile_definitions(orm_postgres_live_test PRIVATE ORM_C_STATIC)
  target_include_directories(orm_postgres_live_test PRIVATE ${ORM_INCLUDE_DIR})
  target_link_libraries(orm_postgres_live_test PRIVATE turbo_orm
                                                         Salts::TinyTest)
  add_dependencies(orm_postgres_live_test orm_postgresql_driver)
  set_target_properties(orm_postgres_live_test
                        PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
  orm_enable_strict_warnings(orm_postgres_live_test)
  if(MSVC)
    target_compile_options(orm_postgres_live_test
                           PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
  endif()
  add_test(NAME orm_postgres_live COMMAND orm_postgres_live_test)
  set_tests_properties(orm_postgres_live PROPERTIES
    ENVIRONMENT "ORM_POSTGRESQL_PLUGIN=$<TARGET_FILE:orm_postgresql_driver>")
endif()

add_executable(orm_row_publisher_test tests/flow/orm_row_publisher_test.c)
target_compile_features(orm_row_publisher_test PRIVATE c_std_11)
target_compile_definitions(orm_row_publisher_test PRIVATE ORM_C_STATIC)
target_include_directories(
  orm_row_publisher_test PRIVATE ${ORM_INCLUDE_DIR}
                                 ${ORM_FLOW_SOURCE_DIR})
orm_link_internal_test(orm_row_publisher_test Salts::DataBind
                       Salts::CFlow Salts::TinyTest)
set_target_properties(orm_row_publisher_test PROPERTIES C_STANDARD_REQUIRED ON
                                                        FOLDER "orm/tests")
orm_enable_strict_warnings(orm_row_publisher_test)
if(MSVC)
  target_compile_options(orm_row_publisher_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_row_publisher COMMAND orm_row_publisher_test)

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
  PRIVATE ORM_C_STATIC ORM_EXPECT_SQLITE=$<BOOL:${ORM_BUILD_SQLITE_DRIVER}>)
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

# Candidate plan adapter is exercised against the real core, not the SDK fixture.
add_executable(orm_driver_plan_view_test
  tests/driver/orm_driver_plan_view_test.c)
target_compile_features(orm_driver_plan_view_test PRIVATE c_std_11)
target_compile_definitions(orm_driver_plan_view_test PRIVATE ORM_C_STATIC)
target_include_directories(orm_driver_plan_view_test PRIVATE
  ${ORM_INCLUDE_DIR} ${ORM_ABI_SOURCE_DIR} ${ORM_FLOW_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/src/driver)
orm_link_internal_test(orm_driver_plan_view_test Salts::TinyTest)
set_target_properties(orm_driver_plan_view_test
  PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_driver_plan_view_test)
# Keep this new boundary strict without changing legacy core warning defaults.
if(MSVC)
  target_compile_options(orm_driver_plan_view_test PRIVATE /WX)
else()
  target_compile_options(orm_driver_plan_view_test PRIVATE -Werror)
endif()
add_test(NAME orm_driver_plan_view COMMAND orm_driver_plan_view_test)

# MySQL issue #33 M1 is protocol-only. It intentionally does not build or
# publish a TurboDb.Driver module and therefore advertises no CRUD capability.
set(ORM_MYSQL_WIRE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/mysql/wire)
add_executable(orm_mysql_wire_test
  tests/driver/orm_mysql_wire_test.c
  src/dbs/mysql/wire/codec.c
  src/dbs/mysql/wire/packet.c
  src/dbs/mysql/wire/result.c)
target_compile_features(orm_mysql_wire_test PRIVATE c_std_11)
target_include_directories(orm_mysql_wire_test PRIVATE
  ${ORM_MYSQL_WIRE_SOURCE_DIR})
target_link_libraries(orm_mysql_wire_test PRIVATE Salts::TinyTest)
set_target_properties(orm_mysql_wire_test PROPERTIES
  C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_mysql_wire_test)
if(MSVC)
  target_compile_options(orm_mysql_wire_test PRIVATE /WX)
else()
  target_compile_options(orm_mysql_wire_test PRIVATE -Werror)
endif()
add_test(NAME orm_mysql_wire COMMAND orm_mysql_wire_test)

add_executable(orm_mysql_parameters_test
  tests/driver/orm_mysql_parameters_test.c
  src/dbs/mysql/parameters.c)
target_compile_features(orm_mysql_parameters_test PRIVATE c_std_11)
target_include_directories(orm_mysql_parameters_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/mysql
  ${ORM_MYSQL_WIRE_SOURCE_DIR})
target_link_libraries(orm_mysql_parameters_test PRIVATE Salts::TinyTest)
set_target_properties(orm_mysql_parameters_test PROPERTIES
  C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_mysql_parameters_test)
if(MSVC)
  target_compile_options(orm_mysql_parameters_test PRIVATE /WX)
else()
  target_compile_options(orm_mysql_parameters_test PRIVATE -Werror)
endif()
add_test(NAME orm_mysql_parameters COMMAND orm_mysql_parameters_test)

add_executable(orm_mysql_statement_test
  tests/driver/orm_mysql_statement_test.c
  src/dbs/mysql/wire/codec.c
  src/dbs/mysql/wire/statement.c)
target_compile_features(orm_mysql_statement_test PRIVATE c_std_11)
target_include_directories(orm_mysql_statement_test PRIVATE
  ${ORM_MYSQL_WIRE_SOURCE_DIR})
target_link_libraries(orm_mysql_statement_test PRIVATE Salts::TinyTest)
set_target_properties(orm_mysql_statement_test PROPERTIES
  C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_mysql_statement_test)
if(MSVC)
  target_compile_options(orm_mysql_statement_test PRIVATE /WX)
else()
  target_compile_options(orm_mysql_statement_test PRIVATE -Werror)
endif()
add_test(NAME orm_mysql_statement COMMAND orm_mysql_statement_test)

if(ORM_MYSQL_LIVE_TESTS)
  set(ORM_MYSQL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/mysql)
  set(ORM_MYSQL_AUTH_SOURCE_DIR ${ORM_MYSQL_SOURCE_DIR}/auth)

  add_executable(orm_mysql_handshake_test
    tests/driver/orm_mysql_handshake_test.c
    src/dbs/mysql/wire/codec.c
    src/dbs/mysql/wire/handshake.c
    src/dbs/mysql/auth/auth.c)
  target_compile_features(orm_mysql_handshake_test PRIVATE c_std_11)
  target_include_directories(orm_mysql_handshake_test PRIVATE
    ${ORM_MYSQL_WIRE_SOURCE_DIR} ${ORM_MYSQL_AUTH_SOURCE_DIR})
  target_link_libraries(orm_mysql_handshake_test PRIVATE
    Salts::TinyTest OpenSSL::Crypto)
  set_target_properties(orm_mysql_handshake_test PROPERTIES
    C_STANDARD_REQUIRED ON FOLDER "orm/tests")
  orm_enable_strict_warnings(orm_mysql_handshake_test)
  if(MSVC)
    target_compile_options(orm_mysql_handshake_test PRIVATE /WX)
  else()
    target_compile_options(orm_mysql_handshake_test PRIVATE -Werror)
  endif()
  add_test(NAME orm_mysql_handshake COMMAND orm_mysql_handshake_test)

  add_executable(orm_mysql_live_test
    tests/integration/mysql_live_test.c
    src/dbs/mysql/session.c
    src/dbs/mysql/wire/codec.c
    src/dbs/mysql/wire/packet.c
    src/dbs/mysql/wire/result.c
    src/dbs/mysql/wire/handshake.c
    src/dbs/mysql/auth/auth.c)
  target_compile_features(orm_mysql_live_test PRIVATE c_std_11)
  target_include_directories(orm_mysql_live_test PRIVATE
    ${ORM_MYSQL_SOURCE_DIR}
    ${ORM_MYSQL_WIRE_SOURCE_DIR}
    ${ORM_MYSQL_AUTH_SOURCE_DIR})
  target_link_libraries(orm_mysql_live_test PRIVATE
    Salts::CNet OpenSSL::Crypto)
  set_target_properties(orm_mysql_live_test PROPERTIES
    C_STANDARD_REQUIRED ON FOLDER "orm/tests")
  orm_enable_strict_warnings(orm_mysql_live_test)
  if(MSVC)
    target_compile_options(orm_mysql_live_test PRIVATE /WX)
  else()
    target_compile_options(orm_mysql_live_test PRIVATE -Werror)
  endif()
  add_test(NAME orm_mysql_live COMMAND orm_mysql_live_test)
  set_tests_properties(orm_mysql_live PROPERTIES ENVIRONMENT
    "ORM_MYSQL_HOST=$ENV{ORM_MYSQL_HOST};ORM_MYSQL_PORT=$ENV{ORM_MYSQL_PORT};ORM_MYSQL_USER=$ENV{ORM_MYSQL_USER};ORM_MYSQL_PASSWORD=$ENV{ORM_MYSQL_PASSWORD};ORM_MYSQL_DATABASE=$ENV{ORM_MYSQL_DATABASE};ORM_MYSQL_CA_FILE=$ENV{ORM_MYSQL_CA_FILE};ORM_MYSQL_SERVER_NAME=$ENV{ORM_MYSQL_SERVER_NAME}")
endif()

add_executable(orm_driver_interface_test
  tests/driver/orm_driver_interface_test.c
  tests/driver/orm_driver_interface_peer_a.c
  tests/driver/orm_driver_interface_peer_b.c)
target_compile_features(orm_driver_interface_test PRIVATE c_std_11)
target_compile_definitions(orm_driver_interface_test PRIVATE ORM_C_STATIC)
target_include_directories(orm_driver_interface_test PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(orm_driver_interface_test PRIVATE Salts::Plugin Salts::CFlow)
set_target_properties(orm_driver_interface_test PROPERTIES
  C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_driver_interface_test)
if(MSVC)
  target_compile_options(orm_driver_interface_test PRIVATE /WX)
else()
  target_compile_options(orm_driver_interface_test PRIVATE -Werror)
endif()
add_test(NAME orm_driver_interface COMMAND orm_driver_interface_test)

add_executable(orm_driver_backend_bridge_test
  tests/driver/orm_driver_backend_bridge_test.c
  src/driver/orm_driver_backend_bridge.c)
target_compile_features(orm_driver_backend_bridge_test PRIVATE c_std_11)
target_compile_definitions(orm_driver_backend_bridge_test PRIVATE ORM_C_STATIC)
target_include_directories(orm_driver_backend_bridge_test PRIVATE
  ${ORM_INCLUDE_DIR} ${ORM_ABI_SOURCE_DIR}
  ${ORM_FLOW_SOURCE_DIR} ${ORM_DRIVER_SOURCE_DIR})
target_link_libraries(orm_driver_backend_bridge_test PRIVATE
  Salts::Core Salts::CFlow Salts::CSTL Salts::TinyTest)
set_target_properties(orm_driver_backend_bridge_test PROPERTIES
  C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_driver_backend_bridge_test)
if(MSVC)
  target_compile_options(orm_driver_backend_bridge_test PRIVATE /WX)
else()
  target_compile_options(orm_driver_backend_bridge_test PRIVATE -Werror)
endif()
add_test(NAME orm_driver_backend_bridge COMMAND orm_driver_backend_bridge_test)

add_executable(orm_driver_backend_adapter_test
  tests/driver/orm_driver_backend_adapter_test.c
  src/driver/orm_driver_backend_bridge.c)
target_compile_features(orm_driver_backend_adapter_test PRIVATE c_std_11)
target_compile_definitions(orm_driver_backend_adapter_test PRIVATE ORM_C_STATIC)
target_include_directories(orm_driver_backend_adapter_test PRIVATE
  ${ORM_INCLUDE_DIR} ${ORM_ABI_SOURCE_DIR}
  ${ORM_FLOW_SOURCE_DIR} ${ORM_DRIVER_SOURCE_DIR})
target_link_libraries(orm_driver_backend_adapter_test PRIVATE
  ${ORM_INTERNAL_TEST_LIBRARY}
  Salts::Core Salts::CSTL Salts::CSerde Salts::TinyTest)
set_target_properties(orm_driver_backend_adapter_test PROPERTIES
  C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_driver_backend_adapter_test)
if(MSVC)
  target_compile_options(orm_driver_backend_adapter_test PRIVATE /WX)
else()
  target_compile_options(orm_driver_backend_adapter_test PRIVATE -Werror)
endif()
add_test(NAME orm_driver_backend_adapter COMMAND orm_driver_backend_adapter_test)

add_executable(orm_driver_owner_bridge_test
  tests/driver/orm_driver_owner_bridge_test.c)
target_compile_features(orm_driver_owner_bridge_test PRIVATE c_std_11)
target_compile_definitions(orm_driver_owner_bridge_test PRIVATE ORM_C_STATIC)
target_include_directories(orm_driver_owner_bridge_test PRIVATE
  ${ORM_INCLUDE_DIR} ${ORM_ABI_SOURCE_DIR} ${ORM_FLOW_SOURCE_DIR}
  ${ORM_DRIVER_SOURCE_DIR})
orm_link_internal_test(orm_driver_owner_bridge_test Salts::TinyTest)
set_target_properties(orm_driver_owner_bridge_test
  PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_driver_owner_bridge_test)
if(MSVC)
  target_compile_options(orm_driver_owner_bridge_test PRIVATE /WX)
else()
  target_compile_options(orm_driver_owner_bridge_test PRIVATE -Werror)
endif()
add_test(NAME orm_driver_owner_bridge COMMAND orm_driver_owner_bridge_test)

# Qualify canonical Plugin loading/admission with real DSOs. These fixtures
# publish only Plugin ABI 2 contracts; no TurboDB-private loader entry exists.
add_library(orm_plugin_no_query_fixture MODULE
  tests/driver/orm_module_no_entry_fixture.c)
target_compile_features(orm_plugin_no_query_fixture PRIVATE c_std_11)
set_target_properties(orm_plugin_no_query_fixture PROPERTIES
  PREFIX "" C_STANDARD_REQUIRED ON C_VISIBILITY_PRESET hidden
  WINDOWS_EXPORT_ALL_SYMBOLS OFF FOLDER "orm/tests")
orm_enable_strict_warnings(orm_plugin_no_query_fixture)

add_library(orm_plugin_bad_abi_fixture MODULE
  tests/driver/orm_module_bad_abi_fixture.c)
target_compile_features(orm_plugin_bad_abi_fixture PRIVATE c_std_11)
target_link_libraries(orm_plugin_bad_abi_fixture PRIVATE Salts::PluginABI)
set_target_properties(orm_plugin_bad_abi_fixture PROPERTIES
  PREFIX "" C_STANDARD_REQUIRED ON C_VISIBILITY_PRESET hidden
  WINDOWS_EXPORT_ALL_SYMBOLS OFF FOLDER "orm/tests")
orm_enable_strict_warnings(orm_plugin_bad_abi_fixture)

add_library(orm_plugin_bad_contract_fixture MODULE
  tests/driver/orm_module_bad_contract_version_fixture.c)
target_compile_features(orm_plugin_bad_contract_fixture PRIVATE c_std_11)
target_include_directories(orm_plugin_bad_contract_fixture PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(orm_plugin_bad_contract_fixture PRIVATE
  Salts::PluginABI Salts::Core Salts::CSerde Salts::CFlow)
set_target_properties(orm_plugin_bad_contract_fixture PROPERTIES
  PREFIX "" C_STANDARD_REQUIRED ON C_VISIBILITY_PRESET hidden
  WINDOWS_EXPORT_ALL_SYMBOLS OFF FOLDER "orm/tests")
orm_enable_strict_warnings(orm_plugin_bad_contract_fixture)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(orm_plugin_bad_contract_fixture PRIVATE "LINKER:--exclude-libs,ALL")
endif()

add_library(orm_plugin_bad_shape_fixture MODULE
  tests/driver/orm_module_bad_interface_fixture.c)
target_compile_features(orm_plugin_bad_shape_fixture PRIVATE c_std_11)
target_include_directories(orm_plugin_bad_shape_fixture PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(orm_plugin_bad_shape_fixture PRIVATE
  Salts::PluginABI Salts::Core Salts::CSerde Salts::CFlow)
set_target_properties(orm_plugin_bad_shape_fixture PROPERTIES
  PREFIX "" C_STANDARD_REQUIRED ON C_VISIBILITY_PRESET hidden
  WINDOWS_EXPORT_ALL_SYMBOLS OFF FOLDER "orm/tests")
orm_enable_strict_warnings(orm_plugin_bad_shape_fixture)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(orm_plugin_bad_shape_fixture PRIVATE "LINKER:--exclude-libs,ALL")
endif()

add_library(orm_runtime_plugin_fixture MODULE
  tests/driver/orm_runtime_plugin_fixture.c)
target_compile_features(orm_runtime_plugin_fixture PRIVATE c_std_11)
target_include_directories(orm_runtime_plugin_fixture PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(orm_runtime_plugin_fixture PRIVATE
  Salts::PluginABI Salts::Core Salts::CSerde Salts::CFlow)
set_target_properties(orm_runtime_plugin_fixture PROPERTIES
  PREFIX "" C_STANDARD_REQUIRED ON C_VISIBILITY_PRESET hidden
  WINDOWS_EXPORT_ALL_SYMBOLS OFF FOLDER "orm/tests")
orm_enable_strict_warnings(orm_runtime_plugin_fixture)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(orm_runtime_plugin_fixture PRIVATE "LINKER:--exclude-libs,ALL")
endif()

add_executable(orm_runtime_registry_test
  tests/driver/orm_runtime_registry_test.c)
target_compile_features(orm_runtime_registry_test PRIVATE c_std_11)
target_include_directories(orm_runtime_registry_test PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(orm_runtime_registry_test PRIVATE turbo_orm Salts::TinyTest)
add_dependencies(orm_runtime_registry_test
  orm_runtime_plugin_fixture orm_plugin_no_query_fixture
  orm_plugin_bad_abi_fixture orm_plugin_bad_contract_fixture
  orm_plugin_bad_shape_fixture)
set_target_properties(orm_runtime_registry_test PROPERTIES
  C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_runtime_registry_test)
if(MSVC)
  target_compile_options(orm_runtime_registry_test PRIVATE /WX)
else()
  target_compile_options(orm_runtime_registry_test PRIVATE -Werror)
endif()
add_test(NAME orm_runtime_registry COMMAND orm_runtime_registry_test)
set_tests_properties(orm_runtime_registry PROPERTIES
  ENVIRONMENT
    "ORM_PLUGIN_FIXTURE=$<TARGET_FILE:orm_runtime_plugin_fixture>;ORM_PLUGIN_NO_QUERY_FIXTURE=$<TARGET_FILE:orm_plugin_no_query_fixture>;ORM_PLUGIN_BAD_ABI_FIXTURE=$<TARGET_FILE:orm_plugin_bad_abi_fixture>;ORM_PLUGIN_BAD_CONTRACT_FIXTURE=$<TARGET_FILE:orm_plugin_bad_contract_fixture>;ORM_PLUGIN_BAD_SHAPE_FIXTURE=$<TARGET_FILE:orm_plugin_bad_shape_fixture>")

# Dedicated race fixture intentionally exports test-only gate symbols in
# addition to canonical salts_plugin_query. It is not the export-surface fixture.
add_library(orm_runtime_race_fixture MODULE
  tests/driver/orm_runtime_race_fixture.c)
target_compile_features(orm_runtime_race_fixture PRIVATE c_std_11)
target_include_directories(orm_runtime_race_fixture PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(orm_runtime_race_fixture PRIVATE
  Salts::PluginABI Salts::Platform Salts::CFlow)
set_target_properties(orm_runtime_race_fixture PROPERTIES
  PREFIX "" C_STANDARD_REQUIRED ON C_VISIBILITY_PRESET hidden
  WINDOWS_EXPORT_ALL_SYMBOLS OFF FOLDER "orm/tests")
orm_enable_strict_warnings(orm_runtime_race_fixture)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(orm_runtime_race_fixture PRIVATE "LINKER:--exclude-libs,ALL")
endif()

add_library(orm_runtime_race_fixture_two MODULE
  tests/driver/orm_runtime_race_fixture.c)
target_compile_features(orm_runtime_race_fixture_two PRIVATE c_std_11)
target_compile_definitions(orm_runtime_race_fixture_two PRIVATE
  ORM_RUNTIME_RACE_DRIVER_ID="race2")
target_include_directories(orm_runtime_race_fixture_two PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(orm_runtime_race_fixture_two PRIVATE
  Salts::PluginABI Salts::Platform Salts::CFlow)
set_target_properties(orm_runtime_race_fixture_two PROPERTIES
  PREFIX "" C_STANDARD_REQUIRED ON C_VISIBILITY_PRESET hidden
  WINDOWS_EXPORT_ALL_SYMBOLS OFF FOLDER "orm/tests")
orm_enable_strict_warnings(orm_runtime_race_fixture_two)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(orm_runtime_race_fixture_two PRIVATE "LINKER:--exclude-libs,ALL")
endif()

add_executable(orm_runtime_race_test
  tests/driver/orm_runtime_race_test.c)
target_compile_features(orm_runtime_race_test PRIVATE c_std_11)
target_compile_definitions(orm_runtime_race_test PRIVATE ORM_C_STATIC)
target_include_directories(orm_runtime_race_test PRIVATE
  ${ORM_INCLUDE_DIR} ${ORM_ABI_SOURCE_DIR}
  ${ORM_FLOW_SOURCE_DIR} ${ORM_RUNTIME_SOURCE_DIR})
orm_link_internal_test(orm_runtime_race_test
  Salts::TinyTest)
add_dependencies(orm_runtime_race_test
  orm_runtime_race_fixture orm_runtime_race_fixture_two)
set_target_properties(orm_runtime_race_test PROPERTIES
  C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_runtime_race_test)
if(MSVC)
  target_compile_options(orm_runtime_race_test PRIVATE /WX)
else()
  target_compile_options(orm_runtime_race_test PRIVATE -Werror)
endif()
add_test(NAME orm_runtime_race COMMAND orm_runtime_race_test)
set_tests_properties(orm_runtime_race PROPERTIES
  ENVIRONMENT
    "ORM_RUNTIME_RACE_FIXTURE=$<TARGET_FILE:orm_runtime_race_fixture>;ORM_RUNTIME_RACE_FIXTURE_TWO=$<TARGET_FILE:orm_runtime_race_fixture_two>;ORM_RUNTIME_RACE_GATE_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/runtime-race-gate")

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

add_executable(orm_redis_flow_test
               tests/flow/orm_redis_flow_test.c
               src/dbs/redis/orm_redis_cursor.c)
target_compile_features(orm_redis_flow_test PRIVATE c_std_11)
target_compile_definitions(orm_redis_flow_test PRIVATE ORM_C_STATIC)
target_include_directories(
  orm_redis_flow_test PRIVATE ${ORM_INCLUDE_DIR} ${ORM_FLOW_SOURCE_DIR}
                              ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/redis)
orm_link_internal_test(orm_redis_flow_test Salts::DataBind
                       Salts::CFlow Salts::TinyTest)
set_target_properties(orm_redis_flow_test
                      PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_redis_flow_test)
if(MSVC)
  target_compile_options(orm_redis_flow_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_redis_flow COMMAND orm_redis_flow_test)

add_executable(orm_mongo_flow_test
               tests/flow/orm_mongo_flow_test.c
               src/dbs/mongo/orm_mongo_cursor.c)
target_compile_features(orm_mongo_flow_test PRIVATE c_std_11)
target_compile_definitions(orm_mongo_flow_test PRIVATE ORM_C_STATIC)
target_include_directories(
  orm_mongo_flow_test PRIVATE ${ORM_INCLUDE_DIR} ${ORM_FLOW_SOURCE_DIR}
                              ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/mongo)
orm_link_internal_test(orm_mongo_flow_test Salts::DataBind
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

add_executable(orm_tidesdb_flow_test
               tests/flow/orm_tidesdb_flow_test.c
               src/dbs/tidesdb/orm_tidesdb_cursor.c)
target_compile_features(orm_tidesdb_flow_test PRIVATE c_std_11)
target_compile_definitions(orm_tidesdb_flow_test PRIVATE ORM_C_STATIC)
target_include_directories(
  orm_tidesdb_flow_test PRIVATE ${ORM_INCLUDE_DIR} ${ORM_FLOW_SOURCE_DIR}
                                ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/tidesdb)
orm_link_internal_test(orm_tidesdb_flow_test Salts::DataBind
                       Salts::CFlow Salts::TinyTest)
set_target_properties(orm_tidesdb_flow_test
                      PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_tidesdb_flow_test)
if(MSVC)
  target_compile_options(orm_tidesdb_flow_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_tidesdb_flow COMMAND orm_tidesdb_flow_test)

if(ORM_BUILD_TIDESDB_DRIVER)
  add_executable(orm_tidesdb_public_flow_test
                 tests/integration/tidesdb_public_flow_test.c)
  target_compile_features(orm_tidesdb_public_flow_test PRIVATE c_std_11)
  target_compile_definitions(orm_tidesdb_public_flow_test PRIVATE ORM_C_STATIC)
  target_include_directories(orm_tidesdb_public_flow_test PRIVATE ${ORM_INCLUDE_DIR})
  target_link_libraries(
    orm_tidesdb_public_flow_test PRIVATE turbo_orm
                                         Salts::CFlow Salts::TinyTest)
  add_dependencies(orm_tidesdb_public_flow_test orm_tidesdb_driver)
  set_target_properties(orm_tidesdb_public_flow_test
                        PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
  orm_enable_strict_warnings(orm_tidesdb_public_flow_test)
  if(MSVC)
    target_compile_options(orm_tidesdb_public_flow_test
                           PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
  endif()
  add_test(NAME orm_tidesdb_public_flow COMMAND orm_tidesdb_public_flow_test)
  set_tests_properties(orm_tidesdb_public_flow PROPERTIES
    ENVIRONMENT "ORM_TIDESDB_PLUGIN=$<TARGET_FILE:orm_tidesdb_driver>")
endif()

if(ORM_BUILD_SQLITE_DRIVER)
add_executable(orm_sqlite_flow_test tests/flow/orm_sqlite_flow_test.c)
target_compile_features(orm_sqlite_flow_test PRIVATE c_std_11)
target_compile_definitions(orm_sqlite_flow_test PRIVATE ORM_C_STATIC)
target_include_directories(
  orm_sqlite_flow_test PRIVATE ${ORM_INCLUDE_DIR}
                               ${ORM_FLOW_SOURCE_DIR}
                               ${CMAKE_CURRENT_SOURCE_DIR}/src/dbs/sqlite)
orm_link_internal_test(orm_sqlite_flow_test SQLite::SQLite3 Salts::DataBind
                       Salts::CFlow Salts::TinyTest)
set_target_properties(orm_sqlite_flow_test PROPERTIES C_STANDARD_REQUIRED ON
                                                       FOLDER "orm/tests")
orm_enable_strict_warnings(orm_sqlite_flow_test)
if(MSVC)
  target_compile_options(orm_sqlite_flow_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_sqlite_flow COMMAND orm_sqlite_flow_test)

add_executable(orm_sqlite_plugin_test
  tests/driver/orm_sqlite_plugin_test.c)
target_compile_features(orm_sqlite_plugin_test PRIVATE c_std_11)
target_compile_definitions(orm_sqlite_plugin_test PRIVATE ORM_C_STATIC)
target_include_directories(orm_sqlite_plugin_test PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(orm_sqlite_plugin_test PRIVATE
  turbo_orm Salts::TinyTest)
add_dependencies(orm_sqlite_plugin_test orm_sqlite_driver)
set_target_properties(orm_sqlite_plugin_test PROPERTIES
  C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_sqlite_plugin_test)
if(MSVC)
  target_compile_options(orm_sqlite_plugin_test PRIVATE /WX)
else()
  target_compile_options(orm_sqlite_plugin_test PRIVATE -Werror)
endif()
add_test(NAME orm_sqlite_plugin COMMAND orm_sqlite_plugin_test)
set_tests_properties(orm_sqlite_plugin PROPERTIES
  ENVIRONMENT "ORM_SQLITE_PLUGIN=$<TARGET_FILE:orm_sqlite_driver>")

add_executable(orm_public_flow_test tests/flow/orm_public_flow_test.c)
target_compile_features(orm_public_flow_test PRIVATE c_std_11)
target_compile_definitions(orm_public_flow_test PRIVATE ORM_C_STATIC)
target_include_directories(orm_public_flow_test PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(orm_public_flow_test PRIVATE orm_c_test_static
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
                      PRIVATE orm_c_test_static Salts::TinyTest)
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

add_executable(orm_cpp_flow_test tests/flow/orm_cpp_flow_test.cpp)
target_compile_features(orm_cpp_flow_test PRIVATE cxx_std_17)
target_compile_definitions(orm_cpp_flow_test PRIVATE ORM_C_STATIC)
target_include_directories(orm_cpp_flow_test PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(orm_cpp_flow_test PRIVATE orm_c_test_static
                                                Salts::CFlow
                                                Salts::TinyTest)
set_target_properties(orm_cpp_flow_test
                      PROPERTIES CXX_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_cpp_flow_test)
add_test(NAME orm_cpp_flow COMMAND orm_cpp_flow_test)

add_executable(orm_public_ownership_test
               tests/flow/orm_public_ownership_test.c)
target_compile_features(orm_public_ownership_test PRIVATE c_std_11)
target_compile_definitions(orm_public_ownership_test PRIVATE ORM_C_STATIC)
target_include_directories(orm_public_ownership_test PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(orm_public_ownership_test
                      PRIVATE orm_c_test_static Salts::CFlow Salts::TinyTest)
set_target_properties(orm_public_ownership_test
                      PROPERTIES C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_public_ownership_test)
if(MSVC)
  target_compile_options(orm_public_ownership_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()
add_test(NAME orm_public_ownership COMMAND orm_public_ownership_test)

add_executable(orm_public_ownership_cpp_test
               tests/flow/orm_public_ownership_cpp_test.cpp)
target_compile_features(orm_public_ownership_cpp_test PRIVATE cxx_std_17)
target_compile_definitions(orm_public_ownership_cpp_test PRIVATE ORM_C_STATIC)
target_include_directories(orm_public_ownership_cpp_test PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(orm_public_ownership_cpp_test
                      PRIVATE orm_c_test_static Salts::TinyTest)
set_target_properties(orm_public_ownership_cpp_test
                      PROPERTIES CXX_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_public_ownership_cpp_test)
add_test(NAME orm_public_ownership_cpp COMMAND orm_public_ownership_cpp_test)

endif()

if(ORM_BUILD_REDIS_DRIVER)
add_executable(orm_redis_query_test
               tests/abi/redis_query_test.c
               src/dbs/redis/query.c)
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
target_include_directories(orm_redis_backend_contract_test PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(orm_redis_backend_contract_test PRIVATE
                      turbo_orm Salts::Core)
add_dependencies(orm_redis_backend_contract_test orm_redis_driver)
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
set_tests_properties(orm_redis_backend_contract PROPERTIES
  ENVIRONMENT "ORM_REDIS_PLUGIN=$<TARGET_FILE:orm_redis_driver>")

add_executable(orm_redis_live_driver_test
               tests/integration/redis_live_driver_test.c)
target_compile_features(orm_redis_live_driver_test PRIVATE c_std_11)
target_compile_definitions(orm_redis_live_driver_test PRIVATE ORM_C_STATIC)
target_include_directories(orm_redis_live_driver_test PRIVATE ${ORM_INCLUDE_DIR})
target_link_libraries(orm_redis_live_driver_test PRIVATE
                      turbo_orm Salts::CFlow Salts::Platform)
add_dependencies(orm_redis_live_driver_test orm_redis_driver)
set_target_properties(orm_redis_live_driver_test PROPERTIES
                      C_STANDARD_REQUIRED ON FOLDER "orm/tests")
orm_enable_strict_warnings(orm_redis_live_driver_test)
if(ORM_REDIS_LIVE_TESTS)
  add_test(NAME orm_redis_live_driver COMMAND orm_redis_live_driver_test)
  set_tests_properties(orm_redis_live_driver PROPERTIES
    ENVIRONMENT "ORM_REDIS_PLUGIN=$<TARGET_FILE:orm_redis_driver>;ORM_REDIS_PORT=$ENV{ORM_REDIS_PORT}")
endif()

endif()

if(WIN32)
  set(ORM_TEST_RUNTIME_PATH
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::Core>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::DataBind>")
  set(ORM_RUNTIME_TESTS orm_redis_flow orm_mongo_flow orm_tidesdb_flow
                        orm_row_publisher orm_pure_c_build orm_sql_render
                        orm_driver_plan_view orm_driver_owner_bridge
                        orm_postgres_flow orm_backend_profile)
  if(ORM_BUILD_SQLITE_DRIVER)
    string(APPEND ORM_TEST_RUNTIME_PATH
           ";PATH=path_list_prepend:$<TARGET_FILE_DIR:SQLite::SQLite3>")
    if(SQLite3_LIBRARY)
      get_filename_component(ORM_TEST_SQLITE_LIBRARY_DIR
                             "${SQLite3_LIBRARY}" DIRECTORY)
      get_filename_component(ORM_TEST_SQLITE_PREFIX_DIR
                             "${ORM_TEST_SQLITE_LIBRARY_DIR}" DIRECTORY)
      if(EXISTS "${ORM_TEST_SQLITE_PREFIX_DIR}/bin")
        string(APPEND ORM_TEST_RUNTIME_PATH
               ";PATH=path_list_prepend:${ORM_TEST_SQLITE_PREFIX_DIR}/bin")
      endif()
    endif()
    list(APPEND ORM_RUNTIME_TESTS orm_sqlite_flow orm_sqlite_plugin
                                  orm_public_flow
                                  orm_sqlite_provider_atomicity orm_cpp_flow
                                  orm_public_ownership orm_public_ownership_cpp)
  endif()
  if(ORM_BUILD_POSTGRESQL_DRIVER OR ORM_BUILD_LEGACY_POSTGRESQL_COMPONENT)
    string(APPEND ORM_TEST_RUNTIME_PATH
           ";PATH=path_list_prepend:$<TARGET_FILE_DIR:PostgreSQL::PostgreSQL>")
    list(APPEND ORM_RUNTIME_TESTS orm_postgres_libpq)
    if(ORM_BUILD_SQLITE_DRIVER)
      list(APPEND ORM_RUNTIME_TESTS
           orm_sql_plugin_matrix_sqlite
           orm_sql_plugin_matrix_postgresql
           orm_sql_plugin_matrix_both)
    endif()
    if(ORM_POSTGRES_LIVE_TESTS)
      list(APPEND ORM_RUNTIME_TESTS orm_postgres_live)
    endif()
  endif()
  if(ORM_BUILD_REDIS_DRIVER)
    string(
      APPEND
      ORM_TEST_RUNTIME_PATH
      ";PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboDB::Redis>"
    )
    list(APPEND ORM_RUNTIME_TESTS orm_redis_query orm_redis_backend_contract)
  endif()
  if(ORM_BUILD_TIDESDB_DRIVER)
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
  if(ORM_BUILD_MONGODB_DRIVER)
    if(TARGET mongoc::shared)
      string(APPEND ORM_TEST_RUNTIME_PATH
             ";PATH=path_list_prepend:$<TARGET_FILE_DIR:mongoc::shared>")
    endif()
    if(TARGET bson::shared)
      string(APPEND ORM_TEST_RUNTIME_PATH
             ";PATH=path_list_prepend:$<TARGET_FILE_DIR:bson::shared>")
    endif()
    list(APPEND ORM_RUNTIME_TESTS orm_mongo_query orm_mongodb_plugin)
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

if(TARGET orm_sqlite_driver)
  set(ORM_SQLITE_DRIVER_RUNTIME_SEARCH_DIRECTORIES)
  orm_collect_runtime_search_directories(
    SQLite::SQLite3 ORM_SQLITE_DRIVER_RUNTIME_SEARCH_DIRECTORIES)
  if(WIN32 AND SQLite3_LIBRARY)
    get_filename_component(ORM_SQLITE_DRIVER_LIBRARY_DIR
                           "${SQLite3_LIBRARY}" DIRECTORY)
    get_filename_component(ORM_SQLITE_DRIVER_PREFIX_DIR
                           "${ORM_SQLITE_DRIVER_LIBRARY_DIR}" DIRECTORY)
    list(APPEND ORM_SQLITE_DRIVER_RUNTIME_SEARCH_DIRECTORIES
         "${ORM_SQLITE_DRIVER_PREFIX_DIR}/bin")
  endif()
  list(REMOVE_DUPLICATES ORM_SQLITE_DRIVER_RUNTIME_SEARCH_DIRECTORIES)
  install(
    TARGETS orm_sqlite_driver
    RUNTIME_DEPENDENCY_SET OrmSqliteDriverRuntimeDependencies
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/turbodb/drivers
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/turbodb/drivers)
  if(WIN32)
    install(
      RUNTIME_DEPENDENCY_SET OrmSqliteDriverRuntimeDependencies
      DIRECTORIES ${ORM_SQLITE_DRIVER_RUNTIME_SEARCH_DIRECTORIES}
      PRE_EXCLUDE_REGEXES "api-ms-win-.*" "ext-ms-win-.*"
                          "clang_rt.asan_dynamic-.*"
                          "turbo_orm\\.dll" "salts_.*\\.dll"
      POST_EXCLUDE_REGEXES ".*[\\/]system32[\\/].*"
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/turbodb/drivers)
  else()
    install(
      RUNTIME_DEPENDENCY_SET OrmSqliteDriverRuntimeDependencies
      DIRECTORIES ${ORM_SQLITE_DRIVER_RUNTIME_SEARCH_DIRECTORIES}
      PRE_EXCLUDE_REGEXES "libturbo_orm\\..*" "libsalts_.*"
      POST_EXCLUDE_REGEXES "^/lib/.*" "^/usr/lib/.*"
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/turbodb/drivers)
  endif()
endif()
if(TARGET orm_postgresql_driver)
  install(
    TARGETS orm_postgresql_driver
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/turbodb/drivers
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/turbodb/drivers)
endif()

cmake_install_headers(
  FILES include/orm/orm.h include/orm/orm.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/shared/orm)
if(ORM_BUILD_LEGACY_POSTGRESQL_COMPONENT)
  cmake_install_headers(
    FILES include/orm/orm_postgresql.h include/orm/orm_postgresql.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/shared/orm)
endif()

cmake_config_package(PACKAGE Orm VERSION "${ORM_VERSION}")

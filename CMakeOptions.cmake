include(CMakeDependentOption)

set(CMAKE_COLOR_DIAGNOSTICS ON)

# building the tests
option(ENABLE_TESTS "Enable the tests" ON)

# Address Sanitizer - only enabled for Debug builds
cmake_dependent_option(ENABLE_ASAN "Enable Address Sanitizer" ON
                       "CMAKE_BUILD_TYPE STREQUAL Debug" OFF)

# if(MSVC) add_compile_options(/bigobj) endif()

option(BUILD_EXAMPLES "Build example programs" ON)
option(BUILD_TESTS "Build test suite" ${ENABLE_TESTS})
option(ORM_BUILD_TESTS "Build ORM test suite" ${BUILD_TESTS})

if(CMAKE_CROSSCOMPILING)
  set(TURBODB_BUILD_DBTOOLS_DEFAULT OFF)
else()
  set(TURBODB_BUILD_DBTOOLS_DEFAULT ON)
endif()
option(TURBODB_BUILD_DBTOOLS "Build standalone database tools"
       ${TURBODB_BUILD_DBTOOLS_DEFAULT})
option(TURBODB_DBTOOLS_WITH_PGSQL
       "Enable PostgreSQL standalone database tools" ON)
option(TURBODB_DBTOOLS_PG_LIVE_TESTS
       "Run standalone database tools against a live PostgreSQL server" OFF)
option(TURBODB_BUILD_REDIS "Build the Redis client library" ON)

# tidesdb compression backends -- only Zstd by default; Snappy and LZ4 are
# opt-in. Override per build with -DTIDESDB_WITH_SNAPPY=ON /
# -DTIDESDB_WITH_LZ4=ON or a preset cacheVariable.
set(TIDESDB_WITH_SNAPPY OFF CACHE BOOL "build with Snappy compression support")
set(TIDESDB_WITH_LZ4 OFF CACHE BOOL "build with LZ4 compression support")

# ORM core stays database-independent. Database Drivers are build products;
# native dependencies remain private to their modules. The direct PostgreSQL
# connector is a separately named 2.x compatibility component.
set(ORM_BUILD_SQLITE_DRIVER_DEFAULT ON)
set(ORM_BUILD_POSTGRESQL_DRIVER_DEFAULT ON)
set(ORM_BUILD_LEGACY_POSTGRESQL_COMPONENT_DEFAULT ON)
set(ORM_BUILD_REDIS_DRIVER_DEFAULT OFF)
set(ORM_BUILD_MONGODB_DRIVER_DEFAULT OFF)

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

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

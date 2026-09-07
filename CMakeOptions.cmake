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

# ORM always ships its embedded TidesDB backend. SQLite and PostgreSQL remain
# enabled by default; native driver dependencies stay private to components.
set(ORM_WITH_SQLITE_DEFAULT ON)
set(ORM_WITH_PGSQL_DEFAULT ON)
set(ORM_WITH_REDIS_DEFAULT OFF)
set(ORM_WITH_MONGODB_DEFAULT OFF)

option(ORM_WITH_PGSQL "Enable PostgreSQL backend for ORM" ${ORM_WITH_PGSQL_DEFAULT})
option(ORM_WITH_REDIS "Enable Redis backend for ORM" ${ORM_WITH_REDIS_DEFAULT})
option(ORM_WITH_MONGODB "Enable MongoDB backend for ORM" ${ORM_WITH_MONGODB_DEFAULT})

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

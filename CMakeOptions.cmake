set(CMAKE_COLOR_DIAGNOSTICS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(BUILD_EXAMPLES "Build example programs" ON)
option(BUILD_TESTS "Build test suite" ON)
option(BUILD_E2E_TESTS "Build end-to-end tests against external databases" OFF)

option(TURBODB_BUILD_ORM "Build ORM libraries and database drivers" ON)
option(TURBODB_BUILD_REDIS "Build the standalone Redis client" ON)
option(TURBODB_BUILD_DBTOOLS "Build standalone database tools" ON)
option(TURBODB_BUILD_SQLPARSER "Build the standalone re2c/Lemon SQL parser" ON)
option(TURBODB_BUILD_APP "Build the TurboDB Studio Windows desktop application" OFF)

if(TURBODB_BUILD_APP)
  if(NOT CMAKE_HOST_WIN32)
    message(FATAL_ERROR
      "TURBODB_BUILD_APP is Windows-only; configure on a Windows host or disable it")
  endif()
  if(NOT TURBODB_BUILD_ORM)
    message(FATAL_ERROR
      "TURBODB_BUILD_APP requires TURBODB_BUILD_ORM=ON")
  endif()
endif()

option(TURBODB_BUILD_TIDESSQL "Build the TidesSQL execution engine" ${TURBODB_BUILD_ORM})
option(TURBODB_BUILD_TIDESSQL_SERVER "Build the standalone tidessqld MySQL/TLS server" OFF)

# The toolchain reads manifest features during project().
set(VCPKG_MANIFEST_FEATURES "")
if(TURBODB_BUILD_ORM OR
   (TURBODB_BUILD_DBTOOLS AND (BUILD_TESTS OR BUILD_E2E_TESTS)) OR
   (TURBODB_BUILD_SQLPARSER AND BUILD_TESTS))
  list(APPEND VCPKG_MANIFEST_FEATURES sqlite)
endif()
if(TURBODB_BUILD_ORM OR
   (TURBODB_BUILD_DBTOOLS AND (BUILD_TESTS OR BUILD_E2E_TESTS)))
  list(APPEND VCPKG_MANIFEST_FEATURES postgresql)
endif()
if(TURBODB_BUILD_APP)
  list(APPEND VCPKG_MANIFEST_FEATURES app)
  list(APPEND VCPKG_OVERLAY_PORTS
       "${CMAKE_CURRENT_LIST_DIR}/app/vcpkg-ports")
endif()

option(ENABLE_SANITIZER_ADDRESS "Enable AddressSanitizer" OFF)
option(ENABLE_SANITIZER_UNDEFINED "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_SANITIZER_LEAK "Enable LeakSanitizer" OFF)
option(ENABLE_SANITIZER_THREAD "Enable ThreadSanitizer" OFF)
option(ENABLE_SANITIZER_MEMORY "Enable MemorySanitizer (Clang only)" OFF)

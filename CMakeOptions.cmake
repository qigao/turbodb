set(CMAKE_COLOR_DIAGNOSTICS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(BUILD_EXAMPLES "Build example programs" ON)
option(BUILD_TESTS "Build test suite" ON)
option(BUILD_E2E_TESTS "Build end-to-end tests against external databases" OFF)

option(TURBODB_BUILD_ORM "Build ORM libraries and database drivers" ON)
option(TURBODB_BUILD_DBTOOLS "Build standalone database tools" ON)
option(TURBODB_BUILD_SQLPARSER "Build the standalone re2c/Lemon SQL parser" ON)

# The toolchain reads manifest features during project().
set(VCPKG_MANIFEST_FEATURES "")
if(TURBODB_BUILD_ORM OR
   (TURBODB_BUILD_DBTOOLS AND (BUILD_TESTS OR BUILD_E2E_TESTS)))
  list(APPEND VCPKG_MANIFEST_FEATURES sqlite)
  list(APPEND VCPKG_MANIFEST_FEATURES postgresql)
endif()

option(ENABLE_SANITIZER_ADDRESS "Enable AddressSanitizer" OFF)
option(ENABLE_SANITIZER_UNDEFINED "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_SANITIZER_LEAK "Enable LeakSanitizer" OFF)
option(ENABLE_SANITIZER_THREAD "Enable ThreadSanitizer" OFF)
option(ENABLE_SANITIZER_MEMORY "Enable MemorySanitizer (Clang only)" OFF)

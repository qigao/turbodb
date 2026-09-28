cmake_minimum_required(VERSION 3.21)
# Development/test builds never inject vcpkg applocal deployment into link steps.
set(VCPKG_APPLOCAL_DEPS OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
if(POLICY CMP0144)
  cmake_policy(SET CMP0144 NEW)
endif()

option(ORM_BUILD_MONGODB_DRIVER
       "Build the independent MongoDB TurboDb.Driver module" OFF)
if(ORM_BUILD_MONGODB_DRIVER)
  list(APPEND VCPKG_MANIFEST_FEATURES mongodb)
endif()

option(ORM_BUILD_SQLITE_DRIVER
       "Build the independent SQLite TurboDb.Driver module" ON)
option(TURBODB_DBTOOLS_WITH_SQLITE
       "Enable SQLite standalone database tools" ON)
if(ORM_BUILD_SQLITE_DRIVER OR TURBODB_DBTOOLS_WITH_SQLITE)
  list(APPEND VCPKG_MANIFEST_FEATURES sqlite)
endif()

option(ORM_BUILD_POSTGRESQL_DRIVER
       "Build the independent PostgreSQL TurboDb.Driver module" ON)
option(ORM_BUILD_LEGACY_POSTGRESQL_COMPONENT
       "Build the legacy Orm::PostgreSQL direct connector compatibility component" ON)
option(TURBODB_DBTOOLS_WITH_PGSQL
       "Enable PostgreSQL standalone database tools" ON)
if(ORM_BUILD_POSTGRESQL_DRIVER OR
   ORM_BUILD_LEGACY_POSTGRESQL_COMPONENT OR
   TURBODB_DBTOOLS_WITH_PGSQL)
  list(APPEND VCPKG_MANIFEST_FEATURES postgresql)
endif()

project(
  TurboDB
  VERSION 1.0.0
  DESCRIPTION "C/C++ database libraries"
  HOMEPAGE_URL "https://github.com/qigao/turbodb"
  LANGUAGES C CXX)

# Project configuration - output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Normalize install prefix (remove trailing slashes)
get_filename_component(CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}" ABSOLUTE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
# Language standards - unified approach
set(CMAKE_CXX_STANDARD 17) # C++17 for compatibility
set(CMAKE_C_STANDARD 11) # C17 to ensure C11 atomic support
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  add_compile_options(/experimental:c11atomics)
endif()
# Enable folder organization in IDEs
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "cmake")

# Enable testing
include(CTest)
enable_testing()

if(BUILD_TESTING)
  add_test(
    NAME turbodb_install_profile_contract
    COMMAND
      ${CMAKE_COMMAND}
      "-DTURBODB_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      -P
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/validate_install_profiles.cmake")
endif()

include(GNUInstallDirs)
include(CMakeOptions.cmake)
include(Sanitizers)
include(CmakeUtils)

if(NOT DEFINED ENV{SALTS_ROOT} OR "$ENV{SALTS_ROOT}" STREQUAL "")
  message(FATAL_ERROR "SALTS_ROOT must name the installed Salts profile")
endif()
file(TO_CMAKE_PATH "$ENV{SALTS_ROOT}" SALTS_ROOT_PATH)
if(NOT IS_DIRECTORY "${SALTS_ROOT_PATH}")
  message(FATAL_ERROR "SALTS_ROOT is not a directory: $ENV{SALTS_ROOT}")
endif()
unset(Salts_DIR CACHE)
unset(Salts_DIR)
find_package(Salts 1.8.3 EXACT CONFIG REQUIRED PATHS "${SALTS_ROOT_PATH}"
             NO_DEFAULT_PATH)
foreach(required_salts_target IN ITEMS Salts::PluginABI Salts::Plugin)
  if(NOT TARGET ${required_salts_target})
    message(FATAL_ERROR
            "The selected Salts 1.8.3 installation must export ${required_salts_target}")
  endif()
endforeach()

if(NOT DEFINED ENV{SALTS_UTILS_ROOT} OR "$ENV{SALTS_UTILS_ROOT}" STREQUAL "")
  message(FATAL_ERROR "SALTS_UTILS_ROOT must name the installed SaltsUtils profile")
endif()
file(TO_CMAKE_PATH "$ENV{SALTS_UTILS_ROOT}" SALTS_UTILS_ROOT_PATH)
if(NOT IS_DIRECTORY "${SALTS_UTILS_ROOT_PATH}")
  message(FATAL_ERROR "SALTS_UTILS_ROOT is not a directory: $ENV{SALTS_UTILS_ROOT}")
endif()
unset(SaltsUtils_DIR CACHE)
unset(SaltsUtils_DIR)
find_package(SaltsUtils 4.1.3 EXACT CONFIG REQUIRED PATHS "${SALTS_UTILS_ROOT_PATH}"
             NO_DEFAULT_PATH)
if(NOT TARGET Salts::DataBind)
  message(FATAL_ERROR "The selected SaltsUtils 4.1.3 installation must export Salts::DataBind")
endif()

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(zstd CONFIG REQUIRED)

# Additional components
if(CMAKE_CROSSCOMPILING)
  add_subdirectory(tools EXCLUDE_FROM_ALL)
else()
  add_subdirectory(tools)
endif()
add_subdirectory(dbtools)
include(FindTools)
if(TURBODB_BUILD_REDIS)
  # Redis remains a standalone shared client library and is also the native
  # dependency of the runtime-loaded Redis TurboDb.Driver module. The Linux
  # hardening preset marks generic SHARED objects -z nodlopen, which is
  # incompatible with a DSO reached through dlopen(). Remove only that loader
  # prohibition in the Redis directory scope; keep RELRO/NOW/noexecstack and
  # restore the parent flags immediately afterwards.
  set(TURBODB_REDIS_SAVED_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    string(REPLACE ",-z,nodlopen" ""
           CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")
  endif()
  add_subdirectory(redis)
  set(CMAKE_SHARED_LINKER_FLAGS "${TURBODB_REDIS_SAVED_SHARED_LINKER_FLAGS}")
  unset(TURBODB_REDIS_SAVED_SHARED_LINKER_FLAGS)
endif()

# TidesDB is a private native dependency of the runtime-loaded
# TurboDb.Driver module. Keep it as a PIC static archive so global shared
# linker hardening such as -z nodlopen cannot make a transitive TidesDB DSO
# impossible for Salts::Plugin to load.
set(TURBODB_BUILD_SHARED_LIBS_WAS_DEFINED FALSE)
if(DEFINED BUILD_SHARED_LIBS)
  set(TURBODB_BUILD_SHARED_LIBS_WAS_DEFINED TRUE)
  set(TURBODB_SAVED_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")
endif()
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(tidesdb)
if(TURBODB_BUILD_SHARED_LIBS_WAS_DEFINED)
  set(BUILD_SHARED_LIBS "${TURBODB_SAVED_BUILD_SHARED_LIBS}")
else()
  unset(BUILD_SHARED_LIBS)
endif()
unset(TURBODB_BUILD_SHARED_LIBS_WAS_DEFINED)
unset(TURBODB_SAVED_BUILD_SHARED_LIBS)

add_subdirectory(orm)
# Root-enabled instrumentation also requires its runtime on consumer links.
# Directory link options do not propagate through the Orm::C target.
if(NOT MSVC AND SANITIZER_LINK_FLAGS)
  target_link_options(turbo_orm INTERFACE ${SANITIZER_LINK_FLAGS})
endif()
if(ORM_BUILD_TESTS AND ORM_BUILD_SQLITE_DRIVER)
  add_subdirectory(orm/tests/ownership)
endif()

if(TURBODB_BUILD_REDIS)
  cmake_config_package(
    PACKAGE
    TurboDB
    EXPORT_SET
    TurboDBTargets
    NAMESPACE
    TurboDB::
    CONFIG_TEMPLATE
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TurboDBConfig.cmake.in"
    VERSION
    "${PROJECT_VERSION}"
    COMPATIBILITY
    SameMajorVersion)
endif()

cmake_minimum_required(VERSION 3.21)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
if(POLICY CMP0144)
  cmake_policy(SET CMP0144 NEW)
endif()

option(ORM_WITH_MONGODB "Enable MongoDB backend" OFF)
if(ORM_WITH_MONGODB)
  list(APPEND VCPKG_MANIFEST_FEATURES mongodb)
endif()

option(ORM_WITH_SQLITE "Enable SQLite backend" ON)
option(TURBODB_DBTOOLS_WITH_SQLITE
       "Enable SQLite standalone database tools" ON)
if(ORM_WITH_SQLITE OR TURBODB_DBTOOLS_WITH_SQLITE)
  list(APPEND VCPKG_MANIFEST_FEATURES sqlite)
endif()

option(ORM_WITH_PGSQL "Enable PostgreSQL backend" ON)
option(TURBODB_DBTOOLS_WITH_PGSQL
       "Enable PostgreSQL standalone database tools" ON)
if(ORM_WITH_PGSQL OR TURBODB_DBTOOLS_WITH_PGSQL)
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
find_package(Salts CONFIG REQUIRED PATHS "${SALTS_ROOT_PATH}"
             NO_DEFAULT_PATH)

if(NOT DEFINED ENV{SALTS_UTILS_ROOT} OR "$ENV{SALTS_UTILS_ROOT}" STREQUAL "")
  message(FATAL_ERROR "SALTS_UTILS_ROOT must name the installed SaltsUtils profile")
endif()
file(TO_CMAKE_PATH "$ENV{SALTS_UTILS_ROOT}" SALTS_UTILS_ROOT_PATH)
if(NOT IS_DIRECTORY "${SALTS_UTILS_ROOT_PATH}")
  message(FATAL_ERROR "SALTS_UTILS_ROOT is not a directory: $ENV{SALTS_UTILS_ROOT}")
endif()
unset(SaltsUtils_DIR CACHE)
unset(SaltsUtils_DIR)
find_package(SaltsUtils CONFIG REQUIRED PATHS "${SALTS_UTILS_ROOT_PATH}"
             NO_DEFAULT_PATH)
if(NOT TARGET Salts::DataBind)
  message(FATAL_ERROR "The selected SaltsUtils installation must export Salts::DataBind")
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
  add_subdirectory(redis)
endif()

add_subdirectory(tidesdb)
add_subdirectory(orm)
# Root-enabled instrumentation also requires its runtime on consumer links.
# Directory link options do not propagate through the Orm::C target.
if(NOT MSVC AND SANITIZER_LINK_FLAGS)
  target_link_options(turbo_orm INTERFACE ${SANITIZER_LINK_FLAGS})
endif()
if(ORM_BUILD_TESTS AND ORM_WITH_SQLITE)
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

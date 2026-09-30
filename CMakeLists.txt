cmake_minimum_required(VERSION 3.21)
# Development/test builds never inject vcpkg applocal deployment into link
# steps.
set(VCPKG_APPLOCAL_DEPS
    OFF
    CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
if(POLICY CMP0144)
  cmake_policy(SET CMP0144 NEW)
endif()
include(CMakeOptions.cmake)

project(
  TurboDB
  VERSION 2.0.1
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
set(CMAKE_C_STANDARD 11) # C11 atomic support
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

include(GNUInstallDirs)
include(Sanitizers)
include(CmakeUtils)
find_package(OpenSSL CONFIG REQUIRED)
find_package(
  Salts
  CONFIG
  REQUIRED
  PATHS
  "$ENV{SALTS_ROOT}"
  NO_DEFAULT_PATH
  NO_CMAKE_FIND_ROOT_PATH)

find_package(
  SaltsUtils
  CONFIG
  REQUIRED
  PATHS
  "$ENV{SALTS_UTILS_ROOT}"
  NO_DEFAULT_PATH
  NO_CMAKE_FIND_ROOT_PATH)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(zstd CONFIG REQUIRED)

# Additional components
if(CMAKE_CROSSCOMPILING)
  add_subdirectory(tools EXCLUDE_FROM_ALL)
else()
  add_subdirectory(tools)
endif()

include(FindTools)

if(TURBODB_BUILD_SQLPARSER)
  add_subdirectory(sqlparser)
endif()

if(TURBODB_BUILD_ORM OR TURBODB_BUILD_DBTOOLS)
  add_subdirectory(drivers/schema)
endif()

if(TURBODB_BUILD_ORM)
  add_subdirectory(redis)
  add_subdirectory(tidesdb)
  add_subdirectory(mysql)
  add_subdirectory(orm)
  add_subdirectory(drivers)
endif()

if(TURBODB_BUILD_DBTOOLS)
  add_subdirectory(dbtools)
endif()

if(TURBODB_BUILD_ORM OR TURBODB_BUILD_DBTOOLS OR TURBODB_BUILD_SQLPARSER)
  include(CMakePackageConfigHelpers)
  configure_package_config_file(
    cmake/TurboDBConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/TurboDBConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/TurboDB")
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/TurboDBConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)
  install(
    EXPORT TurboDBTargets
    NAMESPACE TurboDB::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/TurboDB")
  if(TURBODB_BUILD_ORM)
    include(cmake/InstallCrypto.cmake)
    install(
      EXPORT OrmTargets
      NAMESPACE Orm::
      DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/TurboDB")
  endif()
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/TurboDBConfig.cmake"
                "${CMAKE_CURRENT_BINARY_DIR}/TurboDBConfigVersion.cmake"
          DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/TurboDB")
endif()

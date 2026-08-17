cmake_minimum_required(VERSION 3.20)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
if(POLICY CMP0144)
  cmake_policy(SET CMP0144 NEW)
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

include(GNUInstallDirs)
include(CMakeOptions.cmake)
include(Sanitizers)
include(CmakeUtils)

find_package(ZLIB REQUIRED)
find_package(TurboParser CONFIG REQUIRED PATHS "${TURBOPARSER_ROOT}")
find_package(TurboUtils REQUIRED PATHS "${TURBO_UTILS_ROOT}")
find_package(TurboNet REQUIRED CONFIG PATHS "${TURBO_NET_ROOT}" NO_DEFAULT_PATH)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# Additional components
if(CMAKE_CROSSCOMPILING)
  add_subdirectory(tools EXCLUDE_FROM_ALL)
else()
  add_subdirectory(tools)
endif()
include(FindTools)
add_subdirectory(redis) 

add_subdirectory(tidesdb)
add_subdirectory(orm)

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

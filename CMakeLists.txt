cmake_minimum_required(VERSION 3.27)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
cmake_policy(SET CMP0144 NEW)
include(CMakeOptions.cmake)

project(
  TurboDB
  VERSION 2.3.4
  DESCRIPTION "C/C++ database libraries"
  HOMEPAGE_URL "https://github.com/qigao/turbodb"
  LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
# Language standards - unified approach
set(CMAKE_CXX_STANDARD 17) # C++17 for compatibility
set(CMAKE_C_STANDARD 11) # C11 atomic support
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Enable folder organization in IDEs
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "cmake")

# Register tests selected by the project options.
if(BUILD_TESTS OR BUILD_BENCHMARK)
  enable_testing()
endif()

include(GNUInstallDirs)
include(Sanitizers)
include(CmakeUtils)

find_package(SaltsBase "$ENV{SALTS_BASE_VERSION}" EXACT CONFIG REQUIRED
  PATHS "$ENV{SALTS_BASE_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(CMeta "$ENV{CMETA_VERSION}" EXACT CONFIG REQUIRED
  PATHS "$ENV{CMETA_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(SaltsUtils "$ENV{SALTS_UTILS_VERSION}" EXACT CONFIG REQUIRED
  PATHS "$ENV{SALTS_UTILS_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(SaltsCore "$ENV{SALTS_CORE_VERSION}" EXACT CONFIG REQUIRED
  PATHS "$ENV{SALTSCORE_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

add_library(turbodb_types INTERFACE)
target_include_directories(turbodb_types INTERFACE
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(turbodb_types INTERFACE Salts::Core)
cmake_config_target(turbodb_types ALIAS TurboDB::Types EXPORT_SET TurboDBTargets
  EXPORT_NAME Types FOLDER types NO_VERSION)
cmake_install_headers(FILES include/turbodb/types.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/turbodb)


add_subdirectory(tools EXCLUDE_FROM_ALL)
include(FindTools)
add_subdirectory(sqlparser)
add_subdirectory(drivers/schema)
add_subdirectory(redis)
add_subdirectory(mysql)
add_subdirectory(orm)
if(WIN32)
  add_subdirectory(app)
endif()
add_subdirectory(dbtools)

if(WIN32)
  include(TurboDBRuntime)
endif()

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
install(
      EXPORT OrmTargets
      NAMESPACE Orm::
      DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/TurboDB")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/TurboDBConfig.cmake"
                "${CMAKE_CURRENT_BINARY_DIR}/TurboDBConfigVersion.cmake"
          DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/TurboDB")

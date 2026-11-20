cmake_minimum_required(VERSION 3.27)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
cmake_policy(SET CMP0144 NEW)
include(CMakeOptions.cmake)

project(
  TurboDB
  VERSION 0.1.3
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

# Compiler selection belongs to presets; consume the prepared vcpkg triplet.
list(PREPEND CMAKE_PREFIX_PATH
  "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$ENV{VCPKG_CONFIGURATION_SUBDIR}"
  "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")

find_package(SaltsBase "$ENV{SALTS_BASE_VERSION}" EXACT CONFIG REQUIRED
  PATHS "$ENV{SALTS_BASE_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(CMeta "$ENV{CMETA_VERSION}" EXACT CONFIG REQUIRED
  PATHS "$ENV{CMETA_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(SaltsUtils "$ENV{SALTS_UTILS_VERSION}" EXACT CONFIG REQUIRED
  PATHS "$ENV{SALTS_UTILS_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
# MySQL authentication uses the public SHA-1 digest and secret-clear API.
if("$ENV{SALTS_CNET_VERSION}" VERSION_LESS "0.1.14")
  message(FATAL_ERROR
    "TurboDB requires SaltsCNet >= 0.1.14 for crypto/symmetric.h. "
    "Selected SALTS_CNET_VERSION='$ENV{SALTS_CNET_VERSION}', "
    "SALTS_CNET_ROOT='$ENV{SALTS_CNET_ROOT}'. "
    "Select a matching released SDK version/root and reconfigure with --fresh.")
endif()
find_package(SaltsCNet "$ENV{SALTS_CNET_VERSION}" EXACT CONFIG REQUIRED
  PATHS "$ENV{SALTS_CNET_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

if(UNIX)
  # The selected Unix triplets ship static libpq. Its own metadata owns the
  # pgcommon/pgport/system-library closure omitted by FindPostgreSQL.
  block(SCOPE_FOR VARIABLES)
    set(PKG_CONFIG_ARGN --static)
    pkg_check_modules(TurboDBPostgreSQL REQUIRED IMPORTED_TARGET GLOBAL
      "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/pkgconfig/libpq.pc")
    add_library(PostgreSQL::PostgreSQL ALIAS PkgConfig::TurboDBPostgreSQL)
  endblock()
endif()

if(ANDROID)
  add_link_options("LINKER:-z,max-page-size=16384")
endif()

add_library(turbodb_types INTERFACE)
configure_file(orm/types.h "${CMAKE_CURRENT_BINARY_DIR}/include/turbodb/types.h" COPYONLY)
target_include_directories(turbodb_types INTERFACE
  $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(turbodb_types INTERFACE Salts::Core)
cmake_config_target(turbodb_types ALIAS TurboDB::Types EXPORT_SET TurboDBTargets
  EXPORT_NAME Types FOLDER types NO_VERSION)
cmake_install_headers(FILES orm/types.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/turbodb)


include(FindTools)
add_subdirectory(sqlparser)
add_subdirectory(drivers)
add_subdirectory(orm)
if(WIN32)
  add_subdirectory(app)
endif()
add_subdirectory(dbtools)

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

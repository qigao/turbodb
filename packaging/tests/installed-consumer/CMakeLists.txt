cmake_minimum_required(VERSION 3.25)
project(TurboDBInstalledConsumer LANGUAGES C)

if(NOT DEFINED ENV{TURBODB_ROOT} OR "$ENV{TURBODB_ROOT}" STREQUAL "")
  message(FATAL_ERROR "TURBODB_ROOT must name the installed TurboDB SDK")
endif()

cmake_path(CONVERT "$ENV{TURBODB_ROOT}" TO_CMAKE_PATH_LIST
  _TurboDB_CONSUMER_ROOT NORMALIZE)
find_package(TidesSQL CONFIG REQUIRED
  PATHS "${_TurboDB_CONSUMER_ROOT}"
  NO_DEFAULT_PATH
  NO_CMAKE_FIND_ROOT_PATH)
find_package(TurboDB CONFIG REQUIRED
  PATHS "${_TurboDB_CONSUMER_ROOT}"
  NO_DEFAULT_PATH
  NO_CMAKE_FIND_ROOT_PATH)
unset(_TurboDB_CONSUMER_ROOT)

add_executable(turbodb_installed_consumer main.c)
target_compile_features(turbodb_installed_consumer PRIVATE c_std_11)
target_link_libraries(turbodb_installed_consumer PRIVATE
  Orm::C
  TurboDB::MySQL
  TurboDB::TidesSQL)

if(NOT DEFINED TurboDB_DRIVER_DIR)
  message(FATAL_ERROR "installed TurboDB package did not export TurboDB_DRIVER_DIR")
endif()
set(_TurboDB_MYSQL_DRIVER
  "${TurboDB_DRIVER_DIR}/turbodb_driver_mysql${CMAKE_SHARED_MODULE_SUFFIX}")
if(NOT EXISTS "${_TurboDB_MYSQL_DRIVER}")
  message(FATAL_ERROR "installed MySQL ORM driver is missing: ${_TurboDB_MYSQL_DRIVER}")
endif()
set(_TurboDB_REDIS_DRIVER
  "${TurboDB_DRIVER_DIR}/turbodb_driver_redis${CMAKE_SHARED_MODULE_SUFFIX}")
if(EXISTS "${_TurboDB_REDIS_DRIVER}")
  message(FATAL_ERROR "installed SDK must not contain a Redis ORM driver: ${_TurboDB_REDIS_DRIVER}")
endif()
target_compile_definitions(turbodb_installed_consumer PRIVATE
  TURBODB_INSTALLED_MYSQL_DRIVER="${_TurboDB_MYSQL_DRIVER}")
unset(_TurboDB_MYSQL_DRIVER)
unset(_TurboDB_REDIS_DRIVER)

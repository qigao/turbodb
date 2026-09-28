cmake_minimum_required(VERSION 3.25)
project(TurboDBDependencyContract LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT DEFINED ENV{SALTS_ROOT} OR "$ENV{SALTS_ROOT}" STREQUAL "")
  message(FATAL_ERROR "SALTS_ROOT must name the published Salts SDK root")
endif()
file(TO_CMAKE_PATH "$ENV{SALTS_ROOT}" SALTS_ROOT_PATH)
if(NOT IS_DIRECTORY "${SALTS_ROOT_PATH}")
  message(FATAL_ERROR "SALTS_ROOT is not a directory: $ENV{SALTS_ROOT}")
endif()

if(NOT DEFINED ENV{SALTS_UTILS_ROOT} OR "$ENV{SALTS_UTILS_ROOT}" STREQUAL "")
  message(FATAL_ERROR "SALTS_UTILS_ROOT must name the published SaltsUtils SDK root")
endif()
file(TO_CMAKE_PATH "$ENV{SALTS_UTILS_ROOT}" SALTS_UTILS_ROOT_PATH)
if(NOT IS_DIRECTORY "${SALTS_UTILS_ROOT_PATH}")
  message(FATAL_ERROR "SALTS_UTILS_ROOT is not a directory: $ENV{SALTS_UTILS_ROOT}")
endif()

unset(Salts_DIR CACHE)
unset(Salts_DIR)
find_package(Salts 1.8.3 EXACT CONFIG REQUIRED
  PATHS "${SALTS_ROOT_PATH}" NO_DEFAULT_PATH)

foreach(required_salts_target IN ITEMS
    Salts::CMeta
    Salts::PluginABI
    Salts::Plugin)
  if(NOT TARGET ${required_salts_target})
    message(FATAL_ERROR "published Salts 1.8.3 SDK is missing ${required_salts_target}")
  endif()
endforeach()

unset(SaltsUtils_DIR CACHE)
unset(SaltsUtils_DIR)
find_package(SaltsUtils 4.1.3 EXACT CONFIG REQUIRED
  PATHS "${SALTS_UTILS_ROOT_PATH}" NO_DEFAULT_PATH)

if(NOT TARGET Salts::DataBind)
  message(FATAL_ERROR "published SaltsUtils 4.1.3 SDK is missing Salts::DataBind")
endif()

add_executable(turbodb_reflection_plugin_contract
  reflection_plugin_contract.c
  reflection_plugin_peer_a.c
  reflection_plugin_peer_b.c)
target_link_libraries(turbodb_reflection_plugin_contract PRIVATE Salts::Plugin)

enable_testing()
add_test(NAME turbodb_reflection_plugin_contract
         COMMAND turbodb_reflection_plugin_contract)

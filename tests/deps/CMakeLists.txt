cmake_minimum_required(VERSION 3.25)
project(TurboDBDependencyContract LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

foreach(root_var IN ITEMS SALTS_ROOT SALTS_UTILS_ROOT)
  if(NOT DEFINED ENV{${root_var}} OR "$ENV{${root_var}}" STREQUAL "")
    message(FATAL_ERROR "${root_var} must name the published SDK root")
  endif()
  file(TO_CMAKE_PATH "$ENV{${root_var}}" ${root_var}_PATH)
  if(NOT IS_DIRECTORY "${${root_var}_PATH}")
    message(FATAL_ERROR "${root_var} is not a directory: $ENV{${root_var}}")
  endif()
endforeach()

unset(Salts_DIR CACHE)
unset(Salts_DIR)
find_package(Salts CONFIG REQUIRED
  PATHS "${SALTS_ROOT_PATH}" NO_DEFAULT_PATH)

unset(SaltsUtils_DIR CACHE)
unset(SaltsUtils_DIR)
find_package(SaltsUtils CONFIG REQUIRED
  PATHS "${SALTS_UTILS_ROOT_PATH}" NO_DEFAULT_PATH)

foreach(required_target IN ITEMS
    Salts::CMeta
    Salts::PluginABI
    Salts::Plugin
    Salts::DataBind)
  if(NOT TARGET ${required_target})
    message(FATAL_ERROR "published SDK pair is missing ${required_target}")
  endif()
endforeach()

add_executable(turbodb_reflection_plugin_contract
  reflection_plugin_contract.c
  reflection_plugin_peer_a.c
  reflection_plugin_peer_b.c)
target_link_libraries(turbodb_reflection_plugin_contract PRIVATE Salts::Plugin)

enable_testing()
add_test(NAME turbodb_reflection_plugin_contract
         COMMAND turbodb_reflection_plugin_contract)

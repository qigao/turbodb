cmake_minimum_required(VERSION 3.25)
project(TurboDBDriverSDK LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT DEFINED ENV{SALTS_ROOT} OR "$ENV{SALTS_ROOT}" STREQUAL "")
  message(FATAL_ERROR "SALTS_ROOT must name the matching installed Salts prefix")
endif()
file(TO_CMAKE_PATH "$ENV{SALTS_ROOT}" SDK_SALTS_ROOT)
if(NOT IS_DIRECTORY "${SDK_SALTS_ROOT}")
  message(FATAL_ERROR "SALTS_ROOT is not a directory: ${SDK_SALTS_ROOT}")
endif()
unset(Salts_DIR CACHE)
unset(Salts_DIR)
find_package(Salts CONFIG REQUIRED PATHS "${SDK_SALTS_ROOT}" NO_DEFAULT_PATH)
foreach(required_target IN ITEMS Salts::Core Salts::CSerde Salts::CBind
                                 Salts::CFlow Salts::TinyTest)
  if(NOT TARGET ${required_target})
    message(FATAL_ERROR "Salts package is missing ${required_target}")
  endif()
endforeach()

add_library(orm_driver_sdk INTERFACE)
target_include_directories(orm_driver_sdk INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/../include/orm")
target_link_libraries(orm_driver_sdk INTERFACE
  Salts::Core Salts::CSerde Salts::CBind Salts::CFlow)

add_library(orm_driver_build_options INTERFACE)
if(MSVC)
  target_compile_options(orm_driver_build_options INTERFACE /W4 /WX
    $<$<CONFIG:Debug>:/fsanitize=address>)
  target_link_options(orm_driver_build_options INTERFACE
    $<$<CONFIG:Debug>:/INCREMENTAL:NO>)
elseif(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
  target_compile_options(orm_driver_build_options INTERFACE -Wall -Wextra -Werror
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    $<$<CONFIG:Debug>:-fno-omit-frame-pointer>)
  target_link_options(orm_driver_build_options INTERFACE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>)
else()
  message(FATAL_ERROR "Driver SDK has no sanitizer policy for this compiler")
endif()

add_library(orm_driver_contract STATIC ../src/driver/orm_driver_contract.c)
target_include_directories(orm_driver_contract PRIVATE ../src/driver)
target_link_libraries(orm_driver_contract PUBLIC orm_driver_sdk
  PRIVATE orm_driver_build_options)
set_target_properties(orm_driver_contract PROPERTIES
  POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)

enable_testing()
add_executable(orm_driver_prefix_test ../tests/driver/orm_driver_prefix_test.c)
target_include_directories(orm_driver_prefix_test PRIVATE ../src/driver)
target_link_libraries(orm_driver_prefix_test PRIVATE orm_driver_contract
  orm_driver_build_options Salts::TinyTest)
add_test(NAME orm_driver_prefix COMMAND orm_driver_prefix_test)

add_executable(orm_driver_layout_test ../tests/driver/orm_driver_layout_test.cpp)
target_link_libraries(orm_driver_layout_test PRIVATE orm_driver_sdk
  orm_driver_build_options)
add_test(NAME orm_driver_layout COMMAND orm_driver_layout_test)

add_executable(orm_driver_descriptor_test ../tests/driver/orm_driver_descriptor_test.c)
target_link_libraries(orm_driver_descriptor_test PRIVATE orm_driver_contract
  orm_driver_build_options Salts::TinyTest)
add_test(NAME orm_driver_descriptor COMMAND orm_driver_descriptor_test)

add_executable(orm_driver_descriptor_layout_test
  ../tests/driver/orm_driver_descriptor_layout_test.cpp)
target_link_libraries(orm_driver_descriptor_layout_test PRIVATE orm_driver_sdk
  orm_driver_build_options)
add_test(NAME orm_driver_descriptor_layout COMMAND orm_driver_descriptor_layout_test)

add_executable(orm_driver_handshake_test
  ../tests/driver/orm_driver_handshake_test.c
  ../tests/driver/orm_driver_fixture.c)
target_link_libraries(orm_driver_handshake_test PRIVATE orm_driver_contract
  orm_driver_build_options Salts::TinyTest)
add_test(NAME orm_driver_handshake COMMAND orm_driver_handshake_test)

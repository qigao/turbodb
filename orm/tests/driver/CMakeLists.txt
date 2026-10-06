add_subdirectory(postgresql)
add_subdirectory(sqlite)
add_subdirectory(mysql)
add_subdirectory(tidesdb)
add_subdirectory(cross_driver)

# Candidate plan adapter is exercised against the real core, not the SDK
# fixture.
cmake_add_test(
  NAME orm_driver_plan_view_test
  FOLDER "orm/tests/driver"
  TEST_NAME orm_driver_plan_view
  SOURCES orm_driver_plan_view_test.c
  LIBS ${ORM_INTERNAL_TEST_LIBRARY} Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/abi
    ${ORM_SOURCE_DIR}/src/flow ${CMAKE_CURRENT_SOURCE_DIR}/../../src/driver)

set_property(TARGET orm_driver_plan_view_test PROPERTY LINKER_LANGUAGE CXX)
# Keep this new boundary strict without changing legacy core warning defaults.
if(MSVC)
  target_compile_options(orm_driver_plan_view_test PRIVATE /WX)
else()
  target_compile_options(orm_driver_plan_view_test PRIVATE -Werror)
endif()

if(WIN32)
  set_tests_properties(
    orm_driver_plan_view PROPERTIES ENVIRONMENT_MODIFICATION
                                    "${ORM_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME orm_driver_interface_test
  FOLDER "orm/tests/driver"
  TEST_NAME orm_driver_interface
  SOURCES orm_driver_interface_test.c orm_driver_interface_peer_a.c
    orm_driver_interface_peer_b.c
  LIBS Salts::Plugin Salts::CFlow
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)

cmake_add_test(
  NAME orm_driver_interface_cpp_test
  FOLDER "orm/tests/driver"
  TEST_NAME orm_driver_interface_cpp
  SOURCES orm_driver_interface_cpp_test.cpp
  LIBS Salts::Plugin Salts::CFlow
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)

if(MSVC)
  target_compile_options(orm_driver_interface_test PRIVATE /WX)
  target_compile_options(orm_driver_interface_cpp_test PRIVATE /WX)
else()
  target_compile_options(orm_driver_interface_test PRIVATE -Werror)
  # C keeps struct tags and functions in separate namespaces. The public ABI
  # deliberately preserves historical pairs such as orm_config/struct
  # orm_config, which are legal in C++ but trigger GCC's C++-specific warning.
  target_compile_options(orm_driver_interface_cpp_test PRIVATE -Werror -Wno-shadow)
endif()

cmake_add_test(
  NAME orm_driver_backend_bridge_test
  FOLDER "orm/tests/driver"
  TEST_NAME orm_driver_backend_bridge
  SOURCES orm_driver_backend_bridge_test.c ../../src/driver/orm_driver_backend_bridge.c
  LIBS Salts::Core Salts::CFlow Salts::CSTL Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/abi
    ${ORM_SOURCE_DIR}/src/flow ${ORM_SOURCE_DIR}/src/driver)

if(MSVC)
  target_compile_options(orm_driver_backend_bridge_test PRIVATE /WX)
else()
  target_compile_options(orm_driver_backend_bridge_test PRIVATE -Werror)
endif()

cmake_add_test(
  NAME orm_driver_backend_adapter_test
  FOLDER "orm/tests/driver"
  TEST_NAME orm_driver_backend_adapter
  SOURCES orm_driver_backend_adapter_test.c ../../src/driver/orm_driver_backend_bridge.c
  LIBS ${ORM_INTERNAL_TEST_LIBRARY} Salts::Core Salts::CSTL Salts::CSerde
    Salts::TinyTest
  DEFS ORM_C_STATIC ORM_NATIVE_OWNER_CANDIDATE
  INCLUDES ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/abi
    ${ORM_SOURCE_DIR}/src/flow ${ORM_SOURCE_DIR}/src/driver)

if(MSVC)
  target_compile_options(orm_driver_backend_adapter_test PRIVATE /WX)
else()
  target_compile_options(orm_driver_backend_adapter_test PRIVATE -Werror)
endif()

cmake_add_test(
  NAME orm_driver_owner_bridge_test
  FOLDER "orm/tests/driver"
  TEST_NAME orm_driver_owner_bridge
  SOURCES orm_driver_owner_bridge_test.c
  LIBS ${ORM_INTERNAL_TEST_LIBRARY} Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/abi
    ${ORM_SOURCE_DIR}/src/flow ${ORM_SOURCE_DIR}/src/driver)

set_property(TARGET orm_driver_owner_bridge_test PROPERTY LINKER_LANGUAGE CXX)
if(MSVC)
  target_compile_options(orm_driver_owner_bridge_test PRIVATE /WX)
else()
  target_compile_options(orm_driver_owner_bridge_test PRIVATE -Werror)
endif()

if(WIN32)
  set_tests_properties(
    orm_driver_owner_bridge PROPERTIES ENVIRONMENT_MODIFICATION
                                       "${ORM_TEST_RUNTIME_PATH}")
endif()

# Qualify canonical Plugin loading/admission with real DSOs. These fixtures
# publish only Plugin ABI 4 contracts; no TurboDB-private loader entry exists.
add_library(orm_plugin_no_query_fixture MODULE orm_module_no_entry_fixture.c)
set_target_properties(
  orm_plugin_no_query_fixture
  PROPERTIES PREFIX ""
             C_VISIBILITY_PRESET hidden
             WINDOWS_EXPORT_ALL_SYMBOLS OFF
             FOLDER "orm/tests/driver")
add_library(orm_plugin_bad_abi_fixture MODULE orm_module_bad_abi_fixture.c)
target_link_libraries(orm_plugin_bad_abi_fixture PRIVATE Salts::PluginABI)
set_target_properties(
  orm_plugin_bad_abi_fixture
  PROPERTIES PREFIX ""
             C_VISIBILITY_PRESET hidden
             WINDOWS_EXPORT_ALL_SYMBOLS OFF
             FOLDER "orm/tests/driver")
add_library(orm_plugin_bad_contract_fixture MODULE
            orm_module_bad_contract_version_fixture.c)
target_include_directories(orm_plugin_bad_contract_fixture
                           PRIVATE ${ORM_SOURCE_DIR}/include/orm)
target_link_libraries(
  orm_plugin_bad_contract_fixture PRIVATE Salts::PluginABI Salts::Core
                                          Salts::CSerde Salts::CFlow)
set_target_properties(
  orm_plugin_bad_contract_fixture
  PROPERTIES PREFIX ""
             C_VISIBILITY_PRESET hidden
             WINDOWS_EXPORT_ALL_SYMBOLS OFF
             FOLDER "orm/tests/driver")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(orm_plugin_bad_contract_fixture PRIVATE
                      "LINKER:--exclude-libs,ALL")
endif()
add_library(orm_plugin_bad_storage_fixture MODULE
            orm_module_bad_storage_fixture.c)
target_include_directories(orm_plugin_bad_storage_fixture
                           PRIVATE ${ORM_SOURCE_DIR}/include/orm)
target_link_libraries(
  orm_plugin_bad_storage_fixture PRIVATE Salts::PluginABI Salts::Core
                                         Salts::CSerde Salts::CFlow)
set_target_properties(
  orm_plugin_bad_storage_fixture
  PROPERTIES PREFIX ""
             C_VISIBILITY_PRESET hidden
             WINDOWS_EXPORT_ALL_SYMBOLS OFF
             FOLDER "orm/tests/driver")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(orm_plugin_bad_storage_fixture PRIVATE
                      "LINKER:--exclude-libs,ALL")
endif()
add_library(orm_plugin_bad_shape_fixture MODULE
            orm_module_bad_interface_fixture.c)
target_include_directories(orm_plugin_bad_shape_fixture
                           PRIVATE ${ORM_SOURCE_DIR}/include/orm)
target_link_libraries(
  orm_plugin_bad_shape_fixture PRIVATE Salts::PluginABI Salts::Core
                                       Salts::CSerde Salts::CFlow)
set_target_properties(
  orm_plugin_bad_shape_fixture
  PROPERTIES PREFIX ""
             C_VISIBILITY_PRESET hidden
             WINDOWS_EXPORT_ALL_SYMBOLS OFF
             FOLDER "orm/tests/driver")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(orm_plugin_bad_shape_fixture PRIVATE
                      "LINKER:--exclude-libs,ALL")
endif()
add_library(orm_runtime_plugin_fixture MODULE orm_runtime_plugin_fixture.c)
target_include_directories(orm_runtime_plugin_fixture
                           PRIVATE ${ORM_SOURCE_DIR}/include/orm)
target_link_libraries(
  orm_runtime_plugin_fixture PRIVATE Salts::PluginABI Salts::Core Salts::CSerde
                                     Salts::CFlow)
set_target_properties(
  orm_runtime_plugin_fixture
  PROPERTIES PREFIX ""
             C_VISIBILITY_PRESET hidden
             WINDOWS_EXPORT_ALL_SYMBOLS OFF
             FOLDER "orm/tests/driver")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(orm_runtime_plugin_fixture PRIVATE
                      "LINKER:--exclude-libs,ALL")
endif()
cmake_add_test(
  NAME orm_runtime_registry_test
  FOLDER "orm/tests/driver"
  TEST_NAME orm_runtime_registry
  SOURCES orm_runtime_registry_test.c
  LIBS turbo_orm Salts::TinyTest
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)

add_dependencies(
  orm_runtime_registry_test
  orm_runtime_plugin_fixture
  orm_plugin_no_query_fixture
  orm_plugin_bad_abi_fixture
  orm_plugin_bad_contract_fixture
  orm_plugin_bad_shape_fixture
  orm_plugin_bad_storage_fixture)
if(MSVC)
  target_compile_options(orm_runtime_registry_test PRIVATE /WX)
else()
  target_compile_options(orm_runtime_registry_test PRIVATE -Werror)
endif()

set_tests_properties(
  orm_runtime_registry
  PROPERTIES
    ENVIRONMENT
    "ORM_PLUGIN_FIXTURE=$<TARGET_FILE:orm_runtime_plugin_fixture>;ORM_PLUGIN_NO_QUERY_FIXTURE=$<TARGET_FILE:orm_plugin_no_query_fixture>;ORM_PLUGIN_BAD_ABI_FIXTURE=$<TARGET_FILE:orm_plugin_bad_abi_fixture>;ORM_PLUGIN_BAD_CONTRACT_FIXTURE=$<TARGET_FILE:orm_plugin_bad_contract_fixture>;ORM_PLUGIN_BAD_SHAPE_FIXTURE=$<TARGET_FILE:orm_plugin_bad_shape_fixture>;ORM_PLUGIN_BAD_STORAGE_FIXTURE=$<TARGET_FILE:orm_plugin_bad_storage_fixture>"
)
# Dedicated race fixture intentionally exports test-only gate symbols in
# addition to canonical salts_plugin_query. It is not the export-surface
# fixture.
add_library(orm_runtime_race_fixture MODULE orm_runtime_race_fixture.c)
target_include_directories(orm_runtime_race_fixture PRIVATE ${ORM_SOURCE_DIR}/include/orm)
target_link_libraries(orm_runtime_race_fixture
                      PRIVATE Salts::PluginABI Salts::Platform Salts::CFlow)
set_target_properties(
  orm_runtime_race_fixture
  PROPERTIES PREFIX ""
             C_VISIBILITY_PRESET hidden
             WINDOWS_EXPORT_ALL_SYMBOLS OFF
             FOLDER "orm/tests/driver")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(orm_runtime_race_fixture PRIVATE
                      "LINKER:--exclude-libs,ALL")
endif()
add_library(orm_runtime_race_fixture_two MODULE orm_runtime_race_fixture.c)
target_compile_definitions(orm_runtime_race_fixture_two
                           PRIVATE ORM_RUNTIME_RACE_DRIVER_ID="race2")
target_include_directories(orm_runtime_race_fixture_two
                           PRIVATE ${ORM_SOURCE_DIR}/include/orm)
target_link_libraries(orm_runtime_race_fixture_two
                      PRIVATE Salts::PluginABI Salts::Platform Salts::CFlow)
set_target_properties(
  orm_runtime_race_fixture_two
  PROPERTIES PREFIX ""
             C_VISIBILITY_PRESET hidden
             WINDOWS_EXPORT_ALL_SYMBOLS OFF
             FOLDER "orm/tests/driver")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(orm_runtime_race_fixture_two PRIVATE
                      "LINKER:--exclude-libs,ALL")
endif()
cmake_add_test(
  NAME orm_runtime_race_test
  FOLDER "orm/tests/driver"
  TEST_NAME orm_runtime_race
  SOURCES orm_runtime_race_test.c
  LIBS ${ORM_INTERNAL_TEST_LIBRARY} Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/abi
    ${ORM_SOURCE_DIR}/src/flow ${ORM_SOURCE_DIR}/src/runtime)

set_property(TARGET orm_runtime_race_test PROPERTY LINKER_LANGUAGE CXX)
add_dependencies(orm_runtime_race_test orm_runtime_race_fixture
                 orm_runtime_race_fixture_two)
if(MSVC)
  target_compile_options(orm_runtime_race_test PRIVATE /WX)
else()
  target_compile_options(orm_runtime_race_test PRIVATE -Werror)
endif()

set_tests_properties(
  orm_runtime_race
  PROPERTIES
    ENVIRONMENT
    "ORM_RUNTIME_RACE_FIXTURE=$<TARGET_FILE:orm_runtime_race_fixture>;ORM_RUNTIME_RACE_FIXTURE_TWO=$<TARGET_FILE:orm_runtime_race_fixture_two>;ORM_RUNTIME_RACE_GATE_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/runtime-race-gate"
)

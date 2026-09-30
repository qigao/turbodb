# Reuse the installed retained-owner core's exact source/dependency selection,
# replacing only the row allocation call sites for private fault injection.
if(NOT TARGET orm_c_test_static)
  message(
    FATAL_ERROR "Native ownership tests require the real ORM test library")
endif()
# Backend imports are directory-scoped; use the same required packages/preset.
find_package(SQLite3 REQUIRED)
get_target_property(owner_source_dir turbo_orm SOURCE_DIR)
get_target_property(owner_sources orm_c_test_static SOURCES)
set(owner_absolute_sources ${owner_sources})
# Fault injection is private to this non-installed clone. The probe includes the
# unchanged production row TU and redirects only its allocation call sites.
# Other modules and the shared/static public ORM builds keep their real
# allocators.
set(owner_row_source "${owner_source_dir}/src/flow/orm_row_publisher.c")
list(FIND owner_absolute_sources "${owner_row_source}" owner_row_index)
if(owner_row_index LESS 0)
  message(
    FATAL_ERROR "Row allocation probe requires the real row publisher source")
endif()
list(REMOVE_AT owner_absolute_sources ${owner_row_index})
list(INSERT owner_absolute_sources ${owner_row_index}
     "${CMAKE_CURRENT_SOURCE_DIR}/orm_row_allocator_probe.c")
add_library(orm_owner_test_core STATIC ${owner_absolute_sources})
get_target_property(owner_include_directories orm_c_test_static
                    INCLUDE_DIRECTORIES)
get_target_property(owner_compile_definitions orm_c_test_static
                    COMPILE_DEFINITIONS)
get_target_property(owner_compile_options orm_c_test_static COMPILE_OPTIONS)
get_target_property(owner_compile_features orm_c_test_static COMPILE_FEATURES)
get_target_property(owner_link_libraries orm_c_test_static LINK_LIBRARIES)
get_target_property(owner_interface_link_libraries orm_c_test_static
                    INTERFACE_LINK_LIBRARIES)
if(owner_include_directories)
  set_property(TARGET orm_owner_test_core
               PROPERTY INCLUDE_DIRECTORIES "${owner_include_directories}")
endif()
if(owner_compile_definitions)
  set_property(TARGET orm_owner_test_core
               PROPERTY COMPILE_DEFINITIONS "${owner_compile_definitions}")
endif()
if(owner_compile_options)
  set_property(TARGET orm_owner_test_core PROPERTY COMPILE_OPTIONS
                                                   "${owner_compile_options}")
endif()
if(owner_compile_features)
  set_property(TARGET orm_owner_test_core PROPERTY COMPILE_FEATURES
                                                   "${owner_compile_features}")
endif()
if(owner_link_libraries)
  set_property(TARGET orm_owner_test_core PROPERTY LINK_LIBRARIES
                                                   "${owner_link_libraries}")
endif()
if(owner_interface_link_libraries)
  set_property(
    TARGET orm_owner_test_core PROPERTY INTERFACE_LINK_LIBRARIES
                                        "${owner_interface_link_libraries}")
endif()
target_compile_definitions(orm_owner_test_core
                           PUBLIC ORM_C_STATIC ORM_NATIVE_OWNER_CANDIDATE)
target_include_directories(orm_owner_test_core PUBLIC ../../include/orm)
target_link_libraries(orm_owner_test_core PUBLIC Salts::Core)
set_target_properties(orm_owner_test_core PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  FOLDER "orm/tests/ownership")

cmake_add_test(
  NAME orm_owner_regression_test
  FOLDER "orm/tests/ownership"
  TEST_NAME orm_owner_regression
  SOURCES orm_owner_regression_test.c
  LIBS orm_owner_test_core Salts::DataBind Salts::CFlow Salts::TinyTest)
cmake_add_test(
  NAME orm_owner_state_test
  FOLDER "orm/tests/ownership"
  TEST_NAME orm_owner_state
  SOURCES orm_owner_state_test.c
  LIBS orm_owner_test_core Salts::DataBind Salts::CFlow Salts::TinyTest)
cmake_add_test(
  NAME orm_owner_checked_test
  FOLDER "orm/tests/ownership"
  TEST_NAME orm_owner_checked
  SOURCES orm_owner_test.c orm_transaction_owner_test.c orm_connection_control_test.c
    orm_cursor_open_owner_test.c orm_cursor_execution_owner_test.c
    orm_row_storage_cutover_test.c orm_row_budget_owner_test.c
    orm_row_borrowed_contract_test.c
  LIBS orm_owner_test_core Salts::DataBind Salts::CFlow Salts::TinyTest)
cmake_add_test(
  NAME orm_owner_public_flow_test
  FOLDER "orm/tests/ownership"
  TEST_NAME orm_owner_public_flow
  SOURCES ../flow/sqlite/orm_public_flow_test.c
  LIBS orm_owner_test_core Salts::DataBind Salts::CFlow Salts::TinyTest)
cmake_add_test(
  NAME orm_owner_cpp_flow_test
  FOLDER "orm/tests/ownership"
  TEST_NAME orm_owner_cpp_flow
  SOURCES ../flow/sqlite/orm_cpp_flow_test.cpp
  LIBS orm_owner_test_core Salts::DataBind Salts::CFlow Salts::TinyTest)
cmake_add_test(
  NAME orm_owner_cleanup_test
  FOLDER "orm/tests/ownership"
  TEST_NAME orm_owner_cleanup
  SOURCES orm_cleanup_failure_test.c
  LIBS orm_owner_test_core Salts::DataBind Salts::CFlow Salts::TinyTest)
cmake_add_test(
  NAME orm_owner_race_test
  FOLDER "orm/tests/ownership"
  TEST_NAME orm_owner_race
  SOURCES orm_owner_race_test.c
  LIBS orm_owner_test_core Salts::DataBind Salts::CFlow Salts::TinyTest)
cmake_add_test(
  NAME orm_owner_failure_test
  FOLDER "orm/tests/ownership"
  TEST_NAME orm_owner_failure
  SOURCES orm_connection_failure_test.c
  LIBS orm_owner_test_core Salts::DataBind Salts::CFlow Salts::TinyTest)
set(owner_test_targets
    orm_owner_regression_test
    orm_owner_state_test
    orm_owner_checked_test
    orm_owner_public_flow_test
    orm_owner_cpp_flow_test
    orm_owner_cleanup_test
    orm_owner_race_test
    orm_owner_failure_test)
set(owner_test_names
    orm_owner_regression
    orm_owner_state
    orm_owner_checked
    orm_owner_public_flow
    orm_owner_cpp_flow
    orm_owner_cleanup
    orm_owner_race
    orm_owner_failure)
set_property(
  TARGET ${owner_test_targets}
  PROPERTY
    INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/abi;${CMAKE_CURRENT_SOURCE_DIR}/../../src/flow"
)

set_property(TARGET ${owner_test_targets} PROPERTY C_STANDARD 11)
set_property(TARGET ${owner_test_targets} PROPERTY C_STANDARD_REQUIRED ON)
set_property(TARGET ${owner_test_targets} PROPERTY CXX_STANDARD 17)
set_property(TARGET ${owner_test_targets} PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET ${owner_test_targets} PROPERTY LINKER_LANGUAGE CXX)

# The unmodified C++ regression keeps its existing warning policy.
set(owner_warning_targets ${owner_test_targets})
list(REMOVE_ITEM owner_warning_targets orm_owner_cpp_flow_test)
if(MSVC)
  set_property(
    TARGET ${owner_warning_targets}
    APPEND
    PROPERTY COMPILE_OPTIONS /W4 /WX /Zc:preprocessor)
else()
  set_property(
    TARGET ${owner_warning_targets}
    APPEND
    PROPERTY COMPILE_OPTIONS -Wall -Wextra -Wpedantic -Werror)
endif()

set_tests_properties(${owner_test_names} PROPERTIES TIMEOUT 60)
if(WIN32)
  set_tests_properties(
    ${owner_test_names}
    PROPERTIES
      ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::Core>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::DataBind>"
  )
endif()

# Real libpq failure handles need no service. Keep the adapter and cursor on the
# same private layout as this test core instead of linking the installed DLL.
find_package(PostgreSQL REQUIRED)
cmake_add_test(
  NAME orm_postgres_connection_test
  FOLDER "orm/tests/ownership"
  TEST_NAME orm_postgres_connection
  SOURCES ../flow/postgresql/orm_postgres_connection_test.c
    ${ORM_SOURCE_DIR}/../drivers/postgresql/orm_postgres_libpq.c
    ${ORM_SOURCE_DIR}/../drivers/postgresql/orm_postgres_cursor.c
  LIBS orm_owner_test_core PostgreSQL::PostgreSQL Salts::CSerde Salts::DataBind
    Salts::CFlow Salts::TinyTest
  INCLUDES ../../src/abi ../../src/flow ${ORM_SOURCE_DIR}/../drivers/postgresql)

set_target_properties(
  orm_postgres_connection_test
  PROPERTIES C_STANDARD 11
             C_STANDARD_REQUIRED ON
             LINKER_LANGUAGE CXX)
if(MSVC)
  target_compile_options(orm_postgres_connection_test
                         PRIVATE /W4 /WX /Zc:preprocessor)
else()
  target_compile_options(orm_postgres_connection_test
                         PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

set_tests_properties(orm_postgres_connection PROPERTIES TIMEOUT 30)
if(WIN32)
  set_tests_properties(
    orm_postgres_connection
    PROPERTIES
      ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::Core>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::DataBind>"
  )
endif()

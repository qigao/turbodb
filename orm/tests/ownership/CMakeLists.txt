# Reuse the installed retained-owner core's exact source/dependency selection,
# replacing only the row allocation call sites for private fault injection.
if(NOT TARGET orm_c_test_static)
  message(FATAL_ERROR "Native ownership tests require the real ORM test library")
endif()
# Backend imports are directory-scoped; use the same required packages/preset.
if(ORM_WITH_SQLITE)
  find_package(SQLite3 REQUIRED)
endif()
if(ORM_WITH_MONGODB)
  find_package(mongoc-1.0 CONFIG REQUIRED)
endif()
get_target_property(owner_source_dir orm_c_test_static SOURCE_DIR)
get_target_property(owner_sources orm_c_test_static SOURCES)
set(owner_absolute_sources)
foreach(source IN LISTS owner_sources)
  get_filename_component(source "${source}" ABSOLUTE BASE_DIR "${owner_source_dir}")
  list(APPEND owner_absolute_sources "${source}")
endforeach()
# Fault injection is private to this non-installed clone. The probe includes
# the unchanged production row TU and redirects only its allocation call sites.
# Other modules and the shared/static public ORM builds keep their real allocators.
set(owner_row_source "${owner_source_dir}/src/flow/orm_row_publisher.c")
list(FIND owner_absolute_sources "${owner_row_source}" owner_row_index)
if(owner_row_index LESS 0)
  message(FATAL_ERROR "Row allocation probe requires the real row publisher source")
endif()
list(REMOVE_AT owner_absolute_sources ${owner_row_index})
list(INSERT owner_absolute_sources ${owner_row_index}
     "${CMAKE_CURRENT_SOURCE_DIR}/orm_row_allocator_probe.c")
add_library(orm_owner_test_core STATIC ${owner_absolute_sources})
foreach(property IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS
                          COMPILE_FEATURES LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
  get_target_property(value orm_c_test_static ${property})
  if(value)
    set_property(TARGET orm_owner_test_core PROPERTY ${property} "${value}")
  endif()
endforeach()
target_compile_definitions(orm_owner_test_core PUBLIC ORM_C_STATIC ORM_NATIVE_OWNER_CANDIDATE)
target_include_directories(orm_owner_test_core PUBLIC ../../include/orm)
target_link_libraries(orm_owner_test_core PUBLIC Salts::Core)
set_target_properties(orm_owner_test_core PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

add_executable(orm_owner_regression_test orm_owner_regression_test.c)
add_executable(orm_owner_state_test orm_owner_state_test.c)
add_executable(orm_owner_checked_test orm_owner_test.c orm_transaction_owner_test.c
  orm_connection_control_test.c orm_cursor_open_owner_test.c
  orm_cursor_execution_owner_test.c orm_row_storage_cutover_test.c
  orm_row_budget_owner_test.c orm_row_borrowed_contract_test.c)
add_executable(orm_owner_public_flow_test ../flow/orm_public_flow_test.c)
add_executable(orm_owner_cpp_flow_test ../flow/orm_cpp_flow_test.cpp)
add_executable(orm_owner_cleanup_test orm_cleanup_failure_test.c)
add_executable(orm_owner_race_test orm_owner_race_test.c)
add_executable(orm_owner_failure_test orm_connection_failure_test.c)
foreach(group IN ITEMS regression state checked public_flow cpp_flow cleanup race failure)
  set(target orm_owner_${group}_test)
  target_include_directories(${target} PRIVATE ../../src/abi ../../src/flow)
  target_link_libraries(${target} PRIVATE orm_owner_test_core
    Salts::DataBind Salts::CFlow Salts::TinyTest)
  set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON LINKER_LANGUAGE CXX FOLDER "orm/tests")
  # The unmodified C++ regression keeps its existing warning policy.
  if(NOT group STREQUAL "cpp_flow")
    if(MSVC)
      target_compile_options(${target} PRIVATE /W4 /WX /Zc:preprocessor)
    else()
      target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
  endif()
  add_test(NAME orm_owner_${group} COMMAND ${target})
  set_tests_properties(orm_owner_${group} PROPERTIES TIMEOUT 60)
  if(WIN32)
    set_tests_properties(orm_owner_${group} PROPERTIES ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::Core>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::DataBind>")
  endif()
endforeach()

# The staged owner layout differs from the installed ABI 4 component. Compile
# the real PostgreSQL adapter with the candidate, never link its stable DLL.
if(ORM_POSTGRES_LIVE_TESTS)
  find_package(PostgreSQL REQUIRED)
  foreach(pg_name IN ITEMS orm_owner_postgres orm_owner_postgres_loss)
    set(pg_target ${pg_name}_test)
    add_executable(${pg_target} ${pg_target}.c
      ../../src/dbs/postgres/component.c ../../src/dbs/postgres/backend.c
      ../../src/dbs/postgres/orm_postgres_libpq.c
      ../../src/dbs/postgres/orm_postgres_cursor.c)
    target_compile_definitions(${pg_target} PRIVATE
      ORM_POSTGRESQL_STATIC ORM_WITH_PGSQL)
    target_include_directories(${pg_target} PRIVATE
      ../../src/abi ../../src/flow ../../src/dbs/sql ../../src/dbs/postgres)
    target_link_libraries(${pg_target} PRIVATE orm_owner_test_core
      PostgreSQL::PostgreSQL Salts::DataBind Salts::CFlow Salts::TinyTest)
    set_target_properties(${pg_target} PROPERTIES C_STANDARD 11
      C_STANDARD_REQUIRED ON LINKER_LANGUAGE CXX FOLDER "orm/tests")
    if(MSVC)
      target_compile_options(${pg_target} PRIVATE /W4 /WX /Zc:preprocessor)
    else()
      target_compile_options(${pg_target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME ${pg_name} COMMAND ${pg_target})
    set_tests_properties(${pg_name} PROPERTIES TIMEOUT 60)
    if(WIN32)
      set_tests_properties(${pg_name} PROPERTIES ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::Core>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::DataBind>")
    endif()
  endforeach()
endif()


# Real libpq failure handles need no service. Keep the adapter and cursor on the
# same private layout as this test core instead of linking the installed DLL.
if(ORM_WITH_PGSQL)
  find_package(PostgreSQL REQUIRED)
  add_executable(orm_postgres_connection_test
    ../flow/orm_postgres_connection_test.c
    ../../src/dbs/postgres/orm_postgres_libpq.c
    ../../src/dbs/postgres/orm_postgres_cursor.c)
  target_compile_definitions(orm_postgres_connection_test PRIVATE
    ORM_POSTGRESQL_STATIC ORM_WITH_PGSQL)
  target_include_directories(orm_postgres_connection_test PRIVATE
    ../../src/abi ../../src/flow ../../src/dbs/postgres)
  target_link_libraries(orm_postgres_connection_test PRIVATE orm_owner_test_core
    PostgreSQL::PostgreSQL Salts::CSerde Salts::DataBind Salts::CFlow Salts::TinyTest)
  set_target_properties(orm_postgres_connection_test PROPERTIES C_STANDARD 11
    C_STANDARD_REQUIRED ON LINKER_LANGUAGE CXX FOLDER "orm/tests")
  if(MSVC)
    target_compile_options(orm_postgres_connection_test PRIVATE /W4 /WX /Zc:preprocessor)
  else()
    target_compile_options(orm_postgres_connection_test PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME orm_postgres_connection COMMAND orm_postgres_connection_test)
  set_tests_properties(orm_postgres_connection PROPERTIES TIMEOUT 30)
  if(WIN32)
    set_tests_properties(orm_postgres_connection PROPERTIES ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::Core>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::DataBind>")
  endif()
endif()

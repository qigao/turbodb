# Keep the incomplete ABI 5 owner candidate out of installed ABI 4 artifacts.
# Reuse the real static core's exact source/dependency selection, not a backend stub.
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
add_library(orm_owner_test_core STATIC ${owner_absolute_sources} ../../src/abi/orm_owner.c)
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
add_executable(orm_owner_checked_test orm_owner_test.c orm_transaction_owner_test.c)
add_executable(orm_owner_public_flow_test ../flow/orm_public_flow_test.c)
add_executable(orm_owner_cpp_flow_test ../flow/orm_cpp_flow_test.cpp)
add_executable(orm_owner_cleanup_test orm_cleanup_failure_test.c)
add_executable(orm_owner_race_test orm_owner_race_test.c)
foreach(group IN ITEMS regression state checked public_flow cpp_flow cleanup race)
  set(target orm_owner_${group}_test)
  target_include_directories(${target} PRIVATE ../../src/abi ../../src/flow)
  target_link_libraries(${target} PRIVATE orm_owner_test_core
    Salts::CBind Salts::CFlow Salts::TinyTest)
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
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::Core>")
  endif()
endforeach()

# The staged owner layout differs from the installed ABI 4 component. Compile
# the real PostgreSQL adapter with the candidate, never link its stable DLL.
if(ORM_POSTGRES_LIVE_TESTS)
  find_package(PostgreSQL REQUIRED)
  add_executable(orm_owner_postgres_test orm_owner_postgres_test.c
    ../../src/dbs/postgres/component.c ../../src/dbs/postgres/backend.c
    ../../src/dbs/postgres/orm_postgres_libpq.c
    ../../src/dbs/postgres/orm_postgres_cursor.c)
  target_compile_definitions(orm_owner_postgres_test PRIVATE
    ORM_POSTGRESQL_STATIC ORM_WITH_PGSQL)
  target_include_directories(orm_owner_postgres_test PRIVATE
    ../../src/abi ../../src/flow ../../src/dbs/sql ../../src/dbs/postgres)
  target_link_libraries(orm_owner_postgres_test PRIVATE orm_owner_test_core
    PostgreSQL::PostgreSQL Salts::CBind Salts::CFlow Salts::TinyTest)
  set_target_properties(orm_owner_postgres_test PROPERTIES C_STANDARD 11
    C_STANDARD_REQUIRED ON LINKER_LANGUAGE CXX FOLDER "orm/tests")
  if(MSVC)
    target_compile_options(orm_owner_postgres_test PRIVATE /W4 /WX /Zc:preprocessor)
  else()
    target_compile_options(orm_owner_postgres_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME orm_owner_postgres COMMAND orm_owner_postgres_test)
  set_tests_properties(orm_owner_postgres PROPERTIES TIMEOUT 60)
  if(WIN32)
    set_tests_properties(orm_owner_postgres PROPERTIES ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::Core>")
  endif()
endif()

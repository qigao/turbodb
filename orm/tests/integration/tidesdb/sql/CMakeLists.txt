cmake_add_test(
  NAME orm_tidesdb_sql_relational_owner_test TEST_NAME orm_tidesdb_sql_relational_owner
  FOLDER "orm/tests/integration/tidesdb/sql"
  SOURCES relational_owner_test.c ${PROJECT_SOURCE_DIR}/tidessql/src/bridge.c
  LIBS TidesDB::tidesdb TurboDB::TidesSQL TurboDB::SqlParser ${ORM_INTERNAL_TEST_LIBRARY}
    Salts::CSTL Salts::CSerde Salts::CFlow Salts::TinyTest
  DEFS ORM_C_STATIC ORM_NATIVE_OWNER_CANDIDATE
  INCLUDES ${PROJECT_SOURCE_DIR}/tidessql/src ${ORM_SOURCE_DIR}/../drivers/tidesdb ${ORM_SOURCE_DIR}/src/abi ${ORM_SOURCE_DIR}/src/flow ${ORM_SOURCE_DIR}/src/sql)
set_target_properties(orm_tidesdb_sql_relational_owner_test PROPERTIES LINKER_LANGUAGE CXX)
set_tests_properties(orm_tidesdb_sql_relational_owner PROPERTIES TIMEOUT 60)
if(WIN32)
  set_tests_properties(orm_tidesdb_sql_relational_owner PROPERTIES ENVIRONMENT_MODIFICATION "${ORM_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME orm_tidesdb_sql_relational_test TEST_NAME orm_tidesdb_sql_relational
  FOLDER "orm/tests/integration/tidesdb/sql" SOURCES relational_test.c
  LIBS turbo_orm Salts::CFlow Salts::TinyTest
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)
add_dependencies(orm_tidesdb_sql_relational_test orm_tidesdb_driver)
if(UNIX AND NOT APPLE)
  target_link_libraries(orm_tidesdb_sql_relational_test PRIVATE m)
endif()
# Per-case native database teardown makes the full suite exceed two minutes on macOS.
set_tests_properties(orm_tidesdb_sql_relational PROPERTIES TIMEOUT 240
  ENVIRONMENT "ORM_TIDESDB_PLUGIN=$<TARGET_FILE:orm_tidesdb_driver>")
if(WIN32)
  set_tests_properties(orm_tidesdb_sql_relational PROPERTIES ENVIRONMENT_MODIFICATION "${ORM_TEST_RUNTIME_PATH}")
endif()

# Reuse the native source/dependency selection, replacing block-manager and
# skip-list TUs with private fault probes. Do not link the production native library
# into this executable: shared-library symbol interposition is not portable.
get_target_property(tdb_fault_source_dir tidesdb SOURCE_DIR)
get_target_property(tdb_fault_sources tidesdb SOURCES)
set(tdb_fault_absolute_sources)
foreach(tdb_fault_source IN LISTS tdb_fault_sources)
  cmake_path(ABSOLUTE_PATH tdb_fault_source BASE_DIRECTORY "${tdb_fault_source_dir}")
  list(APPEND tdb_fault_absolute_sources "${tdb_fault_source}")
endforeach()
set(tdb_fault_block_manager "${tdb_fault_source_dir}/src/block_manager.c")
list(FIND tdb_fault_absolute_sources "${tdb_fault_block_manager}" tdb_fault_block_index)
if(tdb_fault_block_index LESS 0)
  message(FATAL_ERROR "TidesDB WAL tests require the real block manager translation unit")
endif()
list(REMOVE_AT tdb_fault_absolute_sources ${tdb_fault_block_index})
list(INSERT tdb_fault_absolute_sources ${tdb_fault_block_index}
  "${CMAKE_CURRENT_SOURCE_DIR}/wal_fault.c")
set(tdb_fault_skip_list "${tdb_fault_source_dir}/src/skip_list.c")
list(FIND tdb_fault_absolute_sources "${tdb_fault_skip_list}" tdb_fault_skip_index)
if(tdb_fault_skip_index LESS 0)
  message(FATAL_ERROR "TidesDB memory tests require the real skip-list translation unit")
endif()
list(REMOVE_AT tdb_fault_absolute_sources ${tdb_fault_skip_index})
list(INSERT tdb_fault_absolute_sources ${tdb_fault_skip_index}
  "${CMAKE_CURRENT_SOURCE_DIR}/skip_list_fault.c")
add_library(orm_tidesdb_wal_fault_core STATIC ${tdb_fault_absolute_sources} memory_fault.c)
foreach(tdb_fault_property IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS
    COMPILE_OPTIONS COMPILE_FEATURES INTERFACE_COMPILE_OPTIONS INTERFACE_COMPILE_FEATURES
    INTERFACE_INCLUDE_DIRECTORIES LINK_LIBRARIES INTERFACE_LINK_LIBRARIES
    C_STANDARD C_STANDARD_REQUIRED C_EXTENSIONS)
  get_target_property(tdb_fault_value tidesdb ${tdb_fault_property})
  if(NOT tdb_fault_value STREQUAL "tdb_fault_value-NOTFOUND")
    set_property(TARGET orm_tidesdb_wal_fault_core PROPERTY ${tdb_fault_property} "${tdb_fault_value}")
  endif()
endforeach()
set_target_properties(orm_tidesdb_wal_fault_core PROPERTIES
  FOLDER "orm/tests/integration/tidesdb/sql" POSITION_INDEPENDENT_CODE ON)

cmake_add_test(
  NAME orm_tidesdb_sql_wal_fault_test TEST_NAME orm_tidesdb_sql_wal_fault
  FOLDER "orm/tests/integration/tidesdb/sql"
  SOURCES storage_test.c ${PROJECT_SOURCE_DIR}/tidessql/src/bridge.c
  DEFS ORM_TDB_STORAGE_FAULTS
  LIBS orm_tidesdb_wal_fault_core Salts::Core Salts::TinyTest
  INCLUDES ${PROJECT_SOURCE_DIR}/tidessql/src ${ORM_SOURCE_DIR}/../drivers/tidesdb)
set_tests_properties(orm_tidesdb_sql_wal_fault PROPERTIES TIMEOUT 90
  ENVIRONMENT "ORM_TDB_STORAGE_TEST_EXE=$<TARGET_FILE:orm_tidesdb_sql_wal_fault_test>")
if(WIN32)
  set_tests_properties(orm_tidesdb_sql_wal_fault PROPERTIES
    ENVIRONMENT_MODIFICATION "${ORM_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME orm_tidesdb_sql_storage_race_test TEST_NAME orm_tidesdb_sql_storage_race
  FOLDER "orm/tests/integration/tidesdb/sql"
  SOURCES storage_race_test.c ${PROJECT_SOURCE_DIR}/tidessql/src/bridge.c
  LIBS TidesDB::tidesdb Salts::Core Salts::TinyTest
  INCLUDES ${PROJECT_SOURCE_DIR}/tidessql/src ${ORM_SOURCE_DIR}/../drivers/tidesdb)
set_tests_properties(orm_tidesdb_sql_storage_race PROPERTIES TIMEOUT 90)
if(WIN32)
  set_tests_properties(orm_tidesdb_sql_storage_race PROPERTIES
    ENVIRONMENT_MODIFICATION "${ORM_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME orm_tidesdb_sql_storage_test TEST_NAME orm_tidesdb_sql_storage
  FOLDER "orm/tests/integration/tidesdb/sql"
  SOURCES storage_test.c ${PROJECT_SOURCE_DIR}/tidessql/src/bridge.c
  LIBS TidesDB::tidesdb Salts::Core Salts::TinyTest
  INCLUDES ${PROJECT_SOURCE_DIR}/tidessql/src ${ORM_SOURCE_DIR}/../drivers/tidesdb)
set_tests_properties(orm_tidesdb_sql_storage PROPERTIES TIMEOUT 90
  ENVIRONMENT "ORM_TDB_STORAGE_TEST_EXE=$<TARGET_FILE:orm_tidesdb_sql_storage_test>")
if(WIN32)
  set_tests_properties(orm_tidesdb_sql_storage PROPERTIES
    ENVIRONMENT_MODIFICATION "${ORM_TEST_RUNTIME_PATH}")
endif()

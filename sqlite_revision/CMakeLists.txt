cmake_minimum_required(VERSION 3.21)

find_package(SQLite3 REQUIRED)
add_library(turbodb_sqlite_revision STATIC src/sqlite_revision.c)
target_compile_features(turbodb_sqlite_revision PUBLIC c_std_11)
target_include_directories(turbodb_sqlite_revision PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(turbodb_sqlite_revision PUBLIC TurboDB::Types SQLite::SQLite3)
cmake_config_target(turbodb_sqlite_revision
  ALIAS TurboDB::SqliteRevision EXPORT_NAME SqliteRevision
  EXPORT_SET TurboDBTargets FOLDER sqlite_revision NO_VERSION)
cmake_install_headers(FILES include/turbodb/sqlite_revision.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/turbodb)

if(BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
  add_executable(turbodb_sqlite_revision_native_test tests/sqlite_revision_test.c)
  target_link_libraries(turbodb_sqlite_revision_native_test PRIVATE TurboDB::SqliteRevision)
  add_test(NAME turbodb-sqlite-revision-native
    COMMAND turbodb_sqlite_revision_native_test
            "${CMAKE_CURRENT_BINARY_DIR}/revision-native-test.db")
  set_tests_properties(turbodb-sqlite-revision-native PROPERTIES TIMEOUT 20)
  add_executable(turbodb_sqlite_revision_fault_test tests/sqlite_revision_fault_test.c)
  target_link_libraries(turbodb_sqlite_revision_fault_test
    PRIVATE TurboDB::SqliteRevision SQLite::SQLite3)
  add_test(NAME turbodb-sqlite-revision-fault-vfs
    COMMAND turbodb_sqlite_revision_fault_test
      "${CMAKE_CURRENT_BINARY_DIR}/revision-fault-test.db")
  set_tests_properties(turbodb-sqlite-revision-fault-vfs PROPERTIES TIMEOUT 20)
  add_executable(turbodb_sqlite_revision_prefix_test tests/sqlite_revision_prefix_test.c)
  target_link_libraries(turbodb_sqlite_revision_prefix_test
    PRIVATE TurboDB::SqliteRevision)
  add_test(NAME turbodb-sqlite-revision-prefix
    COMMAND turbodb_sqlite_revision_prefix_test
      "${CMAKE_CURRENT_BINARY_DIR}/revision-prefix-test.db")
  set_tests_properties(turbodb-sqlite-revision-prefix PROPERTIES TIMEOUT 20)
  add_executable(turbodb_sqlite_revision_projection_test tests/sqlite_revision_projection_test.c)
  target_link_libraries(turbodb_sqlite_revision_projection_test
    PRIVATE TurboDB::SqliteRevision SQLite::SQLite3)
  add_test(NAME turbodb-sqlite-revision-projection
    COMMAND turbodb_sqlite_revision_projection_test
            "${CMAKE_CURRENT_BINARY_DIR}/revision-projection-test.db")
  set_tests_properties(turbodb-sqlite-revision-projection PROPERTIES TIMEOUT 20)
endif()

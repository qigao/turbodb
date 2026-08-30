include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/DbToolsOptions.cmake)
turbodb_dbtools_validate_options(DBTOOLS_OPTIONS_ERROR)
if(NOT DBTOOLS_OPTIONS_ERROR STREQUAL "")
  message(FATAL_ERROR "${DBTOOLS_OPTIONS_ERROR}")
endif()

if(BUILD_TESTS)
  add_test(
    NAME dbtools_options_contract
    COMMAND
      ${CMAKE_COMMAND}
      -DTURBODB_SOURCE_DIR=${PROJECT_SOURCE_DIR}
      -P
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/dbtools_options_contract.cmake)
endif()

if(NOT TURBODB_BUILD_DBTOOLS)
  return()
endif()

if(TURBODB_DBTOOLS_WITH_SQLITE)
  find_package(SQLite3 REQUIRED)
endif()

if(TURBODB_DBTOOLS_WITH_PGSQL)
  find_package(PostgreSQL REQUIRED)
endif()

add_library(
  turbodb_dbtool_core STATIC
  src/dbtool_error.c
  src/dbtool_file.c
  src/dbtool_cli.c)
target_compile_features(turbodb_dbtool_core PUBLIC c_std_11)
target_include_directories(turbodb_dbtool_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(turbodb_dbtool_core PUBLIC TurboUtils::Core)
cmake_config_target(turbodb_dbtool_core NO_INSTALL FOLDER "dbtools")

if(MSVC)
  target_compile_options(turbodb_dbtool_core PRIVATE /W4 /WX /std:c11)
else()
  target_compile_options(turbodb_dbtool_core PRIVATE -Wall -Wextra -Wpedantic
                                                       -Werror)
endif()

function(dbtool_enable_strict_warnings TARGET_NAME)
  if(MSVC)
    target_compile_options(${TARGET_NAME} PRIVATE /W4 /WX /std:c11)
  else()
    target_compile_options(${TARGET_NAME} PRIVATE -Wall -Wextra -Wpedantic
                                                   -Werror)
  endif()
endfunction()

function(dbtool_stage_runtime_dependencies TARGET_NAME)
  if(WIN32)
    add_custom_command(
      TARGET ${TARGET_NAME}
      POST_BUILD
      COMMAND
        ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:${TARGET_NAME}> $<TARGET_FILE_DIR:${TARGET_NAME}>
      COMMAND_EXPAND_LISTS)
  endif()
endfunction()

function(dbtool_enable_postgresql_linker TARGET_NAME)
  # Static libpq packages may close BoringSSL archives through the C++
  # runtime. This affects only final linkage; every dbtools source remains C11.
  set_property(TARGET ${TARGET_NAME} PROPERTY LINKER_LANGUAGE CXX)
endfunction()

if(TURBODB_DBTOOLS_WITH_SQLITE)
  add_library(turbodb_dbtool_sqlite STATIC src/sqlite/dbtool_sqlite.c)
  target_compile_features(turbodb_dbtool_sqlite PUBLIC c_std_11)
  target_include_directories(turbodb_dbtool_sqlite
                             PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(turbodb_dbtool_sqlite PUBLIC turbodb_dbtool_core
                                                     SQLite::SQLite3)
  cmake_config_target(turbodb_dbtool_sqlite NO_INSTALL FOLDER "dbtools")
  dbtool_enable_strict_warnings(turbodb_dbtool_sqlite)

  add_executable(turbodb-sqlite src/sqlite/main.c)
  target_compile_features(turbodb-sqlite PRIVATE c_std_11)
  target_link_libraries(turbodb-sqlite PRIVATE turbodb_dbtool_sqlite)
  set_target_properties(turbodb-sqlite PROPERTIES FOLDER "dbtools")
  dbtool_enable_strict_warnings(turbodb-sqlite)
  dbtool_stage_runtime_dependencies(turbodb-sqlite)
  install(TARGETS turbodb-sqlite RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(TURBODB_DBTOOLS_WITH_PGSQL)
  add_library(turbodb_dbtool_postgresql STATIC
              src/postgresql/dbtool_postgresql.c)
  target_compile_features(turbodb_dbtool_postgresql PUBLIC c_std_11)
  target_include_directories(turbodb_dbtool_postgresql
                             PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(turbodb_dbtool_postgresql
                        PUBLIC turbodb_dbtool_core PostgreSQL::PostgreSQL)
  cmake_config_target(turbodb_dbtool_postgresql NO_INSTALL FOLDER "dbtools")
  dbtool_enable_strict_warnings(turbodb_dbtool_postgresql)

  add_executable(turbodb-postgresql src/postgresql/main.c)
  target_compile_features(turbodb-postgresql PRIVATE c_std_11)
  target_link_libraries(turbodb-postgresql PRIVATE turbodb_dbtool_postgresql)
  set_target_properties(turbodb-postgresql PROPERTIES FOLDER "dbtools")
  dbtool_enable_strict_warnings(turbodb-postgresql)
  dbtool_enable_postgresql_linker(turbodb-postgresql)
  dbtool_stage_runtime_dependencies(turbodb-postgresql)
  install(TARGETS turbodb-postgresql
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(BUILD_TESTS)
  cmake_add_test(
    NAME dbtool_file_test
    SOURCES tests/dbtool_file_test.c
    LIBS turbodb_dbtool_core TurboUtils::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src
    FOLDER "dbtools/tests")
  cmake_add_test(
    NAME dbtool_cli_test
    SOURCES tests/dbtool_cli_test.c
    LIBS turbodb_dbtool_core TurboUtils::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src
    FOLDER "dbtools/tests")
  if(TURBODB_DBTOOLS_WITH_SQLITE)
    cmake_add_test(
      NAME dbtool_sqlite_test
      SOURCES tests/dbtool_sqlite_test.c
      LIBS turbodb_dbtool_sqlite TurboUtils::TinyTest
      INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src
      FOLDER "dbtools/tests")
    dbtool_enable_strict_warnings(dbtool_sqlite_test)
  endif()
  if(TURBODB_DBTOOLS_WITH_PGSQL)
    cmake_add_test(
      NAME dbtool_postgresql_test
      SOURCES tests/dbtool_postgresql_test.c tests/fake_libpq.c
      LIBS turbodb_dbtool_postgresql TurboUtils::TinyTest
      INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src
               ${CMAKE_CURRENT_SOURCE_DIR}/tests
      FOLDER "dbtools/tests")
    dbtool_enable_strict_warnings(dbtool_postgresql_test)
    dbtool_enable_postgresql_linker(dbtool_postgresql_test)
    if(TURBODB_DBTOOLS_PG_LIVE_TESTS)
      cmake_add_test(
        NAME dbtool_postgresql_live_test
        SOURCES tests/postgresql_live_test.c
        LIBS turbodb_dbtool_postgresql TurboUtils::TinyTest
        INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src
        FOLDER "dbtools/tests")
      dbtool_enable_strict_warnings(dbtool_postgresql_live_test)
      dbtool_enable_postgresql_linker(dbtool_postgresql_live_test)
    endif()
  endif()
endif()

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
  else()
    set_property(TARGET ${TARGET_NAME} PROPERTY INSTALL_RPATH "$ORIGIN/../lib")
  endif()
endfunction()

function(dbtool_collect_runtime_directories TARGET_NAME OUTPUT_VARIABLE)
  if(NOT TARGET ${TARGET_NAME})
    message(FATAL_ERROR "missing dbtools runtime target: ${TARGET_NAME}")
  endif()
  set(collected_directories "${${OUTPUT_VARIABLE}}")
  list(APPEND collected_directories "$<TARGET_FILE_DIR:${TARGET_NAME}>")
  get_target_property(runtime_target_imported ${TARGET_NAME} IMPORTED)
  if(runtime_target_imported)
    set(runtime_artifact_properties IMPORTED_LOCATION IMPORTED_IMPLIB)
    get_target_property(runtime_target_configurations ${TARGET_NAME}
                        IMPORTED_CONFIGURATIONS)
    foreach(runtime_target_configuration IN LISTS runtime_target_configurations)
      string(TOUPPER "${runtime_target_configuration}"
             runtime_target_configuration_upper)
      list(APPEND runtime_artifact_properties
           "IMPORTED_LOCATION_${runtime_target_configuration_upper}"
           "IMPORTED_IMPLIB_${runtime_target_configuration_upper}")
    endforeach()
    foreach(runtime_artifact_property IN LISTS runtime_artifact_properties)
      get_target_property(runtime_artifact ${TARGET_NAME}
                          ${runtime_artifact_property})
      if(runtime_artifact AND NOT runtime_artifact MATCHES "-NOTFOUND$" AND
         NOT runtime_artifact MATCHES "\\$<")
        get_filename_component(runtime_artifact_directory "${runtime_artifact}"
                               DIRECTORY)
        get_filename_component(runtime_artifact_prefix
                               "${runtime_artifact_directory}" DIRECTORY)
        list(APPEND collected_directories "${runtime_artifact_directory}"
             "${runtime_artifact_prefix}/bin")
      endif()
    endforeach()
  endif()
  list(REMOVE_DUPLICATES collected_directories)
  set(${OUTPUT_VARIABLE} "${collected_directories}" PARENT_SCOPE)
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
  install(
    TARGETS turbodb-sqlite
    RUNTIME_DEPENDENCY_SET DbToolsRuntimeDependencies
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT dbtools)
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
  install(
    TARGETS turbodb-postgresql
    RUNTIME_DEPENDENCY_SET DbToolsRuntimeDependencies
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT dbtools)
endif()

set(dbtools_runtime_targets TurboUtils::Core)
if(TURBODB_DBTOOLS_WITH_SQLITE)
  list(APPEND dbtools_runtime_targets SQLite::SQLite3)
endif()
if(TURBODB_DBTOOLS_WITH_PGSQL)
  list(APPEND dbtools_runtime_targets PostgreSQL::PostgreSQL)
endif()
set(dbtools_runtime_directories)
foreach(dbtools_runtime_target IN LISTS dbtools_runtime_targets)
  dbtool_collect_runtime_directories(${dbtools_runtime_target}
                                     dbtools_runtime_directories)
endforeach()
if(WIN32)
  install(
    RUNTIME_DEPENDENCY_SET DbToolsRuntimeDependencies
    DIRECTORIES ${dbtools_runtime_directories}
    PRE_EXCLUDE_REGEXES "api-ms-win-.*" "ext-ms-win-.*"
                        "clang_rt.asan_dynamic-.*"
    POST_EXCLUDE_REGEXES ".*[\\/]system32[\\/].*"
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT dbtools)
else()
  install(
    RUNTIME_DEPENDENCY_SET DbToolsRuntimeDependencies
    DIRECTORIES ${dbtools_runtime_directories}
    PRE_EXCLUDE_REGEXES "ld-linux-.*" "libc\\.so.*" "libdl\\.so.*"
                        "libgcc_s\\.so.*" "libm\\.so.*"
                        "libpthread\\.so.*" "libstdc\\+\\+\\.so.*"
    POST_EXCLUDE_REGEXES "^/lib/.*" "^/usr/lib/.*"
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT dbtools)
endif()

if(BUILD_TESTS)
  if(TURBODB_DBTOOLS_WITH_SQLITE)
    add_test(
      NAME dbtools_package_contract
      COMMAND
        ${CMAKE_COMMAND}
        -DDBTOOLS_PACKAGE_TEST_BUILD_DIR=${CMAKE_BINARY_DIR}
        -DDBTOOLS_PACKAGE_TEST_CONFIG=${CMAKE_BUILD_TYPE}
        -DDBTOOLS_PACKAGE_TEST_FIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sqlite_schema.sql
        -DDBTOOLS_PACKAGE_TEST_WITH_PGSQL=$<BOOL:${TURBODB_DBTOOLS_WITH_PGSQL}>
        -P
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/package/run_dbtools_package_contract.cmake)
  endif()
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

cmake_minimum_required(VERSION 3.21)

foreach(required_variable IN ITEMS DBTOOLS_PACKAGE_TEST_BUILD_DIR
                                   DBTOOLS_PACKAGE_TEST_FIXTURE
                                   DBTOOLS_PACKAGE_TEST_WITH_PGSQL)
  if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

set(test_root "${DBTOOLS_PACKAGE_TEST_BUILD_DIR}/dbtools/tests/package")
set(test_prefix "${test_root}/prefix")
set(test_database "${test_root}/schema.db")
file(TO_CMAKE_PATH "${DBTOOLS_PACKAGE_TEST_BUILD_DIR}/" build_root_normalized)
file(TO_CMAKE_PATH "${test_root}/" test_root_normalized)
string(FIND "${test_root_normalized}" "${build_root_normalized}"
       test_root_position)
if(NOT test_root_position EQUAL 0)
  message(FATAL_ERROR "package test root escaped the build tree")
endif()

file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${test_root}")

set(install_command "${CMAKE_COMMAND}" --install
                    "${DBTOOLS_PACKAGE_TEST_BUILD_DIR}"
                    --prefix "${test_prefix}" --component dbtools)
if(DEFINED DBTOOLS_PACKAGE_TEST_CONFIG AND
   NOT "${DBTOOLS_PACKAGE_TEST_CONFIG}" STREQUAL "")
  list(APPEND install_command --config "${DBTOOLS_PACKAGE_TEST_CONFIG}")
endif()
execute_process(
  COMMAND ${install_command}
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_stdout
  ERROR_VARIABLE install_stderr)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR
          "dbtools staging install failed\nstdout:\n${install_stdout}\n"
          "stderr:\n${install_stderr}")
endif()

if(WIN32)
  set(executable_suffix ".exe")
else()
  set(executable_suffix "")
endif()
set(sqlite_executable
    "${test_prefix}/bin/turbodb-sqlite${executable_suffix}")
if(NOT EXISTS "${sqlite_executable}")
  message(FATAL_ERROR "staged SQLite tool is missing: ${sqlite_executable}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env --unset=PATH
          "${sqlite_executable}" --help
  RESULT_VARIABLE help_result
  OUTPUT_VARIABLE help_stdout
  ERROR_VARIABLE help_stderr)
if(NOT help_result EQUAL 0 OR NOT help_stdout MATCHES "schema apply")
  message(FATAL_ERROR
          "installed SQLite help failed (${help_result})\n"
          "stdout:\n${help_stdout}\nstderr:\n${help_stderr}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env --unset=PATH
          "${sqlite_executable}" schema apply --database "${test_database}"
          --file "${DBTOOLS_PACKAGE_TEST_FIXTURE}"
  RESULT_VARIABLE apply_result
  OUTPUT_VARIABLE apply_stdout
  ERROR_VARIABLE apply_stderr)
if(NOT apply_result EQUAL 0 OR NOT apply_stdout MATCHES "statements=4")
  message(FATAL_ERROR
          "installed SQLite apply failed (${apply_result})\n"
          "stdout:\n${apply_stdout}\nstderr:\n${apply_stderr}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env --unset=PATH
          "${sqlite_executable}" schema apply --database "${test_database}"
          --file "${DBTOOLS_PACKAGE_TEST_FIXTURE}"
  RESULT_VARIABLE duplicate_result
  OUTPUT_VARIABLE duplicate_stdout
  ERROR_VARIABLE duplicate_stderr)
if(NOT duplicate_result EQUAL 5 OR
   NOT duplicate_stderr MATCHES "already exists")
  message(FATAL_ERROR
          "installed SQLite duplicate contract failed (${duplicate_result})\n"
          "stdout:\n${duplicate_stdout}\nstderr:\n${duplicate_stderr}")
endif()

if(DBTOOLS_PACKAGE_TEST_WITH_PGSQL)
  set(postgresql_executable
      "${test_prefix}/bin/turbodb-postgresql${executable_suffix}")
  if(NOT EXISTS "${postgresql_executable}")
    message(FATAL_ERROR
            "staged PostgreSQL tool is missing: ${postgresql_executable}")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env --unset=PATH
            "${postgresql_executable}" --help
    RESULT_VARIABLE postgresql_help_result
    OUTPUT_VARIABLE postgresql_help_stdout
    ERROR_VARIABLE postgresql_help_stderr)
  if(NOT postgresql_help_result EQUAL 0 OR
     NOT postgresql_help_stdout MATCHES "conninfo-env")
    message(FATAL_ERROR
            "installed PostgreSQL help failed (${postgresql_help_result})\n"
            "stdout:\n${postgresql_help_stdout}\n"
            "stderr:\n${postgresql_help_stderr}")
  endif()
endif()

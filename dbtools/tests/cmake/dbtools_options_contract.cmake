if(NOT DEFINED TURBODB_SOURCE_DIR OR TURBODB_SOURCE_DIR STREQUAL "")
  message(FATAL_ERROR "TURBODB_SOURCE_DIR is required")
endif()

include("${TURBODB_SOURCE_DIR}/dbtools/cmake/DbToolsOptions.cmake")

function(assert_validation EXPECTED)
  turbodb_dbtools_validate_options(_actual)
  if(NOT "${_actual}" STREQUAL "${EXPECTED}")
    message(FATAL_ERROR
            "validation mismatch: expected='${EXPECTED}' actual='${_actual}'")
  endif()
endfunction()

set(TURBODB_BUILD_DBTOOLS OFF)
set(TURBODB_DBTOOLS_WITH_PGSQL OFF)
set(TURBODB_DBTOOLS_PG_LIVE_TESTS OFF)
set(BUILD_TESTS ON)
assert_validation("")

set(TURBODB_DBTOOLS_PG_LIVE_TESTS ON)
assert_validation(
  "TURBODB_DBTOOLS_PG_LIVE_TESTS requires TURBODB_BUILD_DBTOOLS=ON")

set(TURBODB_BUILD_DBTOOLS ON)
assert_validation(
  "TURBODB_DBTOOLS_PG_LIVE_TESTS requires TURBODB_DBTOOLS_WITH_PGSQL=ON")

set(TURBODB_DBTOOLS_WITH_PGSQL ON)
set(BUILD_TESTS OFF)
assert_validation("TURBODB_DBTOOLS_PG_LIVE_TESTS requires BUILD_TESTS=ON")

set(BUILD_TESTS ON)
set(ENV{TURBODB_DBTOOLS_PG_TEST_CONNINFO} "")
assert_validation(
  "TURBODB_DBTOOLS_PG_LIVE_TESTS requires TURBODB_DBTOOLS_PG_TEST_CONNINFO")

set(ENV{TURBODB_DBTOOLS_PG_TEST_CONNINFO} "host=contract-test")
assert_validation("")

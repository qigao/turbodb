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

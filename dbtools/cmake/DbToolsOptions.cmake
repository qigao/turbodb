function(turbodb_dbtools_validate_options OUT_ERROR)
  set(_error "")
  if(TURBODB_DBTOOLS_PG_LIVE_TESTS)
    if(NOT TURBODB_BUILD_DBTOOLS)
      set(_error
          "TURBODB_DBTOOLS_PG_LIVE_TESTS requires TURBODB_BUILD_DBTOOLS=ON")
    elseif(NOT TURBODB_DBTOOLS_WITH_PGSQL)
      set(_error
          "TURBODB_DBTOOLS_PG_LIVE_TESTS requires TURBODB_DBTOOLS_WITH_PGSQL=ON")
    elseif(NOT BUILD_TESTS)
      set(_error "TURBODB_DBTOOLS_PG_LIVE_TESTS requires BUILD_TESTS=ON")
    elseif(NOT DEFINED ENV{TURBODB_DBTOOLS_PG_TEST_CONNINFO} OR
           "$ENV{TURBODB_DBTOOLS_PG_TEST_CONNINFO}" STREQUAL "")
      set(_error
          "TURBODB_DBTOOLS_PG_LIVE_TESTS requires TURBODB_DBTOOLS_PG_TEST_CONNINFO")
    endif()
  endif()
  set(${OUT_ERROR} "${_error}" PARENT_SCOPE)
endfunction()

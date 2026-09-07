cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED TURBODB_SOURCE_DIR OR "${TURBODB_SOURCE_DIR}" STREQUAL "")
  message(FATAL_ERROR "TURBODB_SOURCE_DIR is required")
endif()

set(preset_file "${TURBODB_SOURCE_DIR}/CMakeUserPresets.json")
file(READ "${preset_file}" presets_json)
string(JSON preset_count LENGTH "${presets_json}" configurePresets)
math(EXPR last_preset_index "${preset_count} - 1")

function(require_preset_cache preset_name variable_name expected_value)
  set(found FALSE)
  foreach(preset_index RANGE 0 ${last_preset_index})
    string(JSON candidate_name GET "${presets_json}" configurePresets
           ${preset_index} name)
    if(candidate_name STREQUAL preset_name)
      set(found TRUE)
      string(JSON actual_value ERROR_VARIABLE value_error
             GET "${presets_json}" configurePresets ${preset_index}
             cacheVariables ${variable_name})
      if(value_error)
        message(FATAL_ERROR
                "preset ${preset_name} must explicitly set ${variable_name}")
      endif()
      if(NOT actual_value STREQUAL expected_value)
        message(FATAL_ERROR
                "preset ${preset_name} sets ${variable_name}=${actual_value}; expected ${expected_value}")
      endif()
      break()
    endif()
  endforeach()
  if(NOT found)
    message(FATAL_ERROR "missing configure preset: ${preset_name}")
  endif()
endfunction()

foreach(full_preset IN ITEMS win-dev-user win-release-user linux-dev-user
                            linux-release-user)
  require_preset_cache("${full_preset}" ORM_WITH_SQLITE ON)
  require_preset_cache("${full_preset}" ORM_WITH_PGSQL ON)
endforeach()

foreach(postgresql_preset IN ITEMS win-release-dbtools-pg-user
                                  linux-release-pg-live-user)
  require_preset_cache("${postgresql_preset}" ORM_WITH_SQLITE OFF)
  require_preset_cache("${postgresql_preset}"
                       TURBODB_DBTOOLS_WITH_SQLITE OFF)
  require_preset_cache("${postgresql_preset}" ORM_WITH_PGSQL ON)
  require_preset_cache("${postgresql_preset}" TURBODB_DBTOOLS_WITH_PGSQL ON)
endforeach()

string(CONCAT full_install_prefix "$" "env{PKG_ROOT}/turbodb/release")
string(CONCAT postgresql_install_prefix "$"
       "env{PKG_ROOT}/turbodb/release-pg")
require_preset_cache(win-release-user CMAKE_INSTALL_PREFIX
                     "${full_install_prefix}")
require_preset_cache(win-release-dbtools-pg-user CMAKE_INSTALL_PREFIX
                     "${postgresql_install_prefix}")
require_preset_cache(linux-release-user CMAKE_INSTALL_PREFIX
                     "${full_install_prefix}")
require_preset_cache(linux-release-pg-live-user CMAKE_INSTALL_PREFIX
                     "${postgresql_install_prefix}")

# Lexer generation must use the explicitly selected host SDK.
if(DEFINED ENV{TURBODB_RE2C_ROOT})
  set(_turbodb_re2c_input "$ENV{TURBODB_RE2C_ROOT}")
else()
  set(_turbodb_re2c_input "$ENV{RE2C_ROOT}")
endif()
if(NOT IS_DIRECTORY "${_turbodb_re2c_input}")
    message(FATAL_ERROR "RE2C_ROOT must name the restored host re2c package")
endif()
unset(RE2C_EXECUTABLE CACHE)
unset(RE2C_EXECUTABLE)
file(TO_CMAKE_PATH "${_turbodb_re2c_input}" _re2c_root)
find_program(RE2C_EXECUTABLE re2c PATHS "${_re2c_root}/bin"
             NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
file(REAL_PATH "${_turbodb_re2c_input}" _re2c_root)
file(REAL_PATH "${RE2C_EXECUTABLE}" _re2c_executable)
cmake_path(IS_PREFIX _re2c_root "${_re2c_executable}" NORMALIZE _re2c_in_root)
if(NOT _re2c_in_root)
    message(FATAL_ERROR "re2c executable is outside RE2C_ROOT: ${RE2C_EXECUTABLE}")
endif()

# Both native and cross builds execute the selected host SDK tools.
if(NOT DEFINED ENV{LEMON_ROOT} OR NOT IS_DIRECTORY "$ENV{LEMON_ROOT}")
    message(FATAL_ERROR "LEMON_ROOT must name the restored host lemon package")
endif()
unset(LEMON_EXECUTABLE CACHE)
unset(LEMON_EXECUTABLE)
file(TO_CMAKE_PATH "$ENV{LEMON_ROOT}" _lemon_root)
find_program(LEMON_EXECUTABLE lemon PATHS "${_lemon_root}/bin"
             NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
file(REAL_PATH "$ENV{LEMON_ROOT}" _lemon_root)
file(REAL_PATH "${LEMON_EXECUTABLE}" _lemon_executable)
cmake_path(IS_PREFIX _lemon_root "${_lemon_executable}" NORMALIZE _lemon_in_root)
if(NOT _lemon_in_root)
    message(FATAL_ERROR "lemon executable is outside LEMON_ROOT: ${LEMON_EXECUTABLE}")
endif()
set(LEMON_DEPENDS "${LEMON_EXECUTABLE}")

unset(LEMPAR CACHE)
unset(LEMPAR)
find_file(LEMPAR lempar.c PATHS "${_lemon_root}/share/lemon"
          NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)

# Note: These variables are set in the root scope and will be inherited
# by all subdirectories added via add_subdirectory().

message(STATUS "Tools detection:")
message(STATUS "  re2c: ${RE2C_EXECUTABLE}")
message(STATUS "  lemon: ${LEMON_EXECUTABLE}")
message(STATUS "  lempar: ${LEMPAR}")

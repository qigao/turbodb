# Find re2c
find_program(RE2C_EXECUTABLE re2c)
if(NOT RE2C_EXECUTABLE)
    message(WARNING "re2c not found - some lexers might not be generated")
endif()

# Find Lemon.
#
# The SQL parser grammar relies on extensions implemented by the repository's
# tools/lemon/lemon.c. A distro/system Lemon is therefore not an ABI-compatible
# generator and must never be selected as a fallback.
#
# Native builds can execute the normal CMake lemon target. Cross builds cannot:
# that target is compiled for the target architecture. Build the same repository
# source once with a host C compiler and make grammar generation depend on it.
if(LEMON_EXECUTABLE)
    if(NOT EXISTS "${LEMON_EXECUTABLE}")
        message(FATAL_ERROR "Configured host lemon executable does not exist: ${LEMON_EXECUTABLE}")
    endif()
    set(LEMON_DEPENDS "")
    message(STATUS "Using configured host lemon: ${LEMON_EXECUTABLE}")
elseif(TARGET lemon AND NOT CMAKE_CROSSCOMPILING)
    set(LEMON_EXECUTABLE $<TARGET_FILE:lemon>)
    set(LEMON_DEPENDS lemon)
    message(STATUS "Using project-provided lemon target")
elseif(CMAKE_CROSSCOMPILING)
    find_program(
        LEMON_HOST_C_COMPILER
        NAMES cc gcc clang
        NO_CMAKE_FIND_ROOT_PATH
        REQUIRED)
    set(LEMON_HOST_DIR "${CMAKE_BINARY_DIR}/host-tools")
    set(LEMON_HOST_EXECUTABLE "${LEMON_HOST_DIR}/lemon")
    add_custom_command(
        OUTPUT "${LEMON_HOST_EXECUTABLE}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${LEMON_HOST_DIR}"
        COMMAND "${LEMON_HOST_C_COMPILER}"
                "${CMAKE_SOURCE_DIR}/tools/lemon/lemon.c"
                -O2
                -o "${LEMON_HOST_EXECUTABLE}"
        DEPENDS "${CMAKE_SOURCE_DIR}/tools/lemon/lemon.c"
        VERBATIM
        COMMENT "Building repository Lemon for the host")
    add_custom_target(lemon_host DEPENDS "${LEMON_HOST_EXECUTABLE}")
    set(LEMON_EXECUTABLE "${LEMON_HOST_EXECUTABLE}")
    set(LEMON_DEPENDS lemon_host)
    message(STATUS
        "Cross-compiling: using repository host lemon: ${LEMON_EXECUTABLE}")
else()
    message(FATAL_ERROR
        "Project-provided Lemon target is unavailable. "
        "Set LEMON_EXECUTABLE to the repository-compatible host generator.")
endif()

# Set path to lemon parser template
set(LEMPAR "${CMAKE_SOURCE_DIR}/tools/lemon/lempar.c" CACHE PATH "Path to lemon parser template")

# Note: These variables are set in the root scope and will be inherited 
# by all subdirectories added via add_subdirectory().

message(STATUS "Tools detection:")
message(STATUS "  re2c: ${RE2C_EXECUTABLE}")
message(STATUS "  lemon: ${LEMON_EXECUTABLE}")
message(STATUS "  lempar: ${LEMPAR}")

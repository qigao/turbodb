# Find re2c
find_program(RE2C_EXECUTABLE re2c)
if(NOT RE2C_EXECUTABLE)
    message(WARNING "re2c not found - some lexers might not be generated")
endif()

# Find lemon (use an explicitly configured host tool, built-in target, or system tool).
# A cross-compiled lemon cannot run on the host.
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
    find_program(LEMON_EXECUTABLE lemon)
    if(LEMON_EXECUTABLE)
        message(STATUS "Cross-compiling: using system host lemon: ${LEMON_EXECUTABLE}")
    else()
        message(FATAL_ERROR
            "Cross-compiling requires a host lemon executable. "
            "Set LEMON_EXECUTABLE to a runnable host tool or install lemon on the host.")
    endif()
    set(LEMON_DEPENDS "")
else()
    # Fallback to system lemon only if project target not available
    find_program(LEMON_EXECUTABLE lemon)
    if(NOT LEMON_EXECUTABLE)
        message(WARNING "lemon not found - some parsers might not be generated")
    else()
        message(WARNING "Using system lemon - version mismatch may occur!")
    endif()
    set(LEMON_DEPENDS "")
endif()

# Set path to lemon parser template
set(LEMPAR "${CMAKE_SOURCE_DIR}/tools/lemon/lempar.c" CACHE PATH "Path to lemon parser template")

# Note: These variables are set in the root scope and will be inherited 
# by all subdirectories added via add_subdirectory().

message(STATUS "Tools detection:")
message(STATUS "  re2c: ${RE2C_EXECUTABLE}")
message(STATUS "  lemon: ${LEMON_EXECUTABLE}")
message(STATUS "  lempar: ${LEMPAR}")

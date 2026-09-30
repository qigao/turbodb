# Project-independent CMake utilities.
# Project paths, export sets and code generators are supplied by callers.
# Compiler policy, dependencies, test labels and runtime environment stay at call sites.
# FOLDER is explicit; this module never infers a project layout or installs test binaries.
include_guard(GLOBAL)

# Configure an existing target. Installation requires an explicit EXPORT_SET;
# NO_INSTALL skips installation. Version defaults use standard PROJECT_VERSION.
function(cmake_config_target target_name)
    set(options NO_INSTALL NO_VERSION)
    set(oneValueArgs FOLDER VERSION SOVERSION EXPORT_NAME EXPORT_SET ALIAS OUTPUT_NAME
                     RUNTIME_DEPENDENCY_SET)
    set(multiValueArgs)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "${options}" "${oneValueArgs}" "${multiValueArgs}")
    if(DEFINED ARG_UNPARSED_ARGUMENTS OR DEFINED ARG_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "cmake_config_target: unrecognized arguments [${ARG_UNPARSED_ARGUMENTS}]; missing values [${ARG_KEYWORDS_MISSING_VALUES}]")
    endif()

    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR "cmake_config_target: target '${target_name}' does not exist")
    endif()
    get_target_property(target_type ${target_name} TYPE)

    if(NOT ARG_NO_INSTALL AND NOT ARG_EXPORT_SET)
        message(FATAL_ERROR "cmake_config_target: EXPORT_SET is required unless NO_INSTALL is specified")
    endif()

    if(ARG_ALIAS)
        if(target_type STREQUAL "EXECUTABLE")
            add_executable(${ARG_ALIAS} ALIAS ${target_name})
        else()
            add_library(${ARG_ALIAS} ALIAS ${target_name})
        endif()
    endif()

    if(ARG_FOLDER)
        set_target_properties(${target_name} PROPERTIES FOLDER ${ARG_FOLDER})
    endif()

    if(ARG_EXPORT_NAME)
        set_target_properties(${target_name} PROPERTIES EXPORT_NAME ${ARG_EXPORT_NAME})
    endif()

    if(ARG_OUTPUT_NAME)
        set_target_properties(${target_name} PROPERTIES OUTPUT_NAME ${ARG_OUTPUT_NAME})
    endif()

    if(NOT ARG_NO_VERSION AND
       (target_type STREQUAL "SHARED_LIBRARY" OR target_type STREQUAL "STATIC_LIBRARY"))
        if(NOT DEFINED ARG_VERSION AND DEFINED PROJECT_VERSION AND NOT PROJECT_VERSION STREQUAL "")
            set(ARG_VERSION ${PROJECT_VERSION})
        endif()
        if(NOT DEFINED ARG_SOVERSION AND DEFINED PROJECT_VERSION_MAJOR AND NOT PROJECT_VERSION_MAJOR STREQUAL "")
            set(ARG_SOVERSION ${PROJECT_VERSION_MAJOR})
        endif()

        if(DEFINED ARG_VERSION AND NOT ARG_VERSION STREQUAL "")
          set_target_properties(${target_name} PROPERTIES VERSION ${ARG_VERSION})
        endif()
        if(DEFINED ARG_SOVERSION AND NOT ARG_SOVERSION STREQUAL "")
          set_target_properties(${target_name} PROPERTIES SOVERSION ${ARG_SOVERSION})
        endif()
    endif()

    if(NOT ARG_NO_INSTALL)
        include(GNUInstallDirs)

        set(runtime_dependency_args)
        if(ARG_RUNTIME_DEPENDENCY_SET)
            list(APPEND runtime_dependency_args
                 RUNTIME_DEPENDENCY_SET ${ARG_RUNTIME_DEPENDENCY_SET})
        endif()
        install(
            TARGETS ${target_name}
            EXPORT ${ARG_EXPORT_SET}
            ${runtime_dependency_args}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()

endfunction()

# PACKAGE and CONFIG_TEMPLATE are required. The caller owns the template and
# dependency discovery it contains; this helper only generates/installs the package.
function(cmake_config_package)
    set(options)
    set(oneValueArgs PACKAGE EXPORT_SET NAMESPACE CONFIG_TEMPLATE VERSION COMPATIBILITY DESTINATION)
    set(multiValueArgs)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "${options}" "${oneValueArgs}" "${multiValueArgs}")
    if(DEFINED ARG_UNPARSED_ARGUMENTS OR DEFINED ARG_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "cmake_config_package: unrecognized arguments [${ARG_UNPARSED_ARGUMENTS}]; missing values [${ARG_KEYWORDS_MISSING_VALUES}]")
    endif()

    if(NOT ARG_PACKAGE)
        message(FATAL_ERROR "cmake_config_package: PACKAGE is required")
    endif()
    if(NOT ARG_CONFIG_TEMPLATE)
        message(FATAL_ERROR "cmake_config_package: CONFIG_TEMPLATE is required")
    endif()
    if(NOT ARG_EXPORT_SET)
        set(ARG_EXPORT_SET "${ARG_PACKAGE}Targets")
    endif()
    if(NOT ARG_NAMESPACE)
        set(ARG_NAMESPACE "${ARG_PACKAGE}::")
    endif()
    if(NOT ARG_VERSION)
        set(ARG_VERSION "${PROJECT_VERSION}")
    endif()
    if(NOT ARG_VERSION)
        message(FATAL_ERROR "cmake_config_package: VERSION is required when PROJECT_VERSION is empty")
    endif()
    if(NOT ARG_COMPATIBILITY)
        set(ARG_COMPATIBILITY SameMajorVersion)
    endif()
    if(NOT ARG_DESTINATION)
        include(GNUInstallDirs)
        set(ARG_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${ARG_PACKAGE}")
    endif()
    if(NOT EXISTS "${ARG_CONFIG_TEMPLATE}")
        message(FATAL_ERROR
            "cmake_config_package: config template does not exist: ${ARG_CONFIG_TEMPLATE}")
    endif()

    include(CMakePackageConfigHelpers)
    set(config_file "${CMAKE_CURRENT_BINARY_DIR}/${ARG_PACKAGE}Config.cmake")
    set(version_file "${CMAKE_CURRENT_BINARY_DIR}/${ARG_PACKAGE}ConfigVersion.cmake")

    configure_package_config_file(
        "${ARG_CONFIG_TEMPLATE}"
        "${config_file}"
        INSTALL_DESTINATION "${ARG_DESTINATION}")
    write_basic_package_version_file(
        "${version_file}"
        VERSION "${ARG_VERSION}"
        COMPATIBILITY "${ARG_COMPATIBILITY}")

    install(
        EXPORT ${ARG_EXPORT_SET}
        FILE "${ARG_PACKAGE}Targets.cmake"
        NAMESPACE "${ARG_NAMESPACE}"
        DESTINATION "${ARG_DESTINATION}")
    install(
        FILES "${config_file}" "${version_file}"
        DESTINATION "${ARG_DESTINATION}")
endfunction()

function(cmake_install_headers)
    set(options)
    set(oneValueArgs DIRECTORY DESTINATION)
    set(multiValueArgs FILES PATTERNS EXCLUDES)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "${options}" "${oneValueArgs}" "${multiValueArgs}")
    if(DEFINED ARG_UNPARSED_ARGUMENTS OR DEFINED ARG_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "cmake_install_headers: unrecognized arguments [${ARG_UNPARSED_ARGUMENTS}]; missing values [${ARG_KEYWORDS_MISSING_VALUES}]")
    endif()

    if(NOT ARG_DESTINATION)
        include(GNUInstallDirs)
        set(ARG_DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    endif()

    if(ARG_FILES)
        install(FILES ${ARG_FILES} DESTINATION ${ARG_DESTINATION})
    endif()

    if(ARG_DIRECTORY)
        set(match_args FILES_MATCHING PATTERN "*.h")
        foreach(p ${ARG_PATTERNS})
            list(APPEND match_args PATTERN "${p}")
        endforeach()
        foreach(e ${ARG_EXCLUDES})
            list(APPEND match_args PATTERN "${e}" EXCLUDE)
        endforeach()

        install(DIRECTORY ${ARG_DIRECTORY}
            DESTINATION ${ARG_DESTINATION}
            ${match_args}
        )
    endif()
endfunction()

# LEXER_RE requires RE2C_EXECUTABLE. GRAMMAR_Y requires LEMON_EXECUTABLE
# and LEMON_TEMPLATE. Tool targets can be supplied through the dependency lists.
# Generated file paths and codegen targets are returned as <TARGET_NAME>_*.
function(cmake_add_grammar TARGET_NAME)
  set(options LEXER_DEPENDS_ON_GRAMMAR)
  set(oneValueArgs LEXER_RE GRAMMAR_Y FOLDER LEXER_OUTPUT RE2C_EXECUTABLE
                   LEMON_EXECUTABLE LEMON_TEMPLATE)
  set(multiValueArgs LEXER_DEPENDS GRAMMAR_DEPENDS)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "${options}" "${oneValueArgs}" "${multiValueArgs}")
  if(DEFINED ARG_UNPARSED_ARGUMENTS OR DEFINED ARG_KEYWORDS_MISSING_VALUES)
      message(FATAL_ERROR "cmake_add_grammar: unrecognized arguments [${ARG_UNPARSED_ARGUMENTS}]; missing values [${ARG_KEYWORDS_MISSING_VALUES}]")
  endif()
  if(NOT ARG_LEXER_RE AND NOT ARG_GRAMMAR_Y)
    message(FATAL_ERROR "cmake_add_grammar: LEXER_RE or GRAMMAR_Y is required")
  endif()
  if(ARG_LEXER_RE AND NOT ARG_RE2C_EXECUTABLE)
    message(FATAL_ERROR "cmake_add_grammar: LEXER_RE requires RE2C_EXECUTABLE")
  endif()
  if(ARG_GRAMMAR_Y AND (NOT ARG_LEMON_EXECUTABLE OR NOT ARG_LEMON_TEMPLATE))
    message(FATAL_ERROR "cmake_add_grammar: GRAMMAR_Y requires LEMON_EXECUTABLE and LEMON_TEMPLATE")
  endif()
  if(ARG_LEXER_DEPENDS_ON_GRAMMAR AND (NOT ARG_LEXER_RE OR NOT ARG_GRAMMAR_Y))
    message(FATAL_ERROR "cmake_add_grammar: LEXER_DEPENDS_ON_GRAMMAR requires LEXER_RE and GRAMMAR_Y")
  endif()
  string(TOLOWER "${TARGET_NAME}" target_name_lower)

  if(ARG_GRAMMAR_Y)
    set(GRAMMAR_H "${CMAKE_CURRENT_BINARY_DIR}/${target_name_lower}_grammar_gen.h")
  endif()

  if(ARG_LEXER_RE)
    if(ARG_LEXER_OUTPUT)
      set(LEXER_GEN "${CMAKE_CURRENT_BINARY_DIR}/${ARG_LEXER_OUTPUT}")
    else()
      set(LEXER_GEN "${CMAKE_CURRENT_BINARY_DIR}/${target_name_lower}_lexer_gen.c")
    endif()
    set(lexer_depends ${ARG_LEXER_RE} ${ARG_LEXER_DEPENDS})
    if(ARG_LEXER_DEPENDS_ON_GRAMMAR)
      list(APPEND lexer_depends ${GRAMMAR_H})
    endif()
    add_custom_command(
      OUTPUT ${LEXER_GEN}
      COMMAND "${ARG_RE2C_EXECUTABLE}" -o ${LEXER_GEN} ${ARG_LEXER_RE}
      DEPENDS ${lexer_depends}
      COMMENT "Generating ${TARGET_NAME} lexer with re2c"
      VERBATIM)
    set(LEXER_TARGET "${TARGET_NAME}_lexer_codegen")
    add_custom_target(${LEXER_TARGET} DEPENDS ${LEXER_GEN})
    if(ARG_FOLDER)
      set_target_properties(${LEXER_TARGET} PROPERTIES FOLDER ${ARG_FOLDER})
    endif()
    set(${TARGET_NAME}_LEXER_GEN ${LEXER_GEN} PARENT_SCOPE)
    set(${TARGET_NAME}_LEXER_TARGET ${LEXER_TARGET} PARENT_SCOPE)
  endif()

  if(ARG_GRAMMAR_Y)
    set(GRAMMAR_GEN "${CMAKE_CURRENT_BINARY_DIR}/${target_name_lower}_grammar_gen.c")
    set(GRAMMAR_Y_GEN "${CMAKE_CURRENT_BINARY_DIR}/${target_name_lower}_grammar_gen.y")
    add_custom_command(
      OUTPUT ${GRAMMAR_GEN} ${GRAMMAR_H}
      COMMAND ${CMAKE_COMMAND} -E copy ${ARG_GRAMMAR_Y} ${GRAMMAR_Y_GEN}
      COMMAND "${ARG_LEMON_EXECUTABLE}" "-T${ARG_LEMON_TEMPLATE}" ${GRAMMAR_Y_GEN}
      DEPENDS ${ARG_GRAMMAR_Y} ${ARG_LEMON_TEMPLATE} ${ARG_GRAMMAR_DEPENDS}
      COMMENT "Generating ${TARGET_NAME} parser with lemon"
      VERBATIM)
    set(GRAMMAR_TARGET "${TARGET_NAME}_grammar_codegen")
    add_custom_target(${GRAMMAR_TARGET} DEPENDS ${GRAMMAR_GEN} ${GRAMMAR_H})
    if(ARG_FOLDER)
      set_target_properties(${GRAMMAR_TARGET} PROPERTIES FOLDER ${ARG_FOLDER})
    endif()
    set(${TARGET_NAME}_GRAMMAR_GEN ${GRAMMAR_GEN} PARENT_SCOPE)
    set(${TARGET_NAME}_GRAMMAR_H ${GRAMMAR_H} PARENT_SCOPE)
    set(${TARGET_NAME}_GRAMMAR_TARGET ${GRAMMAR_TARGET} PARENT_SCOPE)
  endif()
endfunction()

# DIRS is explicit; no source-tree layout is assumed.
function(cmake_add_source VAR)
  set(options RECURSE)
  set(oneValueArgs)
  set(multiValueArgs DIRS EXCLUDES PATTERNS)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "${options}" "${oneValueArgs}" "${multiValueArgs}")
  if(DEFINED ARG_UNPARSED_ARGUMENTS OR DEFINED ARG_KEYWORDS_MISSING_VALUES)
      message(FATAL_ERROR "cmake_add_source: unrecognized arguments [${ARG_UNPARSED_ARGUMENTS}]; missing values [${ARG_KEYWORDS_MISSING_VALUES}]")
  endif()

  set(glob_mode GLOB)
  if(ARG_RECURSE)
    set(glob_mode GLOB_RECURSE)
  endif()

  if(NOT ARG_DIRS)
    message(FATAL_ERROR "cmake_add_source: DIRS is required")
  endif()

  if(NOT ARG_PATTERNS)
    set(ARG_PATTERNS "*.c" "*.cpp" "*.h" "*.hpp" "*.cc" "*.hh")
  endif()

  set(patterns)
  foreach(dir ${ARG_DIRS})
    foreach(pat ${ARG_PATTERNS})
      list(APPEND patterns "${dir}/${pat}")
    endforeach()
  endforeach()

  file(${glob_mode} collected ${patterns})

  if(ARG_EXCLUDES)
    list(REMOVE_ITEM collected ${ARG_EXCLUDES})
  endif()

  set(${VAR} ${collected} PARENT_SCOPE)
endfunction()

# TEST_NAME optionally separates the CTest name from the explicit target NAME.
# TARGET registers another test for an existing executable; ARGS are test arguments.
function(cmake_add_test)
  set(options)
  set(oneValueArgs NAME TARGET TEST_NAME FOLDER)
  set(multiValueArgs SOURCES LIBS DEFS INCLUDES ARGS)
  cmake_parse_arguments(PARSE_ARGV 0 ARG "${options}" "${oneValueArgs}" "${multiValueArgs}")
  if(DEFINED ARG_UNPARSED_ARGUMENTS OR DEFINED ARG_KEYWORDS_MISSING_VALUES)
      message(FATAL_ERROR "cmake_add_test: unrecognized arguments [${ARG_UNPARSED_ARGUMENTS}]; missing values [${ARG_KEYWORDS_MISSING_VALUES}]")
  endif()

  if(ARG_TARGET)
    if(ARG_NAME OR ARG_SOURCES OR ARG_LIBS OR ARG_DEFS OR ARG_INCLUDES OR ARG_FOLDER)
      message(FATAL_ERROR "cmake_add_test: TARGET only accepts TEST_NAME and ARGS")
    endif()
    if(NOT ARG_TEST_NAME OR NOT TARGET "${ARG_TARGET}")
      message(FATAL_ERROR "cmake_add_test: TARGET requires an existing executable and TEST_NAME")
    endif()
    get_target_property(target_type "${ARG_TARGET}" TYPE)
    if(NOT target_type STREQUAL "EXECUTABLE")
      message(FATAL_ERROR "cmake_add_test: TARGET must be an executable")
    endif()
    add_test(NAME ${ARG_TEST_NAME} COMMAND ${ARG_TARGET} ${ARG_ARGS})
    return()
  endif()

  if(ARG_TEST_NAME AND NOT ARG_NAME)
    message(FATAL_ERROR "cmake_add_test: TEST_NAME requires NAME")
  endif()

  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "cmake_add_test: SOURCES is required when creating an executable")
  endif()

  if(ARG_NAME)
    if(TARGET ${ARG_NAME})
      message(FATAL_ERROR "cmake_add_test: target '${ARG_NAME}' already exists")
    endif()
    add_executable(${ARG_NAME} ${ARG_SOURCES})
    target_link_libraries(${ARG_NAME} PRIVATE ${ARG_LIBS})
    target_compile_definitions(${ARG_NAME} PRIVATE ${ARG_DEFS})
    target_include_directories(${ARG_NAME} PRIVATE ${ARG_INCLUDES})
    if(NOT ARG_TEST_NAME)
      set(ARG_TEST_NAME "${ARG_NAME}")
    endif()
    add_test(NAME ${ARG_TEST_NAME} COMMAND ${ARG_NAME} ${ARG_ARGS})
    if(ARG_FOLDER)
      set_target_properties(${ARG_NAME} PROPERTIES FOLDER ${ARG_FOLDER})
    endif()
    return()
  endif()

  foreach(src ${ARG_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    if(TARGET "${name}")
      message(FATAL_ERROR "${CMAKE_CURRENT_FUNCTION}: target '${name}' already exists; use an explicit NAME")
    endif()
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE ${ARG_LIBS})
    target_compile_definitions(${name} PRIVATE ${ARG_DEFS})
    target_include_directories(${name} PRIVATE ${ARG_INCLUDES})
    add_test(NAME ${name} COMMAND ${name} ${ARG_ARGS})

    if(ARG_FOLDER)
      set_target_properties(${name} PROPERTIES FOLDER ${ARG_FOLDER})
    endif()
  endforeach()
endfunction()

function(cmake_add_benchmark)
  set(options)
  set(oneValueArgs NAME FOLDER)
  set(multiValueArgs SOURCES LIBS DEFS INCLUDES)
  cmake_parse_arguments(PARSE_ARGV 0 ARG "${options}" "${oneValueArgs}" "${multiValueArgs}")
  if(DEFINED ARG_UNPARSED_ARGUMENTS OR DEFINED ARG_KEYWORDS_MISSING_VALUES)
      message(FATAL_ERROR "cmake_add_benchmark: unrecognized arguments [${ARG_UNPARSED_ARGUMENTS}]; missing values [${ARG_KEYWORDS_MISSING_VALUES}]")
  endif()

  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "cmake_add_benchmark: SOURCES is required when creating an executable")
  endif()

  if(ARG_NAME)
    if(TARGET ${ARG_NAME})
      message(FATAL_ERROR "cmake_add_benchmark: target '${ARG_NAME}' already exists")
    endif()
    add_executable(${ARG_NAME} ${ARG_SOURCES})
    target_link_libraries(${ARG_NAME} PRIVATE ${ARG_LIBS})
    target_compile_definitions(${ARG_NAME} PRIVATE ${ARG_DEFS})
    target_include_directories(${ARG_NAME} PRIVATE ${ARG_INCLUDES})
    if(ARG_FOLDER)
      set_target_properties(${ARG_NAME} PROPERTIES FOLDER ${ARG_FOLDER})
    endif()
    return()
  endif()

  foreach(src ${ARG_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    if(TARGET "${name}")
      message(FATAL_ERROR "${CMAKE_CURRENT_FUNCTION}: target '${name}' already exists; use an explicit NAME")
    endif()
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE ${ARG_LIBS})
    target_compile_definitions(${name} PRIVATE ${ARG_DEFS})
    target_include_directories(${name} PRIVATE ${ARG_INCLUDES})

    if(ARG_FOLDER)
      set_target_properties(${name} PROPERTIES FOLDER ${ARG_FOLDER})
    endif()
  endforeach()
endfunction()

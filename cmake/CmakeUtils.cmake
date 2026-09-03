# TurboDB CMake utilities

function(cmake_config_target target_name)
    set(options NO_INSTALL NO_VERSION)
    set(oneValueArgs FOLDER VERSION SOVERSION EXPORT_NAME EXPORT_SET ALIAS OUTPUT_NAME
                     RUNTIME_DEPENDENCY_SET)
    set(multiValueArgs)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    get_target_property(target_type ${target_name} TYPE)
    if(NOT target_type)
        message(FATAL_ERROR "cmake_config_target: target '${target_name}' does not exist")
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
        if(NOT ARG_VERSION AND PROJECT_VERSION)
            set(ARG_VERSION ${PROJECT_VERSION})
        endif()
        if(NOT ARG_SOVERSION AND PROJECT_VERSION_MAJOR)
            set(ARG_SOVERSION ${PROJECT_VERSION_MAJOR})
        endif()
        
        if(ARG_VERSION)
          set_target_properties(${target_name} PROPERTIES VERSION ${ARG_VERSION})
        endif()
        if(ARG_SOVERSION)
          set_target_properties(${target_name} PROPERTIES SOVERSION ${ARG_SOVERSION})
        endif()
    endif()

    if(NOT ARG_NO_INSTALL)
        if(ARG_EXPORT_SET)
            set(export_set ${ARG_EXPORT_SET})
        else()
            set(export_set ${CMAKE_CONFIG_TARGET_EXPORT_SET})
        endif()
        if(NOT export_set)
            message(FATAL_ERROR
                "cmake_config_target: EXPORT_SET or CMAKE_CONFIG_TARGET_EXPORT_SET must name the export set")
        endif()

        set(runtime_dependency_args)
        if(ARG_RUNTIME_DEPENDENCY_SET)
            list(APPEND runtime_dependency_args
                 RUNTIME_DEPENDENCY_SET ${ARG_RUNTIME_DEPENDENCY_SET})
        endif()
        install(
            TARGETS ${target_name}
            EXPORT ${export_set}
            ${runtime_dependency_args}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()

endfunction()

function(cmake_config_package)
    set(options)
    set(oneValueArgs PACKAGE EXPORT_SET NAMESPACE CONFIG_TEMPLATE VERSION COMPATIBILITY DESTINATION)
    set(multiValueArgs)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT ARG_PACKAGE)
        message(FATAL_ERROR "cmake_config_package: PACKAGE is required")
    endif()
    if(NOT ARG_EXPORT_SET)
        set(ARG_EXPORT_SET "${ARG_PACKAGE}Targets")
    endif()
    if(NOT ARG_NAMESPACE)
        set(ARG_NAMESPACE "${ARG_PACKAGE}::")
    endif()
    if(NOT ARG_CONFIG_TEMPLATE)
        set(ARG_CONFIG_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${ARG_PACKAGE}Config.cmake.in")
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
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT ARG_DESTINATION)
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

function(cmake_add_grammar TARGET_NAME)
  set(options LEXER_DEPENDS_ON_GRAMMAR)
  set(oneValueArgs LEXER_RE GRAMMAR_Y FOLDER LEXER_OUTPUT)
  set(multiValueArgs LEXER_DEPENDS)
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
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
      if(NOT ARG_GRAMMAR_Y)
        message(FATAL_ERROR "cmake_add_grammar: LEXER_DEPENDS_ON_GRAMMAR requires GRAMMAR_Y")
      endif()
      list(APPEND lexer_depends ${GRAMMAR_H})
    endif()
    add_custom_command(
      OUTPUT ${LEXER_GEN}
      COMMAND ${RE2C_EXECUTABLE} -o ${LEXER_GEN} ${ARG_LEXER_RE}
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
      COMMAND ${LEMON_EXECUTABLE} -T${LEMPAR} ${GRAMMAR_Y_GEN}
      DEPENDS ${ARG_GRAMMAR_Y} ${LEMON_DEPENDS}
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

function(cmake_add_source VAR)
  set(options RECURSE)
  set(oneValueArgs)
  set(multiValueArgs DIRS EXCLUDES PATTERNS)
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  set(glob_mode GLOB)
  if(ARG_RECURSE)
    set(glob_mode GLOB_RECURSE)
  endif()

  if(NOT ARG_DIRS)
    set(ARG_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/include")
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

function(cmake_add_test)
  set(options)
  set(oneValueArgs NAME FOLDER)
  set(multiValueArgs SOURCES LIBS DEFS INCLUDES)
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(ARG_NAME)
    if(TARGET ${ARG_NAME})
      message(FATAL_ERROR "cmake_add_test: target '${ARG_NAME}' already exists")
    endif()
    add_executable(${ARG_NAME} ${ARG_SOURCES})
    target_link_libraries(${ARG_NAME} PRIVATE ${ARG_LIBS})
    target_compile_definitions(${ARG_NAME} PRIVATE ${ARG_DEFS})
    target_include_directories(${ARG_NAME} PRIVATE ${ARG_INCLUDES})
    add_test(NAME ${ARG_NAME} COMMAND ${ARG_NAME})
    if(ARG_FOLDER)
      set_target_properties(${ARG_NAME} PROPERTIES FOLDER ${ARG_FOLDER})
    endif()
    return()
  endif()

  foreach(src ${ARG_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    if(NOT TARGET ${name})
      add_executable(${name} ${src})
      target_link_libraries(${name} PRIVATE ${ARG_LIBS})
      target_compile_definitions(${name} PRIVATE ${ARG_DEFS})
      target_include_directories(${name} PRIVATE ${ARG_INCLUDES})
      add_test(NAME ${name} COMMAND ${name})
      
      if(ARG_FOLDER)
        set_target_properties(${name} PROPERTIES FOLDER ${ARG_FOLDER})
      endif()
      
    endif()
  endforeach()
endfunction()

function(cmake_add_benchmark)
  set(options)
  set(oneValueArgs NAME FOLDER)
  set(multiValueArgs SOURCES LIBS DEFS INCLUDES)
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

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
    if(NOT TARGET ${name})
      add_executable(${name} ${src})
      target_link_libraries(${name} PRIVATE ${ARG_LIBS})
      target_compile_definitions(${name} PRIVATE ${ARG_DEFS})
      target_include_directories(${name} PRIVATE ${ARG_INCLUDES})

      if(ARG_FOLDER)
        set_target_properties(${name} PROPERTIES FOLDER ${ARG_FOLDER})
      endif()
    endif()
  endforeach()
endfunction()

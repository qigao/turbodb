function(orm_add_schema_validation validation_target schema_file)
  if(NOT validation_target OR NOT schema_file)
    message(FATAL_ERROR
            "orm_add_schema_validation requires a target name and schema file")
  endif()
  if(NOT TARGET orm_schema_validate)
    message(FATAL_ERROR
            "orm_add_schema_validation requires ORM_BUILD_SCHEMA_TOOLS=ON")
  endif()
  if(NOT EXISTS "${schema_file}")
    message(FATAL_ERROR
            "orm_add_schema_validation schema file does not exist: ${schema_file}")
  endif()

  if(IS_ABSOLUTE "${schema_file}")
    set(schema_absolute "${schema_file}")
  else()
    set(schema_absolute "${CMAKE_CURRENT_SOURCE_DIR}/${schema_file}")
  endif()
  set(stamp_directory "${CMAKE_CURRENT_BINARY_DIR}/orm_schema_validation")
  set(stamp_file "${stamp_directory}/${validation_target}.stamp")
  add_custom_command(
    OUTPUT "${stamp_file}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${stamp_directory}"
    COMMAND $<TARGET_FILE:orm_schema_validate> "${schema_absolute}"
    COMMAND ${CMAKE_COMMAND} -E touch "${stamp_file}"
    DEPENDS orm_schema_validate "${schema_absolute}"
    COMMENT "Validating ORM schema ${schema_file}"
    VERBATIM)
  add_custom_target("${validation_target}" DEPENDS "${stamp_file}")
endfunction()

function(orm_add_schema_generation output_header schema_file)
  if(NOT output_header OR NOT schema_file)
    message(FATAL_ERROR
            "orm_add_schema_generation requires an output header and schema file")
  endif()
  if(NOT TARGET orm_schema_generate)
    message(FATAL_ERROR
            "orm_add_schema_generation requires ORM_BUILD_SCHEMA_TOOLS=ON")
  endif()
  if(NOT EXISTS "${schema_file}")
    message(FATAL_ERROR
            "orm_add_schema_generation schema file does not exist: ${schema_file}")
  endif()

  if(IS_ABSOLUTE "${schema_file}")
    set(schema_absolute "${schema_file}")
  else()
    set(schema_absolute "${CMAKE_CURRENT_SOURCE_DIR}/${schema_file}")
  endif()
  if(IS_ABSOLUTE "${output_header}")
    set(output_absolute "${output_header}")
  else()
    set(output_absolute "${CMAKE_CURRENT_BINARY_DIR}/${output_header}")
  endif()
  get_filename_component(output_directory "${output_absolute}" DIRECTORY)
  add_custom_command(
    OUTPUT "${output_absolute}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${output_directory}"
    COMMAND $<TARGET_FILE:orm_schema_generate> "${schema_absolute}" "${output_absolute}"
    DEPENDS orm_schema_generate "${schema_absolute}"
    COMMENT "Generating ORM metadata ${output_header}"
    VERBATIM)
  set_source_files_properties("${output_absolute}" PROPERTIES GENERATED TRUE)
endfunction()

function(orm_add_c_schema_generation output_orm_header output_model_header
                                     output_model_source schema_file)
  if(NOT output_orm_header OR NOT output_model_header OR NOT output_model_source
     OR NOT schema_file)
    message(
      FATAL_ERROR
        "orm_add_c_schema_generation requires ORM header, model header, model source, and schema file"
    )
  endif()
  if(NOT TARGET orm_schema_generate)
    message(FATAL_ERROR
            "orm_add_c_schema_generation requires ORM_BUILD_SCHEMA_TOOLS=ON")
  endif()
  if(NOT TARGET TurboParser::DataBind OR NOT TARGET TurboUtils::Core)
    message(
      FATAL_ERROR
        "orm_add_c_schema_generation requires TurboParser::DataBind and TurboUtils::Core"
    )
  endif()
  if(NOT EXISTS "${schema_file}")
    message(
      FATAL_ERROR
        "orm_add_c_schema_generation schema file does not exist: ${schema_file}")
  endif()

  find_program(
    ORM_TBE_COMPILER_EXECUTABLE
    NAMES tbe_compiler
    HINTS "${TURBOPARSER_ROOT}/bin"
    DOC "TurboParser TBE schema compiler")
  if(NOT ORM_TBE_COMPILER_EXECUTABLE)
    message(
      FATAL_ERROR
        "orm_add_c_schema_generation requires tbe_compiler in TURBOPARSER_ROOT/bin or PATH"
    )
  endif()

  foreach(path_variable IN ITEMS schema_file output_orm_header output_model_header
                                 output_model_source)
    if(IS_ABSOLUTE "${${path_variable}}")
      set(${path_variable}_absolute "${${path_variable}}")
    elseif(path_variable STREQUAL "schema_file")
      set(${path_variable}_absolute
          "${CMAKE_CURRENT_SOURCE_DIR}/${${path_variable}}")
    else()
      set(${path_variable}_absolute
          "${CMAKE_CURRENT_BINARY_DIR}/${${path_variable}}")
    endif()
  endforeach()

  get_filename_component(output_orm_directory "${output_orm_header_absolute}"
                         DIRECTORY)
  get_filename_component(output_model_header_directory
                         "${output_model_header_absolute}" DIRECTORY)
  get_filename_component(output_model_source_directory
                         "${output_model_source_absolute}" DIRECTORY)
  get_filename_component(output_model_header_name
                         "${output_model_header_absolute}" NAME)

  if(WIN32)
    set(orm_generator_path_separator "$<SEMICOLON>")
  else()
    set(orm_generator_path_separator ":")
  endif()
  set(orm_generator_path
      "$<TARGET_FILE_DIR:TurboUtils::Core>${orm_generator_path_separator}$<TARGET_FILE_DIR:TurboParser::DataBind>${orm_generator_path_separator}$ENV{PATH}"
  )

  add_custom_command(
    OUTPUT "${output_orm_header_absolute}" "${output_model_header_absolute}"
           "${output_model_source_absolute}"
    COMMAND
      ${CMAKE_COMMAND} -E make_directory "${output_orm_directory}"
      "${output_model_header_directory}" "${output_model_source_directory}"
    COMMAND
      ${CMAKE_COMMAND} -E env "PATH=${orm_generator_path}"
      "${ORM_TBE_COMPILER_EXECUTABLE}" "${schema_file_absolute}" --lang c
      --output "${output_model_header_absolute}" --source-output
      "${output_model_source_absolute}"
    COMMAND
      $<TARGET_FILE:orm_schema_generate> --language c --model-header
      "${output_model_header_name}" "${schema_file_absolute}"
      "${output_orm_header_absolute}"
    DEPENDS orm_schema_generate "${schema_file_absolute}"
            "${ORM_TBE_COMPILER_EXECUTABLE}"
    COMMENT "Generating TBE C model and ORM facade for ${schema_file}"
    VERBATIM)
  set_source_files_properties(
    "${output_orm_header_absolute}" "${output_model_header_absolute}"
    "${output_model_source_absolute}" PROPERTIES GENERATED TRUE)
endfunction()

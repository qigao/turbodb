if(NOT RE2C_EXECUTABLE)
  message(FATAL_ERROR "TidesDB SQL lexer requires re2c")
endif()
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/lexer.c
  COMMAND "${RE2C_EXECUTABLE}" -o ${CMAKE_CURRENT_BINARY_DIR}/lexer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lexer.re
  DEPENDS lexer.re sql.h VERBATIM)
add_library(orm_tidesdb_sql STATIC parser.c ${CMAKE_CURRENT_BINARY_DIR}/lexer.c)
target_compile_features(orm_tidesdb_sql PRIVATE c_std_11)
target_compile_definitions(orm_tidesdb_sql PRIVATE ORM_C_STATIC ORM_NATIVE_OWNER_CANDIDATE)
target_include_directories(orm_tidesdb_sql PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/abi ${ORM_SOURCE_DIR}/src/flow)
target_link_libraries(orm_tidesdb_sql PRIVATE Salts::CSTL Salts::CFlow)
set_target_properties(orm_tidesdb_sql PROPERTIES C_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden FOLDER "drivers/tidesdb/sql")
target_compile_options(orm_tidesdb_sql PRIVATE ${ORM_WARNING_OPTIONS})

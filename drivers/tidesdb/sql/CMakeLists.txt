# ORM plan conversion and the legacy ORMTDB row adapter belong to the driver.
add_library(orm_tidesdb_sql STATIC parser.c table.c
  ${ORM_SOURCE_DIR}/src/sql/orm_mysql_render.c)
target_compile_definitions(orm_tidesdb_sql PRIVATE ORM_C_STATIC ORM_NATIVE_OWNER_CANDIDATE)
target_include_directories(orm_tidesdb_sql PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  ${ORM_SOURCE_DIR}/src/abi ${ORM_SOURCE_DIR}/src/flow ${ORM_SOURCE_DIR}/include/orm)
target_include_directories(orm_tidesdb_sql PUBLIC ${PROJECT_SOURCE_DIR}/tidessql/src
  INTERFACE ${ORM_SOURCE_DIR}/src/sql)
target_link_libraries(orm_tidesdb_sql PUBLIC TurboDB::TidesSQL TurboDB::SqlParser
  Salts::CSTL Salts::CFlow)
set_target_properties(orm_tidesdb_sql PROPERTIES
  C_VISIBILITY_PRESET hidden FOLDER "drivers/tidesdb/sql")

# Recompile the production source selection with the private static ABI.
get_target_property(orm_core_sources turbo_orm SOURCES)
add_library(orm_c_test_static STATIC ${orm_core_sources})
target_sources(orm_c_test_static PRIVATE ${ORM_SOURCE_DIR}/../drivers/sqlite/backend.c
                                         ${ORM_SOURCE_DIR}/../drivers/sqlite/async.c
                                         ${ORM_SOURCE_DIR}/../drivers/sqlite/orm_sqlite_cursor.c)
# Internal tests that compile cursor/backend sources directly must see the
# same opaque-handle layout as the static core. This definition is not part of
# the installed Orm::C/Orm::Cpp interface.
target_compile_definitions(orm_c_test_static
                           PUBLIC ORM_C_STATIC ORM_NATIVE_OWNER_CANDIDATE)
target_include_directories(
  orm_c_test_static
  PUBLIC ${ORM_SOURCE_DIR}/include/orm
  PRIVATE ${ORM_SOURCE_DIR}/src/abi ${ORM_SOURCE_DIR}/src/flow ${ORM_SOURCE_DIR}/src/sql
          ${ORM_SOURCE_DIR}/src/driver ${ORM_SOURCE_DIR}/src/runtime)
target_link_libraries(
  orm_c_test_static
  PUBLIC TurboDB::Types Salts::Core
  PRIVATE Salts::CSerde
          Salts::DataBind
          Salts::Plugin
          Salts::CFlow
          Salts::CSTL
          Salts::Platform
          ${CMAKE_DL_LIBS})
target_compile_definitions(orm_c_test_static
                           PRIVATE ORM_LEGACY_SQLITE_FACTORY)
target_link_libraries(orm_c_test_static PRIVATE SQLite::SQLite3)
set_target_properties(orm_c_test_static PROPERTIES
  FOLDER "orm/tests/core")
set(ORM_INTERNAL_TEST_LIBRARY orm_c_test_static PARENT_SCOPE)

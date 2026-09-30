foreach(driver IN ITEMS mysql postgresql)
  add_library(orm_coexistence_${driver}_fixture MODULE ../orm_runtime_plugin_fixture.c)
    target_compile_definitions(orm_coexistence_${driver}_fixture
    PRIVATE ORM_FIXTURE_DRIVER_ID="${driver}")
  target_include_directories(orm_coexistence_${driver}_fixture
    PRIVATE ${ORM_SOURCE_DIR}/include/orm)
  target_link_libraries(orm_coexistence_${driver}_fixture
    PRIVATE Salts::PluginABI Salts::CSerde Salts::CFlow)
  set_target_properties(orm_coexistence_${driver}_fixture PROPERTIES
    PREFIX ""  C_VISIBILITY_PRESET hidden
    WINDOWS_EXPORT_ALL_SYMBOLS OFF FOLDER "orm/tests/driver/cross_driver")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(orm_coexistence_${driver}_fixture PRIVATE "LINKER:--exclude-libs,ALL")
  endif()
endforeach()
target_compile_definitions(orm_coexistence_mysql_fixture PRIVATE ORM_FIXTURE_ROW_VALUE=42)
target_compile_definitions(orm_coexistence_postgresql_fixture PRIVATE ORM_FIXTURE_ROW_VALUE=84)

cmake_add_test(
  NAME orm_runtime_coexistence_test
  FOLDER "orm/tests/driver/cross_driver"
  TEST_NAME orm_runtime_coexistence
  SOURCES orm_runtime_coexistence_test.c
  LIBS ${ORM_INTERNAL_TEST_LIBRARY} Salts::CFlow Salts::TinyTest Salts::TinyMock
  INCLUDES ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/runtime)
set_target_properties(orm_runtime_coexistence_test PROPERTIES
   LINKER_LANGUAGE CXX)
add_dependencies(orm_runtime_coexistence_test
  orm_coexistence_mysql_fixture orm_coexistence_postgresql_fixture)
set_tests_properties(orm_runtime_coexistence PROPERTIES TIMEOUT 30 ENVIRONMENT
  "ORM_MYSQL_MOCK_PLUGIN=$<TARGET_FILE:orm_coexistence_mysql_fixture>;ORM_POSTGRESQL_MOCK_PLUGIN=$<TARGET_FILE:orm_coexistence_postgresql_fixture>")
if(WIN32)
  set_tests_properties(orm_runtime_coexistence PROPERTIES
    ENVIRONMENT_MODIFICATION "${ORM_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME orm_sql_plugin_matrix_test
  FOLDER "orm/tests/driver/cross_driver"
  TEST_NAME orm_sql_plugin_matrix_sqlite
  SOURCES orm_sql_plugin_matrix.c
  LIBS turbo_orm
  INCLUDES ${ORM_SOURCE_DIR}/include/orm
  ARGS sqlite "$<TARGET_FILE:orm_sqlite_driver>" "$<TARGET_FILE:orm_postgresql_driver>")

add_dependencies(orm_sql_plugin_matrix_test orm_sqlite_driver
                 orm_postgresql_driver)
if(MSVC)
  target_compile_options(orm_sql_plugin_matrix_test PRIVATE /WX)
else()
  target_compile_options(orm_sql_plugin_matrix_test PRIVATE -Werror)
endif()
get_target_property(ORM_SQL_PLUGIN_MATRIX_LINK_CONTRACT
                    orm_sql_plugin_matrix_test LINK_LIBRARIES)
set(ORM_SQL_PLUGIN_MATRIX_ALLOWED_LINKS ${ORM_SQL_PLUGIN_MATRIX_LINK_CONTRACT})
list(REMOVE_ITEM ORM_SQL_PLUGIN_MATRIX_ALLOWED_LINKS SQLite::SQLite3
     PostgreSQL::PostgreSQL)
if(NOT ORM_SQL_PLUGIN_MATRIX_ALLOWED_LINKS STREQUAL
   ORM_SQL_PLUGIN_MATRIX_LINK_CONTRACT)
  message(FATAL_ERROR "SQL Plugin matrix app links a database driver SDK")
endif()

if(WIN32)
  set_tests_properties(
    orm_sql_plugin_matrix_sqlite PROPERTIES ENVIRONMENT_MODIFICATION
                                            "${ORM_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  TARGET orm_sql_plugin_matrix_test
  TEST_NAME orm_sql_plugin_matrix_postgresql
  ARGS postgresql "$<TARGET_FILE:orm_sqlite_driver>"
    "$<TARGET_FILE:orm_postgresql_driver>")
if(WIN32)
  set_tests_properties(
    orm_sql_plugin_matrix_postgresql PROPERTIES ENVIRONMENT_MODIFICATION
                                                "${ORM_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  TARGET orm_sql_plugin_matrix_test
  TEST_NAME orm_sql_plugin_matrix_both
  ARGS both "$<TARGET_FILE:orm_sqlite_driver>" "$<TARGET_FILE:orm_postgresql_driver>")
if(WIN32)
  set_tests_properties(
    orm_sql_plugin_matrix_both PROPERTIES ENVIRONMENT_MODIFICATION
                                          "${ORM_TEST_RUNTIME_PATH}")
endif()

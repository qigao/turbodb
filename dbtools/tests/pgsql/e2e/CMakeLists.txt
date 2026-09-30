find_package(PostgreSQL REQUIRED)
cmake_add_test(
  NAME dbtool_postgresql_live_test
  SOURCES postgresql_live_test.c
  LIBS turbodb_dbtool_core PostgreSQL::PostgreSQL Salts::TinyTest
  FOLDER "dbtools/tests/pgsql/e2e")
set_target_properties(dbtool_postgresql_live_test PROPERTIES LINKER_LANGUAGE CXX)
if(TARGET orm_postgresql_driver)
  target_compile_definitions(dbtool_postgresql_live_test PRIVATE
    "DBTOOL_POSTGRESQL_PLUGIN=\"$<TARGET_FILE:orm_postgresql_driver>\"")
  add_dependencies(dbtool_postgresql_live_test orm_postgresql_driver)
endif()
if(MSVC)
  target_compile_options(dbtool_postgresql_live_test PRIVATE /W4 /WX
                                                    )
else()
  target_compile_options(dbtool_postgresql_live_test PRIVATE -Wall -Wextra
                                                       -Wpedantic -Werror)
endif()
set_tests_properties(dbtool_postgresql_live_test PROPERTIES LABELS e2e RUN_SERIAL TRUE)

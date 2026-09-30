if(BUILD_TESTS)
  find_package(SQLite3 REQUIRED)
  add_executable(dbtool_sqlite_test dbtool_sqlite_test.c
    ../../../drivers/sqlite/schema.c)
  target_link_libraries(dbtool_sqlite_test PRIVATE turbodb_schema_support SQLite::SQLite3
                                                   Salts::TinyTest)
  target_include_directories(dbtool_sqlite_test
                             PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
  set_target_properties(dbtool_sqlite_test PROPERTIES FOLDER "dbtools/tests")
  if(MSVC)
    target_compile_options(dbtool_sqlite_test PRIVATE /W4 /WX /std:c11)
  else()
    target_compile_options(dbtool_sqlite_test PRIVATE -Wall -Wextra
                                                      -Wpedantic -Werror)
  endif()
  add_test(NAME dbtool_sqlite_test COMMAND dbtool_sqlite_test)
endif()

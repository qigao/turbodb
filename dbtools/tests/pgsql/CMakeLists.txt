if(BUILD_TESTS)
  find_package(PostgreSQL REQUIRED)
  add_executable(dbtool_postgresql_test dbtool_postgresql_test.c
                                        fake_libpq.c ../../../drivers/postgresql/schema.c)
  target_link_libraries(dbtool_postgresql_test
                        PRIVATE turbodb_schema_support Salts::TinyTest)
  target_include_directories(
    dbtool_postgresql_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src
                                   ${CMAKE_CURRENT_SOURCE_DIR} ${PostgreSQL_INCLUDE_DIRS})
  set_target_properties(dbtool_postgresql_test
                        PROPERTIES FOLDER "dbtools/tests" LINKER_LANGUAGE CXX)
  if(MSVC)
    target_compile_options(dbtool_postgresql_test PRIVATE /W4 /WX /std:c11)
  else()
    target_compile_options(dbtool_postgresql_test PRIVATE -Wall -Wextra
                                                          -Wpedantic -Werror)
  endif()
  add_test(NAME dbtool_postgresql_test COMMAND dbtool_postgresql_test)
endif()

if(BUILD_E2E_TESTS)
  add_subdirectory(e2e)
endif()

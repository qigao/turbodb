cmake_add_test(
  NAME orm_row_publisher_test
  FOLDER "orm/tests/flow"
  TEST_NAME orm_row_publisher
  SOURCES orm_row_publisher_test.c
  LIBS ${ORM_INTERNAL_TEST_LIBRARY} Salts::DataBind Salts::CFlow Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/flow)

set_property(TARGET orm_row_publisher_test PROPERTY LINKER_LANGUAGE CXX)

if(WIN32)
  set_tests_properties(orm_row_publisher PROPERTIES ENVIRONMENT_MODIFICATION
                                                    "${ORM_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME orm_pure_c_build_test
  FOLDER "orm/tests/flow"
  TEST_NAME orm_pure_c_build
  SOURCES orm_pure_c_build_test.c
  LIBS turbo_orm Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)

if(WIN32)
  set_tests_properties(orm_pure_c_build PROPERTIES ENVIRONMENT_MODIFICATION
                                                   "${ORM_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME orm_sql_render_test
  FOLDER "orm/tests/flow"
  TEST_NAME orm_sql_render
  SOURCES orm_sql_render_test.c
  LIBS ${ORM_INTERNAL_TEST_LIBRARY} Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/abi
    ${ORM_SOURCE_DIR}/src/flow ${CMAKE_CURRENT_SOURCE_DIR}/../../src/sql)

set_property(TARGET orm_sql_render_test PROPERTY LINKER_LANGUAGE CXX)

if(WIN32)
  set_tests_properties(orm_sql_render PROPERTIES ENVIRONMENT_MODIFICATION
                                                 "${ORM_TEST_RUNTIME_PATH}")
endif()

add_subdirectory(command)
add_subdirectory(mongodb)
add_subdirectory(postgresql)
add_subdirectory(redis)
add_subdirectory(sqlite)
add_subdirectory(tidesdb)

if(BUILD_TESTS)
  add_subdirectory(plugin)
  cmake_add_test(
    NAME
    dbtool_file_test
    SOURCES
    dbtool_file_test.c
    LIBS
    turbodb_dbtool_core
    Salts::TinyTest
    INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    FOLDER
    "dbtools/tests")
  cmake_add_test(
    NAME
    dbtool_cli_test
    SOURCES
    dbtool_cli_test.c
    LIBS
    turbodb_dbtool_core
    Salts::TinyTest
    INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    FOLDER
    "dbtools/tests")
endif()

add_subdirectory(sqlite)

add_subdirectory(pgsql)

add_subdirectory(mysql)

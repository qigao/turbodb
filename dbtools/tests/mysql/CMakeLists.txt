if(BUILD_TESTS)
  cmake_add_test(
    NAME dbtool_mysql_test
    SOURCES dbtool_mysql_test.c
    LIBS turbodb_schema_support Salts::JsonParser Salts::TinyTest CMeta::TinyMock
    INCLUDES ${PROJECT_SOURCE_DIR}/drivers/mysql
    FOLDER "dbtools/tests/drivers/mysql")
endif()
if(BUILD_TESTS)
  add_subdirectory(e2e)
endif()

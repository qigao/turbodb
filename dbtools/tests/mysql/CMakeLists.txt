if(BUILD_TESTS)
  cmake_add_test(
    NAME dbtool_mysql_test
    SOURCES dbtool_mysql_test.c
    LIBS turbodb_schema_support Salts::JsonParser Salts::TinyTest Salts::TinyMock
    INCLUDES ${PROJECT_SOURCE_DIR}/mysql
    FOLDER "dbtools/tests/mysql")
endif()
if(BUILD_E2E_TESTS)
  add_subdirectory(e2e)
endif()

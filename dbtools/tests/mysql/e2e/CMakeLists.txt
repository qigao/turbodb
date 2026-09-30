cmake_add_test(
  NAME dbtool_mysql_live_test
  SOURCES mysql_live_test.c
  LIBS turbodb_dbtool_core Salts::TinyTest
  FOLDER "dbtools/tests/mysql/e2e")
if(TARGET orm_mysql_driver)
  target_compile_definitions(dbtool_mysql_live_test PRIVATE
    "DBTOOL_MYSQL_PLUGIN=\"$<TARGET_FILE:orm_mysql_driver>\"")
  add_dependencies(dbtool_mysql_live_test orm_mysql_driver)
endif()
set_tests_properties(dbtool_mysql_live_test PROPERTIES LABELS e2e RUN_SERIAL TRUE)

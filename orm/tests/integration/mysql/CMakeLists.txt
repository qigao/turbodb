cmake_add_test(
  NAME orm_mysql_tidessqld_e2e_test
  TEST_NAME orm_mysql_tidessqld_e2e
  FOLDER "orm/tests/integration/mysql"
  SOURCES tidessqld_driver_e2e_test.c
          ${PROJECT_SOURCE_DIR}/tidessql/tests/daemon/process_fixture.c
  LIBS turbo_orm Salts::CFlow Salts::Platform Salts::Core Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm
           ${PROJECT_SOURCE_DIR}/tidessql/tests/daemon)

target_compile_definitions(
  orm_mysql_tidessqld_e2e_test PRIVATE
  "TEST_TIDESSQLD=\"$<TARGET_FILE:tidessqld>\""
  "TEST_ORM_MYSQL_PLUGIN=\"$<TARGET_FILE:orm_mysql_driver>\""
  "TEST_TLS_CA=\"${PROJECT_SOURCE_DIR}/tidessql/tests/protocol/fixtures/p256-test-root-cert.pem\""
  "TEST_TLS_CERT=\"${PROJECT_SOURCE_DIR}/tidessql/tests/protocol/fixtures/p256-test-server-cert.pem\""
  "TEST_TLS_KEY=\"${PROJECT_SOURCE_DIR}/tidessql/tests/protocol/fixtures/p256-test-server-key.pem\"")
add_dependencies(orm_mysql_tidessqld_e2e_test tidessqld orm_mysql_driver)
set_target_properties(orm_mysql_tidessqld_e2e_test PROPERTIES LINKER_LANGUAGE CXX)

if(MSVC)
  target_compile_options(orm_mysql_tidessqld_e2e_test PRIVATE /WX)
else()
  target_compile_options(orm_mysql_tidessqld_e2e_test PRIVATE -Werror)
endif()

set_tests_properties(
  orm_mysql_tidessqld_e2e
  PROPERTIES LABELS integration RUN_SERIAL TRUE TIMEOUT 90)
if(WIN32)
  set_tests_properties(
    orm_mysql_tidessqld_e2e
    PROPERTIES
      ENVIRONMENT_MODIFICATION
      "${ORM_TEST_RUNTIME_PATH};PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::CNet>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::TomlParser>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboDB::MySQL>;PATH=path_list_prepend:$<TARGET_FILE_DIR:orm_mysql_driver>")
endif()

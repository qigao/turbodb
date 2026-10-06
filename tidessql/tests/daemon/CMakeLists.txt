cmake_add_test(
  NAME tidessqld_config_test TEST_NAME tidessqld_config
  FOLDER "tidessql/tests/daemon"
  SOURCES config_test.c
  LIBS tidessqld_runtime Salts::Core Salts::TinyTest)
target_compile_definitions(tidessqld_config_test PRIVATE
  "TEST_TLS_CERT=\"${PROJECT_SOURCE_DIR}/tidessql/tests/protocol/fixtures/p256-test-server-cert.pem\""
  "TEST_TLS_KEY=\"${PROJECT_SOURCE_DIR}/tidessql/tests/protocol/fixtures/p256-test-server-key.pem\"")
set_target_properties(tidessqld_config_test PROPERTIES LINKER_LANGUAGE CXX)
set_tests_properties(tidessqld_config PROPERTIES TIMEOUT 60)
if(WIN32)
  set_tests_properties(tidessqld_config PROPERTIES ENVIRONMENT_MODIFICATION
    "${TIDESSQL_TEST_RUNTIME_PATH};PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::CNet>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::TomlParser>")
endif()

cmake_add_test(
  NAME tidessqld_process_e2e_test TEST_NAME tidessqld_process_e2e
  FOLDER "tidessql/tests/daemon"
  SOURCES process_e2e_test.c process_fixture.c
  LIBS TurboDB::MySQL Salts::Core Salts::TinyTest)
target_compile_definitions(tidessqld_process_e2e_test PRIVATE
  "TEST_TIDESSQLD=\"$<TARGET_FILE:tidessqld>\""
  "TEST_TLS_CA=\"${PROJECT_SOURCE_DIR}/tidessql/tests/protocol/fixtures/p256-test-root-cert.pem\""
  "TEST_TLS_CERT=\"${PROJECT_SOURCE_DIR}/tidessql/tests/protocol/fixtures/p256-test-server-cert.pem\""
  "TEST_TLS_KEY=\"${PROJECT_SOURCE_DIR}/tidessql/tests/protocol/fixtures/p256-test-server-key.pem\"")
set_target_properties(tidessqld_process_e2e_test PROPERTIES LINKER_LANGUAGE CXX)
set_tests_properties(tidessqld_process_e2e PROPERTIES TIMEOUT 90)
if(WIN32)
  set_tests_properties(tidessqld_process_e2e PROPERTIES ENVIRONMENT_MODIFICATION
    "${TIDESSQL_TEST_RUNTIME_PATH};PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::CNet>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::TomlParser>;PATH=path_list_prepend:$<TARGET_FILE_DIR:TurboDB::MySQL>")
endif()

cmake_add_test(
  NAME tidessqld_cli_test TEST_NAME tidessqld_cli
  FOLDER "tidessql/tests/daemon"
  SOURCES cli_test.c process_fixture.c
  LIBS Salts::Core Salts::TinyTest)
target_compile_definitions(tidessqld_cli_test PRIVATE
  "TEST_TIDESSQLD=\"$<TARGET_FILE:tidessqld>\""
  "TEST_TLS_CERT=\"${PROJECT_SOURCE_DIR}/tidessql/tests/protocol/fixtures/p256-test-server-cert.pem\""
  "TEST_TLS_KEY=\"${PROJECT_SOURCE_DIR}/tidessql/tests/protocol/fixtures/p256-test-server-key.pem\"")
add_dependencies(tidessqld_cli_test tidessqld)
set_target_properties(tidessqld_cli_test PROPERTIES LINKER_LANGUAGE CXX)
set_tests_properties(tidessqld_cli PROPERTIES TIMEOUT 60)
if(WIN32)
  set_tests_properties(tidessqld_cli PROPERTIES ENVIRONMENT_MODIFICATION
    "${TIDESSQL_TEST_RUNTIME_PATH};PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::CNet>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::TomlParser>")
endif()

set(TURBODB_MYSQL_CONNECTOR_J_JAR "" CACHE FILEPATH
  "Optional MySQL Connector/J jar used by the real tidessqld compatibility test")
if(TURBODB_MYSQL_CONNECTOR_J_JAR)
  if(NOT EXISTS "${TURBODB_MYSQL_CONNECTOR_J_JAR}")
    message(FATAL_ERROR
      "TURBODB_MYSQL_CONNECTOR_J_JAR does not exist: ${TURBODB_MYSQL_CONNECTOR_J_JAR}")
  endif()
  find_package(Java 17 COMPONENTS Runtime REQUIRED)
  cmake_add_test(
    NAME tidessqld_connector_j_e2e_test TEST_NAME tidessqld_connector_j_e2e
    FOLDER "tidessql/tests/daemon"
    SOURCES connector_j_e2e_test.c process_fixture.c
    LIBS Salts::Core Salts::TinyTest)
  target_compile_definitions(tidessqld_connector_j_e2e_test PRIVATE
    "TEST_TIDESSQLD=\"$<TARGET_FILE:tidessqld>\""
    "TEST_JAVA=\"${Java_JAVA_EXECUTABLE}\""
    "TEST_CONNECTOR_J=\"${TURBODB_MYSQL_CONNECTOR_J_JAR}\""
    "TEST_CONNECTOR_J_PROBE=\"${CMAKE_CURRENT_SOURCE_DIR}/ConnectorJProbe.java\""
    "TEST_TLS_CA=\"${PROJECT_SOURCE_DIR}/tidessql/tests/protocol/fixtures/p256-test-root-cert.pem\""
    "TEST_TLS_CERT=\"${PROJECT_SOURCE_DIR}/tidessql/tests/protocol/fixtures/p256-test-server-cert.pem\""
    "TEST_TLS_KEY=\"${PROJECT_SOURCE_DIR}/tidessql/tests/protocol/fixtures/p256-test-server-key.pem\"")
  add_dependencies(tidessqld_connector_j_e2e_test tidessqld)
  set_target_properties(tidessqld_connector_j_e2e_test PROPERTIES LINKER_LANGUAGE CXX)
  set_tests_properties(tidessqld_connector_j_e2e PROPERTIES TIMEOUT 90)
  if(WIN32)
    set_tests_properties(tidessqld_connector_j_e2e PROPERTIES ENVIRONMENT_MODIFICATION
      "${TIDESSQL_TEST_RUNTIME_PATH};PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::CNet>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::TomlParser>")
  endif()
endif()

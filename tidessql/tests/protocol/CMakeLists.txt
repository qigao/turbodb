cmake_add_test(
  NAME tidessql_mysql_frontend_test TEST_NAME tidessql_mysql_frontend
  FOLDER "tidessql/tests/protocol"
  SOURCES mysql_frontend_test.c ${PROJECT_SOURCE_DIR}/mysql/wire/handshake.c
    ${PROJECT_SOURCE_DIR}/mysql/wire/result.c
  LIBS tidessql_mysql_frontend GmSSL::GmSSL Salts::TinyTest)
set_target_properties(tidessql_mysql_frontend_test PROPERTIES LINKER_LANGUAGE CXX)
set_tests_properties(tidessql_mysql_frontend PROPERTIES TIMEOUT 60)
if(WIN32)
  set_tests_properties(tidessql_mysql_frontend PROPERTIES ENVIRONMENT_MODIFICATION "${TIDESSQL_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME tidessql_mysql_authenticate_test TEST_NAME tidessql_mysql_authenticate
  FOLDER "tidessql/tests/protocol"
  SOURCES mysql_authenticate_test.c ${PROJECT_SOURCE_DIR}/mysql/wire/handshake.c
    ${PROJECT_SOURCE_DIR}/mysql/wire/result.c
  LIBS tidessql_mysql_frontend GmSSL::GmSSL Salts::TinyTest)
set_target_properties(tidessql_mysql_authenticate_test PROPERTIES LINKER_LANGUAGE CXX)
set_tests_properties(tidessql_mysql_authenticate PROPERTIES TIMEOUT 60)
if(WIN32)
  set_tests_properties(tidessql_mysql_authenticate PROPERTIES ENVIRONMENT_MODIFICATION "${TIDESSQL_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME tidessql_mysql_tls_test TEST_NAME tidessql_mysql_tls
  FOLDER "tidessql/tests/protocol"
  SOURCES mysql_tls_test.c ${PROJECT_SOURCE_DIR}/mysql/wire/handshake.c
  LIBS tidessql_mysql_frontend GmSSL::GmSSL Salts::CNet Salts::TinyTest)
target_compile_definitions(tidessql_mysql_tls_test PRIVATE
  "TEST_TLS_CA=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures/p256-test-root-cert.pem\""
  "TEST_TLS_WRONG_CA=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures/p256-test-wrong-root-cert.pem\""
  "TEST_TLS_CERT=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures/p256-test-server-cert.pem\""
  "TEST_TLS_KEY=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures/p256-test-server-key.pem\"")
set_target_properties(tidessql_mysql_tls_test PROPERTIES LINKER_LANGUAGE CXX)
set_tests_properties(tidessql_mysql_tls PROPERTIES TIMEOUT 60)
if(WIN32)
  set_tests_properties(tidessql_mysql_tls PROPERTIES ENVIRONMENT_MODIFICATION
    "${TIDESSQL_TEST_RUNTIME_PATH};PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::CNet>")
endif()

if(TURBODB_BUILD_ORM)
  cmake_add_test(
    NAME tidessql_mysql_server_e2e_test TEST_NAME tidessql_mysql_server_e2e
    FOLDER "tidessql/tests/protocol"
    SOURCES mysql_server_e2e_test.c
    LIBS tidessql_mysql_frontend TurboDB::MySQL GmSSL::GmSSL Salts::CNet Salts::TinyTest)
  target_compile_definitions(tidessql_mysql_server_e2e_test PRIVATE
    "TEST_TLS_CA=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures/p256-test-root-cert.pem\""
    "TEST_TLS_CERT=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures/p256-test-server-cert.pem\""
    "TEST_TLS_KEY=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures/p256-test-server-key.pem\"")
  set_target_properties(tidessql_mysql_server_e2e_test PROPERTIES LINKER_LANGUAGE CXX)
  set_tests_properties(tidessql_mysql_server_e2e PROPERTIES TIMEOUT 60)
  if(WIN32)
    set_tests_properties(tidessql_mysql_server_e2e PROPERTIES ENVIRONMENT_MODIFICATION
      "${TIDESSQL_TEST_RUNTIME_PATH};PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::CNet>")
  endif()
else()
  message(STATUS
    "Skipping tidessql_mysql_server_e2e because TURBODB_BUILD_ORM is disabled")
endif()

cmake_add_test(
  NAME tidessql_mysql_server_transaction_e2e_test
  TEST_NAME tidessql_mysql_server_transaction_e2e
  FOLDER "tidessql/tests/protocol"
  SOURCES mysql_server_transaction_e2e_test.c
    ${PROJECT_SOURCE_DIR}/mysql/session.c
    ${PROJECT_SOURCE_DIR}/mysql/transaction_control.c
    ${PROJECT_SOURCE_DIR}/mysql/parameters.c
    ${PROJECT_SOURCE_DIR}/mysql/cursor_row.c
    ${PROJECT_SOURCE_DIR}/mysql/row_reader.c
    ${PROJECT_SOURCE_DIR}/mysql/wire/codec.c
    ${PROJECT_SOURCE_DIR}/mysql/wire/packet.c
    ${PROJECT_SOURCE_DIR}/mysql/wire/query.c
    ${PROJECT_SOURCE_DIR}/mysql/wire/result.c
    ${PROJECT_SOURCE_DIR}/mysql/wire/handshake.c
    ${PROJECT_SOURCE_DIR}/mysql/wire/statement.c
    ${PROJECT_SOURCE_DIR}/mysql/wire/row.c
    ${PROJECT_SOURCE_DIR}/mysql/auth/auth.c
  LIBS tidessql_mysql_frontend GmSSL::GmSSL Salts::CNet Salts::CSerde
    Salts::Coroutine Salts::TinyTest
  INCLUDES ${PROJECT_SOURCE_DIR}/mysql ${PROJECT_SOURCE_DIR}/mysql/wire
    ${PROJECT_SOURCE_DIR}/mysql/auth)
target_compile_definitions(tidessql_mysql_server_transaction_e2e_test PRIVATE
  MYSQL_ENABLE_FAULT_INJECTION=1
  "TEST_TLS_CA=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures/p256-test-root-cert.pem\""
  "TEST_TLS_CERT=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures/p256-test-server-cert.pem\""
  "TEST_TLS_KEY=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures/p256-test-server-key.pem\"")
set_target_properties(tidessql_mysql_server_transaction_e2e_test PROPERTIES LINKER_LANGUAGE CXX)
set_tests_properties(tidessql_mysql_server_transaction_e2e PROPERTIES TIMEOUT 90)
if(WIN32)
  set_tests_properties(tidessql_mysql_server_transaction_e2e PROPERTIES ENVIRONMENT_MODIFICATION
    "${TIDESSQL_TEST_RUNTIME_PATH};PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::CNet>")
endif()

cmake_add_test(
  NAME tidessql_mysql_password_test TEST_NAME tidessql_mysql_password
  FOLDER "tidessql/tests/protocol"
  SOURCES mysql_password_test.c
  LIBS tidessql_mysql_frontend GmSSL::GmSSL Salts::TinyTest)
set_target_properties(tidessql_mysql_password_test PROPERTIES LINKER_LANGUAGE CXX)
set_tests_properties(tidessql_mysql_password PROPERTIES TIMEOUT 60)
if(WIN32)
  set_tests_properties(tidessql_mysql_password PROPERTIES ENVIRONMENT_MODIFICATION "${TIDESSQL_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME tidessql_mysql_wire_test TEST_NAME tidessql_mysql_wire
  FOLDER "tidessql/tests/protocol"
  SOURCES mysql_wire_test.c
    ${PROJECT_SOURCE_DIR}/mysql/wire/statement.c
    ${PROJECT_SOURCE_DIR}/mysql/wire/result.c
    ${PROJECT_SOURCE_DIR}/mysql/wire/row.c
  LIBS tidessql_mysql_wire Salts::TinyTest)
set_target_properties(tidessql_mysql_wire_test PROPERTIES LINKER_LANGUAGE CXX)
set_tests_properties(tidessql_mysql_wire PROPERTIES TIMEOUT 30)
if(WIN32)
  set_tests_properties(tidessql_mysql_wire PROPERTIES ENVIRONMENT_MODIFICATION "${TIDESSQL_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME tidessql_mysql_handshake_test TEST_NAME tidessql_mysql_handshake
  FOLDER "tidessql/tests/protocol"
  SOURCES mysql_handshake_test.c ${PROJECT_SOURCE_DIR}/mysql/wire/handshake.c
  LIBS tidessql_mysql_wire Salts::TinyTest)
set_target_properties(tidessql_mysql_handshake_test PROPERTIES LINKER_LANGUAGE CXX)
set_tests_properties(tidessql_mysql_handshake PROPERTIES TIMEOUT 30)
if(WIN32)
  set_tests_properties(tidessql_mysql_handshake PROPERTIES ENVIRONMENT_MODIFICATION "${TIDESSQL_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME tidessql_mysql_dispatch_test TEST_NAME tidessql_mysql_dispatch
  FOLDER "tidessql/tests/protocol"
  SOURCES mysql_dispatch_test.c
    ${PROJECT_SOURCE_DIR}/mysql/wire/statement.c
    ${PROJECT_SOURCE_DIR}/mysql/wire/result.c
    ${PROJECT_SOURCE_DIR}/mysql/wire/row.c
  LIBS tidessql_mysql_frontend Salts::TinyTest)
set_target_properties(tidessql_mysql_dispatch_test PROPERTIES LINKER_LANGUAGE CXX)
set_tests_properties(tidessql_mysql_dispatch PROPERTIES TIMEOUT 60)
if(WIN32)
  set_tests_properties(tidessql_mysql_dispatch PROPERTIES ENVIRONMENT_MODIFICATION "${TIDESSQL_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME tidessql_mysql_registry_test TEST_NAME tidessql_mysql_registry
  FOLDER "tidessql/tests/protocol"
  SOURCES mysql_registry_test.c ${PROJECT_SOURCE_DIR}/mysql/wire/statement.c
  LIBS tidessql_mysql_frontend Salts::TinyTest)
set_target_properties(tidessql_mysql_registry_test PROPERTIES LINKER_LANGUAGE CXX)
set_tests_properties(tidessql_mysql_registry PROPERTIES TIMEOUT 60)
if(WIN32)
  set_tests_properties(tidessql_mysql_registry PROPERTIES ENVIRONMENT_MODIFICATION "${TIDESSQL_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME tidessql_mysql_registry_owner_test TEST_NAME tidessql_mysql_registry_owner
  FOLDER "tidessql/tests/protocol"
  SOURCES mysql_registry_owner_test.c ${PROJECT_SOURCE_DIR}/mysql/wire/statement.c
    ${PROJECT_SOURCE_DIR}/mysql/wire/result.c
  LIBS tidessql_mysql_frontend TurboDB::SqlParser TidesDB::tidesdb Salts::CFlow Salts::TinyTest)
target_include_directories(tidessql_mysql_registry_owner_test PRIVATE ${PROJECT_SOURCE_DIR}/tidessql/src)
set_target_properties(tidessql_mysql_registry_owner_test PROPERTIES LINKER_LANGUAGE CXX)
set_tests_properties(tidessql_mysql_registry_owner PROPERTIES TIMEOUT 60)
if(WIN32)
  set_tests_properties(tidessql_mysql_registry_owner PROPERTIES ENVIRONMENT_MODIFICATION "${TIDESSQL_TEST_RUNTIME_PATH}")
endif()

get_property(tidessql_tests DIRECTORY PROPERTY TESTS)
set_property(TEST ${tidessql_tests} APPEND PROPERTY LABELS tidessql)

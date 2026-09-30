#include <cnet/cnet.h>
#include <tinymock.h>

TINYMOCk_MOCK(int, test_init, cnet_client *, const cnet_client_config *)
TINYMOCk_MOCK(int, test_connect, cnet_client *, const cnet_connect_options *, cnet_connection *)
TINYMOCk_MOCK(int, test_poll, cnet_client *, uint32_t, size_t *)
TINYMOCk_MOCK(int, test_stop, cnet_client *, uint32_t)
TINYMOCk_MOCK(int, test_destroy, cnet_client *)

/* Compile the real session with test-local transport admission/progress seams.
 * No mock hook or alternate implementation is added to the production API. */
#define cnet_client_init test_init
#define cnet_connect test_connect
#define cnet_client_poll test_poll
#define cnet_client_stop test_stop
#define cnet_client_destroy test_destroy
#include "../../session.c"
#undef cnet_client_init
#undef cnet_connect
#undef cnet_client_poll
#undef cnet_client_stop
#undef cnet_client_destroy

spec("MySQL session transport failure ownership") {
  (void)ttest_config__;
  static mysql_session_config_t config;
  static mysql_session_error_t error;
  static const mysql_session_cursor_limits_t bounds = {8u, 4u, 4096u, 4096u, 4096u};
  static const uint8_t sql[] = "select 1";

  before_each() {
    config = (mysql_session_config_t){
        .host = "test.invalid", .port = 3306u, .username = "unit",
        .password = "fixture", .database = "unit", .ca_file = "fixture-ca.pem",
        .server_name = "test.invalid", .timeout_ms = 25u};
    memset(&error, 0, sizeof(error));
    mock_test_init_set_default_return(TINYMOCk_RETURN(SALTS_OK));
    mock_test_connect_set_default_return(TINYMOCk_RETURN(SALTS_OK));
    mock_test_poll_set_default_return(TINYMOCk_RETURN(SALTS_OK));
    mock_test_stop_set_default_return(TINYMOCk_RETURN(SALTS_OK));
    mock_test_destroy_set_default_return(TINYMOCk_RETURN(SALTS_OK));
  }

  after_each() {
    mock_test_init_verify();
    mock_test_connect_verify();
    mock_test_poll_verify();
    mock_test_stop_verify();
    mock_test_destroy_verify();
  }

  it("admits async work without polling and advances with one zero timeout poll") {
    mysql_async_source source = {0};
    check_equal(mysql_session_start_async_source(&config, sql, sizeof(sql) - 1u,
        NULL, 0u, &bounds, &source, &error), MYSQL_SESSION_OK);
    tinymock_mock_verify_never(&tinymock_test_poll);
    mock_test_poll_expect(TINYMOCk_ANY, TINYMOCk_ARG((uint32_t)0u),
                         TINYMOCk_ANY, TINYMOCk_RETURN(SALTS_OK));
    check_equal(mysql_session_async_next(&source).kind, MYSQL_ASYNC_WAIT);
    tinymock_mock_verify_times(&tinymock_test_poll, 1u);
    mysql_session_async_destroy(&source);
    check_null(source.context);
  }

  it("preserves async poll errors without polling a terminal session again") {
    mysql_async_source source = {0};
    check_equal(mysql_session_start_async_source(&config, sql, sizeof(sql) - 1u,
        NULL, 0u, &bounds, &source, &error), MYSQL_SESSION_OK);
    mock_test_poll_set_default_return(TINYMOCk_RETURN(SALTS_EIO));
    mysql_async_step step = mysql_session_async_next(&source);
    check_equal(step.kind, MYSQL_ASYNC_ERROR);
    check_equal(step.status, MYSQL_SESSION_IO);
    check_equal(mysql_session_async_next(&source).kind, MYSQL_ASYNC_ERROR);
    tinymock_mock_verify_times(&tinymock_test_poll, 1u);
    mysql_session_async_destroy(&source);
  }

  it("delivers metadata and a buffered row without a blocking poll") {
    mysql_async_source source = {0};
    check_equal(mysql_session_start_async_source(&config, sql, sizeof(sql) - 1u,
        NULL, 0u, &bounds, &source, &error), MYSQL_SESSION_OK);
    mysql_session_t *session = source.context;
    session->phase = MYSQL_PHASE_CURSOR_READY;
    session->result_column_count = 1u;
    mysql_async_step step = mysql_session_async_next(&source);
    check_equal(step.kind, MYSQL_ASYNC_METADATA);
    check_equal(step.column_count, 1u);
    check_true(step.columns == session->cursor_columns);
    check_null(session->prepared_sql);
    session->phase = MYSQL_PHASE_CURSOR_ROW_READY;
    session->cursor_message[0] = 42u;
    session->cursor_row_size = 1u;
    step = mysql_session_async_next(&source);
    check_equal(step.kind, MYSQL_ASYNC_ROW);
    check_equal(step.row[0], 42u);
    session->phase = MYSQL_PHASE_DONE;
    check_equal(mysql_session_async_next(&source).kind, MYSQL_ASYNC_DONE);
    tinymock_mock_verify_never(&tinymock_test_poll);
    mysql_session_async_destroy(&source);
  }

  it("retains callback storage after close timeout and permits a later close") {
    mysql_async_source source = {0};
    check_equal(mysql_session_start_async_source(&config, sql, sizeof(sql) - 1u,
        NULL, 0u, &bounds, &source, &error), MYSQL_SESSION_OK);
    mysql_session_t *session = source.context;
    mysql_session_async_cancel(&source);
    session->client.impl = (void *)session; /* Mock a still-owned transport. */
    mock_test_stop_set_default_return(TINYMOCk_RETURN(SALTS_ETIMEDOUT));
    mock_test_destroy_set_default_return(TINYMOCk_RETURN(SALTS_EBUSY));
    check_equal(mysql_session_async_close(&source, &error), MYSQL_SESSION_TIMEOUT);
    check_true(source.context == session);
    check_equal(error.stage, "async-close");
    tinymock_mock_verify_times(&tinymock_test_destroy, 1u);
    check_equal(mysql_session_async_next(&source).status, MYSQL_SESSION_INVALID_STATE);
    session->client.impl = NULL;
    mock_test_stop_set_default_return(TINYMOCk_RETURN(SALTS_OK));
    mock_test_destroy_set_default_return(TINYMOCk_RETURN(SALTS_OK));
    check_equal(mysql_session_async_close(&source, &error), MYSQL_SESSION_OK);
    check_null(source.context);
    mysql_session_async_destroy(&source);
    tinymock_mock_verify_times(&tinymock_test_destroy, 1u);
  }

  it("rejects occupied async output and overflowing command lengths before transport admission") {
    mysql_async_source source = {(void *)&config};
    check_equal(mysql_session_start_async_source(&config, sql, sizeof(sql) - 1u,
        NULL, 0u, &bounds, &source, &error), MYSQL_SESSION_INVALID);
    check_true(source.context == &config);
    source.context = NULL;
    check_equal(mysql_session_start_async_source(&config, sql, SIZE_MAX,
        NULL, 0u, &bounds, &source, &error), MYSQL_SESSION_INVALID);
    tinymock_mock_verify_never(&tinymock_test_init);
  }

  it("rejects missing TLS trust before initializing a client") {
    config.ca_file = "";
    check_equal(mysql_session_connect_and_ping(&config, &error), MYSQL_SESSION_INVALID);
    check_equal(error.stage, "config");
    tinymock_mock_verify_never(&tinymock_test_init);
    tinymock_mock_verify_never(&tinymock_test_connect);
    tinymock_mock_verify_never(&tinymock_test_destroy);
  }

  it("does not close or destroy a client whose initialization failed") {
    mock_test_init_set_default_return(TINYMOCk_RETURN(SALTS_ENOMEM));
    check_equal(mysql_session_connect_and_ping(&config, &error), MYSQL_SESSION_IO);
    check_equal(error.cnet_status, SALTS_ENOMEM);
    check_equal(error.stage, "client-init");
    tinymock_mock_verify_times(&tinymock_test_init, 1u);
    tinymock_mock_verify_never(&tinymock_test_connect);
    tinymock_mock_verify_never(&tinymock_test_stop);
    tinymock_mock_verify_never(&tinymock_test_destroy);
  }

  it("releases an initialized client exactly once after connection admission fails") {
    mock_test_connect_set_default_return(TINYMOCk_RETURN(SALTS_ENOBUFS));
    check_equal(mysql_session_connect_and_ping(&config, &error), MYSQL_SESSION_IO);
    check_equal(error.cnet_status, SALTS_ENOBUFS);
    check_equal(error.stage, "connect");
    tinymock_mock_verify_times(&tinymock_test_connect, 1u);
    tinymock_mock_verify_never(&tinymock_test_poll);
    tinymock_mock_verify_times(&tinymock_test_stop, 1u);
    tinymock_mock_verify_times(&tinymock_test_destroy, 1u);
  }

  it("preserves poll failure diagnostics while destroying the session") {
    mock_test_poll_set_default_return(TINYMOCk_RETURN(SALTS_EIO));
    check_equal(mysql_session_connect_and_ping(&config, &error), MYSQL_SESSION_IO);
    check_equal(error.cnet_status, SALTS_EIO);
    check_equal(error.stage, "poll");
    tinymock_mock_verify_times(&tinymock_test_poll, 1u);
    tinymock_mock_verify_times(&tinymock_test_stop, 1u);
    tinymock_mock_verify_times(&tinymock_test_destroy, 1u);
  }

  it("uses the remaining timeout in the final poll and then reports timeout") {
    mock_test_poll_expect(TINYMOCk_ANY, TINYMOCk_ARG((uint32_t)10u),
                         TINYMOCk_ANY, TINYMOCk_RETURN(SALTS_OK));
    mock_test_poll_expect(TINYMOCk_ANY, TINYMOCk_ARG((uint32_t)10u),
                         TINYMOCk_ANY, TINYMOCk_RETURN(SALTS_OK));
    mock_test_poll_expect(TINYMOCk_ANY, TINYMOCk_ARG((uint32_t)5u),
                         TINYMOCk_ANY, TINYMOCk_RETURN(SALTS_OK));
    mock_test_stop_expect(TINYMOCk_ANY, TINYMOCk_ARG(config.timeout_ms),
                         TINYMOCk_RETURN(SALTS_OK));
    check_equal(mysql_session_connect_and_ping(&config, &error), MYSQL_SESSION_TIMEOUT);
    check_equal(error.stage, "timeout");
    tinymock_mock_verify_times(&tinymock_test_poll, 3u);
    tinymock_mock_verify_times(&tinymock_test_destroy, 1u);
  }

  it("attempts destroy even when stop reports a progress failure") {
    mock_test_poll_set_default_return(TINYMOCk_RETURN(SALTS_EIO));
    mock_test_stop_set_default_return(TINYMOCk_RETURN(SALTS_EIO));
    check_equal(mysql_session_connect_and_ping(&config, &error), MYSQL_SESSION_IO);
    check_equal(error.stage, "poll");
    tinymock_mock_verify_times(&tinymock_test_stop, 1u);
    tinymock_mock_verify_times(&tinymock_test_destroy, 1u);
  }

  it("does not publish a transaction when its connection is rejected") {
    const size_t command_capacity = 4096u;
    mysql_transaction_session_t *transaction = NULL;
    mock_test_connect_set_default_return(TINYMOCk_RETURN(SALTS_ENOBUFS));
    check_equal(mysql_transaction_session_begin(&config, MYSQL_ISOLATION_READ_COMMITTED,
                    command_capacity, &transaction, &error), MYSQL_SESSION_IO);
    check_null(transaction);
    check_equal(error.stage, "connect");
    tinymock_mock_verify_times(&tinymock_test_stop, 1u);
    tinymock_mock_verify_times(&tinymock_test_destroy, 1u);
  }

  it("rejects an overlong host URI before allocating transport resources") {
    enum { HOST_CAPACITY = 600 };
    char host[HOST_CAPACITY];
    memset(host, 'a', sizeof(host) - 1u);
    host[sizeof(host) - 1u] = '\0';
    config.host = host;
    check_equal(mysql_session_connect_and_ping(&config, &error), MYSQL_SESSION_INVALID);
    check_equal(error.stage, "uri");
    tinymock_mock_verify_never(&tinymock_test_init);
    tinymock_mock_verify_never(&tinymock_test_connect);
    tinymock_mock_verify_never(&tinymock_test_destroy);
  }
}

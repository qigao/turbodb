#include <cnet/cnet.h>
#include <tinytest.h>

enum { MYSQL_TRANSPORT_FAKE_POLL_CAPACITY = 3u };

typedef struct mysql_transport_fake_state {
  int init_result;
  int connect_result;
  int poll_result;
  int stop_result;
  int destroy_result;
  size_t init_calls;
  size_t connect_calls;
  size_t poll_calls;
  size_t stop_calls;
  size_t destroy_calls;
  uint32_t poll_timeouts[MYSQL_TRANSPORT_FAKE_POLL_CAPACITY];
  uint32_t stop_timeout;
} mysql_transport_fake_state;

static mysql_transport_fake_state transport_fake;

static int test_init(cnet_client *client, const cnet_client_config *config) {
  (void)client;
  (void)config;
  ++transport_fake.init_calls;
  return transport_fake.init_result;
}

static int test_connect(cnet_client *client, const cnet_connect_options *options,
                        cnet_connection *connection) {
  (void)client;
  (void)options;
  (void)connection;
  ++transport_fake.connect_calls;
  return transport_fake.connect_result;
}

static int test_poll(cnet_client *client, uint32_t timeout_ms, size_t *processed) {
  (void)client;
  (void)processed;
  if (transport_fake.poll_calls < MYSQL_TRANSPORT_FAKE_POLL_CAPACITY)
    transport_fake.poll_timeouts[transport_fake.poll_calls] = timeout_ms;
  ++transport_fake.poll_calls;
  return transport_fake.poll_result;
}

static int test_stop(cnet_client *client, uint32_t timeout_ms) {
  (void)client;
  ++transport_fake.stop_calls;
  transport_fake.stop_timeout = timeout_ms;
  return transport_fake.stop_result;
}

static int test_destroy(cnet_client *client) {
  (void)client;
  ++transport_fake.destroy_calls;
  return transport_fake.destroy_result;
}

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
    transport_fake = (mysql_transport_fake_state){
        .init_result = SALTS_OK,
        .connect_result = SALTS_OK,
        .poll_result = SALTS_OK,
        .stop_result = SALTS_OK,
        .destroy_result = SALTS_OK};
  }

  it("admits async work without polling and advances with one zero timeout poll") {
    mysql_async_source source = {0};
    check_equal(mysql_session_start_async_source(&config, sql, sizeof(sql) - 1u,
        NULL, 0u, &bounds, &source, &error), MYSQL_SESSION_OK);
    check_equal(transport_fake.poll_calls, (size_t)0u);
    check_equal(mysql_session_async_next(&source).kind, MYSQL_ASYNC_WAIT);
    check_equal(transport_fake.poll_calls, (size_t)1u);
    check_equal(transport_fake.poll_timeouts[0], (uint32_t)0u);
    mysql_session_async_destroy(&source);
    check_null(source.context);
  }

  it("preserves async poll errors without polling a terminal session again") {
    mysql_async_source source = {0};
    check_equal(mysql_session_start_async_source(&config, sql, sizeof(sql) - 1u,
        NULL, 0u, &bounds, &source, &error), MYSQL_SESSION_OK);
    transport_fake.poll_result = SALTS_EIO;
    mysql_async_step step = mysql_session_async_next(&source);
    check_equal(step.kind, MYSQL_ASYNC_ERROR);
    check_equal(step.status, MYSQL_SESSION_IO);
    check_equal(mysql_session_async_next(&source).kind, MYSQL_ASYNC_ERROR);
    check_equal(transport_fake.poll_calls, (size_t)1u);
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
    check_equal(transport_fake.poll_calls, (size_t)0u);
    mysql_session_async_destroy(&source);
  }

  it("retains callback storage after close timeout and permits a later close") {
    mysql_async_source source = {0};
    check_equal(mysql_session_start_async_source(&config, sql, sizeof(sql) - 1u,
        NULL, 0u, &bounds, &source, &error), MYSQL_SESSION_OK);
    mysql_session_t *session = source.context;
    mysql_session_async_cancel(&source);
    session->client.impl = (void *)session; /* Mock a still-owned transport. */
    transport_fake.stop_result = SALTS_ETIMEDOUT;
    transport_fake.destroy_result = SALTS_EBUSY;
    check_equal(mysql_session_async_close(&source, &error), MYSQL_SESSION_TIMEOUT);
    check_true(source.context == session);
    check_equal(error.stage, "async-close");
    check_equal(transport_fake.destroy_calls, (size_t)1u);
    check_equal(mysql_session_async_next(&source).status, MYSQL_SESSION_INVALID_STATE);
    session->client.impl = NULL;
    transport_fake.stop_result = SALTS_OK;
    transport_fake.destroy_result = SALTS_OK;
    check_equal(mysql_session_async_close(&source, &error), MYSQL_SESSION_OK);
    check_null(source.context);
    mysql_session_async_destroy(&source);
    check_equal(transport_fake.destroy_calls, (size_t)2u);
  }

  it("rejects occupied async output and overflowing command lengths before transport admission") {
    mysql_async_source source = {(void *)&config};
    check_equal(mysql_session_start_async_source(&config, sql, sizeof(sql) - 1u,
        NULL, 0u, &bounds, &source, &error), MYSQL_SESSION_INVALID);
    check_true(source.context == &config);
    source.context = NULL;
    check_equal(mysql_session_start_async_source(&config, sql, SIZE_MAX,
        NULL, 0u, &bounds, &source, &error), MYSQL_SESSION_INVALID);
    check_equal(transport_fake.init_calls, (size_t)0u);
  }

  it("rejects missing TLS trust before initializing a client") {
    config.ca_file = "";
    check_equal(mysql_session_connect_and_ping(&config, &error), MYSQL_SESSION_INVALID);
    check_equal(error.stage, "config");
    check_equal(transport_fake.init_calls, (size_t)0u);
    check_equal(transport_fake.connect_calls, (size_t)0u);
    check_equal(transport_fake.destroy_calls, (size_t)0u);
  }

  it("does not close or destroy a client whose initialization failed") {
    transport_fake.init_result = SALTS_ENOMEM;
    check_equal(mysql_session_connect_and_ping(&config, &error), MYSQL_SESSION_IO);
    check_equal(error.cnet_status, SALTS_ENOMEM);
    check_equal(error.stage, "client-init");
    check_equal(transport_fake.init_calls, (size_t)1u);
    check_equal(transport_fake.connect_calls, (size_t)0u);
    check_equal(transport_fake.stop_calls, (size_t)0u);
    check_equal(transport_fake.destroy_calls, (size_t)0u);
  }

  it("releases an initialized client exactly once after connection admission fails") {
    transport_fake.connect_result = SALTS_ENOBUFS;
    check_equal(mysql_session_connect_and_ping(&config, &error), MYSQL_SESSION_IO);
    check_equal(error.cnet_status, SALTS_ENOBUFS);
    check_equal(error.stage, "connect");
    check_equal(transport_fake.connect_calls, (size_t)1u);
    check_equal(transport_fake.poll_calls, (size_t)0u);
    check_equal(transport_fake.stop_calls, (size_t)1u);
    check_equal(transport_fake.destroy_calls, (size_t)1u);
  }

  it("preserves poll failure diagnostics while destroying the session") {
    transport_fake.poll_result = SALTS_EIO;
    check_equal(mysql_session_connect_and_ping(&config, &error), MYSQL_SESSION_IO);
    check_equal(error.cnet_status, SALTS_EIO);
    check_equal(error.stage, "poll");
    check_equal(transport_fake.poll_calls, (size_t)1u);
    check_equal(transport_fake.stop_calls, (size_t)1u);
    check_equal(transport_fake.destroy_calls, (size_t)1u);
  }

  it("uses the remaining timeout in the final poll and then reports timeout") {
    check_equal(mysql_session_connect_and_ping(&config, &error), MYSQL_SESSION_TIMEOUT);
    check_equal(error.stage, "timeout");
    check_equal(transport_fake.poll_calls, (size_t)3u);
    check_equal(transport_fake.poll_timeouts[0], (uint32_t)10u);
    check_equal(transport_fake.poll_timeouts[1], (uint32_t)10u);
    check_equal(transport_fake.poll_timeouts[2], (uint32_t)5u);
    check_equal(transport_fake.stop_timeout, config.timeout_ms);
    check_equal(transport_fake.destroy_calls, (size_t)1u);
  }

  it("attempts destroy even when stop reports a progress failure") {
    transport_fake.poll_result = SALTS_EIO;
    transport_fake.stop_result = SALTS_EIO;
    check_equal(mysql_session_connect_and_ping(&config, &error), MYSQL_SESSION_IO);
    check_equal(error.stage, "poll");
    check_equal(transport_fake.stop_calls, (size_t)1u);
    check_equal(transport_fake.destroy_calls, (size_t)1u);
  }

  it("does not publish a transaction when its connection is rejected") {
    const size_t command_capacity = 4096u;
    mysql_transaction_session_t *transaction = NULL;
    transport_fake.connect_result = SALTS_ENOBUFS;
    check_equal(mysql_transaction_session_begin(&config, MYSQL_ISOLATION_READ_COMMITTED,
                    command_capacity, &transaction, &error), MYSQL_SESSION_IO);
    check_null(transaction);
    check_equal(error.stage, "connect");
    check_equal(transport_fake.stop_calls, (size_t)1u);
    check_equal(transport_fake.destroy_calls, (size_t)1u);
  }

  it("rejects an overlong host URI before allocating transport resources") {
    enum { HOST_CAPACITY = 600 };
    char host[HOST_CAPACITY];
    memset(host, 'a', sizeof(host) - 1u);
    host[sizeof(host) - 1u] = '\0';
    config.host = host;
    check_equal(mysql_session_connect_and_ping(&config, &error), MYSQL_SESSION_INVALID);
    check_equal(error.stage, "uri");
    check_equal(transport_fake.init_calls, (size_t)0u);
    check_equal(transport_fake.connect_calls, (size_t)0u);
    check_equal(transport_fake.destroy_calls, (size_t)0u);
  }
}

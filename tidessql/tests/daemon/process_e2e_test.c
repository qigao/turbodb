#include "process_fixture.h"

#include <session_async.h>
#include <session_transaction.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <tinytest.h>

#include <stdio.h>
#include <stdlib.h>

enum {
  TEST_TIMEOUT_MS = 15000,
  TEST_CLIENT_TIMEOUT_MS = 5000,
  TEST_BUFFER_BYTES = 65536
};

static const char password[] = "test-password";

spec("standalone tidessqld process with the existing MySQL driver") {
  it("serves a TLS prepared query from an isolated daemon process") {
    char *directory = tt_make_temp_dir("tidessqld-process");
    check_not_null(directory);
    char *config = tidessqld_test_join_path(directory, "tidessqld.toml");
    char *database = tidessqld_test_join_path(directory, "database");
    check_not_null(config);
    check_not_null(database);
    check_equal(tidessqld_test_write_config(
                    config, database, TEST_TLS_CERT, TEST_TLS_KEY),
                0);

    tidessqld_test_daemon daemon = {0};
    const int started = tidessqld_test_daemon_start(
        &daemon, TEST_TIDESSQLD, config);
    uint16_t port = 0;
    const int ready = started == 0
                          ? tidessqld_test_daemon_wait_for_port(
                                &daemon, TEST_TIMEOUT_MS, &port)
                          : -1;
    mysql_session_config_t client = {
        .host = "127.0.0.1",
        .port = port,
        .username = "alice",
        .password = password,
        .database = "tenant",
        .ca_file = TEST_TLS_CA,
        .server_name = "localhost",
        .timeout_ms = TEST_CLIENT_TIMEOUT_MS};
    mysql_session_cursor_limits_t limits = {
        .max_result_rows = 8,
        .max_columns = 8,
        .max_metadata_bytes = TEST_BUFFER_BYTES,
        .max_row_bytes = TEST_BUFFER_BYTES,
        .max_command_bytes = TEST_BUFFER_BYTES};
    mysql_session_error_t client_error = {0};
    mysql_transaction_session_t *transaction = NULL;
    mysql_session_command_result_t command = {0};
    mysql_session_status_t transaction_status =
        ready == 0
            ? mysql_transaction_session_begin(
                  &client, MYSQL_ISOLATION_SERIALIZABLE,
                  TEST_BUFFER_BYTES, &transaction, &client_error)
            : MYSQL_SESSION_INVALID_STATE;
    static const char create_sql[] =
        "CREATE TABLE process_items("
        "id BIGINT PRIMARY KEY,value BIGINT NOT NULL)";
    static const char insert_sql[] =
        "INSERT INTO process_items VALUES(1,40)";
    static const char update_sql[] =
        "UPDATE process_items SET value=42 WHERE id=1";
    static const uint8_t savepoint[] = "verified";
    if (transaction_status == MYSQL_SESSION_OK)
      transaction_status = mysql_transaction_session_execute_prepared(
          transaction, (const uint8_t *)create_sql, sizeof(create_sql) - 1u,
          NULL, 0, &command, &client_error);
    if (transaction_status == MYSQL_SESSION_OK)
      transaction_status = mysql_transaction_session_execute_prepared(
          transaction, (const uint8_t *)insert_sql, sizeof(insert_sql) - 1u,
          NULL, 0, &command, &client_error);
    if (transaction_status == MYSQL_SESSION_OK)
      transaction_status = mysql_transaction_session_savepoint(
          transaction, savepoint, sizeof(savepoint) - 1u, &client_error);
    if (transaction_status == MYSQL_SESSION_OK)
      transaction_status = mysql_transaction_session_execute_prepared(
          transaction, (const uint8_t *)update_sql, sizeof(update_sql) - 1u,
          NULL, 0, &command, &client_error);
    if (transaction_status == MYSQL_SESSION_OK)
      transaction_status = mysql_transaction_session_release_savepoint(
          transaction, savepoint, sizeof(savepoint) - 1u, &client_error);
    if (transaction_status == MYSQL_SESSION_OK)
      transaction_status = mysql_transaction_session_commit(
          transaction, &client_error);
    mysql_transaction_session_destroy(transaction);
    transaction = NULL;

    mysql_async_source source = {0};
    const uint8_t query[] =
        "SELECT value FROM process_items WHERE id=1";
    const mysql_session_status_t opened =
        transaction_status == MYSQL_SESSION_OK
            ? mysql_session_start_async_source(
                  &client, query, sizeof(query) - 1u, NULL, 0,
                  &limits, &source, &client_error)
            : MYSQL_SESSION_INVALID_STATE;
    bool metadata = false;
    bool row = false;
    bool done = false;
    bool failed = false;
    mysql_session_status_t step_status = MYSQL_SESSION_OK;
    char step_message[256] = {0};
    const uint64_t deadline = salts_monotonic_ms() + TEST_TIMEOUT_MS;
    while (opened == MYSQL_SESSION_OK && !done &&
           salts_monotonic_ms() < deadline) {
      const mysql_async_step step = mysql_session_async_next(&source);
      if (step.kind == MYSQL_ASYNC_METADATA)
        metadata = step.column_count == 1u;
      else if (step.kind == MYSQL_ASYNC_ROW)
        row = step.row_size > 0u;
      else if (step.kind == MYSQL_ASYNC_DONE)
        done = true;
      else if (step.kind == MYSQL_ASYNC_ERROR) {
        failed = true;
        step_status = step.status;
        if (step.message != NULL)
          (void)snprintf(
              step_message, sizeof(step_message), "%s", step.message);
        break;
      }
      if (!done)
        salts_sleep_ms(1);
    }
    const mysql_session_status_t closed =
        source.context != NULL
            ? mysql_session_async_close(&source, &client_error)
            : MYSQL_SESSION_OK;
    const int stopped =
        started == 0
            ? tidessqld_test_daemon_stop(&daemon, TEST_TIMEOUT_MS)
            : -1;
    tidessqld_test_daemon_force_cleanup(&daemon, TEST_TIMEOUT_MS);
    const int removed = tt_remove_tree(directory);
    if (failed || opened != MYSQL_SESSION_OK || closed != MYSQL_SESSION_OK)
      (void)fprintf(
          stderr,
          "client status=%d step=%d stage=%s message=%s step_message=%s\n",
          (int)client_error.status, (int)step_status, client_error.stage,
          client_error.message, step_message);
    free(database);
    free(config);
    free(directory);

    check_equal(started, 0);
    check_equal(ready, 0);
    check_greater(port, 0u);
    check_equal(transaction_status, MYSQL_SESSION_OK);
    check_equal(opened, MYSQL_SESSION_OK);
    check_false(failed);
    check_true(metadata);
    check_true(row);
    check_true(done);
    check_equal(closed, MYSQL_SESSION_OK);
    check_equal(stopped, 0);
    check_equal(removed, 0);
  }
}

#include "session.h"
#include "session_probe.h"
#include "cursor.h"
#include "session_cursor.h"
#include "session_transaction.h"
#include "session_fault.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

static void mysql_live_sleep_ms(unsigned milliseconds) {
#if defined(_WIN32)
  Sleep((DWORD)milliseconds);
#else
  struct timespec delay;
  delay.tv_sec = (time_t)(milliseconds / 1000u);
  delay.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
  (void)nanosleep(&delay, NULL);
#endif
}

static const char *required_env(const char *name) {
  const char *value = getenv(name);
  if (value == NULL || value[0] == '\0') {
    fprintf(stderr, "missing required environment: %s\n", name);
    return NULL;
  }
  return value;
}

static int expect_token_kind(
    cserde_reader *reader, cserde_token_kind kind, cserde_token *token) {
  if (cserde_reader_next(reader, token) != CSERDE_OK ||
      token->kind != kind) {
    fprintf(stderr, "unexpected CSerde token kind expected=%d got=%d\n",
            (int)kind, (int)token->kind);
    return 0;
  }
  return 1;
}

static int expect_token_text(
    cserde_reader *reader, const char *expected) {
  cserde_token token;
  const size_t size = strlen(expected);
  if (!expect_token_kind(reader, CSERDE_STRING, &token))
    return 0;
  if (token.value.slice.size != size ||
      memcmp(token.value.slice.data, expected, size) != 0) {
    fprintf(stderr, "unexpected CSerde text expected=%s\n", expected);
    return 0;
  }
  return 1;
}

int main(void) {
  const char *host = required_env("ORM_MYSQL_HOST");
  const char *port_text = required_env("ORM_MYSQL_PORT");
  const char *user = required_env("ORM_MYSQL_USER");
  const char *password = required_env("ORM_MYSQL_PASSWORD");
  const char *ca_file = required_env("ORM_MYSQL_CA_FILE");
  const char *server_name = required_env("ORM_MYSQL_SERVER_NAME");
  mysql_session_config_t config;
  mysql_session_error_t error;
  unsigned long port;
  mysql_session_status_t status;

  if (host == NULL || port_text == NULL || user == NULL || password == NULL ||
      ca_file == NULL || server_name == NULL)
    return 2;

  port = strtoul(port_text, NULL, 10);
  if (port == 0ul || port > 65535ul) {
    fprintf(stderr, "invalid ORM_MYSQL_PORT: %s\n", port_text);
    return 2;
  }

  config = (mysql_session_config_t){
      .host = host,
      .port = (uint16_t)port,
      .username = user,
      .password = password,
      .database = getenv("ORM_MYSQL_DATABASE"),
      .ca_file = ca_file,
      .server_name = server_name,
      .timeout_ms = 10000u};

  status = mysql_session_connect_and_ping(&config, &error);
  if (status != MYSQL_SESSION_OK) {
    fprintf(stderr,
            "mysql ping failed status=%d stage=%s cnet=%d native=%d "
            "server=%u sqlstate=%s message=%s\n",
            (int)status, error.stage, error.cnet_status,
            error.cnet_native_status, (unsigned int)error.server_error,
            error.sql_state, error.message);
    return 1;
  }

  {
    static const uint8_t sql[] =
        "SELECT 'hello', NULL, CONCAT('x', CHAR(0), 'y')";
    mysql_session_text_probe_t probe;

    status = mysql_session_text_query_probe(
        &config, sql, sizeof(sql) - 1u, &probe, &error);
    if (status != MYSQL_SESSION_OK) {
      fprintf(stderr,
              "mysql COM_QUERY text probe failed status=%d stage=%s cnet=%d "
              "native=%d server=%u sqlstate=%s message=%s\n",
              (int)status, error.stage, error.cnet_status,
              error.cnet_native_status, (unsigned int)error.server_error,
              error.sql_state, error.message);
      return 1;
    }

    if (probe.row_count != 1u || probe.column_count != 3u ||
        probe.columns[0].is_null ||
        probe.columns[0].size != 5u ||
        memcmp(probe.columns[0].data, "hello", 5u) != 0 ||
        !probe.columns[1].is_null ||
        probe.columns[1].size != 0u ||
        probe.columns[2].is_null ||
        probe.columns[2].size != 3u ||
        probe.columns[2].data[0] != (uint8_t)'x' ||
        probe.columns[2].data[1] != UINT8_C(0) ||
        probe.columns[2].data[2] != (uint8_t)'y') {
      fprintf(stderr,
              "unexpected COM_QUERY text row rows=%u columns=%u "
              "first_size=%zu null=%u third_size=%zu\n",
              (unsigned int)probe.row_count,
              (unsigned int)probe.column_count,
              probe.columns[0].size,
              (unsigned int)probe.columns[1].is_null,
              probe.columns[2].size);
      return 1;
    }
  }

  {
    mysql_session_prepared_probe_t probe;
    status = mysql_session_prepared_probe(&config, &probe, &error);
    if (status != MYSQL_SESSION_OK) {
      fprintf(stderr,
              "mysql prepared probe failed status=%d stage=%s cnet=%d native=%d "
              "server=%u sqlstate=%s message=%s\n",
              (int)status, error.stage, error.cnet_status,
              error.cnet_native_status, (unsigned int)error.server_error,
              error.sql_state, error.message);
      return 1;
    }
    if (probe.row_count != 1u ||
        probe.signed_value != INT64_C(-42) ||
        probe.unsigned_value != UINT64_MAX ||
        probe.text_size != 5u ||
        memcmp(probe.text, "hello", 5u) != 0 ||
        probe.decimal_size != 6u ||
        memcmp(probe.decimal, "123.45", 6u) != 0) {
      fprintf(stderr,
              "unexpected prepared probe row count=%u signed=%lld unsigned=%llu "
              "text=%.*s decimal=%.*s\n",
              (unsigned int)probe.row_count,
              (long long)probe.signed_value,
              (unsigned long long)probe.unsigned_value,
              (int)probe.text_size, probe.text,
              (int)probe.decimal_size, probe.decimal);
      return 1;
    }
  }

  {
    static const uint8_t sql[] =
        "SELECT s,u,txt,decv FROM m3_probe WHERE s=?";
    const mysql_stmt_value_t parameter = {
        .kind = MYSQL_STMT_VALUE_SINT64,
        .data.sint64_value = INT64_C(-42)};
    const mysql_session_cursor_limits_t source_limits = {
        .max_result_rows = 4u,
        .max_columns = 8u,
        .max_metadata_bytes = 1024u,
        .max_row_bytes = 4096u,
        .max_command_bytes = 4096u};
    mysql_cursor_source_t source = {0};
    const mysql_column_definition_t *columns = NULL;
    size_t column_count = 0u;
    mysql_cursor_config_t cursor_config =
        MYSQL_CURSOR_CONFIG_INIT(4u, 16384u, 8u, 1024u, 4096u);
    orm_row_cursor cursor = {0};
    orm_error_t cursor_error = {
        .struct_size = (uint32_t)sizeof(orm_error_t),
        .status = ORM_STATUS_OK,
        .message = {0}};
    cserde_reader reader = {0};
    cserde_token token;
    orm_row_cursor_step step;
    uint64_t reported_columns = 0u;

    status = mysql_session_open_prepared_source(
        &config, sql, sizeof(sql) - 1u,
        &parameter, 1u, &source_limits,
        &source, &columns, &column_count, &error);
    if (status != MYSQL_SESSION_OK) {
      fprintf(stderr,
              "mysql cursor source failed status=%d stage=%s cnet=%d native=%d "
              "server=%u sqlstate=%s message=%s\n",
              (int)status, error.stage, error.cnet_status,
              error.cnet_native_status, (unsigned int)error.server_error,
              error.sql_state, error.message);
      return 1;
    }
    if (column_count != 4u) {
      fprintf(stderr, "unexpected prepared cursor column count=%zu\n",
              column_count);
      source.ops->destroy(source.context);
      return 1;
    }

    if (mysql_cursor_start(
            &cursor, &source, columns, column_count,
            &cursor_config, &cursor_error) != ORM_STATUS_OK) {
      fprintf(stderr, "mysql cursor start failed status=%d message=%s\n",
              (int)cursor_error.status, cursor_error.message);
      if (source.ops != NULL && source.context != NULL)
        source.ops->destroy(source.context);
      return 1;
    }

    if (cursor.ops->column_count(
            cursor.context, &reported_columns) != ORM_STATUS_OK ||
        reported_columns != UINT64_C(4)) {
      fprintf(stderr, "unexpected Driver cursor column count=%llu\n",
              (unsigned long long)reported_columns);
      cursor.ops->destroy(cursor.context);
      return 1;
    }

    step = cursor.ops->next(cursor.context, &reader);
    if (step.kind != ORM_ROW_CURSOR_ROW) {
      fprintf(stderr, "expected MySQL cursor ROW step got=%d status=%d message=%s\n",
              (int)step.kind, (int)step.status,
              step.message != NULL ? step.message : "");
      cursor.ops->destroy(cursor.context);
      return 1;
    }

    if (!expect_token_kind(&reader, CSERDE_MAP_BEGIN, &token) ||
        !expect_token_text(&reader, "s") ||
        !expect_token_kind(&reader, CSERDE_SINT, &token) ||
        token.value.sint != INT64_C(-42) ||
        !expect_token_text(&reader, "u") ||
        !expect_token_kind(&reader, CSERDE_UINT, &token) ||
        token.value.uint != UINT64_MAX ||
        !expect_token_text(&reader, "txt") ||
        !expect_token_text(&reader, "hello") ||
        !expect_token_text(&reader, "decv") ||
        !expect_token_text(&reader, "123.45") ||
        !expect_token_kind(&reader, CSERDE_MAP_END, &token) ||
        cserde_reader_next(&reader, &token) != CSERDE_DONE) {
      fprintf(stderr, "unexpected CSerde payload from MySQL cursor\n");
      cursor.ops->destroy(cursor.context);
      return 1;
    }

    memset(&reader, 0, sizeof(reader));
    step = cursor.ops->next(cursor.context, &reader);
    if (step.kind != ORM_ROW_CURSOR_DONE) {
      fprintf(stderr, "expected MySQL cursor DONE step got=%d status=%d\n",
              (int)step.kind, (int)step.status);
      cursor.ops->destroy(cursor.context);
      return 1;
    }

    cursor.ops->destroy(cursor.context);
  }

  {
    static const uint8_t sql[] =
        "UPDATE m4_command SET txt=CONCAT(txt, ?) WHERE s=?";
    static const uint8_t changed[] = {'c','h','a','n','g','e','d'};
    const mysql_stmt_value_t parameters[2] = {
      {.kind = MYSQL_STMT_VALUE_TEXT,
       .data.bytes = {changed, sizeof(changed)}},
      {.kind = MYSQL_STMT_VALUE_SINT64,
       .data.sint64_value = INT64_C(-7)}
    };
    mysql_session_command_result_t command_result;

    status = mysql_session_execute_prepared(
        &config, sql, sizeof(sql) - 1u,
        parameters, 2u, 4096u,
        &command_result, &error);
    if (status != MYSQL_SESSION_OK) {
      fprintf(stderr,
              "mysql prepared command failed status=%d stage=%s cnet=%d native=%d "
              "server=%u sqlstate=%s message=%s\n",
              (int)status, error.stage, error.cnet_status,
              error.cnet_native_status, (unsigned int)error.server_error,
              error.sql_state, error.message);
      return 1;
    }
    if (command_result.affected_rows != UINT64_C(1) ||
        command_result.last_insert_id != UINT64_C(0)) {
      fprintf(stderr,
              "unexpected prepared command result affected=%llu insert_id=%llu "
              "status_flags=%u warnings=%u\n",
              (unsigned long long)command_result.affected_rows,
              (unsigned long long)command_result.last_insert_id,
              (unsigned int)command_result.status_flags,
              (unsigned int)command_result.warnings);
      return 1;
    }
  }

  {
    static const uint8_t update_sql[] =
        "UPDATE m4_txn SET txt=? WHERE s=?";
    static const uint8_t verify_sql[] =
        "SELECT txt FROM m4_txn WHERE s=?";
    static const uint8_t changed[] =
        {'c','h','a','n','g','e','d'};
    const mysql_stmt_value_t update_parameters[2] = {
      {.kind = MYSQL_STMT_VALUE_TEXT,
       .data.bytes = {changed, sizeof(changed)}},
      {.kind = MYSQL_STMT_VALUE_SINT64,
       .data.sint64_value = INT64_C(-9)}
    };
    const mysql_stmt_value_t verify_parameter = {
      .kind = MYSQL_STMT_VALUE_SINT64,
      .data.sint64_value = INT64_C(-9)};
    mysql_transaction_session_t *transaction = NULL;
    mysql_session_command_result_t command_result;
    const mysql_session_cursor_limits_t source_limits = {
      .max_result_rows = 2u,
      .max_columns = 4u,
      .max_metadata_bytes = 512u,
      .max_row_bytes = 1024u,
      .max_command_bytes = 4096u};
    mysql_cursor_source_t source = {0};
    const mysql_column_definition_t *columns = NULL;
    size_t column_count = 0u;
    mysql_cursor_config_t cursor_config =
        MYSQL_CURSOR_CONFIG_INIT(2u, 4096u, 4u, 512u, 1024u);
    orm_row_cursor cursor = {0};
    orm_error_t cursor_error = {
      .struct_size = (uint32_t)sizeof(orm_error_t),
      .status = ORM_STATUS_OK,
      .message = {0}};
    cserde_reader reader = {0};
    cserde_token token;
    orm_row_cursor_step step;

    status = mysql_transaction_session_begin(
        &config, MYSQL_ISOLATION_READ_COMMITTED, 4096u,
        &transaction, &error);
    if (status != MYSQL_SESSION_OK) {
      fprintf(stderr,
              "mysql transaction begin failed status=%d stage=%s cnet=%d native=%d "
              "server=%u sqlstate=%s message=%s\n",
              (int)status, error.stage, error.cnet_status,
              error.cnet_native_status, (unsigned int)error.server_error,
              error.sql_state, error.message);
      return 1;
    }

    status = mysql_transaction_session_execute_prepared(
        transaction, update_sql, sizeof(update_sql) - 1u,
        update_parameters, 2u, &command_result, &error);
    if (status != MYSQL_SESSION_OK ||
        command_result.affected_rows != UINT64_C(1)) {
      fprintf(stderr,
              "mysql transaction update failed status=%d affected=%llu "
              "stage=%s message=%s\n",
              (int)status,
              (unsigned long long)command_result.affected_rows,
              error.stage, error.message);
      mysql_transaction_session_destroy(transaction);
      return 1;
    }

    status = mysql_transaction_session_rollback(
        transaction, &error);
    if (status != MYSQL_SESSION_OK) {
      fprintf(stderr,
              "mysql transaction rollback failed status=%d stage=%s message=%s\n",
              (int)status, error.stage, error.message);
      mysql_transaction_session_destroy(transaction);
      return 1;
    }
    mysql_transaction_session_destroy(transaction);

    status = mysql_session_open_prepared_source(
        &config, verify_sql, sizeof(verify_sql) - 1u,
        &verify_parameter, 1u, &source_limits,
        &source, &columns, &column_count, &error);
    if (status != MYSQL_SESSION_OK || column_count != 1u) {
      fprintf(stderr,
              "mysql rollback verification source failed status=%d columns=%zu "
              "stage=%s message=%s\n",
              (int)status, column_count, error.stage, error.message);
      if (source.ops != NULL && source.context != NULL)
        source.ops->destroy(source.context);
      return 1;
    }

    if (mysql_cursor_start(
            &cursor, &source, columns, column_count,
            &cursor_config, &cursor_error) != ORM_STATUS_OK) {
      fprintf(stderr,
              "mysql rollback verification cursor failed status=%d message=%s\n",
              (int)cursor_error.status, cursor_error.message);
      if (source.ops != NULL && source.context != NULL)
        source.ops->destroy(source.context);
      return 1;
    }

    step = cursor.ops->next(cursor.context, &reader);
    if (step.kind != ORM_ROW_CURSOR_ROW ||
        !expect_token_kind(&reader, CSERDE_MAP_BEGIN, &token) ||
        !expect_token_text(&reader, "txt") ||
        !expect_token_text(&reader, "seed") ||
        !expect_token_kind(&reader, CSERDE_MAP_END, &token) ||
        cserde_reader_next(&reader, &token) != CSERDE_DONE) {
      fprintf(stderr, "MySQL rollback did not preserve transaction fixture\n");
      cursor.ops->destroy(cursor.context);
      return 1;
    }

    memset(&reader, 0, sizeof(reader));
    step = cursor.ops->next(cursor.context, &reader);
    if (step.kind != ORM_ROW_CURSOR_DONE) {
      fprintf(stderr,
              "mysql rollback verification expected DONE got=%d status=%d\n",
              (int)step.kind, (int)step.status);
      cursor.ops->destroy(cursor.context);
      return 1;
    }

    cursor.ops->destroy(cursor.context);
  }

  {
    static const uint8_t update_sql[] =
        "UPDATE m4_txn SET txt=? WHERE s=?";
    static const uint8_t verify_sql[] =
        "SELECT txt FROM m4_txn WHERE s=?";
    static const uint8_t savepoint_name[] = "sp1";
    static const uint8_t changed[] =
        {'s','a','v','e','p','o','i','n','t'};
    const mysql_stmt_value_t update_parameters[2] = {
      {.kind = MYSQL_STMT_VALUE_TEXT,
       .data.bytes = {changed, sizeof(changed)}},
      {.kind = MYSQL_STMT_VALUE_SINT64,
       .data.sint64_value = INT64_C(-9)}
    };
    const mysql_stmt_value_t verify_parameter = {
      .kind = MYSQL_STMT_VALUE_SINT64,
      .data.sint64_value = INT64_C(-9)};
    mysql_transaction_session_t *transaction = NULL;
    mysql_session_command_result_t command_result;
    const mysql_session_cursor_limits_t source_limits = {
      .max_result_rows = 2u,
      .max_columns = 4u,
      .max_metadata_bytes = 512u,
      .max_row_bytes = 1024u,
      .max_command_bytes = 4096u};
    mysql_cursor_source_t source = {0};
    const mysql_column_definition_t *columns = NULL;
    size_t column_count = 0u;
    mysql_cursor_config_t cursor_config =
        MYSQL_CURSOR_CONFIG_INIT(2u, 4096u, 4u, 512u, 1024u);
    orm_row_cursor cursor = {0};
    orm_error_t cursor_error = {
      .struct_size = (uint32_t)sizeof(orm_error_t),
      .status = ORM_STATUS_OK,
      .message = {0}};
    cserde_reader reader = {0};
    cserde_token token;
    orm_row_cursor_step step;

    status = mysql_transaction_session_begin(
        &config, MYSQL_ISOLATION_REPEATABLE_READ, 4096u,
        &transaction, &error);
    if (status != MYSQL_SESSION_OK) {
      fprintf(stderr,
              "mysql savepoint transaction begin failed status=%d stage=%s message=%s\n",
              (int)status, error.stage, error.message);
      return 1;
    }

    status = mysql_transaction_session_savepoint(
        transaction, savepoint_name, sizeof(savepoint_name) - 1u, &error);
    if (status != MYSQL_SESSION_OK) {
      fprintf(stderr,
              "mysql SAVEPOINT failed status=%d stage=%s message=%s\n",
              (int)status, error.stage, error.message);
      mysql_transaction_session_destroy(transaction);
      return 1;
    }

    status = mysql_transaction_session_execute_prepared(
        transaction, update_sql, sizeof(update_sql) - 1u,
        update_parameters, 2u, &command_result, &error);
    if (status != MYSQL_SESSION_OK ||
        command_result.affected_rows != UINT64_C(1)) {
      fprintf(stderr,
              "mysql savepoint update failed status=%d affected=%llu "
              "stage=%s message=%s\n",
              (int)status,
              (unsigned long long)command_result.affected_rows,
              error.stage, error.message);
      mysql_transaction_session_destroy(transaction);
      return 1;
    }

    status = mysql_transaction_session_rollback_to_savepoint(
        transaction, savepoint_name, sizeof(savepoint_name) - 1u, &error);
    if (status != MYSQL_SESSION_OK) {
      fprintf(stderr,
              "mysql ROLLBACK TO SAVEPOINT failed status=%d stage=%s message=%s\n",
              (int)status, error.stage, error.message);
      mysql_transaction_session_destroy(transaction);
      return 1;
    }

    status = mysql_transaction_session_release_savepoint(
        transaction, savepoint_name, sizeof(savepoint_name) - 1u, &error);
    if (status != MYSQL_SESSION_OK) {
      fprintf(stderr,
              "mysql RELEASE SAVEPOINT failed status=%d stage=%s message=%s\n",
              (int)status, error.stage, error.message);
      mysql_transaction_session_destroy(transaction);
      return 1;
    }

    status = mysql_transaction_session_commit(transaction, &error);
    if (status != MYSQL_SESSION_OK) {
      fprintf(stderr,
              "mysql savepoint transaction commit failed status=%d stage=%s message=%s\n",
              (int)status, error.stage, error.message);
      mysql_transaction_session_destroy(transaction);
      return 1;
    }
    mysql_transaction_session_destroy(transaction);

    status = mysql_session_open_prepared_source(
        &config, verify_sql, sizeof(verify_sql) - 1u,
        &verify_parameter, 1u, &source_limits,
        &source, &columns, &column_count, &error);
    if (status != MYSQL_SESSION_OK || column_count != 1u) {
      fprintf(stderr,
              "mysql savepoint verification source failed status=%d columns=%zu "
              "stage=%s message=%s\n",
              (int)status, column_count, error.stage, error.message);
      if (source.ops != NULL && source.context != NULL)
        source.ops->destroy(source.context);
      return 1;
    }

    if (mysql_cursor_start(
            &cursor, &source, columns, column_count,
            &cursor_config, &cursor_error) != ORM_STATUS_OK) {
      fprintf(stderr,
              "mysql savepoint verification cursor failed status=%d message=%s\n",
              (int)cursor_error.status, cursor_error.message);
      if (source.ops != NULL && source.context != NULL)
        source.ops->destroy(source.context);
      return 1;
    }

    step = cursor.ops->next(cursor.context, &reader);
    if (step.kind != ORM_ROW_CURSOR_ROW ||
        !expect_token_kind(&reader, CSERDE_MAP_BEGIN, &token) ||
        !expect_token_text(&reader, "txt") ||
        !expect_token_text(&reader, "seed") ||
        !expect_token_kind(&reader, CSERDE_MAP_END, &token) ||
        cserde_reader_next(&reader, &token) != CSERDE_DONE) {
      fprintf(stderr,
              "MySQL ROLLBACK TO SAVEPOINT did not preserve fixture\n");
      cursor.ops->destroy(cursor.context);
      return 1;
    }

    memset(&reader, 0, sizeof(reader));
    step = cursor.ops->next(cursor.context, &reader);
    if (step.kind != ORM_ROW_CURSOR_DONE) {
      fprintf(stderr,
              "mysql savepoint verification expected DONE got=%d status=%d\n",
              (int)step.kind, (int)step.status);
      cursor.ops->destroy(cursor.context);
      return 1;
    }

    cursor.ops->destroy(cursor.context);
  }

  {
    static const uint8_t timeout_sql[] =
        "SET SESSION wait_timeout=1";
    static const uint8_t update_sql[] =
        "UPDATE m5_disconnect SET n=n+1 WHERE id=1";
    static const uint8_t verify_sql[] =
        "SELECT CAST(n AS CHAR) FROM m5_disconnect WHERE id=1";
    mysql_transaction_session_t *transaction = NULL;
    mysql_session_command_result_t command_result;
    mysql_session_text_probe_t probe;

    status = mysql_transaction_session_begin(
        &config, MYSQL_ISOLATION_READ_COMMITTED, 4096u,
        &transaction, &error);
    if (status != MYSQL_SESSION_OK) {
      fprintf(stderr,
              "mysql disconnect transaction begin failed status=%d "
              "stage=%s message=%s\n",
              (int)status, error.stage, error.message);
      return 1;
    }

    status = mysql_transaction_session_execute_prepared(
        transaction, timeout_sql, sizeof(timeout_sql) - 1u,
        NULL, 0u, &command_result, &error);
    if (status != MYSQL_SESSION_OK) {
      fprintf(stderr,
              "mysql wait_timeout setup failed status=%d stage=%s message=%s\n",
              (int)status, error.stage, error.message);
      mysql_transaction_session_destroy(transaction);
      return 1;
    }

    mysql_live_sleep_ms(2500u);

    status = mysql_transaction_session_execute_prepared(
        transaction, update_sql, sizeof(update_sql) - 1u,
        NULL, 0u, &command_result, &error);
    if (status != MYSQL_SESSION_IO &&
        status != MYSQL_SESSION_TIMEOUT) {
      fprintf(stderr,
              "mysql real disconnect expected IO/TIMEOUT got=%d "
              "affected=%llu stage=%s message=%s\n",
              (int)status,
              (unsigned long long)command_result.affected_rows,
              error.stage, error.message);
      mysql_transaction_session_destroy(transaction);
      return 1;
    }

    mysql_transaction_session_destroy(transaction);

    status = mysql_session_text_query_probe(
        &config, verify_sql, sizeof(verify_sql) - 1u,
        &probe, &error);
    if (status != MYSQL_SESSION_OK ||
        probe.row_count != 1u ||
        probe.column_count != 1u ||
        probe.columns[0].is_null ||
        probe.columns[0].size != 1u ||
        probe.columns[0].data[0] != (uint8_t)'0') {
      fprintf(stderr,
              "mysql disconnect no-replay verification failed status=%d "
              "rows=%u columns=%u value=%.*s stage=%s message=%s\n",
              (int)status,
              (unsigned int)probe.row_count,
              (unsigned int)probe.column_count,
              (int)probe.columns[0].size,
              probe.columns[0].data,
              error.stage, error.message);
      return 1;
    }
  }

  {
    static const uint8_t update_sql[] =
        "UPDATE m4_commit_unknown SET n=n+1 WHERE s=?";
    const mysql_stmt_value_t parameter = {
      .kind = MYSQL_STMT_VALUE_SINT64,
      .data.sint64_value = INT64_C(-11)};
    mysql_transaction_session_t *transaction = NULL;
    mysql_session_command_result_t command_result;

    status = mysql_transaction_session_begin(
        &config, MYSQL_ISOLATION_READ_COMMITTED, 4096u,
        &transaction, &error);
    if (status != MYSQL_SESSION_OK) {
      fprintf(stderr,
              "mysql commit-unknown transaction begin failed status=%d "
              "stage=%s message=%s\n",
              (int)status, error.stage, error.message);
      return 1;
    }

    status = mysql_transaction_session_execute_prepared(
        transaction, update_sql, sizeof(update_sql) - 1u,
        &parameter, 1u, &command_result, &error);
    if (status != MYSQL_SESSION_OK ||
        command_result.affected_rows != UINT64_C(1)) {
      fprintf(stderr,
              "mysql commit-unknown update failed status=%d affected=%llu "
              "stage=%s message=%s\n",
              (int)status,
              (unsigned long long)command_result.affected_rows,
              error.stage, error.message);
      mysql_transaction_session_destroy(transaction);
      return 1;
    }

#if defined(MYSQL_ENABLE_FAULT_INJECTION)
    mysql_transaction_session_test_drop_commit_ack(transaction, 1);
    status = mysql_transaction_session_commit(transaction, &error);
    if (status != MYSQL_SESSION_COMMIT_UNKNOWN ||
        error.status != MYSQL_SESSION_COMMIT_UNKNOWN ||
        mysql_transaction_session_test_commit_send_count(transaction) != 1u) {
      fprintf(stderr,
              "mysql lost COMMIT ack classification failed status=%d "
              "error_status=%d sends=%u stage=%s message=%s\n",
              (int)status, (int)error.status,
              mysql_transaction_session_test_commit_send_count(transaction),
              error.stage, error.message);
      mysql_transaction_session_destroy(transaction);
      return 1;
    }
#else
#error MYSQL_ENABLE_FAULT_INJECTION must be enabled for live qualification
#endif

    mysql_transaction_session_destroy(transaction);
  }

  return 0;
}

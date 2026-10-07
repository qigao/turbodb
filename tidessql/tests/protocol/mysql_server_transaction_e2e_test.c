#include "server.h"

#include <session_cursor.h>
#include <session_transaction.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <coro_executor.h>
#include <gmssl/mem.h>
#include <tinytest.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum {
  TEST_TIMEOUT_MS = 10000,
  TEST_IO_TIMEOUT_MS = 2000,
  TEST_STOP_SLICE_MS = 50,
  TEST_BUFFER_BYTES = 65536,
  TEST_QUEUE = 64,
  TEST_REQUESTS = 32,
  TEST_CONNECTIONS = 16
};

static const char test_password[] = "test-password";

typedef struct test_server_task {
  tdsql_mysql_server *server;
  atomic_int stop;
  atomic_int started;
  atomic_int done;
  atomic_int status;
  turbodb_error_t error;
} test_server_task;

static tdsql_database *database;
static char *directory;
static tdsql_mysql_account account;
static tdsql_mysql_database_binding binding;
static tdsql_mysql_auth_policy policy;
static tdsql_mysql_server server;
static tdsql_mysql_server_config server_config;
static coro_executor_t *executor;
static test_server_task server_task;
static mysql_session_config_t client;
static turbodb_error_t error;

#if defined(MYSQL_ENABLE_FAULT_INJECTION)
/* Live qualification only. The production TurboDB::MySQL target omits these
 * symbols and the state they control. */
mysql_session_status_t mysql_transaction_session_test_open(
    const mysql_session_config_t *config, size_t max_command_bytes,
    mysql_transaction_session_t **out_transaction,
    mysql_session_error_t *error);
void mysql_transaction_session_test_disconnect(
    mysql_transaction_session_t *transaction);
mysql_session_status_t mysql_transaction_session_test_execute_control(
    mysql_transaction_session_t *transaction,
    const uint8_t *sql, size_t sql_size,
    mysql_session_error_t *error);
void mysql_transaction_session_test_drop_commit_ack(
    mysql_transaction_session_t *transaction, int enabled);
unsigned mysql_transaction_session_test_commit_send_count(
    const mysql_transaction_session_t *transaction);
#else
#error MYSQL_ENABLE_FAULT_INJECTION must be enabled for this qualification test
#endif

static tdsql_mysql_password_record verifier(void) {
  return (tdsql_mysql_password_record){
      .iterations = TDSQL_MYSQL_PASSWORD_MIN_ITERATIONS,
      .salt = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
      .hash = {0xca, 0xe9, 0xc8, 0x01, 0x37, 0x45, 0x96, 0xf1,
               0x7d, 0xe4, 0x8b, 0xa4, 0xed, 0x70, 0x61, 0x69,
               0x2b, 0x5d, 0x0a, 0xb4, 0x33, 0x93, 0x2a, 0x7d,
               0x3c, 0xdf, 0x69, 0x8d, 0xfd, 0x8b, 0xb3, 0xe8}};
}

static cnet_client_config transport_config(void) {
  return (cnet_client_config){
      .backend =
#if defined(_WIN32)
          NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
          NATIVE_IO_BACKEND_EPOLL,
#else
          NATIVE_IO_BACKEND_KQUEUE,
#endif
      .connection_capacity = TEST_CONNECTIONS,
      .command_capacity = TEST_QUEUE,
      .request_capacity = TEST_REQUESTS,
      .completion_batch_capacity = TEST_REQUESTS,
      .event_capacity = TEST_QUEUE,
      .max_send_bytes = TEST_BUFFER_BYTES,
      .receive_buffer_bytes = TEST_BUFFER_BYTES,
      .connect_timeout_ms = TEST_IO_TIMEOUT_MS,
      .read_timeout_ms = TEST_IO_TIMEOUT_MS,
      .write_timeout_ms = TEST_IO_TIMEOUT_MS,
      .tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES,
      .tls_handshake_timeout_ms = TEST_IO_TIMEOUT_MS};
}

static void test_server_run(coro_t *coroutine, void *argument) {
  test_server_task *task = argument;
  turbodb_status_t result = TURBODB_STATUS_OK;
  (void)coroutine;
  atomic_store_explicit(&task->started, 1, memory_order_release);

  while (!atomic_load_explicit(&task->stop, memory_order_acquire)) {
    size_t events = 0;
    result = tdsql_mysql_server_poll(task->server, 1, &events, &task->error);
    if (result != TURBODB_STATUS_OK) break;
    if (coro_executor_yield() != SALTS_OK) {
      result = TURBODB_STATUS_INTERNAL_ERROR;
      break;
    }
  }

  const uint64_t deadline = cmeta_monotonic_ms() + TEST_TIMEOUT_MS;
  turbodb_status_t stopped;
  do {
    stopped = tdsql_mysql_server_stop(
        task->server, TEST_STOP_SLICE_MS, &task->error);
    if (stopped == TURBODB_STATUS_BUSY && cmeta_monotonic_ms() < deadline)
      (void)coro_executor_yield();
  } while (stopped == TURBODB_STATUS_BUSY && cmeta_monotonic_ms() < deadline);
  if (result == TURBODB_STATUS_OK) result = stopped;

  atomic_store_explicit(&task->status, (int)result, memory_order_release);
  atomic_store_explicit(&task->done, 1, memory_order_release);
}

static void test_server_cancel(void *argument, int status) {
  test_server_task *task = argument;
  (void)status;
  atomic_store_explicit(
      &task->status, (int)TURBODB_STATUS_INTERNAL_ERROR, memory_order_release);
  atomic_store_explicit(&task->done, 1, memory_order_release);
}

static bool wait_for_atomic(atomic_int *value) {
  const uint64_t deadline = cmeta_monotonic_ms() + TEST_TIMEOUT_MS;
  while (!atomic_load_explicit(value, memory_order_acquire) &&
         cmeta_monotonic_ms() < deadline)
    cmeta_sleep_ms(1);
  return atomic_load_explicit(value, memory_order_acquire) != 0;
}

static void stop_server_executor(void) {
  if (!executor) return;
  atomic_store_explicit(&server_task.stop, 1, memory_order_release);
  check_true(wait_for_atomic(&server_task.done));
  check_equal(coro_executor_shutdown(executor), SALTS_OK);
  check_equal(coro_executor_wait(executor), SALTS_OK);
  check_equal(coro_executor_destroy(executor), SALTS_OK);
  executor = NULL;
  check_equal(
      atomic_load_explicit(&server_task.status, memory_order_acquire),
      TURBODB_STATUS_OK);
  check_false(server.initialized);
}

static mysql_session_status_t execute(
    const char *sql, const mysql_stmt_value_t *values, size_t value_count,
    mysql_session_command_result_t *result, mysql_session_error_t *client_error) {
  return mysql_session_execute_prepared(
      &client, (const uint8_t *)sql, strlen(sql), values, value_count,
      TEST_BUFFER_BYTES, result, client_error);
}

static bool read_score(int64_t id, int64_t *score, mysql_session_error_t *client_error) {
  static const uint8_t sql[] = "SELECT score FROM items WHERE id=?";
  const mysql_stmt_value_t parameter = {
      .kind = MYSQL_STMT_VALUE_SINT64, .data.sint64_value = id};
  const mysql_session_cursor_limits_t limits = {
      .max_result_rows = 2,
      .max_columns = 2,
      .max_metadata_bytes = TEST_BUFFER_BYTES,
      .max_row_bytes = TEST_BUFFER_BYTES,
      .max_command_bytes = TEST_BUFFER_BYTES};
  mysql_cursor_source_t source = {0};
  const mysql_column_definition_t *columns = NULL;
  size_t column_count = 0;
  bool valid = false;

  if (mysql_session_open_prepared_source(
          &client, sql, sizeof(sql) - 1, &parameter, 1, &limits, &source,
          &columns, &column_count, client_error) != MYSQL_SESSION_OK)
    return false;
  if (column_count == 1) {
    const mysql_cursor_source_step_t row = source.ops->next(source.context);
    mysql_binary_value_t value = {0};
    if (row.kind == MYSQL_CURSOR_SOURCE_ROW &&
        mysql_wire_decode_binary_row(
            row.row, row.row_size, columns, column_count, &value, 1) ==
            MYSQL_WIRE_STATUS_OK &&
        value.kind == MYSQL_BINARY_VALUE_SINT64) {
      const mysql_cursor_source_step_t done = source.ops->next(source.context);
      if (done.kind == MYSQL_CURSOR_SOURCE_DONE) {
        *score = value.data.sint64_value;
        valid = true;
      }
    }
  }
  source.ops->destroy(source.context);
  return valid;
}

spec("TidesSQL MySQL server remote transaction end to end") {
  before_each() {
    database = NULL;
    directory = tt_make_temp_dir("tidessql-mysql-transaction");
    check_not_null(directory);
    server = (tdsql_mysql_server){0};
    executor = NULL;
    memset(&server_task, 0, sizeof(server_task));
    turbodb_error_init(&error);

    const turbodb_option_t options[] = {
        {turbodb_view("path"), turbodb_view(directory)},
        {turbodb_view("column_family"), turbodb_view("server")},
        {turbodb_view("sql_initialize"), turbodb_view("true")}};
    tdsql_config database_config = tdsql_config_default();
    database_config.options = options;
    database_config.option_count = sizeof(options) / sizeof(options[0]);
    check_equal(
        tdsql_database_open(&database_config, &database, &error),
        TURBODB_STATUS_OK);

    account = (tdsql_mysql_account){
        .username = turbodb_view("alice"),
        .password = verifier(),
        .databases = UINT64_C(1),
        .default_database = 0};
    binding = (tdsql_mysql_database_binding){turbodb_view("tenant"), database};
    tdsql_mysql_auth_policy requested = tdsql_mysql_auth_policy_default();
    requested.accounts = &account;
    requested.account_count = 1;
    requested.databases = &binding;
    requested.database_count = 1;
    check_equal(
        tdsql_mysql_auth_policy_init(&policy, &requested, &error),
        TURBODB_STATUS_OK);

    server_config = (tdsql_mysql_server_config){
        .policy = &policy,
        .registry = tdsql_mysql_registry_config_default(),
        .server_version = turbodb_view("TidesSQL-transaction-test"),
        .listener = {.backend = transport_config().backend,
                     .host = "127.0.0.1",
                     .port = 0,
                     .backlog = TEST_CONNECTIONS},
        .transport = transport_config(),
        .tls = {.size = sizeof(cnet_tls_server_config),
                .cert_file = TEST_TLS_CERT,
                .key_file = TEST_TLS_KEY},
        .max_connections = TEST_CONNECTIONS,
        .input_bytes = TEST_BUFFER_BYTES,
        .scratch_bytes = TEST_BUFFER_BYTES,
        .output_bytes = TEST_BUFFER_BYTES};
    check_equal(
        tdsql_mysql_server_init(&server, &server_config, &error),
        TURBODB_STATUS_OK);

    uint16_t port = 0;
    check_equal(
        tdsql_mysql_server_port(&server, &port, &error), TURBODB_STATUS_OK);
    check_greater(port, 0u);
    client = (mysql_session_config_t){
        .host = "127.0.0.1",
        .port = port,
        .username = "alice",
        .password = test_password,
        .database = "tenant",
        .ca_file = TEST_TLS_CA,
        .server_name = "localhost",
        .timeout_ms = TEST_IO_TIMEOUT_MS};

    server_task.server = &server;
    atomic_init(&server_task.stop, 0);
    atomic_init(&server_task.started, 0);
    atomic_init(&server_task.done, 0);
    atomic_init(&server_task.status, (int)TURBODB_STATUS_OK);
    turbodb_error_init(&server_task.error);
    const coro_executor_config_t executor_config = {
        .worker_count = 1,
        .queue_capacity_per_worker = 2,
        .coroutine_pool = {.initial_capacity = 1, .max_capacity = 1}};
    executor = coro_executor_create(&executor_config);
    check_not_null(executor);
    if (executor) {
      const coro_executor_task_t task = {
          .run = test_server_run,
          .cancel = test_server_cancel,
          .finalize = NULL,
          .arg = &server_task};
      check_equal(coro_executor_submit(executor, &task), SALTS_OK);
      check_true(wait_for_atomic(&server_task.started));
    }
  }

  after_each() {
    if (executor) {
      stop_server_executor();
    } else if (server.initialized) {
      check_equal(
          tdsql_mysql_server_stop(&server, TEST_IO_TIMEOUT_MS, &error),
          TURBODB_STATUS_OK);
    }
    if (database) {
      check_equal(tdsql_database_close(database, &error), TURBODB_STATUS_OK);
      database = NULL;
    }
    if (directory) {
      check_equal(tt_remove_tree(directory), 0);
      free(directory);
      directory = NULL;
    }
    gmssl_secure_clear(&account, sizeof(account));
  }

  it("commits DML and DDL while savepoint rollback removes later work") {
    mysql_session_error_t client_error = {0};
    mysql_session_command_result_t command = {0};
    mysql_transaction_session_t *transaction = NULL;
    static const uint8_t savepoint[] = "after_committed_work";

    check_equal(
        execute("CREATE TABLE items(id BIGINT PRIMARY KEY,score BIGINT)",
                NULL, 0, &command, &client_error),
        MYSQL_SESSION_OK);
    const mysql_stmt_value_t seed[] = {
        {.kind = MYSQL_STMT_VALUE_SINT64, .data.sint64_value = 1},
        {.kind = MYSQL_STMT_VALUE_SINT64, .data.sint64_value = 10}};
    check_equal(
        execute("INSERT INTO items VALUES(?,?)", seed, 2, &command,
                &client_error),
        MYSQL_SESSION_OK);
    check_equal(command.affected_rows, UINT64_C(1));

    check_equal(
        mysql_transaction_session_begin(
            &client, MYSQL_ISOLATION_SERIALIZABLE, TEST_BUFFER_BYTES,
            &transaction, &client_error),
        MYSQL_SESSION_OK);
    check_not_null(transaction);
    const mysql_stmt_value_t update[] = {
        {.kind = MYSQL_STMT_VALUE_SINT64, .data.sint64_value = 20},
        {.kind = MYSQL_STMT_VALUE_SINT64, .data.sint64_value = 1}};
    check_equal(
        mysql_transaction_session_execute_prepared(
            transaction, (const uint8_t *)"UPDATE items SET score=? WHERE id=?",
            strlen("UPDATE items SET score=? WHERE id=?"), update, 2, &command,
            &client_error),
        MYSQL_SESSION_OK);
    check_equal(command.affected_rows, UINT64_C(1));
    check_equal(
        mysql_transaction_session_execute_prepared(
            transaction,
            (const uint8_t *)"CREATE TABLE committed_table(id BIGINT PRIMARY KEY)",
            strlen("CREATE TABLE committed_table(id BIGINT PRIMARY KEY)"),
            NULL, 0, &command, &client_error),
        MYSQL_SESSION_OK);
    check_equal(
        mysql_transaction_session_savepoint(
            transaction, savepoint, sizeof(savepoint) - 1, &client_error),
        MYSQL_SESSION_OK);

    const mysql_stmt_value_t discarded[] = {
        {.kind = MYSQL_STMT_VALUE_SINT64, .data.sint64_value = 2},
        {.kind = MYSQL_STMT_VALUE_SINT64, .data.sint64_value = 30}};
    check_equal(
        mysql_transaction_session_execute_prepared(
            transaction, (const uint8_t *)"INSERT INTO items VALUES(?,?)",
            strlen("INSERT INTO items VALUES(?,?)"), discarded, 2, &command,
            &client_error),
        MYSQL_SESSION_OK);
    check_equal(
        mysql_transaction_session_execute_prepared(
            transaction,
            (const uint8_t *)"CREATE TABLE discarded_table(id BIGINT PRIMARY KEY)",
            strlen("CREATE TABLE discarded_table(id BIGINT PRIMARY KEY)"),
            NULL, 0, &command, &client_error),
        MYSQL_SESSION_OK);
    check_equal(
        mysql_transaction_session_rollback_to_savepoint(
            transaction, savepoint, sizeof(savepoint) - 1, &client_error),
        MYSQL_SESSION_OK);
    check_equal(
        mysql_transaction_session_release_savepoint(
            transaction, savepoint, sizeof(savepoint) - 1, &client_error),
        MYSQL_SESSION_OK);
    check_equal(
        mysql_transaction_session_commit(transaction, &client_error),
        MYSQL_SESSION_OK);
    mysql_transaction_session_destroy(transaction);
    transaction = NULL;

    int64_t score = 0;
    check_true(read_score(1, &score, &client_error));
    check_equal(score, INT64_C(20));
    check_false(read_score(2, &score, &client_error));
    check_equal(
        execute("INSERT INTO committed_table VALUES(?)", seed, 1, &command,
                &client_error),
        MYSQL_SESSION_OK);
    check_equal(
        execute("INSERT INTO discarded_table VALUES(?)", seed, 1, &command,
                &client_error),
        MYSQL_SESSION_SQL_ERROR);

    check_equal(
        mysql_transaction_session_begin(
            &client, MYSQL_ISOLATION_SERIALIZABLE, TEST_BUFFER_BYTES,
            &transaction, &client_error),
        MYSQL_SESSION_OK);
    const mysql_stmt_value_t rolled_back[] = {
        {.kind = MYSQL_STMT_VALUE_SINT64, .data.sint64_value = 99},
        {.kind = MYSQL_STMT_VALUE_SINT64, .data.sint64_value = 1}};
    check_equal(
        mysql_transaction_session_execute_prepared(
            transaction, (const uint8_t *)"UPDATE items SET score=? WHERE id=?",
            strlen("UPDATE items SET score=? WHERE id=?"), rolled_back, 2,
            &command, &client_error),
        MYSQL_SESSION_OK);
    check_equal(
        mysql_transaction_session_rollback(transaction, &client_error),
        MYSQL_SESSION_OK);
    mysql_transaction_session_destroy(transaction);
    transaction = NULL;
    check_true(read_score(1, &score, &client_error));
    check_equal(score, INT64_C(20));

    check_equal(
        mysql_transaction_session_begin(
            &client, MYSQL_ISOLATION_SERIALIZABLE, TEST_BUFFER_BYTES,
            &transaction, &client_error),
        MYSQL_SESSION_OK);
    check_equal(
        mysql_transaction_session_execute_prepared(
            transaction, (const uint8_t *)"UPDATE items SET score=? WHERE id=?",
            strlen("UPDATE items SET score=? WHERE id=?"), rolled_back, 2,
            &command, &client_error),
        MYSQL_SESSION_OK);
    mysql_transaction_session_destroy(transaction);
    transaction = NULL;
    check_true(read_score(1, &score, &client_error));
    check_equal(score, INT64_C(20));
  }

  it("persists SET autocommit in one remote session and applies its boundaries") {
    mysql_session_error_t client_error = {0};
    mysql_session_command_result_t command = {0};
    mysql_transaction_session_t *session = NULL;
    const mysql_stmt_value_t first[] = {
        {.kind = MYSQL_STMT_VALUE_SINT64, .data.sint64_value = 1},
        {.kind = MYSQL_STMT_VALUE_SINT64, .data.sint64_value = 10}};
    const mysql_stmt_value_t second[] = {
        {.kind = MYSQL_STMT_VALUE_SINT64, .data.sint64_value = 2},
        {.kind = MYSQL_STMT_VALUE_SINT64, .data.sint64_value = 20}};

    check_equal(
        execute("CREATE TABLE items(id BIGINT PRIMARY KEY,score BIGINT)",
                NULL, 0, &command, &client_error),
        MYSQL_SESSION_OK);
    check_equal(
        mysql_transaction_session_test_open(
            &client, TEST_BUFFER_BYTES, &session, &client_error),
        MYSQL_SESSION_OK);
    check_not_null(session);
    check_equal(
        mysql_transaction_session_test_execute_control(
            session, (const uint8_t *)"SET autocommit=OFF",
            strlen("SET autocommit=OFF"), &client_error),
        MYSQL_SESSION_OK);
    check_equal(
        mysql_transaction_session_execute_prepared(
            session, (const uint8_t *)"INSERT INTO items VALUES(?,?)",
            strlen("INSERT INTO items VALUES(?,?)"), first, 2, &command,
            &client_error),
        MYSQL_SESSION_OK);
    check_equal(command.affected_rows, UINT64_C(1));
    mysql_transaction_session_test_disconnect(session);
    session = NULL;

    int64_t score = 0;
    check_false(read_score(1, &score, &client_error));

    check_equal(
        mysql_transaction_session_test_open(
            &client, TEST_BUFFER_BYTES, &session, &client_error),
        MYSQL_SESSION_OK);
    check_not_null(session);
    check_equal(
        mysql_transaction_session_test_execute_control(
            session, (const uint8_t *)"SET autocommit=OFF",
            strlen("SET autocommit=OFF"), &client_error),
        MYSQL_SESSION_OK);
    check_equal(
        mysql_transaction_session_execute_prepared(
            session, (const uint8_t *)"INSERT INTO items VALUES(?,?)",
            strlen("INSERT INTO items VALUES(?,?)"), second, 2, &command,
            &client_error),
        MYSQL_SESSION_OK);
    check_equal(
        mysql_transaction_session_test_execute_control(
            session, (const uint8_t *)"SET autocommit=ON",
            strlen("SET autocommit=ON"), &client_error),
        MYSQL_SESSION_OK);
    mysql_transaction_session_destroy(session);
    session = NULL;

    check_true(read_score(2, &score, &client_error));
    check_equal(score, INT64_C(20));
  }

  it("round trips every supported binary parameter kind and result metadata") {
    static const uint8_t sql[] =
        "SELECT ? AS n,? AS i,? AS u,? AS d,? AS b,? AS t,? AS bytes";
    static const uint8_t text[] = {'q', 'u', 'o', 't', 'e', 'd', '\''};
    static const uint8_t blob[] = {0, 255, 1};
    const mysql_stmt_value_t values[] = {
        {.kind = MYSQL_STMT_VALUE_NULL},
        {.kind = MYSQL_STMT_VALUE_SINT64, .data.sint64_value = INT64_MIN},
        {.kind = MYSQL_STMT_VALUE_UINT64, .data.uint64_value = UINT64_MAX},
        {.kind = MYSQL_STMT_VALUE_DOUBLE, .data.double_value = 1.5},
        {.kind = MYSQL_STMT_VALUE_BOOL, .data.bool_value = 1},
        {.kind = MYSQL_STMT_VALUE_TEXT,
         .data.bytes = {.data = text, .size = sizeof(text)}},
        {.kind = MYSQL_STMT_VALUE_BLOB,
         .data.bytes = {.data = blob, .size = sizeof(blob)}}};
    const mysql_session_cursor_limits_t limits = {
        .max_result_rows = 2,
        .max_columns = sizeof(values) / sizeof(values[0]),
        .max_metadata_bytes = TEST_BUFFER_BYTES,
        .max_row_bytes = TEST_BUFFER_BYTES,
        .max_command_bytes = TEST_BUFFER_BYTES};
    mysql_cursor_source_t source = {0};
    const mysql_column_definition_t *columns = NULL;
    size_t column_count = 0;
    mysql_session_error_t client_error = {0};
    const mysql_session_status_t opened = mysql_session_open_prepared_source(
        &client, sql, sizeof(sql) - 1, values,
        sizeof(values) / sizeof(values[0]), &limits, &source, &columns,
        &column_count, &client_error);
    check_equal(opened, MYSQL_SESSION_OK);
    if (opened == MYSQL_SESSION_OK) {
      check_equal(column_count, sizeof(values) / sizeof(values[0]));
      check_equal(columns[0].type, MYSQL_FIELD_TYPE_NULL);
      check_equal(columns[1].type, MYSQL_FIELD_TYPE_LONGLONG);
      check_equal(columns[1].flags & MYSQL_COLUMN_FLAG_UNSIGNED, 0u);
      check_equal(columns[2].type, MYSQL_FIELD_TYPE_LONGLONG);
      check_equal(columns[2].flags & MYSQL_COLUMN_FLAG_UNSIGNED,
                  MYSQL_COLUMN_FLAG_UNSIGNED);
      check_equal(columns[3].type, MYSQL_FIELD_TYPE_DOUBLE);
      check_equal(columns[4].type, MYSQL_FIELD_TYPE_TINY);
      check_equal(columns[5].type, MYSQL_FIELD_TYPE_VAR_STRING);
      check_equal(columns[6].type, MYSQL_FIELD_TYPE_BLOB);

      const mysql_cursor_source_step_t row = source.ops->next(source.context);
      mysql_binary_value_t decoded[sizeof(values) / sizeof(values[0])] = {0};
      check_equal(row.kind, MYSQL_CURSOR_SOURCE_ROW);
      check_equal(
          mysql_wire_decode_binary_row(row.row, row.row_size, columns,
                                       column_count, decoded, column_count),
          MYSQL_WIRE_STATUS_OK);
      check_equal(decoded[0].kind, MYSQL_BINARY_VALUE_NULL);
      check_equal(decoded[1].kind, MYSQL_BINARY_VALUE_SINT64);
      check_equal(decoded[1].data.sint64_value, INT64_MIN);
      check_equal(decoded[2].kind, MYSQL_BINARY_VALUE_UINT64);
      check_equal(decoded[2].data.uint64_value, UINT64_MAX);
      check_equal(decoded[3].kind, MYSQL_BINARY_VALUE_DOUBLE);
      check_equal(decoded[3].data.double_value, 1.5);
      check_equal(decoded[4].kind, MYSQL_BINARY_VALUE_SINT64);
      check_equal(decoded[4].data.sint64_value, INT64_C(1));
      check_equal(decoded[5].kind, MYSQL_BINARY_VALUE_BYTES);
      check_equal(decoded[5].data.bytes.length, sizeof(text));
      check_equal(decoded[5].data.bytes.data, text, sizeof(text));
      check_equal(decoded[6].kind, MYSQL_BINARY_VALUE_BYTES);
      check_equal(decoded[6].data.bytes.length, sizeof(blob));
      check_equal(decoded[6].data.bytes.data, blob, sizeof(blob));
      check_equal(source.ops->next(source.context).kind, MYSQL_CURSOR_SOURCE_DONE);
      source.ops->destroy(source.context);
    }
  }

  it("rolls back an active remote transaction when service shutdown drops it") {
    mysql_session_error_t client_error = {0};
    mysql_session_command_result_t command = {0};
    mysql_transaction_session_t *transaction = NULL;
    check_equal(
        execute("CREATE TABLE items(id BIGINT PRIMARY KEY,score BIGINT)",
                NULL, 0, &command, &client_error),
        MYSQL_SESSION_OK);
    const mysql_stmt_value_t seed[] = {
        {.kind = MYSQL_STMT_VALUE_SINT64, .data.sint64_value = 1},
        {.kind = MYSQL_STMT_VALUE_SINT64, .data.sint64_value = 10}};
    check_equal(
        execute("INSERT INTO items VALUES(?,?)", seed, 2, &command,
                &client_error),
        MYSQL_SESSION_OK);
    check_equal(
        mysql_transaction_session_begin(
            &client, MYSQL_ISOLATION_SERIALIZABLE, TEST_BUFFER_BYTES,
            &transaction, &client_error),
        MYSQL_SESSION_OK);
    const mysql_stmt_value_t update[] = {
        {.kind = MYSQL_STMT_VALUE_SINT64, .data.sint64_value = 77},
        {.kind = MYSQL_STMT_VALUE_SINT64, .data.sint64_value = 1}};
    check_equal(
        mysql_transaction_session_execute_prepared(
            transaction, (const uint8_t *)"UPDATE items SET score=? WHERE id=?",
            strlen("UPDATE items SET score=? WHERE id=?"), update, 2, &command,
            &client_error),
        MYSQL_SESSION_OK);
    check_equal(command.affected_rows, UINT64_C(1));

    stop_server_executor();
    mysql_transaction_session_destroy(transaction);
    transaction = NULL;

    tdsql_connection *local = NULL;
    tdsql_result *result = NULL;
    const tdsql_request request =
        tdsql_request_default(turbodb_view("SELECT score FROM items WHERE id=1"));
    check_equal(tdsql_database_connect(database, &local, &error), TURBODB_STATUS_OK);
    check_equal(tdsql_connection_query(local, &request, &result, &error), TURBODB_STATUS_OK);
    tdsql_row row = {0};
    check_equal(tdsql_result_next(result, &row, &error), TURBODB_STATUS_OK);
    check_equal(row.state, TDSQL_ROW);
    check_equal(row.count, 1u);
    check_equal(row.values[0].kind, TURBODB_VALUE_INT64);
    check_equal(row.values[0].data.int64_value, INT64_C(10));
    check_equal(tdsql_result_next(result, &row, &error), TURBODB_STATUS_OK);
    check_equal(row.state, TDSQL_DONE);
    check_equal(tdsql_result_destroy_checked(result, &error), TURBODB_STATUS_OK);
    check_equal(tdsql_connection_close(local, &error), TURBODB_STATUS_OK);
  }

  it("reports a lost COMMIT acknowledgement once and observes the real store") {
    mysql_session_error_t client_error = {0};
    mysql_session_command_result_t command = {0};
    mysql_transaction_session_t *transaction = NULL;
    check_equal(
        execute("CREATE TABLE items(id BIGINT PRIMARY KEY,score BIGINT)",
                NULL, 0, &command, &client_error),
        MYSQL_SESSION_OK);
    const mysql_stmt_value_t seed[] = {
        {.kind = MYSQL_STMT_VALUE_SINT64, .data.sint64_value = 1},
        {.kind = MYSQL_STMT_VALUE_SINT64, .data.sint64_value = 10}};
    check_equal(
        execute("INSERT INTO items VALUES(?,?)", seed, 2, &command,
                &client_error),
        MYSQL_SESSION_OK);
    check_equal(
        mysql_transaction_session_begin(
            &client, MYSQL_ISOLATION_SERIALIZABLE, TEST_BUFFER_BYTES,
            &transaction, &client_error),
        MYSQL_SESSION_OK);
    const mysql_stmt_value_t id = {
        .kind = MYSQL_STMT_VALUE_SINT64, .data.sint64_value = 1};
    static const uint8_t update[] =
        "UPDATE items SET score=score+1 WHERE id=?";
    check_equal(
        mysql_transaction_session_execute_prepared(
            transaction, update, sizeof(update) - 1, &id, 1, &command,
            &client_error),
        MYSQL_SESSION_OK);
    check_equal(command.affected_rows, UINT64_C(1));

    mysql_transaction_session_test_drop_commit_ack(transaction, 1);
    check_equal(
        mysql_transaction_session_commit(transaction, &client_error),
        MYSQL_SESSION_COMMIT_UNKNOWN);
    check_equal(client_error.status, MYSQL_SESSION_COMMIT_UNKNOWN);
    check_equal(strcmp(client_error.stage, "commit-unknown"), 0);
    check_equal(
        mysql_transaction_session_test_commit_send_count(transaction), 1u);
    mysql_transaction_session_destroy(transaction);
    transaction = NULL;

    stop_server_executor();

    tdsql_connection *local = NULL;
    tdsql_result *result = NULL;
    const tdsql_request request =
        tdsql_request_default(turbodb_view("SELECT score FROM items WHERE id=1"));
    check_equal(tdsql_database_connect(database, &local, &error), TURBODB_STATUS_OK);
    check_equal(tdsql_connection_query(local, &request, &result, &error), TURBODB_STATUS_OK);
    tdsql_row row = {0};
    check_equal(tdsql_result_next(result, &row, &error), TURBODB_STATUS_OK);
    check_equal(row.state, TDSQL_ROW);
    check_equal(row.count, 1u);
    check_equal(row.values[0].kind, TURBODB_VALUE_INT64);
    check_true(row.values[0].data.int64_value == INT64_C(10) ||
               row.values[0].data.int64_value == INT64_C(11));
    check_equal(tdsql_result_next(result, &row, &error), TURBODB_STATUS_OK);
    check_equal(row.state, TDSQL_DONE);
    check_equal(tdsql_result_destroy_checked(result, &error), TURBODB_STATUS_OK);
    check_equal(tdsql_connection_close(local, &error), TURBODB_STATUS_OK);
  }
}

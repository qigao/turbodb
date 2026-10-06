#include <cflow/executor.h>
#include <salts/thread.h>
#define TINYMOCK_GENERATE_FUNCTION_OVERRIDES 1
#include <tinymock.h>
#include <stdatomic.h>
#include <string.h>

FunctionDecl(value, int, rejected_post,
    (void *, executor, CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
     &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER));
TINYMOCk_FUNCTION_DECLARE(rejected_post);
static int reject_post;
static cflow_admission_status test_post(cflow_executor *executor,
                                        cflow_task_fn fn, void *user) {
  return reject_post ? (cflow_admission_status)rejected_post(executor)
                     : cflow_executor_try_post(executor, fn, user);
}
#define cflow_executor_try_post test_post
#include "../../../../drivers/sqlite/async.c"
#undef cflow_executor_try_post

enum { TEST_WAIT_ATTEMPTS = 5000, TEST_TIMEOUT_TICKS = 100000 };
static atomic_int gate_entered, gate_released, gate_owner_thread;
static SALTS_THREAD_LOCAL int on_test_owner;

static void sqlite_gate(sqlite3_context *context, int count,
                        sqlite3_value **values) {
  (void)count;
  (void)values;
  atomic_store(&gate_owner_thread, on_test_owner);
  atomic_fetch_add(&gate_entered, 1);
  while (!atomic_load(&gate_released)) salts_sleep_ms(1u);
  sqlite3_result_int(context, 7);
}

static int await_gate(void) {
  for (unsigned attempt = 0; attempt < TEST_WAIT_ATTEMPTS; ++attempt) {
    if (atomic_load(&gate_entered)) return 1;
    salts_sleep_ms(1u);
  }
  return 0;
}

static orm_row_cursor_step await_step(orm_row_cursor *cursor,
                                      cserde_reader *row) {
  orm_row_cursor_step step = ORM_ROW_CURSOR_STEP_INIT;
  for (unsigned attempt = 0; attempt < TEST_WAIT_ATTEMPTS; ++attempt) {
    step = cursor->ops->next(cursor->context, row);
    if (step.kind != ORM_ROW_CURSOR_WAIT) return step;
    salts_sleep_ms(1u);
  }
  return step;
}

static int64_t read_id(cserde_reader *row) {
  cserde_token token = {0};
  check_equal(cserde_reader_next(row, &token), CSERDE_OK);
  check_equal(token.kind, CSERDE_MAP_BEGIN);
  check_equal(cserde_reader_next(row, &token), CSERDE_OK);
  check_equal(token.kind, CSERDE_STRING);
  check_equal(cserde_reader_next(row, &token), CSERDE_OK);
  check_equal(token.kind, CSERDE_SINT);
  return token.value.sint;
}

spec("SQLite bounded async execution") {
  (void)ttest_config__;
  static orm_connection_t *connection;
  static orm_query_t *query;
  static orm_sqlite_backend_state *owner;
  static cflow_scheduler scheduler;
  static orm_async_config_t config;
  static orm_row_cursor cursor;
  static cserde_reader row;
  static orm_error_t error;
  before_each() {
    orm_config_t options;
    orm_config(&options);
    const orm_option_t filename = {orm_view("filename"), orm_view(":memory:")};
    options.driver = orm_view("sqlite");
    options.options = &filename;
    options.option_count = 1u;
    connection = NULL;
    query = NULL;
    cursor = (orm_row_cursor){0};
    row = (cserde_reader){0};
    scheduler = (cflow_scheduler){0};
    reject_post = 0;
    atomic_store(&gate_entered, 0);
    atomic_store(&gate_released, 0);
    atomic_store(&gate_owner_thread, -1);
    on_test_owner = 1;
    orm_error_init(&error);
    check_equal(orm_connect(&options, &connection, &error), ORM_STATUS_OK);
    owner = connection->backend.context;
    check_equal(sqlite3_create_function_v2(owner->database, "test_gate", 0,
        SQLITE_UTF8, NULL, sqlite_gate, NULL, NULL, NULL), SQLITE_OK);
    check_true(cflow_scheduler_test_init(&scheduler));
    config = (orm_async_config_t){sizeof(config), &scheduler, 1u, TEST_TIMEOUT_TICKS};
    check_equal(orm_raw(connection, orm_view("select test_gate() as id"),
                        &query, &error), ORM_STATUS_OK);
    TINYMOCk_FUNCTION_RESET(rejected_post);
  }
  after_each() {
    atomic_store(&gate_released, 1);
    orm_row_cursor_dispose(&cursor);
    check_equal(owner->async_active, 0);
    check_equal(atomic_load(&owner->cursor_count), (size_t)0u);
    check_null(sqlite3_next_stmt(owner->database, NULL));
    orm_query_destroy(query);
    orm_disconnect(connection);
    cflow_scheduler_destroy(&scheduler);
    TINYMOCk_FUNCTION_VERIFY_TIMES(rejected_post, reject_post ? 1u : 0u);
    TINYMOCk_FUNCTION_DESTROY(rejected_post);
  }

  it("defers prepare until demand and returns WAIT while another thread runs SQLite") {
    check_equal(orm_sqlite_backend_open_async(owner, &query->plan,
        &connection->limits, &config, &cursor, &error), ORM_STATUS_OK);
    check_null(sqlite3_next_stmt(owner->database, NULL));
    check_equal(atomic_load(&gate_entered), 0);
    check_equal(cursor.ops->next(cursor.context, &row).kind, ORM_ROW_CURSOR_WAIT);
    check_true(await_gate());
    check_equal(atomic_load(&gate_owner_thread), 0);
    check_equal(cursor.ops->next(cursor.context, &row).kind, ORM_ROW_CURSOR_WAIT);
    atomic_store(&gate_released, 1);
    check_equal(await_step(&cursor, &row).kind, ORM_ROW_CURSOR_ROW);
    check_equal(read_id(&row), INT64_C(7));
    check_equal(await_step(&cursor, &row).kind, ORM_ROW_CURSOR_DONE);
  }

  it("keeps the connection reserved through cancel until background work is drained") {
    check_equal(orm_sqlite_backend_open_async(owner, &query->plan,
        &connection->limits, &config, &cursor, &error), ORM_STATUS_OK);
    check_equal(cursor.ops->next(cursor.context, &row).kind, ORM_ROW_CURSOR_WAIT);
    check_true(await_gate());
    cursor.ops->cancel(cursor.context);
    cursor.ops->cancel(cursor.context);
    orm_row_cursor other = {0};
    orm_transaction_backend transaction = {0};
    check_equal(connection->backend.ops->open_cursor(owner, &query->plan,
        &connection->limits, &other, &error), ORM_STATUS_BUSY);
    check_equal(connection->backend.ops->execute_command(owner, &query->plan,
        &connection->limits, NULL, &error), ORM_STATUS_BUSY);
    check_equal(connection->backend.ops->begin_transaction(owner,
        ORM_ISOLATION_SERIALIZABLE, &transaction, &error), ORM_STATUS_BUSY);
    check_equal(orm_sqlite_backend_open_async(owner, &query->plan,
        &connection->limits, &config, &other, &error), ORM_STATUS_BUSY);
    atomic_store(&gate_released, 1);
    orm_row_cursor_dispose(&cursor);
    check_equal(connection->backend.ops->open_cursor(owner, &query->plan,
        &connection->limits, &cursor, &error), ORM_STATUS_OK);
    check_equal(cursor.ops->next(cursor.context, &row).kind, ORM_ROW_CURSOR_ROW);
    check_equal(read_id(&row), INT64_C(7));
  }

  it("cancels before first demand without executing SQLite") {
    check_equal(orm_sqlite_backend_open_async(owner, &query->plan,
        &connection->limits, &config, &cursor, &error), ORM_STATUS_OK);
    cursor.ops->cancel(cursor.context);
    check_equal(cursor.ops->next(cursor.context, &row).kind, ORM_ROW_CURSOR_DONE);
    check_equal(atomic_load(&gate_entered), 0);
  }

  it("expires before admission without poisoning the connection") {
    check_equal(orm_sqlite_backend_open_async(owner, &query->plan,
        &connection->limits, &config, &cursor, &error), ORM_STATUS_OK);
    (void)cflow_scheduler_advance(&scheduler, TEST_TIMEOUT_TICKS);
    const orm_row_cursor_step step = cursor.ops->next(cursor.context, &row);
    check_equal(step.kind, ORM_ROW_CURSOR_ERROR);
    check_equal(step.status, ORM_STATUS_SQL_ERROR);
    check_not_null(strstr(step.message, "deadline"));
    check_equal(atomic_load(&gate_entered), 0);
  }

  it("reports deadline while a worker is running and drains before reuse") {
    check_equal(orm_sqlite_backend_open_async(owner, &query->plan,
        &connection->limits, &config, &cursor, &error), ORM_STATUS_OK);
    check_equal(cursor.ops->next(cursor.context, &row).kind, ORM_ROW_CURSOR_WAIT);
    check_true(await_gate());
    (void)cflow_scheduler_advance(&scheduler, TEST_TIMEOUT_TICKS);
    const orm_row_cursor_step step = cursor.ops->next(cursor.context, &row);
    check_equal(step.kind, ORM_ROW_CURSOR_ERROR);
    check_equal(step.status, ORM_STATUS_SQL_ERROR);
    check_equal(owner->async_active, 1);
    atomic_store(&gate_released, 1);
    orm_row_cursor_dispose(&cursor);
    check_equal(sqlite3_exec(owner->database, "select 1", NULL, NULL, NULL), SQLITE_OK);
  }

  it("rejects a full executor without executing or leaking a statement") {
    check_equal(orm_sqlite_backend_open_async(owner, &query->plan,
        &connection->limits, &config, &cursor, &error), ORM_STATUS_OK);
    reject_post = 1;
    int rejected = CFLOW_ADMISSION_FULL;
    check_true(TINYMOCk_FUNCTION_SET_RETURN(rejected_post, rejected));
    const orm_row_cursor_step step = cursor.ops->next(cursor.context, &row);
    check_equal(step.kind, ORM_ROW_CURSOR_ERROR);
    check_equal(step.status, ORM_STATUS_BUSY);
    check_equal(atomic_load(&gate_entered), 0);
  }

  it("interrupts a running recursive query and drains its statement") {
    orm_query_destroy(query);
    query = NULL;
    check_equal(orm_raw(connection, orm_view(
        "with recursive seq(n) as (select test_gate() union all "
        "select n+1 from seq where n<1000000000) select sum(n) as id from seq"),
        &query, &error), ORM_STATUS_OK);
    check_equal(orm_sqlite_backend_open_async(owner, &query->plan,
        &connection->limits, &config, &cursor, &error), ORM_STATUS_OK);
    check_equal(cursor.ops->next(cursor.context, &row).kind, ORM_ROW_CURSOR_WAIT);
    check_true(await_gate());
    cursor.ops->cancel(cursor.context);
    atomic_store(&gate_released, 1);
    orm_row_cursor_dispose(&cursor);
    check_equal(sqlite3_exec(owner->database, "select 1", NULL, NULL, NULL), SQLITE_OK);
  }

  it("rejects asynchronous work on a SQLite connection without a mutex") {
    orm_sqlite_backend_state single = {0};
    atomic_init(&single.cursor_count, 0u);
    check_equal(sqlite3_open_v2(":memory:", &single.database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL), SQLITE_OK);
    const orm_status_t status = orm_sqlite_backend_open_async(&single, &query->plan,
        &connection->limits, &config, &cursor, &error);
    const int close_status = sqlite3_close(single.database);
    check_equal(status, ORM_STATUS_UNSUPPORTED);
    check_equal(close_status, SQLITE_OK);
    check_null(cursor.context);
  }

  it("rejects asynchronous admission during a transaction") {
    orm_transaction_backend transaction = {0};
    check_equal(connection->backend.ops->begin_transaction(owner,
        ORM_ISOLATION_SERIALIZABLE, &transaction, &error), ORM_STATUS_OK);
    const orm_status_t status = orm_sqlite_backend_open_async(owner, &query->plan,
        &connection->limits, &config, &cursor, &error);
    transaction.ops->destroy(transaction.context);
    check_equal(status, ORM_STATUS_BUSY);
    check_null(cursor.context);
  }

  it("rejects async admission while a synchronous cursor retains SQLite") {
    check_equal(connection->backend.ops->open_cursor(owner, &query->plan,
        &connection->limits, &cursor, &error), ORM_STATUS_OK);
    orm_row_cursor other = {0};
    check_equal(orm_sqlite_backend_open_async(owner, &query->plan,
        &connection->limits, &config, &other, &error), ORM_STATUS_BUSY);
    check_null(other.context);
  }

  it("delivers preparation errors from the worker") {
    orm_query_destroy(query);
    query = NULL;
    check_equal(orm_raw(connection, orm_view("select missing from no_such_table"),
                        &query, &error), ORM_STATUS_OK);
    check_equal(orm_sqlite_backend_open_async(owner, &query->plan,
        &connection->limits, &config, &cursor, &error), ORM_STATUS_OK);
    const orm_row_cursor_step step = await_step(&cursor, &row);
    check_equal(step.kind, ORM_ROW_CURSOR_ERROR);
    check_equal(step.status, ORM_STATUS_SQL_ERROR);
    check_not_null(strstr(step.message, "no_such_table"));
  }

  it("preserves result limits and row order across worker tasks") {
    orm_query_destroy(query);
    query = NULL;
    check_equal(orm_raw(connection, orm_view("select 7 as id union all select 11 union all select 19"),
                        &query, &error), ORM_STATUS_OK);
    orm_limits limits = connection->limits;
    limits.max_result_rows = 2u;
    check_equal(orm_sqlite_backend_open_async(owner, &query->plan,
        &limits, &config, &cursor, &error), ORM_STATUS_OK);
    check_equal(await_step(&cursor, &row).kind, ORM_ROW_CURSOR_ROW);
    check_equal(read_id(&row), INT64_C(7));
    check_equal(await_step(&cursor, &row).kind, ORM_ROW_CURSOR_ROW);
    check_equal(read_id(&row), INT64_C(11));
    const orm_row_cursor_step step = await_step(&cursor, &row);
    check_equal(step.kind, ORM_ROW_CURSOR_ERROR);
    check_equal(step.status, ORM_STATUS_LIMIT_EXCEEDED);
  }
}

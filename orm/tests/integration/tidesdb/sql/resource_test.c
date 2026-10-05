#include "backend.h"
#include "bridge.h"
#include "row_fault.h"
#include "../row_fixture.h"
#include <tinytest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned begins, commits, rollbacks;
static int observe_begin(orm_tidesdb_database_t *db, orm_tidesdb_isolation_level_t isolation,
                         orm_tidesdb_transaction_t **txn) {
  const int status = orm_tidesdb_txn_begin_with_isolation(db, isolation, txn);
  if (status == ORM_TDB_SUCCESS) ++begins;
  return status;
}
static int observe_commit(orm_tidesdb_transaction_t *txn) {
  ++commits;
  return orm_tidesdb_txn_commit(txn);
}
static int observe_rollback(orm_tidesdb_transaction_t *txn) {
  ++rollbacks;
  return orm_tidesdb_txn_rollback(txn);
}
#define orm_tidesdb_txn_commit observe_commit
#define orm_tidesdb_txn_rollback observe_rollback
#define orm_tidesdb_txn_begin_with_isolation observe_begin
#include "../../../../../drivers/tidesdb/backend.c"
#undef orm_tidesdb_txn_begin_with_isolation
#undef orm_tidesdb_txn_rollback
#undef orm_tidesdb_txn_commit

enum { MAX_PROBE_CALLS = 64, OPTION_NUMBER_BYTES = 32 };
static orm_connection_t *connection;
static orm_transaction_t *transaction;
static orm_query_t *query;
static cflow_publisher publisher;
static orm_error_t error;
static uint64_t affected;
static char *directory;
static orm_tidesdb_transaction_t *reader;
static orm_tidesdb_iterator_t *iterator;
static size_t encoded_size, key_size;
static const char insert_sql[] = "INSERT INTO people (id, score) VALUES (1,10)";
static const char update_sql[] = "UPDATE people SET score=20 WHERE id=1";
static const char delete_sql[] = "DELETE FROM people WHERE id=1";
static const char select_sql[] = "SELECT id, score FROM people WHERE id=1";

static void drop_query(void) {
  if (cflow_publisher_valid(&publisher)) cflow_publisher_destroy(&publisher);
  publisher = (cflow_publisher){0};
  orm_query_destroy(query); query = NULL;
}

static void connect_config(orm_config_t *config) {
  drop_query();
  orm_disconnect(connection); connection = NULL;
  check_equal(orm_connect_with_factory_v1(config, orm_tidesdb_backend_create,
      &connection, &error), ORM_STATUS_OK);
}

static void prepare(const char *sql) {
  drop_query();
  check_equal(orm_raw(connection, orm_view(sql), &query, &error), ORM_STATUS_OK);
}

/* Exercise the driver's actual command contract to inspect the exact status
 * and native commit/rollback counts. Public Publisher propagation is tested
 * separately in sql_test.c; all calls here belong to this single test thread. */
static orm_status_t execute(const char *sql) {
  prepare(sql);
  orm_error_init(&error);
  affected = UINT64_MAX;
  return transaction == NULL
      ? connection->backend.ops->execute_command(connection->backend.context,
          &query->plan, &connection->limits, &affected, &error)
      : transaction->backend.ops->execute_command(transaction->backend.context,
          &query->plan, &connection->limits, &affected, &error);
}

static void command(const char *sql) {
  check_equal(execute(sql), ORM_STATUS_OK);
  check_equal(affected, UINT64_C(1));
}

static void open_rows(const char *sql) {
  prepare(sql);
  orm_flow_config_t config;
  orm_flow_config(&config, &orm_tides_public_row_data);
  check_equal(orm_query_open_flow(query, &config, &publisher, &error), ORM_STATUS_OK);
}

static void verify_row(const char *sql, long id, long score, int present) {
  open_rows(sql);
  orm_tides_public_row value = {0};
  if (present) {
    check_equal(cflow_publisher_resume(&publisher, NULL, &value).kind, CFLOW_STEP_VALUE);
    check_equal(value.id, id); check_equal(value.score, score);
  }
  check_equal(cflow_publisher_resume(&publisher, NULL, &value).kind, CFLOW_STEP_DONE);
  drop_query();
}

static void measure_row(void) {
  orm_tidesdb_backend_state *backend = connection->backend.context;
  check_equal(orm_tidesdb_txn_begin_with_isolation(backend->database,
      ORM_TDB_ISOLATION_SERIALIZABLE, &reader), ORM_TDB_SUCCESS);
  check_equal(orm_tidesdb_iter_new(reader, backend->column_family, &iterator), ORM_TDB_SUCCESS);
  const uint8_t prefix[] = "orm:";
  check_equal(orm_tidesdb_iter_seek(iterator, prefix, sizeof(prefix) - 1u), ORM_TDB_SUCCESS);
  uint8_t *bytes = NULL;
  check_equal(orm_tidesdb_iter_key(iterator, &bytes, &key_size), ORM_TDB_SUCCESS);
  check_equal(orm_tidesdb_iter_value(iterator, &bytes, &encoded_size), ORM_TDB_SUCCESS);
  check_greater(encoded_size, (size_t)1);
  orm_tidesdb_iter_free(iterator); iterator = NULL;
  check_equal(orm_tidesdb_txn_rollback(reader), ORM_TDB_SUCCESS);
  orm_tidesdb_txn_free(reader); reader = NULL;
}

static void reconnect_limit(const char *option, uint64_t value) {
  orm_config_t config; orm_config(&config);
  config.driver = orm_view("tidesdb");
  char number[OPTION_NUMBER_BYTES];
  const int written = snprintf(number, sizeof(number), "%llu", (unsigned long long)value);
  check_greater(written, 0); check_less((size_t)written, sizeof(number));
  orm_option_t options[] = {{orm_view("path"), orm_view(directory)},
                            {orm_view(option), orm_view(number)}};
  config.options = options; config.option_count = 1;
  if (strcmp(option, "max_parameter_bytes") == 0) config.max_parameter_bytes = value;
  else if (strcmp(option, "max_result_bytes") == 0) config.max_result_bytes = value;
  else config.option_count = 2;
  connect_config(&config);
}

static void expect_row_error(const char *sql, const char *message) {
  open_rows(sql);
  orm_tides_public_row value = {0};
  const cflow_step step = cflow_publisher_resume(&publisher, NULL, &value);
  check_equal(step.kind, CFLOW_STEP_ERROR);
  check_not_null(step.error); check_not_null(strstr(step.error, message));
  check_equal(cflow_publisher_resume(&publisher, NULL, &value).kind, CFLOW_STEP_ERROR);
  drop_query();
}

static void allocation_sweep(const char *sql, orm_tdb_row_fault site, int existing) {
  if (existing) command(insert_sql);
  orm_tdb_row_fault_arm(site, 0);
  const orm_status_t baseline = execute(sql);
  const size_t calls = orm_tdb_row_fault_calls();
  orm_tdb_row_fault_disarm();
  check_equal(baseline, ORM_STATUS_OK);
  check_greater(calls, (size_t)0); check_less(calls, (size_t)MAX_PROBE_CALLS);
  /* Restore the same input before every failing call, without armed cleanup. */
  if (strcmp(sql, delete_sql) != 0) command(delete_sql);
  if (existing) command(insert_sql);
  for (size_t at = 1; at <= calls; ++at) {
    info("site=%d allocation=%zu/%zu", (int)site, at, calls);
    begins = 0; commits = 0; rollbacks = 0;
    orm_tdb_row_fault_arm(site, at);
    const orm_status_t status = execute(sql);
    orm_tdb_row_fault_disarm();
    check_equal(orm_tdb_row_fault_hits(), (size_t)1);
    check_equal(status, ORM_STATUS_OUT_OF_MEMORY);
    check_equal(error.status, ORM_STATUS_OUT_OF_MEMORY);
    check_equal(affected, UINT64_C(0));
    check_equal(commits, 0u);
    check_equal(rollbacks, begins);
    check_equal(connection->failure, ORM_STATUS_OK);
    verify_row(select_sql, 1, 10, existing);
  }
  command(sql);
}

spec("TidesDB resource failure contracts") {
  before_each() {
    connection = NULL; transaction = NULL; query = NULL;
    reader = NULL; iterator = NULL; publisher = (cflow_publisher){0};
    orm_error_init(&error); commits = 0; rollbacks = 0;
    directory = tt_make_temp_dir("orm-tidesdb-resource"); check_not_null(directory);
    orm_config_t config; orm_config(&config);
    const orm_option_t option = {orm_view("path"), orm_view(directory)};
    config.driver = orm_view("tidesdb"); config.options = &option; config.option_count = 1;
    connect_config(&config);
  }
  after_each() {
    orm_tdb_row_fault_disarm();
    orm_tidesdb_iter_free(iterator);
    if (reader != NULL) {
      check_equal(orm_tidesdb_txn_rollback(reader), ORM_TDB_SUCCESS);
      orm_tidesdb_txn_free(reader);
    }
    drop_query(); orm_transaction_destroy(transaction); orm_disconnect(connection);
    if (directory != NULL) { check_equal(tt_remove_tree(directory), 0); free(directory); }
  }
  after_all() { orm_tidesdb_module_cleanup(); }

  it("rejects every numeric INSERT allocation failure without creating a row") {
    allocation_sweep(insert_sql, ORM_TDB_ROW_NUMERIC_COPY, 0);
  }
  it("preserves predicate and assignment allocation errors during UPDATE") {
    allocation_sweep(update_sql, ORM_TDB_ROW_NUMERIC_COPY, 1);
  }
  it("does not commit an empty DELETE after predicate allocation failure") {
    allocation_sweep(delete_sql, ORM_TDB_ROW_NUMERIC_COPY, 1);
  }
  it("rolls back each partial row decode failure before UPDATE") {
    allocation_sweep(update_sql, ORM_TDB_ROW_DECODE_COPY, 1);
  }
  it("rolls back each encoded append failure before publishing UPDATE") {
    allocation_sweep(update_sql, ORM_TDB_ROW_ENCODE_APPEND, 1);
  }
  it("keeps earlier explicit-transaction writes after predicate allocation failure") {
    command(insert_sql);
    check_equal(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
        &transaction, &error), ORM_STATUS_OK);
    command("INSERT INTO people (id, score) VALUES (2,30)");
    commits = 0; rollbacks = 0;
    /* The first numeric copy builds the key; the second evaluates WHERE. */
    orm_tdb_row_fault_arm(ORM_TDB_ROW_NUMERIC_COPY, 2);
    const orm_status_t status = execute(update_sql);
    orm_tdb_row_fault_disarm();
    check_equal(status, ORM_STATUS_OUT_OF_MEMORY);
    check_equal(orm_tdb_row_fault_hits(), (size_t)1);
    check_equal(affected, UINT64_C(0));
    check_equal(commits, 0u); check_equal(rollbacks, 0u);
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    orm_transaction_destroy(transaction); transaction = NULL;
    verify_row(select_sql, 1, 10, 1);
    verify_row("SELECT id, score FROM people WHERE id=2", 2, 30, 1);
  }
  it("enforces the encoded row byte budget at and around the exact boundary") {
    command(insert_sql); measure_row(); command(delete_sql);
    reconnect_limit("max_parameter_bytes", encoded_size - 1u);
    commits = 0;
    check_equal(execute(insert_sql), ORM_STATUS_LIMIT_EXCEEDED);
    check_equal(affected, UINT64_C(0)); check_equal(commits, 0u);
    verify_row(select_sql, 1, 10, 0);
    reconnect_limit("max_parameter_bytes", encoded_size);
    command(insert_sql); verify_row(select_sql, 1, 10, 1); command(delete_sql);
    reconnect_limit("max_parameter_bytes", encoded_size + 1u);
    command(insert_sql); verify_row(select_sql, 1, 10, 1);
  }
  it("charges scanned rows even when WHERE produces no result") {
    command(insert_sql); command("INSERT INTO people (id, score) VALUES (2,20)");
    reconnect_limit("max_scan_rows", 1);
    expect_row_error("SELECT id, score FROM people WHERE score<0", "max_scan_rows");
    reconnect_limit("max_scan_rows", 2);
    verify_row("SELECT id, score FROM people WHERE score<0", 0, 0, 0);
  }
  it("charges both key and encoded value bytes to the scan budget") {
    command(insert_sql); measure_row();
    check_less(key_size, SIZE_MAX - encoded_size);
    reconnect_limit("max_scan_bytes", key_size + encoded_size - 1u);
    expect_row_error(select_sql, "max_scan_bytes");
    reconnect_limit("max_scan_bytes", key_size + encoded_size);
    verify_row(select_sql, 1, 10, 1);
  }
  it("rejects an oversized result before yielding a row") {
    command(insert_sql); measure_row();
    reconnect_limit("max_result_bytes", encoded_size - 1u);
    expect_row_error(select_sql, "max_result_bytes");
    reconnect_limit("max_result_bytes", encoded_size);
    verify_row(select_sql, 1, 10, 1);
  }
}

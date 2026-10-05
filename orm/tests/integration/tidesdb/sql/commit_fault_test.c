#include "backend.h"
#include "bridge.h"
#include "wal_fault.h"
#include "memory_fault.h"
#include "../row_fixture.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

static unsigned rollback_calls;
static bool inject_memory_commit;
static orm_tdb_memory_phase memory_phase;
static int observe_commit(orm_tidesdb_transaction_t *transaction) {
  if (!inject_memory_commit) return orm_tidesdb_txn_commit(transaction);
  inject_memory_commit = false;
  orm_tdb_memory_fault_arm(memory_phase, 1u);
  const int status = orm_tidesdb_txn_commit(transaction);
  orm_tdb_memory_fault_disarm();
  return status;
}
static int observe_rollback(orm_tidesdb_transaction_t *transaction) {
  ++rollback_calls;
  return orm_tidesdb_txn_rollback(transaction);
}
/* Observe cleanup only; execution and WAL failures use the real native core. */
#define orm_tidesdb_txn_rollback observe_rollback
#define orm_tidesdb_txn_commit observe_commit
#include "../../../../../drivers/tidesdb/backend.c"
#undef orm_tidesdb_txn_commit
#undef orm_tidesdb_txn_rollback

static orm_connection_t *connection;
static orm_transaction_t *transaction;
static orm_query_t *query, *pending_query;
static cflow_publisher publisher, pending;
static orm_error_t error;
static char *directory;
static orm_tidesdb_transaction_t *competitor;
static orm_tidesdb_iterator_t *iterator;
static orm_tidesdb_database_t *setup_database;

static void prepare(const char *sql) {
  if (cflow_publisher_valid(&publisher)) cflow_publisher_destroy(&publisher);
  publisher = (cflow_publisher){0};
  orm_query_destroy(query); query = NULL;
  check_equal(orm_raw(connection, orm_view(sql), &query, &error), ORM_STATUS_OK);
  const orm_status_t status = transaction == NULL
      ? orm_query_open_command_flow(query, &publisher, &error)
      : orm_query_open_command_flow_in_transaction(query, transaction, &publisher, &error);
  check_equal(status, ORM_STATUS_OK);
}

static void execute(void) {
  orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
  check_equal(cflow_publisher_resume(&publisher, NULL, &result).kind, CFLOW_STEP_VALUE_AND_DONE);
  check_equal(result.affected_rows, UINT64_C(1));
  cflow_publisher_destroy(&publisher); publisher = (cflow_publisher){0};
}

static void reject_prepared_autocommit(orm_tdb_wal_fault fault) {
  orm_tdb_wal_fault_arm(fault);
  orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
  const cflow_step step = cflow_publisher_resume(&publisher, NULL, &result);
  check_equal(step.kind, CFLOW_STEP_ERROR);
  check_not_null(step.error);
  check_not_null(strstr(step.error, "outcome unknown"));
  check_not_null(strstr(step.error, "I/O failure (-4)"));
  check_equal(result.affected_rows, UINT64_C(0));
  check_equal(orm_tdb_wal_fault_hits(), (size_t)1);
  check_equal(connection->failure, ORM_STATUS_COMMIT_UNKNOWN);
  check_false(connection->native_active);
  check_equal(rollback_calls, 0u);
  check_equal(cflow_publisher_resume(&publisher, NULL, &result).kind, CFLOW_STEP_ERROR);
  check_equal(orm_tdb_wal_fault_hits(), (size_t)1);
}

static void reject_autocommit(orm_tdb_wal_fault fault) {
  prepare("INSERT INTO people (id, score) VALUES (1, 10)");
  reject_prepared_autocommit(fault);
}

static void check_unknown_transaction(const char *native_error) {
  check_equal(error.status, ORM_STATUS_COMMIT_UNKNOWN);
  check_not_null(strstr(error.message, native_error));
  check_equal(transaction->state, ORM_TRANSACTION_COMMIT_UNKNOWN);
  check_equal(connection->failure, ORM_STATUS_COMMIT_UNKNOWN);
  check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_INVALID_STATE);
  check_equal(orm_transaction_rollback(transaction, &error), ORM_STATUS_INVALID_STATE);
  check_equal(orm_transaction_savepoint(transaction, orm_view("after_unknown"), &error), ORM_STATUS_INVALID_STATE);
  check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_OK);
  check_equal(rollback_calls, 0u);
}

static void reject_explicit_commit(orm_tdb_wal_fault fault) {
  check_equal(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
      &transaction, &error), ORM_STATUS_OK);
  prepare("INSERT INTO people (id, score) VALUES (1, 10)"); execute();
  orm_tdb_wal_fault_arm(fault);
  check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_COMMIT_UNKNOWN);
  check_unknown_transaction("I/O failure (-4)");
  check_equal(orm_tdb_wal_fault_hits(), (size_t)1);
}

static void check_memory_failure(orm_tdb_memory_phase phase) {
  const orm_tdb_memory_observation observed = orm_tdb_memory_fault_observe();
  check_equal(observed.failures, (size_t)1);
  check_equal(observed.wal_frames, phase == ORM_TDB_MEMORY_AFTER_WAL ? (size_t)1 : (size_t)0);
  check_equal(connection->failure, ORM_STATUS_COMMIT_UNKNOWN);
  check_false(connection->native_active);
  check_equal(rollback_calls, 0u);
}

static void reject_memory_transaction(orm_tdb_memory_phase phase) {
  check_equal(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
      &transaction, &error), ORM_STATUS_OK);
  for (size_t i = 0; i < ORM_TDB_MEMORY_BATCH_KEYS; ++i) {
    char sql[ORM_TDB_MEMORY_KEY_BYTES];
    const int size = snprintf(sql, sizeof(sql), "INSERT INTO people (id) VALUES (%zu)", i);
    check_greater(size, 0); check_less((size_t)size, sizeof(sql));
    prepare(sql); execute();
  }
  orm_tdb_memory_fault_arm(phase,
      phase == ORM_TDB_MEMORY_AFTER_WAL ? ORM_TDB_MEMORY_BATCH_ALLOCATION : 1u);
  const orm_status_t status = orm_transaction_commit(transaction, &error);
  orm_tdb_memory_fault_disarm();
  check_equal(status, ORM_STATUS_COMMIT_UNKNOWN);
  check_memory_failure(phase);
  check_unknown_transaction("memory allocation failed (-1)");
}

static void reject_memory_autocommit(orm_tdb_memory_phase phase) {
  /* Exceed the native 64 KiB arena block so applying this single row must
   * allocate after the WAL, independent of an already reserved small block. */
  enum { PAYLOAD_BYTES = 128 * 1024 };
  static const unsigned char payload[PAYLOAD_BYTES] = {0};
  check_equal(orm_raw(connection, orm_view("INSERT INTO people (id, note) VALUES (1,?)"),
      &query, &error), ORM_STATUS_OK);
  check_equal(orm_query_bind(query, orm_blob(payload, sizeof(payload)), &error), ORM_STATUS_OK);
  check_equal(orm_query_open_command_flow(query, &publisher, &error), ORM_STATUS_OK);
  memory_phase = phase; inject_memory_commit = true;
  orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
  const cflow_step step = cflow_publisher_resume(&publisher, NULL, &result);
  check_false(inject_memory_commit);
  check_equal(step.kind, CFLOW_STEP_ERROR);
  check_not_null(step.error);
  check_not_null(strstr(step.error, "outcome unknown"));
  check_not_null(strstr(step.error, "memory allocation failed (-1)"));
  check_equal(result.affected_rows, UINT64_C(0));
  check_memory_failure(phase);
  check_equal(cflow_publisher_resume(&publisher, NULL, &result).kind, CFLOW_STEP_ERROR);
  orm_query_t *next = NULL;
  check_equal(orm_raw(connection, orm_view("SELECT id FROM people"), &next, &error), ORM_STATUS_INVALID_STATE);
  check_null(next);
}

spec("TidesDB uncertain commit through the ORM owner") {
  before_all() { check_equal(orm_tdb_memory_fault_init(), 0); }
  before_each() {
    connection = NULL; transaction = NULL; query = NULL; pending_query = NULL;
    competitor = NULL; iterator = NULL;
    setup_database = NULL;
    publisher = (cflow_publisher){0}; pending = (cflow_publisher){0};
    rollback_calls = 0;
    inject_memory_commit = false;
    orm_error_init(&error);
    directory = tt_make_temp_dir("orm-tidesdb-commit");
    check_not_null(directory);
    /* Open an existing FULL-sync CF through the ordinary driver. Production
     * defaults and public configuration stay unchanged. */
    orm_tidesdb_config_t native_config = orm_tidesdb_default_config();
    native_config.db_path = directory;
    check_equal(orm_tidesdb_open(&native_config, &setup_database), ORM_TDB_SUCCESS);
    orm_tidesdb_column_family_config_t family_config = orm_tidesdb_default_column_family_config();
    family_config.sync_mode = ORM_TDB_SYNC_FULL;
    check_equal(orm_tidesdb_create_column_family(setup_database,
        orm_tidesdb_default_column_family, &family_config), ORM_TDB_SUCCESS);
    check_equal(orm_tidesdb_close(setup_database), ORM_TDB_SUCCESS);
    setup_database = NULL;
    orm_config_t config; orm_config(&config);
    const orm_option_t option = {orm_view("path"), orm_view(directory)};
    config.driver = orm_view("tidesdb"); config.options = &option; config.option_count = 1;
    check_equal(orm_connect_with_factory_v1(&config, orm_tidesdb_backend_create,
        &connection, &error), ORM_STATUS_OK);
    orm_tidesdb_backend_state *backend = connection->backend.context;
    check_equal(orm_tidesdb_verify_sync_full(backend->column_family), ORM_TDB_SUCCESS);
  }
  after_each() {
    orm_tdb_memory_fault_disarm(); inject_memory_commit = false;
    orm_tidesdb_iter_free(iterator);
    if (competitor != NULL) {
      check_equal(orm_tidesdb_txn_rollback(competitor), ORM_TDB_SUCCESS);
      orm_tidesdb_txn_free(competitor);
    }
    if (cflow_publisher_valid(&publisher)) cflow_publisher_destroy(&publisher);
    if (cflow_publisher_valid(&pending)) cflow_publisher_destroy(&pending);
    orm_query_destroy(query); orm_query_destroy(pending_query);
    orm_transaction_destroy(transaction);
    orm_disconnect(connection);
    if (setup_database != NULL)
      check_equal(orm_tidesdb_close(setup_database), ORM_TDB_SUCCESS);
    if (directory != NULL) {
      check_equal(tt_remove_tree(directory), 0);
      free(directory);
    }
  }
  after_all() { orm_tidesdb_module_cleanup(); orm_tdb_memory_fault_finish(); }

  it("quarantines autocommit after allocation failure before WAL admission") {
    reject_memory_autocommit(ORM_TDB_MEMORY_BEFORE_WAL);
  }
  it("quarantines autocommit after memtable allocation failure beyond WAL admission") {
    reject_memory_autocommit(ORM_TDB_MEMORY_AFTER_WAL);
  }
  it("makes explicit commit terminal after allocation failure before WAL admission") {
    reject_memory_transaction(ORM_TDB_MEMORY_BEFORE_WAL);
  }
  it("makes explicit commit terminal after batch allocation failure beyond WAL admission") {
    reject_memory_transaction(ORM_TDB_MEMORY_AFTER_WAL);
  }

  it("quarantines an autocommit connection after a full WAL write reports IO failure") {
    reject_autocommit(ORM_TDB_WAL_FAIL_AFTER);
    orm_query_t *next = NULL;
    check_equal(orm_raw(connection, orm_view("SELECT id FROM people"), &next, &error), ORM_STATUS_INVALID_STATE);
    check_null(next);
    check_equal(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
        &transaction, &error), ORM_STATUS_INVALID_STATE);
    check_null(transaction);
  }
  it("conservatively reports unknown when native IO errors do not identify their phase") {
    reject_autocommit(ORM_TDB_WAL_FAIL_BEFORE);
  }
  it("does not report success or affected rows after a short WAL write") {
    reject_autocommit(ORM_TDB_WAL_FAIL_PARTIAL);
  }
  it("quarantines an uncertain UPDATE and clears its provisional affected row count") {
    prepare("INSERT INTO people (id, score) VALUES (1, 10)"); execute();
    prepare("UPDATE people SET score = 20 WHERE id = 1");
    reject_prepared_autocommit(ORM_TDB_WAL_FAIL_AFTER);
  }
  it("quarantines an uncertain DELETE and clears its provisional affected row count") {
    prepare("INSERT INTO people (id, score) VALUES (1, 10)"); execute();
    prepare("DELETE FROM people WHERE id = 1");
    reject_prepared_autocommit(ORM_TDB_WAL_FAIL_AFTER);
  }
  it("blocks a previously prepared command after another command has an unknown outcome") {
    check_equal(orm_raw(connection, orm_view("INSERT INTO people (id) VALUES (2)"),
        &pending_query, &error), ORM_STATUS_OK);
    check_equal(orm_query_open_command_flow(pending_query, &pending, &error), ORM_STATUS_OK);
    reject_autocommit(ORM_TDB_WAL_FAIL_AFTER);
    orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
    const cflow_step step = cflow_publisher_resume(&pending, NULL, &result);
    check_equal(step.kind, CFLOW_STEP_ERROR);
    check_not_null(step.error);
    check_not_null(strstr(step.error, "uncertain commit"));
    check_equal(rollback_calls, 0u);
    check_equal(orm_tdb_wal_fault_hits(), (size_t)1);
  }
  it("makes an explicit unknown commit terminal without a rollback during close") {
    reject_explicit_commit(ORM_TDB_WAL_FAIL_AFTER);
  }
  if (orm_tdb_wal_uses_explicit_sync()) {
    it("quarantines autocommit after failure at the explicit WAL sync") {
      reject_autocommit(ORM_TDB_WAL_SYNC_FAIL_BEFORE);
    }
    it("quarantines autocommit when a real sync succeeds but its result reports failure") {
      reject_autocommit(ORM_TDB_WAL_SYNC_FAIL_AFTER);
    }
    it("makes explicit commit terminal after failure at the WAL sync") {
      reject_explicit_commit(ORM_TDB_WAL_SYNC_FAIL_BEFORE);
    }
    it("makes explicit commit terminal after an error reported beyond the real WAL sync") {
      reject_explicit_commit(ORM_TDB_WAL_SYNC_FAIL_AFTER);
    }
  } else {
    it_skip("explicit sync fault cases require the native non-O_DSYNC path") {}
  }
  it("keeps a confirmed pre-WAL conflict rollbackable without quarantining the connection") {
    prepare("INSERT INTO people (id, score) VALUES (1, 10)"); execute();
    check_equal(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
        &transaction, &error), ORM_STATUS_OK);
    prepare("UPDATE people SET score = 20 WHERE id = 1"); execute();
    orm_tidesdb_backend_state *backend = connection->backend.context;
    check_equal(orm_tidesdb_txn_begin_with_isolation(backend->database,
        ORM_TDB_ISOLATION_SERIALIZABLE, &competitor), ORM_TDB_SUCCESS);
    check_equal(orm_tidesdb_iter_new(competitor, backend->column_family, &iterator), ORM_TDB_SUCCESS);
    const uint8_t prefix[] = "orm:";
    check_equal(orm_tidesdb_iter_seek(iterator, prefix, sizeof(prefix) - 1u), ORM_TDB_SUCCESS);
    uint8_t *key = NULL, *bytes = NULL;
    size_t key_size = 0, byte_size = 0;
    check_equal(orm_tidesdb_iter_key(iterator, &key, &key_size), ORM_TDB_SUCCESS);
    check_equal(orm_tidesdb_iter_value(iterator, &bytes, &byte_size), ORM_TDB_SUCCESS);
    check_equal(orm_tidesdb_txn_put(competitor, backend->column_family,
        key, key_size, bytes, byte_size, 0), ORM_TDB_SUCCESS);
    orm_tidesdb_iter_free(iterator); iterator = NULL;
    check_equal(orm_tidesdb_txn_commit(competitor), ORM_TDB_SUCCESS);
    orm_tidesdb_txn_free(competitor); competitor = NULL;
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_BUSY);
    check_equal(connection->failure, ORM_STATUS_OK);
    check_equal(transaction->state, ORM_TRANSACTION_ACTIVE);
    check_equal(orm_transaction_rollback(transaction, &error), ORM_STATUS_OK);
    check_equal(rollback_calls, 1u);
  }
}

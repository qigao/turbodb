#include "bridge.h"
#include <tinytest.h>
#include <cmeta_error.h>
#include <cmeta_process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(ORM_TDB_STORAGE_FAULTS)
#include "wal_fault.h"
#include "memory_fault.h"
#endif

enum {
  TRANSACTION_SLOTS = 3,
  SCAN_ROW_LIMIT = 32,
  CHILD_TIMEOUT_MS = 15000,
  CHILD_OUTPUT_BYTES = 16384,
  CHILD_CHECK_FAILED = 70,
  INVALID_SYNC_MODE = 99
};
static const char family_name[] = "sql-storage-contract";
static const char epoch_key[] = "catalog/epoch";
static const char table_version_key[] = "catalog/people/write-version";
static const char row_key[] = "data/people/1";
static const char old_index_key[] = "unique/name/old";
static const char new_index_key[] = "unique/name/new";
static const char statement_savepoint[] = "private-statement";
static const char outer_savepoint[] = "user-outer";
static orm_tidesdb_database_t *database;
static orm_tidesdb_column_family_t *family;
static orm_tidesdb_transaction_t *transactions[TRANSACTION_SLOTS];
static orm_tidesdb_iterator_t *iterator;
static cmeta_process_t *child;
static char *directory;

static void open_database(void) {
  orm_tidesdb_config_t config = orm_tidesdb_default_config();
  config.db_path = directory;
  check_equal(orm_tidesdb_open(&config, &database), ORM_TDB_SUCCESS);
}

static void create_family(int mode) {
  orm_tidesdb_column_family_config_t config = orm_tidesdb_default_column_family_config();
  config.sync_mode = mode;
  check_equal(orm_tidesdb_create_column_family(database, family_name, &config), ORM_TDB_SUCCESS);
  family = orm_tidesdb_get_column_family(database, family_name);
  check_not_null(family);
}

static void begin(size_t slot) {
  check_null(transactions[slot]);
  check_equal(orm_tidesdb_txn_begin_with_isolation(database,
      ORM_TDB_ISOLATION_SERIALIZABLE, &transactions[slot]), ORM_TDB_SUCCESS);
}

static int write_value(orm_tidesdb_transaction_t *transaction, const char *key, const char *value) {
  return orm_tidesdb_txn_put(transaction, family, (const uint8_t *)key, strlen(key),
      (const uint8_t *)value, strlen(value), 0);
}

static void put(size_t slot, const char *key, const char *value) {
  check_equal(write_value(transactions[slot], key, value), ORM_TDB_SUCCESS);
}

static void erase(size_t slot, const char *key) {
  check_equal(orm_tidesdb_txn_delete(transactions[slot], family,
      (const uint8_t *)key, strlen(key)), ORM_TDB_SUCCESS);
}

static void value(size_t slot, const char *key, const char *expected) {
  uint8_t *bytes = NULL;
  size_t length = 0;
  const int status = orm_tidesdb_txn_get(transactions[slot], family,
      (const uint8_t *)key, strlen(key), &bytes, &length);
  const int expected_status = expected == NULL ? ORM_TDB_ERR_NOT_FOUND : ORM_TDB_SUCCESS;
  const int equal = expected == NULL ? bytes == NULL :
      bytes != NULL && length == strlen(expected) && memcmp(bytes, expected, length) == 0;
  orm_tidesdb_free(bytes);
  info("key=%s status=%d size=%zu", key, status, length);
  check_equal(status, expected_status);
  check_true(equal);
}

static void commit(size_t slot, int expected) {
  const int status = orm_tidesdb_txn_commit(transactions[slot]);
  if (status != ORM_TDB_SUCCESS)
    check_equal(orm_tidesdb_txn_rollback(transactions[slot]), ORM_TDB_SUCCESS);
  orm_tidesdb_txn_free(transactions[slot]);
  transactions[slot] = NULL;
  check_equal(status, expected);
}

static void rollback(size_t slot) {
  check_equal(orm_tidesdb_txn_rollback(transactions[slot]), ORM_TDB_SUCCESS);
  orm_tidesdb_txn_free(transactions[slot]);
  transactions[slot] = NULL;
}

static size_t scan(size_t slot, const char *prefix) {
  size_t count = 0;
  const size_t prefix_size = strlen(prefix);
  check_equal(orm_tidesdb_iter_new(transactions[slot], family, &iterator), ORM_TDB_SUCCESS);
  int status = orm_tidesdb_iter_seek(iterator, (const uint8_t *)prefix, prefix_size);
  while (status == ORM_TDB_SUCCESS && orm_tidesdb_iter_valid(iterator)) {
    uint8_t *key = NULL;
    size_t size = 0;
    check_equal(orm_tidesdb_iter_key(iterator, &key, &size), ORM_TDB_SUCCESS);
    if (size < prefix_size || memcmp(key, prefix, prefix_size) != 0) break;
    check_less(count, (size_t)SCAN_ROW_LIMIT);
    ++count;
    status = orm_tidesdb_iter_next(iterator);
  }
  check_true(status == ORM_TDB_SUCCESS || status == ORM_TDB_ERR_NOT_FOUND);
  orm_tidesdb_iter_free(iterator);
  iterator = NULL;
  return count;
}

static void seed(void) {
  begin(0);
  put(0, epoch_key, "1");
  put(0, row_key, "old");
  put(0, old_index_key, "1");
  commit(0, ORM_TDB_SUCCESS);
}

static void guarded_inserts(size_t initial_rows) {
  begin(0); begin(1);
  value(0, table_version_key, "1"); value(1, table_version_key, "1");
  check_equal(scan(0, "data/"), initial_rows);
  check_equal(scan(1, "data/"), initial_rows);
  /* Every writer must update this key, including point writes, DDL, and
   * maintenance. Protecting only scan-based writers would leave the hole. */
  put(0, table_version_key, "2"); put(0, row_key, "first");
  put(1, table_version_key, "2"); put(1, "data/people/2", "second");
  commit(0, ORM_TDB_SUCCESS);
  commit(1, ORM_TDB_ERR_CONFLICT);
  begin(2);
  value(2, table_version_key, "2"); value(2, row_key, "first");
  value(2, "data/people/2", NULL);
  check_equal(scan(2, "data/"), initial_rows + 1u);
  rollback(2);
}

/* Child exit deliberately bypasses database close and all test cleanup. This
 * covers process-loss WAL recovery, not hardware power loss. */
static void child_require(int status) {
  if (status != ORM_TDB_SUCCESS) {
    (void)fprintf(stderr, "storage child failed: %d\n", status);
    (void)fflush(stderr);
    _Exit(CHILD_CHECK_FAILED);
  }
}

#if defined(ORM_TDB_STORAGE_FAULTS)
static void memory_batch_key(char *out, size_t index) {
  const int written = snprintf(out, ORM_TDB_MEMORY_KEY_BYTES, "memory/row/%zu", index);
  if (written < 0 || written >= ORM_TDB_MEMORY_KEY_BYTES) _Exit(CHILD_CHECK_FAILED);
}

static void memory_commit_failure(int after_wal) {
  for (size_t i = 0; i < ORM_TDB_MEMORY_BATCH_KEYS; ++i) {
    char key[ORM_TDB_MEMORY_KEY_BYTES];
    memory_batch_key(key, i);
    child_require(write_value(transactions[0], key, "complete-batch"));
  }
  orm_tdb_memory_fault_arm(after_wal ? ORM_TDB_MEMORY_AFTER_WAL : ORM_TDB_MEMORY_BEFORE_WAL,
      after_wal ? ORM_TDB_MEMORY_BATCH_ALLOCATION : 1u);
  const int status = orm_tidesdb_txn_commit(transactions[0]);
  orm_tdb_memory_fault_disarm();
  const orm_tdb_memory_observation observed = orm_tdb_memory_fault_observe();
  if (status != ORM_TDB_ERR_MEMORY || observed.failures != 1u ||
      observed.wal_frames != (after_wal ? 1u : 0u)) _Exit(CHILD_CHECK_FAILED);
  child_require(orm_tidesdb_txn_rollback(transactions[0]));
}
#endif

static void crash_worker(const char *mode) {
  orm_tidesdb_config_t config = orm_tidesdb_default_config();
  config.db_path = ".";
  child_require(orm_tidesdb_open(&config, &database));
  family = orm_tidesdb_get_column_family(database, family_name);
  if (family == NULL) _Exit(CHILD_CHECK_FAILED);
  child_require(orm_tidesdb_verify_sync_full(family));
  child_require(orm_tidesdb_txn_begin_with_isolation(database,
      ORM_TDB_ISOLATION_SERIALIZABLE, &transactions[0]));
  child_require(write_value(transactions[0], epoch_key, "2"));
  child_require(write_value(transactions[0], row_key, "new"));
  child_require(orm_tidesdb_txn_delete(transactions[0], family,
      (const uint8_t *)old_index_key, strlen(old_index_key)));
  child_require(write_value(transactions[0], new_index_key, "1"));
  if (strcmp(mode, "committed") == 0)
    child_require(orm_tidesdb_txn_commit(transactions[0]));
#if defined(ORM_TDB_STORAGE_FAULTS)
  else if (strcmp(mode, "memory-before") == 0) memory_commit_failure(0);
  else if (strcmp(mode, "memory-after") == 0) memory_commit_failure(1);
#endif
  else if (strcmp(mode, "pending") != 0) {
#if defined(ORM_TDB_STORAGE_FAULTS)
    orm_tdb_wal_fault fault = ORM_TDB_WAL_FAULT_NONE;
    if (strcmp(mode, "fail-before") == 0) fault = ORM_TDB_WAL_FAIL_BEFORE;
    else if (strcmp(mode, "fail-partial") == 0) fault = ORM_TDB_WAL_FAIL_PARTIAL;
    else if (strcmp(mode, "fail-after") == 0) fault = ORM_TDB_WAL_FAIL_AFTER;
    else if (strcmp(mode, "exit-partial") == 0) fault = ORM_TDB_WAL_EXIT_PARTIAL;
    else if (strcmp(mode, "exit-after") == 0) fault = ORM_TDB_WAL_EXIT_AFTER;
    else if (strcmp(mode, "sync-fail-before") == 0) fault = ORM_TDB_WAL_SYNC_FAIL_BEFORE;
    else if (strcmp(mode, "sync-fail-after") == 0) fault = ORM_TDB_WAL_SYNC_FAIL_AFTER;
    else if (strcmp(mode, "sync-exit-before") == 0) fault = ORM_TDB_WAL_SYNC_EXIT_BEFORE;
    else if (strcmp(mode, "sync-exit-after") == 0) fault = ORM_TDB_WAL_SYNC_EXIT_AFTER;
    else _Exit(CHILD_CHECK_FAILED);
    orm_tdb_wal_fault_arm(fault);
    const int status = orm_tidesdb_txn_commit(transactions[0]);
    if (status != ORM_TDB_ERR_IO || orm_tdb_wal_fault_hits() != 1u)
      _Exit(CHILD_CHECK_FAILED);
    /* A successful native rollback cannot retract a complete WAL frame. */
    child_require(orm_tidesdb_txn_rollback(transactions[0]));
#else
    _Exit(CHILD_CHECK_FAILED);
#endif
  }
  (void)fputs("storage-crash-point\n", stderr);
  (void)fflush(stderr);
  _Exit(EXIT_SUCCESS);
}

static void run_crash(const char *environment) {
  const char *program = getenv("ORM_TDB_STORAGE_TEST_EXE");
  const char *env[] = {environment, NULL};
  cmeta_process_options_t options;
  cmeta_process_result_t result = {0};
  cmeta_process_options_init(&options);
  check_not_null(program);
  options.program = program;
  options.cwd = directory;
  options.env = env;
  options.timeout_ms = CHILD_TIMEOUT_MS;
  options.max_output_bytes = CHILD_OUTPUT_BYTES;
  check_equal(cmeta_process_spawn(&options, &child), SALTS_OK);
  check_equal(cmeta_process_wait(child, &result), SALTS_OK);
  char output[CHILD_OUTPUT_BYTES] = {0};
  size_t total = 0;
  while (total < sizeof(output) - 1u) {
    size_t count = 0;
    int status = cmeta_process_read_stderr(child, output + total,
        sizeof(output) - 1u - total, &count);
    total += count;
    if (status == SALTS_EOF || count == 0) break;
    check_equal(status, SALTS_OK);
  }
  info("storage child stderr: %s", output);
  check_equal(result.state, SALTS_PROCESS_EXITED);
  check_equal(result.exit_code, EXIT_SUCCESS);
  check_not_null(strstr(output, "storage-crash-point"));
  cmeta_process_destroy(child);
  child = NULL;
}

static void close_database(void) {
  check_equal(orm_tidesdb_close(database), ORM_TDB_SUCCESS);
  database = NULL;
  family = NULL;
}

static void recover_after_exit(const char *environment, int changed) {
  seed(); close_database();
  run_crash(environment);
  open_database();
  family = orm_tidesdb_get_column_family(database, family_name);
  check_not_null(family);
  check_equal(orm_tidesdb_verify_sync_full(family), ORM_TDB_SUCCESS);
  begin(0);
  value(0, epoch_key, changed ? "2" : "1");
  value(0, row_key, changed ? "new" : "old");
  value(0, old_index_key, changed ? NULL : "1");
  value(0, new_index_key, changed ? "1" : NULL);
#if defined(ORM_TDB_STORAGE_FAULTS)
  if (strstr(environment, "=memory-") != NULL) {
    for (size_t i = 0; i < ORM_TDB_MEMORY_BATCH_KEYS; ++i) {
      char key[ORM_TDB_MEMORY_KEY_BYTES];
      memory_batch_key(key, i);
      value(0, key, changed ? "complete-batch" : NULL);
    }
  }
#endif
  rollback(0);
}

spec("TidesDB SQL native storage prerequisites") {
  before_all() {
#if defined(ORM_TDB_STORAGE_FAULTS)
    check_equal(orm_tdb_memory_fault_init(), 0);
#endif
    const char *mode = getenv("ORM_TDB_STORAGE_CHILD");
    if (mode != NULL) crash_worker(mode);
  }
  after_all() {
    /* Module cleanup is terminal for this loaded image, not per connection. */
    orm_tidesdb_module_cleanup();
#if defined(ORM_TDB_STORAGE_FAULTS)
    orm_tdb_memory_fault_finish();
#endif
  }
  before_each() {
    database = NULL; family = NULL; iterator = NULL; child = NULL;
    memset(transactions, 0, sizeof(transactions));
    directory = tt_make_temp_dir("orm-tidesdb-storage");
    check_not_null(directory);
    open_database();
    create_family(ORM_TDB_SYNC_FULL);
  }
  after_each() {
    cmeta_process_destroy(child);
    orm_tidesdb_iter_free(iterator);
    for (size_t i = 0; i < TRANSACTION_SLOTS; ++i) {
      if (transactions[i] != NULL) {
        check_equal(orm_tidesdb_txn_rollback(transactions[i]), ORM_TDB_SUCCESS);
        orm_tidesdb_txn_free(transactions[i]);
      }
    }
    if (database != NULL) close_database();
    if (directory != NULL) {
      check_equal(tt_remove_tree(directory), 0);
      free(directory);
    }
  }

  it("keeps legacy sync defaults and rejects invalid durability configuration") {
    orm_tidesdb_column_family_config_t config = orm_tidesdb_default_column_family_config();
    check_equal(config.sync_mode, ORM_TDB_SYNC_DEFAULT);
    check_equal(orm_tidesdb_create_column_family(database, "legacy", &config), ORM_TDB_SUCCESS);
    orm_tidesdb_column_family_t *legacy = orm_tidesdb_get_column_family(database, "legacy");
    check_not_null(legacy);
    check_equal(orm_tidesdb_verify_sync_full(legacy), ORM_TDB_ERR_PRECONDITION);
    check_equal(orm_tidesdb_verify_sync_full(NULL), ORM_TDB_ERR_INVALID_ARGS);
    config.sync_mode = INVALID_SYNC_MODE;
    check_equal(orm_tidesdb_create_column_family(database, "invalid", &config), ORM_TDB_ERR_INVALID_ARGS);
    check_null(orm_tidesdb_get_column_family(database, "invalid"));
  }

  it("persists the explicitly selected full WAL synchronization across reopen") {
    check_equal(orm_tidesdb_verify_sync_full(family), ORM_TDB_SUCCESS);
    close_database();
    open_database();
    family = orm_tidesdb_get_column_family(database, family_name);
    check_not_null(family);
    check_equal(orm_tidesdb_verify_sync_full(family), ORM_TDB_SUCCESS);
  }

  it("rolls back a statement row and index changes while preserving earlier writes") {
    seed();
    begin(0);
    check_equal(orm_tidesdb_txn_savepoint(transactions[0], outer_savepoint), ORM_TDB_SUCCESS);
    put(0, epoch_key, "2");
    check_equal(orm_tidesdb_txn_savepoint(transactions[0], statement_savepoint), ORM_TDB_SUCCESS);
    put(0, row_key, "new");
    erase(0, old_index_key);
    put(0, new_index_key, "1");
    value(0, row_key, "new");
    value(0, old_index_key, NULL);
    check_equal(orm_tidesdb_txn_rollback_to_savepoint(transactions[0], statement_savepoint), ORM_TDB_SUCCESS);
    value(0, epoch_key, "2");
    value(0, row_key, "old");
    value(0, old_index_key, "1");
    value(0, new_index_key, NULL);
    /* Native rollback consumes the target savepoint, unlike MySQL ROLLBACK TO. */
    check_equal(orm_tidesdb_txn_release_savepoint(transactions[0], statement_savepoint), ORM_TDB_ERR_NOT_FOUND);
    check_equal(orm_tidesdb_txn_release_savepoint(transactions[0], outer_savepoint), ORM_TDB_SUCCESS);
    put(0, "data/people/2", "after-rollback");
    commit(0, ORM_TDB_SUCCESS);
    begin(0);
    value(0, epoch_key, "2");
    value(0, row_key, "old");
    value(0, new_index_key, NULL);
    value(0, "data/people/2", "after-rollback");
    rollback(0);
  }

  it("rolls back a successful released statement with its enclosing transaction") {
    seed();
    begin(0);
    check_equal(orm_tidesdb_txn_savepoint(transactions[0], statement_savepoint), ORM_TDB_SUCCESS);
    put(0, row_key, "new");
    erase(0, old_index_key);
    put(0, new_index_key, "1");
    check_equal(orm_tidesdb_txn_release_savepoint(transactions[0], statement_savepoint), ORM_TDB_SUCCESS);
    rollback(0);
    begin(0);
    value(0, row_key, "old");
    value(0, old_index_key, "1");
    value(0, new_index_key, NULL);
    rollback(0);
  }

  it("allows only one competing transaction to claim the same absent unique key") {
    begin(0); begin(1);
    value(0, new_index_key, NULL);
    value(1, new_index_key, NULL);
    put(0, new_index_key, "1"); put(0, row_key, "winner");
    put(1, new_index_key, "2"); put(1, "data/people/2", "loser");
    commit(0, ORM_TDB_SUCCESS);
    commit(1, ORM_TDB_ERR_CONFLICT);
    begin(2);
    value(2, new_index_key, "1"); value(2, row_key, "winner");
    value(2, "data/people/2", NULL);
    rollback(2);
  }

  it("rejects a write compiled against a concurrently changed catalog epoch") {
    seed();
    begin(0); begin(1);
    value(0, epoch_key, "1");
    value(1, epoch_key, "1");
    put(1, epoch_key, "2");
    commit(1, ORM_TDB_SUCCESS);
    put(0, row_key, "stale-schema");
    commit(0, ORM_TDB_ERR_CONFLICT);
    begin(2);
    value(2, epoch_key, "2"); value(2, row_key, "old");
    rollback(2);
  }

  it("keeps point reads and prefix scans on one transaction snapshot") {
    seed();
    begin(0);
    value(0, row_key, "old");
    check_equal(scan(0, "data/"), (size_t)1);
    begin(1);
    put(1, row_key, "new"); put(1, "data/people/2", "inserted");
    commit(1, ORM_TDB_SUCCESS);
    value(0, row_key, "old");
    check_equal(scan(0, "data/"), (size_t)1);
    rollback(0);
    begin(2);
    value(2, row_key, "new");
    check_equal(scan(2, "data/"), (size_t)2);
    rollback(2);
  }

  it("reproduces native unguarded iterator write skew requiring a SQL table guard") {
    /* Characterization of the pinned source, NOT a passing serializability
     * guarantee. The guarded cases below define the SQL admission contract.
     * An upstream fix changes this result and requires reviewing this baseline. */
    begin(0); begin(1);
    check_equal(scan(0, "data/"), (size_t)0);
    check_equal(scan(1, "data/"), (size_t)0);
    put(0, row_key, "first"); put(1, "data/people/2", "second");
    commit(0, ORM_TDB_SUCCESS);
    commit(1, ORM_TDB_SUCCESS);
    begin(2);
    check_equal(scan(2, "data/"), (size_t)2);
    rollback(2);
  }

  it("uses a shared table version to reject write skew after an empty scan") {
    begin(0); put(0, table_version_key, "1"); commit(0, ORM_TDB_SUCCESS);
    guarded_inserts(0);
  }

  it("uses the same table version to protect nonempty predicate scans") {
    begin(0);
    put(0, table_version_key, "1"); put(0, "data/seed", "existing");
    commit(0, ORM_TDB_SUCCESS);
    guarded_inserts(1);
  }

  it("rejects a scan-based write when another writer deletes its input row") {
    seed();
    begin(0); put(0, table_version_key, "1"); commit(0, ORM_TDB_SUCCESS);
    begin(0); begin(1);
    value(0, table_version_key, "1");
    check_equal(scan(0, "data/"), (size_t)1);
    value(1, table_version_key, "1");
    put(1, table_version_key, "2"); erase(1, row_key); erase(1, old_index_key);
    commit(1, ORM_TDB_SUCCESS);
    put(0, table_version_key, "2"); put(0, row_key, "stale-input");
    commit(0, ORM_TDB_ERR_CONFLICT);
    begin(2);
    value(2, row_key, NULL); value(2, old_index_key, NULL);
    rollback(2);
  }

  it("recovers committed catalog row and index changes after exit without close") {
    recover_after_exit("ORM_TDB_STORAGE_CHILD=committed", 1);
  }

  it("discards an uncommitted row index and catalog batch after process exit") {
    recover_after_exit("ORM_TDB_STORAGE_CHILD=pending", 0);
  }

#if defined(ORM_TDB_STORAGE_FAULTS)
  it("keeps the old batch after allocation failure before WAL admission") {
    recover_after_exit("ORM_TDB_STORAGE_CHILD=memory-before", 0);
  }
  it("recovers the entire WAL batch after memtable allocation failure and rollback") {
    recover_after_exit("ORM_TDB_STORAGE_CHILD=memory-after", 1);
  }
  it("keeps the old batch when the WAL syscall fails before writing") {
    recover_after_exit("ORM_TDB_STORAGE_CHILD=fail-before", 0);
  }
  it("does not replay part of a batch after a short WAL frame write") {
    recover_after_exit("ORM_TDB_STORAGE_CHILD=fail-partial", 0);
  }
  it("replays a complete WAL frame despite commit IO error and successful rollback") {
    recover_after_exit("ORM_TDB_STORAGE_CHILD=fail-after", 1);
  }
  it("does not replay a torn batch after exit inside the WAL syscall") {
    recover_after_exit("ORM_TDB_STORAGE_CHILD=exit-partial", 0);
  }
  it("replays a complete batch after exit before memtable publication") {
    recover_after_exit("ORM_TDB_STORAGE_CHILD=exit-after", 1);
  }
  if (orm_tdb_wal_uses_explicit_sync()) {
    it("recovers a complete batch after the WAL sync syscall reports failure") {
      recover_after_exit("ORM_TDB_STORAGE_CHILD=sync-fail-before", 1);
    }
    it("recovers a complete batch when the sync probe reports error after a real barrier") {
      recover_after_exit("ORM_TDB_STORAGE_CHILD=sync-fail-after", 1);
    }
    it("recovers the OS-cached batch after exit before the explicit WAL sync") {
      recover_after_exit("ORM_TDB_STORAGE_CHILD=sync-exit-before", 1);
    }
    it("recovers a synced batch after exit before commit publishes it") {
      recover_after_exit("ORM_TDB_STORAGE_CHILD=sync-exit-after", 1);
    }
  } else {
    it_skip("explicit sync fault cases require the native non-O_DSYNC path") {}
  }
#endif
}

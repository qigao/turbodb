#include "bridge.h"
#include <salts/clock.h>
#include <salts/thread.h>
#include <tinytest.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Four worker-owned transactions share one native database/CF. Fixed slots
 * outlive every worker; results are read only after join. The mutex protects
 * the start gate and completion counters, never database I/O. Failure opens
 * the gate; teardown joins all workers before closing the database. */
enum { WORKERS = 4, ROUNDS = 16, WAIT_MS = 5000, SCAN_LIMIT = WORKERS + 1 };
typedef enum race_kind { UNIQUE_RACE, SCAN_RACE, DISJOINT_WRITES } race_kind;
typedef struct race_worker {
  salts_thread_t thread;
  size_t id;
  int prepared, committed, rolled_back;
  bool attempted;
  size_t scanned;
} race_worker;
static struct {
  salts_mutex_t mutex;
  salts_cond_t changed;
  size_t ready, done, launched;
  bool stop;
  race_kind kind;
  race_worker workers[WORKERS];
  orm_tidesdb_database_t *database;
  orm_tidesdb_column_family_t *family;
  orm_tidesdb_transaction_t *reader;
  char *directory;
} fixture;
static const char family_name[] = "sql-parallel-contract";
static const char guard_key[] = "catalog/people/write-version";
static const char unique_key[] = "unique/shared";
static const char *const row_keys[WORKERS] = {"data/0", "data/1", "data/2", "data/3"};
static const char *const index_keys[WORKERS] = {"index/0", "index/1", "index/2", "index/3"};
static const char *const markers[WORKERS] = {"writer-0", "writer-1", "writer-2", "writer-3"};
static const char initial_guard[] = "initial";

static int put(orm_tidesdb_transaction_t *txn, const char *key, const char *value) {
  return orm_tidesdb_txn_put(txn, fixture.family, (const uint8_t *)key, strlen(key),
      (const uint8_t *)value, strlen(value), 0);
}

static int expect_value(orm_tidesdb_transaction_t *txn, const char *key, const char *expected) {
  uint8_t *bytes = NULL;
  size_t length = 0;
  int status = orm_tidesdb_txn_get(txn, fixture.family,
      (const uint8_t *)key, strlen(key), &bytes, &length);
  if (expected == NULL) {
    if (status == ORM_TDB_ERR_NOT_FOUND) status = ORM_TDB_SUCCESS;
    else if (status == ORM_TDB_SUCCESS) status = ORM_TDB_ERR_CORRUPTION;
  } else if (status == ORM_TDB_SUCCESS &&
      (bytes == NULL || length != strlen(expected) || memcmp(bytes, expected, length) != 0)) {
    status = ORM_TDB_ERR_CORRUPTION;
  }
  orm_tidesdb_free(bytes);
  return status;
}

static int scan_empty(orm_tidesdb_transaction_t *txn, size_t *count) {
  orm_tidesdb_iterator_t *iter = NULL;
  int status = orm_tidesdb_iter_new(txn, fixture.family, &iter);
  if (status != ORM_TDB_SUCCESS) return status;
  const uint8_t prefix[] = "data/";
  const size_t prefix_size = sizeof(prefix) - 1u;
  status = orm_tidesdb_iter_seek(iter, prefix, prefix_size);
  while (status == ORM_TDB_SUCCESS && orm_tidesdb_iter_valid(iter)) {
    uint8_t *key = NULL;
    size_t length = 0;
    status = orm_tidesdb_iter_key(iter, &key, &length);
    if (status != ORM_TDB_SUCCESS || length < prefix_size ||
        memcmp(key, prefix, prefix_size) != 0) break;
    if (++*count >= SCAN_LIMIT) { status = ORM_TDB_ERR_TOO_LARGE; break; }
    status = orm_tidesdb_iter_next(iter);
  }
  orm_tidesdb_iter_free(iter);
  if (status == ORM_TDB_ERR_NOT_FOUND) status = ORM_TDB_SUCCESS;
  return status;
}

/* The monotonic deadline bounds spurious wakes. stop releases preparation
 * waiters, but completion waits still drain every launched worker. */
static bool wait_locked(const size_t *count, size_t expected, bool preparing) {
  const uint64_t started = salts_monotonic_ms();
  while (*count < expected && !(preparing && fixture.stop)) {
    const uint64_t elapsed = salts_monotonic_ms() - started;
    if (elapsed >= WAIT_MS) break;
    const int status = salts_cond_timedwait(&fixture.changed, &fixture.mutex,
        salts_ms_to_ns((uint64_t)WAIT_MS - elapsed));
    if (status != 0 && salts_monotonic_ms() - started < WAIT_MS) break;
  }
  if (*count < expected) {
    fixture.stop = true;
    salts_cond_broadcast(&fixture.changed);
    return false;
  }
  return true;
}

static int prepare_worker(race_worker *worker, orm_tidesdb_transaction_t *txn) {
  int status = ORM_TDB_SUCCESS;
  if (fixture.kind == UNIQUE_RACE)
    status = expect_value(txn, unique_key, NULL);
  else if (fixture.kind == SCAN_RACE) {
    status = expect_value(txn, guard_key, initial_guard);
    if (status == ORM_TDB_SUCCESS) status = scan_empty(txn, &worker->scanned);
    if (status == ORM_TDB_SUCCESS && worker->scanned != 0u) status = ORM_TDB_ERR_CORRUPTION;
  }
  if (status != ORM_TDB_SUCCESS) return status;
  /* Reserve the shared key first: all participants use the same write order. */
  if (fixture.kind != DISJOINT_WRITES)
    status = put(txn, fixture.kind == UNIQUE_RACE ? unique_key : guard_key, markers[worker->id]);
  if (status == ORM_TDB_SUCCESS) status = put(txn, row_keys[worker->id], markers[worker->id]);
  if (status == ORM_TDB_SUCCESS) status = put(txn, index_keys[worker->id], markers[worker->id]);
  return status;
}

static void worker_main(void *arg) {
  race_worker *worker = arg;
  orm_tidesdb_transaction_t *txn = NULL;
  worker->prepared = orm_tidesdb_txn_begin_with_isolation(fixture.database,
      ORM_TDB_ISOLATION_SERIALIZABLE, &txn);
  if (worker->prepared == ORM_TDB_SUCCESS) worker->prepared = prepare_worker(worker, txn);
  salts_mutex_lock(&fixture.mutex);
  if (worker->prepared != ORM_TDB_SUCCESS) fixture.stop = true;
  ++fixture.ready;
  salts_cond_broadcast(&fixture.changed);
  const bool proceed = wait_locked(&fixture.ready, WORKERS, true) && !fixture.stop;
  salts_mutex_unlock(&fixture.mutex);
  if (proceed) {
    worker->attempted = true;
    worker->committed = orm_tidesdb_txn_commit(txn);
  }
  if (txn != NULL && (!worker->attempted || worker->committed != ORM_TDB_SUCCESS))
    worker->rolled_back = orm_tidesdb_txn_rollback(txn);
  orm_tidesdb_txn_free(txn);
  salts_mutex_lock(&fixture.mutex);
  ++fixture.done;
  salts_cond_broadcast(&fixture.changed);
  salts_mutex_unlock(&fixture.mutex);
}

static void join_workers(void) {
  if (fixture.launched == 0) return;
  salts_mutex_lock(&fixture.mutex);
  const bool finished = wait_locked(&fixture.done, fixture.launched, false);
  salts_mutex_unlock(&fixture.mutex);
  if (!finished) {
    (void)fputs("TidesDB race workers did not drain within deadline\n", stderr);
    abort();
  }
  for (size_t i = 0; i < fixture.launched; ++i) {
    if (salts_thread_join(&fixture.workers[i].thread) != 0) abort();
  }
  fixture.launched = 0;
}

static void open_database(void) {
  orm_tidesdb_config_t config = orm_tidesdb_default_config();
  config.db_path = fixture.directory;
  check_equal(orm_tidesdb_open(&config, &fixture.database), ORM_TDB_SUCCESS);
}

static void begin_reader(void) {
  check_equal(orm_tidesdb_txn_begin_with_isolation(fixture.database,
      ORM_TDB_ISOLATION_SERIALIZABLE, &fixture.reader), ORM_TDB_SUCCESS);
}

static void reset_rows(void) {
  begin_reader();
  for (size_t i = 0; i < WORKERS; ++i) {
    check_equal(orm_tidesdb_txn_delete(fixture.reader, fixture.family,
        (const uint8_t *)row_keys[i], strlen(row_keys[i])), ORM_TDB_SUCCESS);
    check_equal(orm_tidesdb_txn_delete(fixture.reader, fixture.family,
        (const uint8_t *)index_keys[i], strlen(index_keys[i])), ORM_TDB_SUCCESS);
  }
  check_equal(orm_tidesdb_txn_delete(fixture.reader, fixture.family,
      (const uint8_t *)unique_key, strlen(unique_key)), ORM_TDB_SUCCESS);
  check_equal(put(fixture.reader, guard_key, initial_guard), ORM_TDB_SUCCESS);
  check_equal(orm_tidesdb_txn_commit(fixture.reader), ORM_TDB_SUCCESS);
  orm_tidesdb_txn_free(fixture.reader); fixture.reader = NULL;
}

static void verify_old_snapshot(void) {
  check_equal(expect_value(fixture.reader, guard_key, initial_guard), ORM_TDB_SUCCESS);
  check_equal(expect_value(fixture.reader, unique_key, NULL), ORM_TDB_SUCCESS);
  for (size_t i = 0; i < WORKERS; ++i) {
    check_equal(expect_value(fixture.reader, row_keys[i], NULL), ORM_TDB_SUCCESS);
    check_equal(expect_value(fixture.reader, index_keys[i], NULL), ORM_TDB_SUCCESS);
  }
}

static void verify_batch(void) {
  size_t successes = 0, winner = 0;
  begin_reader();
  for (size_t i = 0; i < WORKERS; ++i) {
    const race_worker *worker = &fixture.workers[i];
    info("worker=%zu prepared=%d commit=%d rollback=%d", i,
        worker->prepared, worker->committed, worker->rolled_back);
    check_equal(worker->prepared, ORM_TDB_SUCCESS);
    check_true(worker->attempted);
    check_true(worker->committed == ORM_TDB_SUCCESS || worker->committed == ORM_TDB_ERR_CONFLICT);
    const bool won = worker->committed == ORM_TDB_SUCCESS;
    if (won) { ++successes; winner = i; }
    else check_equal(worker->rolled_back, ORM_TDB_SUCCESS);
    check_equal(expect_value(fixture.reader, row_keys[i], won ? markers[i] : NULL), ORM_TDB_SUCCESS);
    check_equal(expect_value(fixture.reader, index_keys[i], won ? markers[i] : NULL), ORM_TDB_SUCCESS);
  }
  check_equal(successes, fixture.kind == DISJOINT_WRITES ? (size_t)WORKERS : (size_t)1);
  check_equal(expect_value(fixture.reader, guard_key,
      fixture.kind == SCAN_RACE ? markers[winner] : initial_guard), ORM_TDB_SUCCESS);
  check_equal(expect_value(fixture.reader, unique_key,
      fixture.kind == UNIQUE_RACE ? markers[winner] : NULL), ORM_TDB_SUCCESS);
  check_equal(orm_tidesdb_txn_rollback(fixture.reader), ORM_TDB_SUCCESS);
  orm_tidesdb_txn_free(fixture.reader); fixture.reader = NULL;
}

static void run_race(race_kind kind) {
  fixture.kind = kind;
  for (size_t round = 0; round < ROUNDS; ++round) {
    info("round=%zu", round);
    reset_rows();
    /* This read-only transaction remains owned by the test thread while the
     * writers commit; it must retain the complete pre-commit snapshot. */
    begin_reader();
    verify_old_snapshot();
    fixture.ready = 0; fixture.done = 0; fixture.stop = false;
    memset(fixture.workers, 0, sizeof(fixture.workers));
    for (size_t i = 0; i < WORKERS; ++i) {
      fixture.workers[i].id = i;
      check_equal(salts_thread_create(&fixture.workers[i].thread,
          worker_main, &fixture.workers[i]), 0);
      ++fixture.launched;
    }
    join_workers();
    check_false(fixture.stop);
    verify_old_snapshot();
    check_equal(orm_tidesdb_txn_rollback(fixture.reader), ORM_TDB_SUCCESS);
    orm_tidesdb_txn_free(fixture.reader); fixture.reader = NULL;
    verify_batch();
  }
  check_equal(orm_tidesdb_close(fixture.database), ORM_TDB_SUCCESS);
  fixture.database = NULL; fixture.family = NULL;
  open_database();
  fixture.family = orm_tidesdb_get_column_family(fixture.database, family_name);
  check_not_null(fixture.family);
  check_equal(orm_tidesdb_verify_sync_full(fixture.family), ORM_TDB_SUCCESS);
  verify_batch();
}

spec("TidesDB parallel storage contracts") {
  before_each() {
    memset(&fixture, 0, sizeof(fixture));
    salts_mutex_init(&fixture.mutex); salts_cond_init(&fixture.changed);
    check_not_null(fixture.mutex); check_not_null(fixture.changed);
    fixture.directory = tt_make_temp_dir("orm-tidesdb-race");
    check_not_null(fixture.directory);
    open_database();
    orm_tidesdb_column_family_config_t config = orm_tidesdb_default_column_family_config();
    config.sync_mode = ORM_TDB_SYNC_FULL;
    check_equal(orm_tidesdb_create_column_family(fixture.database, family_name, &config), ORM_TDB_SUCCESS);
    fixture.family = orm_tidesdb_get_column_family(fixture.database, family_name);
    check_not_null(fixture.family);
  }
  after_each() {
    if (fixture.launched != 0) {
      salts_mutex_lock(&fixture.mutex);
      fixture.stop = true;
      salts_cond_broadcast(&fixture.changed);
      salts_mutex_unlock(&fixture.mutex);
      join_workers();
    }
    if (fixture.reader != NULL) {
      check_equal(orm_tidesdb_txn_rollback(fixture.reader), ORM_TDB_SUCCESS);
      orm_tidesdb_txn_free(fixture.reader);
    }
    if (fixture.database != NULL) check_equal(orm_tidesdb_close(fixture.database), ORM_TDB_SUCCESS);
    salts_cond_destroy(&fixture.changed); salts_mutex_destroy(&fixture.mutex);
    if (fixture.directory != NULL) {
      check_equal(tt_remove_tree(fixture.directory), 0);
      free(fixture.directory);
    }
  }
  after_all() { orm_tidesdb_module_cleanup(); }
  it("allows one unique-key claimant and no losing row or index fragments") { run_race(UNIQUE_RACE); }
  it("serializes empty-scan writers through the table version key") { run_race(SCAN_RACE); }
  it("commits all disjoint row and index batches without false conflicts") { run_race(DISJOINT_WRITES); }
}

#include "orm_internal.h"
#include <tinytest.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Native SQLite calls run on the fixture's creating thread. The test thread
 * owns separately retained handles and probes only owner admission. No CFlow
 * Publisher is dispatched concurrently; cancellation stays on its own thread. */
enum { RACE_TIMEOUT_MS = 5000 };
typedef enum race_operation {
  RACE_COMMIT, RACE_ROLLBACK, RACE_SAVEPOINT, RACE_DESTROY,
  RACE_QUERY_WRITE, RACE_QUERY_CLOSE
} race_operation;

typedef struct race_fixture {
  salts_mutex_t mutex;
  salts_cond_t condition;
  salts_thread_t worker;
  race_operation operation;
  bool ready, start, entered, permit_native, returned, permit_return;
  bool completed, finish, exited, stop, timed_out;
  bool release_worker_reference, worker_has_transaction, executing;
  orm_status_t setup_status, operation_status;
  orm_connection_t *connection;
  orm_query_t *query;
  orm_transaction_t *transaction;
  orm_transaction_t *worker_transaction;
  cflow_publisher publisher;
  orm_transaction_backend_ops observed;
  const orm_transaction_backend_ops *native;
  unsigned native_calls, destroy_calls;
} race_fixture;

static race_fixture fixture;
static const char race_sql[] = "insert into owner_race_data values (?1)";
static const char race_savepoint[] = "owner_race_point";

/* Predicates and counters share this lock. Spurious wakes do not extend the
 * absolute budget. Timeout opens every gate, so ordinary assertion teardown
 * can still join the fixture instead of leaving a worker holding stack state. */
static bool wait_locked(const bool *predicate) {
  const uint64_t started = salts_monotonic_ms();
  while (!*predicate && !fixture.stop) {
    const uint64_t elapsed = salts_monotonic_ms() - started;
    if (elapsed >= RACE_TIMEOUT_MS) {
      fixture.timed_out = true;
      fixture.stop = true;
      salts_cond_broadcast(&fixture.condition);
      break;
    }
    const int status = salts_cond_timedwait(&fixture.condition, &fixture.mutex,
        salts_ms_to_ns((uint64_t)RACE_TIMEOUT_MS - elapsed));
    /* Salts reports timeout as a negative result. Recheck the predicate and
     * absolute clock; an early/spurious wake must not satisfy the barrier. */
    if (status != 0 && salts_monotonic_ms() - started < RACE_TIMEOUT_MS) {
      fixture.timed_out = true;
      fixture.stop = true;
      salts_cond_broadcast(&fixture.condition);
      break;
    }
  }
  return *predicate;
}

static bool await(const bool *predicate) {
  salts_mutex_lock(&fixture.mutex);
  const bool reached = wait_locked(predicate);
  salts_mutex_unlock(&fixture.mutex);
  return reached;
}

static void signal_flag(bool *predicate) {
  salts_mutex_lock(&fixture.mutex);
  *predicate = true;
  salts_cond_broadcast(&fixture.condition);
  salts_mutex_unlock(&fixture.mutex);
}

static void pause_callback(bool *phase, const bool *permission) {
  salts_mutex_lock(&fixture.mutex);
  *phase = true;
  salts_cond_broadcast(&fixture.condition);
  (void)wait_locked(permission);
  salts_mutex_unlock(&fixture.mutex);
}

static void relinquish_worker_reference(void) {
  if (fixture.release_worker_reference && fixture.worker_has_transaction) {
    fixture.worker_has_transaction = false;
    orm_transaction_release(fixture.worker_transaction);
  }
}

static void before_native(void) {
  relinquish_worker_reference();
  pause_callback(&fixture.entered, &fixture.permit_native);
  salts_mutex_lock(&fixture.mutex);
  ++fixture.native_calls;
  salts_mutex_unlock(&fixture.mutex);
}

static void after_native(void) {
  pause_callback(&fixture.returned, &fixture.permit_return);
}

static orm_status_t gated_commit(void *context, orm_error_t *error) {
  before_native();
  const orm_status_t status = fixture.native->commit(context, error);
  after_native();
  return status;
}

static orm_status_t gated_rollback(void *context, orm_error_t *error) {
  if (fixture.executing && fixture.operation == RACE_ROLLBACK) {
    before_native();
    const orm_status_t status = fixture.native->rollback(context, error);
    after_native();
    return status;
  }
  return fixture.native->rollback(context, error);
}

static orm_status_t gated_savepoint(void *context, vstr name, orm_error_t *error) {
  before_native();
  const orm_status_t status = fixture.native->savepoint(context, name, error);
  after_native();
  return status;
}

static void observed_destroy(void *context) {
  salts_mutex_lock(&fixture.mutex);
  ++fixture.destroy_calls;
  salts_mutex_unlock(&fixture.mutex);
  if (fixture.executing && fixture.operation == RACE_DESTROY) {
    before_native();
    fixture.native->destroy(context);
    after_native();
  } else {
    fixture.native->destroy(context);
  }
}

static unsigned read_counter(const unsigned *counter) {
  salts_mutex_lock(&fixture.mutex);
  const unsigned value = *counter;
  salts_mutex_unlock(&fixture.mutex);
  return value;
}

static uint32_t parent_dependents(void) {
  salts_mutex_lock(&fixture.connection->owner.mutex);
  const uint32_t value = fixture.connection->owner.dependents;
  salts_mutex_unlock(&fixture.connection->owner.mutex);
  return value;
}

static orm_status_t setup_native(orm_connection_t **connection,
                                  orm_query_t **query,
                                  orm_transaction_t **transaction) {
  orm_config_t config;
  orm_error_t error;
  orm_result_t *result = NULL;
  orm_config(&config);
  orm_error_init(&error);
  const orm_option_t filename = {orm_view("filename"), orm_view(":memory:")};
  config.driver = orm_view("sqlite");
  config.options = &filename;
  config.option_count = 1u;
  orm_status_t status = orm_connect(&config, connection, &error);
  if (status != ORM_STATUS_OK) return status;
  status = orm_raw(*connection, orm_view("create table owner_race_data(id integer)"),
                   query, &error);
  if (status == ORM_STATUS_OK) status = orm_query_execute(*query, &result, &error);
  orm_result_destroy(result);
  orm_query_destroy(*query);
  *query = NULL;
  if (status != ORM_STATUS_OK) return status;
  status = orm_transaction_begin(*connection, ORM_ISOLATION_SERIALIZABLE,
                                  transaction, &error);
  if (status == ORM_STATUS_OK) status = orm_raw(*connection, orm_view(race_sql), query, &error);
  if (status == ORM_STATUS_OK) status = orm_query_bind(*query, orm_i64(1), &error);
  if (status == ORM_STATUS_OK && fixture.operation == RACE_DESTROY)
    status = orm_transaction_commit(*transaction, &error);
  return status;
}

static void worker_main(void *context) {
  (void)context;
  orm_connection_t *connection = NULL;
  orm_query_t *query = NULL;
  orm_transaction_t *transaction = NULL;
  orm_error_t error;
  orm_error_init(&error);
  fixture.setup_status = setup_native(&connection, &query, &transaction);
  fixture.worker_has_transaction = transaction != NULL;
  if (fixture.setup_status == ORM_STATUS_OK) {
    fixture.worker_transaction = transaction;
    fixture.native = transaction->backend.ops;
    fixture.observed = *fixture.native;
    fixture.observed.commit = gated_commit;
    fixture.observed.rollback = gated_rollback;
    fixture.observed.savepoint = gated_savepoint;
    fixture.observed.destroy = observed_destroy;
    transaction->backend.ops = &fixture.observed;
    orm_connection_retain(connection);
    orm_query_retain(query);
    orm_transaction_retain(transaction);
    /* Publish a different owned reference, never the worker's borrowed slot. */
    fixture.connection = connection;
    fixture.query = query;
    fixture.transaction = transaction;
  }
  signal_flag(&fixture.ready);
  const bool started = await(&fixture.start);
  if (started && fixture.setup_status == ORM_STATUS_OK) {
    fixture.executing = true;
    switch (fixture.operation) {
      case RACE_COMMIT: fixture.operation_status = orm_transaction_commit(transaction, &error); break;
      case RACE_ROLLBACK: fixture.operation_status = orm_transaction_rollback(transaction, &error); break;
      case RACE_SAVEPOINT: fixture.operation_status = orm_transaction_savepoint(transaction, orm_view(race_savepoint), &error); break;
      case RACE_DESTROY: fixture.operation_status = orm_transaction_close(transaction, &error); break;
      case RACE_QUERY_WRITE: fixture.operation_status = orm_query_bind(query, orm_i64(2), &error); break;
      case RACE_QUERY_CLOSE: fixture.operation_status = orm_query_close(query, &error); break;
    }
    fixture.executing = false;
  }
  signal_flag(&fixture.completed);
  (void)await(&fixture.finish);
  orm_query_destroy(query);
  if (fixture.worker_has_transaction) orm_transaction_release(transaction);
  orm_disconnect(connection);
  signal_flag(&fixture.exited);
}

static void drop_publisher(void) {
  if (!cflow_publisher_valid(&fixture.publisher)) return;
  cflow_publisher released = fixture.publisher;
  memset(&fixture.publisher, 0, sizeof(fixture.publisher));
  cflow_publisher_destroy(&released);
}

static void drop_transaction(void) {
  orm_transaction_t *released = fixture.transaction;
  fixture.transaction = NULL;
  orm_transaction_release(released);
}

static void launch(race_operation operation, bool release_worker_reference) {
  fixture.operation = operation;
  fixture.release_worker_reference = release_worker_reference;
  check_equal(salts_thread_create(&fixture.worker, worker_main, NULL), 0);
  check_true(await(&fixture.ready));
  check_equal(fixture.setup_status, ORM_STATUS_OK);
}

static void start_native(race_operation operation, bool release_worker_reference) {
  launch(operation, release_worker_reference);
  signal_flag(&fixture.start);
  check_true(await(&fixture.entered));
}

static void require_rejected_admission(void) {
  orm_error_t error;
  orm_error_init(&error);
  check_equal(orm_transaction_close(fixture.transaction, &error), ORM_STATUS_BUSY);
  check_equal(orm_connection_close(fixture.connection, &error), ORM_STATUS_BUSY);
  check_equal(orm_query_open_command_flow_in_transaction(
      fixture.query, fixture.transaction, &fixture.publisher, &error), ORM_STATUS_BUSY);
  check_false(cflow_publisher_valid(&fixture.publisher));
}

static void check_control(race_operation operation) {
  start_native(operation, false);
  require_rejected_admission();
  check_equal(read_counter(&fixture.native_calls), 0u);
  signal_flag(&fixture.permit_native);
  check_true(await(&fixture.returned));
  check_equal(read_counter(&fixture.native_calls), 1u);
  require_rejected_admission();
  signal_flag(&fixture.permit_return);
  check_true(await(&fixture.completed));
  check_equal(fixture.operation_status, ORM_STATUS_OK);
  check_equal(read_counter(&fixture.destroy_calls), 0u);
}

spec("native owner cross-thread admission and callback completion") {
  (void)ttest_config__;
  before_each() {
    memset(&fixture, 0, sizeof(fixture));
    salts_mutex_init(&fixture.mutex);
    salts_cond_init(&fixture.condition);
    check_not_null(fixture.mutex);
    check_not_null(fixture.condition);
  }
  after_each() {
    if (fixture.worker != NULL) {
      /* Release main-thread objects before permitting ordinary worker cleanup.
       * If an assertion failed inside a callback window, its native operation
       * hold must preserve the worker. stop releases any remaining gates. */
      drop_publisher();
      orm_query_release(fixture.query); fixture.query = NULL;
      drop_transaction();
      orm_connection_release(fixture.connection); fixture.connection = NULL;
      signal_flag(&fixture.stop);
      if (!await(&fixture.exited)) {
        /* stop wakes the worker too; wait for its explicit exit with a fresh
         * bounded join predicate, rather than detach a live fixture. */
        salts_mutex_lock(&fixture.mutex);
        const uint64_t deadline = salts_monotonic_ms() + RACE_TIMEOUT_MS;
        while (!fixture.exited && salts_monotonic_ms() < deadline) {
          const uint64_t now = salts_monotonic_ms();
          if (now >= deadline) break;
          (void)salts_cond_timedwait(&fixture.condition, &fixture.mutex,
                                    salts_ms_to_ns(deadline - now));
        }
        const bool exited = fixture.exited;
        salts_mutex_unlock(&fixture.mutex);
        if (!exited) {
          (void)fprintf(stderr, "native race fixture failed to stop within its deadline\n");
          abort();
        }
      }
      check_equal(salts_thread_join(&fixture.worker), 0);
      check_false(fixture.timed_out);
    }
    salts_cond_destroy(&fixture.condition);
    salts_mutex_destroy(&fixture.mutex);
  }

  it("rejects close and new work until the native commit callback returns") {
    check_control(RACE_COMMIT);
    check_equal(fixture.transaction->state, ORM_TRANSACTION_COMMITTED);
  }
  it("rejects close and new work until the native rollback callback returns") {
    check_control(RACE_ROLLBACK);
    check_equal(fixture.transaction->state, ORM_TRANSACTION_ROLLED_BACK);
  }
  it("guards a savepoint callback on the same native admission boundary") {
    check_control(RACE_SAVEPOINT);
    check_equal(fixture.transaction->state, ORM_TRANSACTION_ACTIVE);
  }
  it("keeps the final transaction reference alive beyond native commit return") {
    start_native(RACE_COMMIT, true);
    require_rejected_admission();
    drop_transaction();
    check_equal(read_counter(&fixture.destroy_calls), 0u);
    check_equal(parent_dependents(), (uint32_t)2u);
    signal_flag(&fixture.permit_native);
    check_true(await(&fixture.returned));
    check_equal(read_counter(&fixture.destroy_calls), 0u);
    check_equal(parent_dependents(), (uint32_t)2u);
    signal_flag(&fixture.permit_return);
    check_true(await(&fixture.completed));
    check_equal(fixture.operation_status, ORM_STATUS_OK);
    check_equal(read_counter(&fixture.destroy_calls), 1u);
    check_equal(parent_dependents(), (uint32_t)1u);
  }
  it("keeps the closing handle and parent through the native destroy callback") {
    orm_error_t error;
    orm_error_init(&error);
    start_native(RACE_DESTROY, true);
    check_equal(orm_transaction_close(fixture.transaction, &error), ORM_STATUS_BUSY);
    drop_transaction();
    check_equal(parent_dependents(), (uint32_t)2u);
    signal_flag(&fixture.permit_native);
    check_true(await(&fixture.returned));
    check_equal(parent_dependents(), (uint32_t)2u);
    signal_flag(&fixture.permit_return);
    check_true(await(&fixture.completed));
    check_equal(fixture.operation_status, ORM_STATUS_OK);
    check_equal(read_counter(&fixture.destroy_calls), 1u);
    check_equal(parent_dependents(), (uint32_t)1u);
  }
  it("does not admit commit merely because an existing Publisher was cancelled") {
    orm_error_t error;
    orm_error_init(&error);
    launch(RACE_COMMIT, false);
    check_equal(orm_query_open_command_flow_in_transaction(fixture.query,
        fixture.transaction, &fixture.publisher, &error), ORM_STATUS_OK);
    cflow_publisher_cancel(&fixture.publisher);
    signal_flag(&fixture.start);
    check_true(await(&fixture.completed));
    check_equal(fixture.operation_status, ORM_STATUS_BUSY);
    check_equal(read_counter(&fixture.native_calls), 0u);
    check_equal(orm_transaction_close(fixture.transaction, &error), ORM_STATUS_BUSY);
    drop_publisher();
    check_equal(orm_transaction_close(fixture.transaction, &error), ORM_STATUS_INVALID_STATE);
  }
  it("serializes a query mutation racing with Publisher admission") {
    orm_error_t error;
    orm_error_init(&error);
    launch(RACE_QUERY_WRITE, false);
    signal_flag(&fixture.start);
    check_equal(orm_query_open_command_flow(fixture.query, &fixture.publisher, &error), ORM_STATUS_OK);
    check_true(await(&fixture.completed));
    check_true(fixture.operation_status == ORM_STATUS_OK || fixture.operation_status == ORM_STATUS_BUSY);
    const size_t expected = fixture.operation_status == ORM_STATUS_OK ? 2u : 1u;
    check_equal(vec_size(&fixture.query->plan.raw_parameters), expected);
    check_equal(orm_query_bind(fixture.query, orm_i64(3), &error), ORM_STATUS_BUSY);
    check_equal(vec_size(&fixture.query->plan.raw_parameters), expected);
  }
  it("permits only the two legal outcomes of query close racing with admission") {
    orm_error_t error;
    orm_error_init(&error);
    launch(RACE_QUERY_CLOSE, false);
    signal_flag(&fixture.start);
    const orm_status_t opened = orm_query_open_command_flow(fixture.query, &fixture.publisher, &error);
    check_true(await(&fixture.completed));
    check_true((opened == ORM_STATUS_OK && fixture.operation_status == ORM_STATUS_BUSY) ||
               (opened == ORM_STATUS_INVALID_STATE && fixture.operation_status == ORM_STATUS_OK));
    check_equal(cflow_publisher_valid(&fixture.publisher), opened == ORM_STATUS_OK);
  }
  it("rejects Publisher admission when another thread closed the query first") {
    orm_error_t error;
    orm_error_init(&error);
    launch(RACE_QUERY_CLOSE, false);
    signal_flag(&fixture.start);
    check_true(await(&fixture.completed));
    check_equal(fixture.operation_status, ORM_STATUS_OK);
    check_equal(orm_query_open_command_flow(fixture.query, &fixture.publisher, &error), ORM_STATUS_INVALID_STATE);
    check_false(cflow_publisher_valid(&fixture.publisher));
  }
  it("leaves the query unchanged when Publisher admission wins before close") {
    orm_error_t error;
    orm_error_init(&error);
    launch(RACE_QUERY_CLOSE, false);
    check_equal(orm_query_open_command_flow(fixture.query, &fixture.publisher, &error), ORM_STATUS_OK);
    signal_flag(&fixture.start);
    check_true(await(&fixture.completed));
    check_equal(fixture.operation_status, ORM_STATUS_BUSY);
    check_equal(vec_size(&fixture.query->plan.raw_parameters), (size_t)1u);
    check_equal(orm_query_bind(fixture.query, orm_i64(3), &error), ORM_STATUS_BUSY);
  }
}

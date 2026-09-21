/* Test-first connection-lane regression. Shares orm_owner_checked's runner. */
#define TINYTEST_NO_MAIN
#include "orm_internal.h"
#include <tinytest.h>
#include <string.h>

/* Delegate all database work to the existing SQLite backend. The observer
 * reenters the host after the native effect, but before returning the callback.
 * It never supplies an owner reference, a lock, or a replacement SQL result. */
enum { CONTROL_CONNECTION_COUNT = 2u };
typedef enum control_stage {
  CONTROL_NONE, CONTROL_BEGIN, CONTROL_COMMIT, CONTROL_ROLLBACK,
  CONTROL_SAVEPOINT, CONTROL_ROLLBACK_TO, CONTROL_RELEASE
} control_stage;
typedef struct control_connection {
  orm_connection_t *handle;
  orm_backend_ops native_ops;
  orm_backend_ops observed_ops;
  unsigned begin_calls;
} control_connection;

static control_connection control_connections[CONTROL_CONNECTION_COUNT];
static orm_transaction_t *control_outer;
static orm_transaction_t *control_nested;
static orm_transaction_backend_ops control_native_ops;
static orm_transaction_backend_ops control_observed_ops;
static control_stage control_armed;
static unsigned control_target;
static unsigned control_probe_calls;
static unsigned control_nested_native_calls;
static int control_nested_was_empty;
static orm_status_t control_nested_status;
static orm_status_t control_nested_error_status;

static void control_probe(control_stage stage) {
  if (control_armed != stage) return;
  control_armed = CONTROL_NONE;
  ++control_probe_calls;
  control_connection *target = &control_connections[control_target];
  const unsigned before = target->begin_calls;
  orm_error_t error;
  orm_error_init(&error);
  control_nested_status = orm_transaction_begin(
      target->handle, ORM_ISOLATION_SERIALIZABLE, &control_nested, &error);
  control_nested_native_calls = target->begin_calls - before;
  control_nested_was_empty = control_nested == NULL;
  control_nested_error_status = error.status;
  /* An unexpectedly admitted handle stays in the fixture until the outer
   * callback has returned. No TinyTest longjmp may escape a native callback. */
}

static orm_status_t control_begin(void *context, orm_isolation_t isolation,
    orm_transaction_backend *out, orm_error_t *error) {
  for (unsigned index = 0u; index < CONTROL_CONNECTION_COUNT; ++index) {
    control_connection *connection = &control_connections[index];
    if (connection->handle != NULL &&
        connection->handle->backend.context == context) {
      ++connection->begin_calls;
      const orm_status_t status = connection->native_ops.begin_transaction(
          context, isolation, out, error);
      if (status == ORM_STATUS_OK && index == 0u) control_probe(CONTROL_BEGIN);
      return status;
    }
  }
  memset(out, 0, sizeof(*out));
  orm_error_set(error, ORM_STATUS_INTERNAL_ERROR, "unrecognized test connection");
  return ORM_STATUS_INTERNAL_ERROR;
}
static orm_status_t control_commit(void *context, orm_error_t *error) {
  const orm_status_t status = control_native_ops.commit(context, error);
  if (status == ORM_STATUS_OK) control_probe(CONTROL_COMMIT);
  return status;
}
static orm_status_t control_rollback(void *context, orm_error_t *error) {
  const orm_status_t status = control_native_ops.rollback(context, error);
  if (status == ORM_STATUS_OK) control_probe(CONTROL_ROLLBACK);
  return status;
}
static orm_status_t control_savepoint(void *context, vstr name, orm_error_t *error) {
  const orm_status_t status = control_native_ops.savepoint(context, name, error);
  if (status == ORM_STATUS_OK) control_probe(CONTROL_SAVEPOINT);
  return status;
}
static orm_status_t control_rollback_to(void *context, vstr name, orm_error_t *error) {
  const orm_status_t status = control_native_ops.rollback_to_savepoint(context, name, error);
  if (status == ORM_STATUS_OK) control_probe(CONTROL_ROLLBACK_TO);
  return status;
}
static orm_status_t control_release(void *context, vstr name, orm_error_t *error) {
  const orm_status_t status = control_native_ops.release_savepoint(context, name, error);
  if (status == ORM_STATUS_OK) control_probe(CONTROL_RELEASE);
  return status;
}
static void control_drop(orm_transaction_t **handle) {
  orm_transaction_t *owned = *handle;
  *handle = NULL;
  orm_transaction_release(owned);
}

static void control_scenario(control_stage stage, unsigned target) {
  const vstr savepoint = orm_view("control_probe");
  orm_error_t error;
  orm_status_t outer_status = ORM_STATUS_OK;
  orm_error_init(&error);
  control_target = target;
  if (stage == CONTROL_BEGIN) control_armed = stage;
  check_equal(orm_transaction_begin(control_connections[0].handle,
      ORM_ISOLATION_SERIALIZABLE, &control_outer, &error), ORM_STATUS_OK);
  check_not_null(control_outer);

  /* Only the outer transaction is observed; a nested transaction, even on the
   * same connection, keeps its real native function table and cleanup. */
  control_native_ops = *control_outer->backend.ops;
  control_observed_ops = control_native_ops;
  control_observed_ops.commit = control_commit;
  control_observed_ops.rollback = control_rollback;
  control_observed_ops.savepoint = control_savepoint;
  control_observed_ops.rollback_to_savepoint = control_rollback_to;
  control_observed_ops.release_savepoint = control_release;
  control_outer->backend.ops = &control_observed_ops;
  if (stage == CONTROL_ROLLBACK_TO || stage == CONTROL_RELEASE)
    check_equal(orm_transaction_savepoint(control_outer, savepoint, &error), ORM_STATUS_OK);
  if (stage != CONTROL_BEGIN) control_armed = stage;

  switch (stage) {
    case CONTROL_COMMIT:
      outer_status = orm_transaction_commit(control_outer, &error); break;
    case CONTROL_ROLLBACK:
      outer_status = orm_transaction_rollback(control_outer, &error); break;
    case CONTROL_SAVEPOINT:
      outer_status = orm_transaction_savepoint(control_outer, savepoint, &error); break;
    case CONTROL_ROLLBACK_TO:
      outer_status = orm_transaction_rollback_to_savepoint(control_outer, savepoint, &error); break;
    case CONTROL_RELEASE:
      outer_status = orm_transaction_release_savepoint(control_outer, savepoint, &error); break;
    default: break;
  }
  control_armed = CONTROL_NONE;
  /* Clean even the RED implementation's unexpected successful admission before
   * any target assertion fails; both connection references remain valid. */
  if (control_nested != NULL) {
    const orm_status_t status = orm_transaction_rollback(control_nested, &error);
    control_drop(&control_nested);
    check_equal(status, ORM_STATUS_OK);
  }
  check_equal(outer_status, ORM_STATUS_OK);
  check_equal(control_probe_calls, 1u);
  check_equal(control_nested_native_calls, target == 0u ? 0u : 1u);
  check_equal(control_nested_status, target == 0u ? ORM_STATUS_BUSY : ORM_STATUS_OK);
  check_equal(control_nested_error_status, control_nested_status);
  check_equal(control_nested_was_empty, target == 0u);
  check_equal(control_connections[0].handle->failure, ORM_STATUS_OK);
  check_equal(control_connections[1].handle->failure, ORM_STATUS_OK);

  if (control_outer->state == ORM_TRANSACTION_ACTIVE)
    check_equal(orm_transaction_rollback(control_outer, &error), ORM_STATUS_OK);
  check_equal(orm_transaction_close(control_outer, &error), ORM_STATUS_OK);
  control_drop(&control_outer);
  /* The reservation must end at callback completion, not poison the connection
   * or permanently forbid starting its next transaction. */
  check_equal(orm_transaction_begin(control_connections[0].handle,
      ORM_ISOLATION_SERIALIZABLE, &control_outer, &error), ORM_STATUS_OK);
  check_equal(orm_transaction_rollback(control_outer, &error), ORM_STATUS_OK);
  control_drop(&control_outer);
  for (unsigned index = 0u; index < CONTROL_CONNECTION_COUNT; ++index)
    check_equal(orm_connection_close(control_connections[index].handle, &error), ORM_STATUS_OK);
}

spec("connection control callback admission") {
  (void)ttest_config__;
  before_each() {
    memset(control_connections, 0, sizeof(control_connections));
    control_outer = control_nested = NULL;
    control_armed = CONTROL_NONE;
    control_target = control_probe_calls = control_nested_native_calls = 0u;
    control_nested_was_empty = 0;
    control_nested_status = control_nested_error_status = ORM_STATUS_INTERNAL_ERROR;
    for (unsigned index = 0u; index < CONTROL_CONNECTION_COUNT; ++index) {
      control_connection *connection = &control_connections[index];
      orm_config_t config;
      orm_error_t error;
      const orm_option_t filename = {orm_view("filename"), orm_view(":memory:")};
      orm_config(&config); orm_error_init(&error);
      config.driver = orm_view("sqlite");
      config.options = &filename; config.option_count = 1u;
      check_equal(orm_connect(&config, &connection->handle, &error), ORM_STATUS_OK);
      connection->native_ops = *connection->handle->backend.ops;
      connection->observed_ops = connection->native_ops;
      connection->observed_ops.begin_transaction = control_begin;
      connection->handle->backend.ops = &connection->observed_ops;
    }
  }
  after_each() {
    control_armed = CONTROL_NONE;
    control_drop(&control_nested);
    control_drop(&control_outer);
    for (unsigned index = 0u; index < CONTROL_CONNECTION_COUNT; ++index) {
      orm_connection_t *owned = control_connections[index].handle;
      control_connections[index].handle = NULL;
      orm_connection_release(owned);
    }
  }
  it("blocks same-connection BEGIN while BEGIN has not returned") {
    control_scenario(CONTROL_BEGIN, 0u);
  }
  it("blocks same-connection BEGIN while COMMIT has not returned") {
    control_scenario(CONTROL_COMMIT, 0u);
  }
  it("blocks same-connection BEGIN while ROLLBACK has not returned") {
    control_scenario(CONTROL_ROLLBACK, 0u);
  }
  it("blocks same-connection BEGIN while SAVEPOINT has not returned") {
    control_scenario(CONTROL_SAVEPOINT, 0u);
  }
  it("blocks same-connection BEGIN while ROLLBACK TO has not returned") {
    control_scenario(CONTROL_ROLLBACK_TO, 0u);
  }
  it("blocks same-connection BEGIN while RELEASE SAVEPOINT has not returned") {
    control_scenario(CONTROL_RELEASE, 0u);
  }
  it("allows another connection BEGIN during BEGIN") {
    control_scenario(CONTROL_BEGIN, 1u);
  }
  it("allows another connection BEGIN during COMMIT") {
    control_scenario(CONTROL_COMMIT, 1u);
  }
  it("allows another connection BEGIN during ROLLBACK") {
    control_scenario(CONTROL_ROLLBACK, 1u);
  }
  it("allows another connection BEGIN during SAVEPOINT") {
    control_scenario(CONTROL_SAVEPOINT, 1u);
  }
  it("allows another connection BEGIN during ROLLBACK TO") {
    control_scenario(CONTROL_ROLLBACK_TO, 1u);
  }
  it("allows another connection BEGIN during RELEASE SAVEPOINT") {
    control_scenario(CONTROL_RELEASE, 1u);
  }
}

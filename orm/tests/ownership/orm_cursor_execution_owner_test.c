/* Test-first continuation of #28. Observers delegate to the real SQLite core;
 * they never acquire owner holds or provide exclusion/cleanup for production. */
#define TINYTEST_NO_MAIN
#include "orm_internal.h"
#include <cmeta/struct.h>
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

enum { EXEC_DATABASES = 2u, EXEC_CURSORS = 4u };
typedef enum exec_stage { EXEC_NONE, EXEC_NEXT, EXEC_READER, EXEC_CANCEL,
                          EXEC_DESTROY, EXEC_COMMAND, EXEC_BEGIN,
                          EXEC_TX_ROLLBACK, EXEC_TX_DESTROY } exec_stage;
typedef enum exec_operation { EXEC_NEST_COMMAND, EXEC_NEST_BEGIN, EXEC_NEST_OPEN,
                              EXEC_NEST_NEXT, EXEC_NEST_CANCEL, EXEC_NEST_DESTROY } exec_operation;
typedef struct exec_database {
  orm_connection_t *handle;
  void *context;
  orm_backend_ops native, observed;
  orm_query_t *command;
  cflow_publisher publisher;
  unsigned opens, commands, begins, destroys;
} exec_database;
typedef struct exec_cursor {
  void *context;
  orm_row_cursor_ops native, observed;
  cserde_reader reader;
  orm_query_t *query;
  cflow_publisher publisher;
  unsigned database, nexts, cancels, destroys;
} exec_cursor;
static exec_database databases[EXEC_DATABASES];
static exec_cursor cursors[EXEC_CURSORS];
static orm_flow_config_t flow;
static orm_transaction_t *outer_transaction, *nested_transaction;
static exec_stage armed;
static exec_operation operation;
static unsigned opening, target, probes, nested_entries, cleanup_inside;
static orm_status_t nested_status, nested_error;
static cflow_step_kind nested_step;
static exec_stage cleanup_stage;
static void (*cleanup_probe)(void);
static unsigned held_during_cleanup, tx_rollbacks, tx_destroys;
static orm_transaction_backend_ops tx_native, tx_observed;

Struct(exec_row, (int, id));
static const cmeta_type_identity row_identity = CMETA_TYPE_ID_ATOM_INIT("orm.exec.Row");
static const cmeta_type_traits row_traits = {
  .flags = CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY
};
static const cmeta_type_desc row_type = {
  .name = "exec_row", .size = sizeof(exec_row), .align = _Alignof(exec_row),
  .kind = CMETA_T_OBJECT, .traits = &row_traits, .identity = &row_identity
};
static const cmeta_data_field_desc row_fields[] = {
  {"orm.exec.Row.id", "id", offsetof(exec_row, id), &cmeta_data_int}
};
static const cmeta_data_struct_shape row_shape = {
  .layout = StructMeta(exec_row), .fields = row_fields, .field_count = 1u
};
static const cmeta_data_desc row_data = {
  .struct_size = offsetof(cmeta_data_desc, shape) + sizeof(void *),
  .abi_version = CMETA_DATA_DESC_ABI_VERSION, .stable_id = "orm.exec.Row.data",
  .display_name = "execution row", .kind = CMETA_DATA_STRUCT,
  .storage_type = &row_type, .shape = &row_shape
};

static exec_database *database_for(void *context) {
  for (unsigned i = 0u; i < EXEC_DATABASES; ++i)
    if (databases[i].context == context) return &databases[i];
  abort();
}
static exec_cursor *cursor_for(void *context) {
  for (unsigned i = 0u; i < EXEC_CURSORS; ++i)
    if (cursors[i].context == context) return &cursors[i];
  abort();
}
static void drop(cflow_publisher *publisher) {
  if (cflow_publisher_valid(publisher)) cflow_publisher_destroy(publisher);
  memset(publisher, 0, sizeof(*publisher));
}
static orm_status_t open_rows(unsigned index, orm_query_t *query, orm_error_t *error) {
  opening = index;
  return orm_query_open_flow(query, &flow, &cursors[index].publisher, error);
}

static void probe(exec_stage stage) {
  if (cleanup_probe != NULL && cleanup_stage == stage) {
    void (*callback)(void) = cleanup_probe;
    cleanup_probe = NULL;
    callback();
  }
  if (armed != stage) return;
  armed = EXEC_NONE; /* Only distinct Publishers reenter, never the same object. */
  ++probes;
  exec_cursor *cursor = &cursors[target];
  exec_database *database = &databases[cursor->database];
  const unsigned cleanup_before = cursor->cancels + cursor->destroys;
  unsigned before;
  orm_error_t error;
  orm_error_init(&error);
  switch (operation) {
    case EXEC_NEST_COMMAND: {
      before = database->commands;
      orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
      nested_step = cflow_publisher_resume(&database->publisher, NULL, &result).kind;
      nested_entries = database->commands - before;
      break;
    }
    case EXEC_NEST_BEGIN:
      before = database->begins;
      nested_status = orm_transaction_begin(database->handle, ORM_ISOLATION_SERIALIZABLE,
                                             &nested_transaction, &error);
      nested_entries = database->begins - before;
      break;
    case EXEC_NEST_OPEN:
      before = database->opens;
      cursors[3].database = cursor->database;
      nested_status = open_rows(3u, cursor->query, &error);
      nested_entries = database->opens - before;
      break;
    case EXEC_NEST_NEXT: {
      before = cursor->nexts;
      exec_row row = {0};
      nested_step = cflow_publisher_resume(&cursor->publisher, NULL, &row).kind;
      nested_entries = cursor->nexts - before;
      break;
    }
    case EXEC_NEST_CANCEL:
      before = cursor->cancels;
      cflow_publisher_cancel(&cursor->publisher);
      nested_entries = cursor->cancels - before;
      break;
    case EXEC_NEST_DESTROY:
      before = cursor->destroys;
      drop(&cursor->publisher);
      nested_entries = cursor->destroys - before;
      break;
  }
  nested_error = error.status;
  cleanup_inside = cursor->cancels + cursor->destroys - cleanup_before;
  /* No assertion/longjmp escapes a native callback. Unexpectedly admitted
   * outputs stay owned and are cleaned only after the outer callback returns. */
}

static cserde_status observed_read(void *context, cserde_token *out) {
  exec_cursor *cursor = context;
  probe(EXEC_READER);
  return cserde_reader_next(&cursor->reader, out);
}
static const cserde_reader_ops reader_ops = {
  sizeof(cserde_reader_ops), CSERDE_READER_OPS_ABI_VERSION, observed_read
};
static orm_row_cursor_step observed_next(void *context, cserde_reader *out) {
  exec_cursor *cursor = cursor_for(context);
  ++cursor->nexts;
  probe(EXEC_NEXT);
  const orm_row_cursor_step result = cursor->native.next(context, out);
  if (result.kind == ORM_ROW_CURSOR_ROW || result.kind == ORM_ROW_CURSOR_ROW_AND_DONE) {
    cursor->reader = *out;
    out->ops = &reader_ops;
    out->context = cursor;
  }
  return result;
}
static void observed_cancel(void *context) {
  exec_cursor *cursor = cursor_for(context);
  ++cursor->cancels;
  probe(EXEC_CANCEL);
  cursor->native.cancel(context);
}
static void observed_destroy(void *context) {
  exec_cursor *cursor = cursor_for(context);
  ++cursor->destroys;
  probe(EXEC_DESTROY);
  cursor->native.destroy(context);
  cursor->context = NULL;
}
static orm_status_t observed_open(void *context, const orm_query_plan *plan,
    const orm_limits *limits, orm_row_cursor *out, orm_error_t *error) {
  exec_database *database = database_for(context);
  ++database->opens;
  const orm_status_t status = database->native.open_cursor(context, plan, limits, out, error);
  if (status == ORM_STATUS_OK) {
    exec_cursor *cursor = &cursors[opening];
    if (cursor->context != NULL) abort();
    cursor->context = out->context;
    cursor->native = *out->ops;
    cursor->observed = cursor->native;
    cursor->observed.next = observed_next;
    cursor->observed.cancel = observed_cancel;
    cursor->observed.destroy = observed_destroy;
    out->ops = &cursor->observed;
  }
  return status;
}
static orm_status_t observed_command(void *context, const orm_query_plan *plan,
    const orm_limits *limits, uint64_t *affected, orm_error_t *error) {
  exec_database *database = database_for(context);
  ++database->commands;
  probe(EXEC_COMMAND);
  return database->native.execute_command(context, plan, limits, affected, error);
}
static orm_status_t observed_begin(void *context, orm_isolation_t isolation,
    orm_transaction_backend *out, orm_error_t *error) {
  exec_database *database = database_for(context);
  ++database->begins;
  probe(EXEC_BEGIN);
  return database->native.begin_transaction(context, isolation, out, error);
}
static void observed_close(void *context) {
  exec_database *database = database_for(context);
  ++database->destroys;
  database->native.destroy(context);
}

static void scenario(exec_stage stage, exec_operation nested, unsigned cursor_index) {
  orm_error_t error;
  orm_error_init(&error);
  target = cursor_index; operation = nested; armed = stage;
  const bool independent = cursors[target].database != 0u;
  if (stage == EXEC_COMMAND) {
    orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
    check_equal(cflow_publisher_resume(&databases[0].publisher, NULL, &result).kind,
                CFLOW_STEP_VALUE_AND_DONE);
  } else if (stage == EXEC_BEGIN) {
    check_equal(orm_transaction_begin(databases[0].handle, ORM_ISOLATION_SERIALIZABLE,
                  &outer_transaction, &error), ORM_STATUS_OK);
  } else if (stage == EXEC_CANCEL) {
    cflow_publisher_cancel(&cursors[0].publisher);
  } else if (stage == EXEC_DESTROY) {
    drop(&cursors[0].publisher);
  } else {
    exec_row row = {0};
    check_equal(cflow_publisher_resume(&cursors[0].publisher, NULL, &row).kind,
                CFLOW_STEP_VALUE);
    check_equal(row.id, 7);
  }
  check_equal(probes, 1u);
  check_equal(nested_entries, independent ? 1u : 0u);
  if (!independent) check_equal(cleanup_inside, 0u);
  if (nested == EXEC_NEST_COMMAND)
    check_equal(nested_step, independent ? CFLOW_STEP_VALUE_AND_DONE : CFLOW_STEP_ERROR);
  else if (nested == EXEC_NEST_NEXT)
    check_equal(nested_step, independent ? CFLOW_STEP_VALUE : CFLOW_STEP_ERROR);
  else if (nested == EXEC_NEST_BEGIN || nested == EXEC_NEST_OPEN) {
    check_equal(nested_status, independent ? ORM_STATUS_OK : ORM_STATUS_BUSY);
    check_equal(nested_error, nested_status);
    if (!independent) {
      check_null(nested_transaction);
      check_false(cflow_publisher_valid(&cursors[3].publisher));
    }
  } else {
    /* Cancellation consumes no Publisher/query hold. Destroy may transfer its
     * existing hold to pending cleanup, but must complete after callback exit. */
    check_equal(cursors[target].cancels, 1u);
    check_equal(cursors[target].destroys, nested == EXEC_NEST_DESTROY ? 1u : 0u);
    check_equal(cursors[target].query->owner.dependents,
                nested == EXEC_NEST_DESTROY ? 0u : 1u);
  }
  check_false(databases[0].handle->native_active);
  check_false(databases[1].handle->native_active);
  check_equal(databases[0].handle->failure, ORM_STATUS_OK);
  check_equal(databases[1].handle->failure, ORM_STATUS_OK);
}

/* These observers request cleanup through public APIs; they never install a
 * queue, take a reservation, add a parent hold, or implement the missing behavior. */
static void cancel_then_drop_sibling(void) {
  cflow_publisher_cancel(&cursors[1].publisher);
  cflow_publisher_cancel(&cursors[1].publisher);
  drop(&cursors[1].publisher);
  cleanup_inside = cursors[1].cancels + cursors[1].destroys;
  held_during_cleanup = cursors[1].query->owner.dependents;
}

static void drop_sibling_during_its_deferred_cancel(void) {
  drop(&cursors[1].publisher);
  cleanup_inside = cursors[1].destroys;
  held_during_cleanup = cursors[1].query->owner.dependents;
}

static void queue_cancel_then_observe_it(void) {
  cflow_publisher_cancel(&cursors[1].publisher);
  cleanup_stage = EXEC_CANCEL;
  cleanup_probe = drop_sibling_during_its_deferred_cancel;
}

static void drop_another_cursor_during_deferred_destroy(void) {
  drop(&cursors[0].publisher);
  cleanup_inside = cursors[0].cancels + cursors[0].destroys;
}

static void queue_destroy_then_observe_it(void) {
  drop(&cursors[1].publisher);
  cleanup_stage = EXEC_DESTROY;
  cleanup_probe = drop_another_cursor_during_deferred_destroy;
}

static orm_status_t observed_tx_rollback(void *context, orm_error_t *error) {
  ++tx_rollbacks;
  probe(EXEC_TX_ROLLBACK);
  return tx_native.rollback(context, error);
}

static void observed_tx_destroy(void *context) {
  ++tx_destroys;
  probe(EXEC_TX_DESTROY);
  tx_native.destroy(context);
}

static void observe_transaction(void) {
  tx_native = *outer_transaction->backend.ops;
  tx_observed = tx_native;
  tx_observed.rollback = observed_tx_rollback;
  tx_observed.destroy = observed_tx_destroy;
  outer_transaction->backend.ops = &tx_observed;
}

static void release_transaction_in_callback(void) {
  orm_transaction_t *transaction = outer_transaction;
  outer_transaction = NULL;
  orm_transaction_release(transaction);
  cleanup_inside = tx_rollbacks + tx_destroys;
}

static void close_transaction_in_callback(void) {
  orm_error_t error;
  orm_error_init(&error);
  nested_status = orm_transaction_close(outer_transaction, &error);
  nested_error = error.status;
  cleanup_inside = tx_destroys;
}

spec("native cursor execution and cleanup share connection admission") {
  before_each() {
    memset(databases, 0, sizeof(databases));
    memset(cursors, 0, sizeof(cursors));
    outer_transaction = nested_transaction = NULL;
    cleanup_probe = NULL; cleanup_stage = EXEC_NONE;
    held_during_cleanup = tx_rollbacks = tx_destroys = 0u;
    armed = EXEC_NONE; operation = EXEC_NEST_COMMAND;
    opening = target = probes = nested_entries = cleanup_inside = 0u;
    nested_status = nested_error = ORM_STATUS_OK; nested_step = CFLOW_STEP_DONE;
    orm_flow_config(&flow, &row_data);
    for (unsigned i = 0u; i < EXEC_DATABASES; ++i) {
      orm_config_t config; orm_error_t error;
      orm_config(&config); orm_error_init(&error);
      const orm_option_t filename = {orm_view("filename"), orm_view(":memory:")};
      config.driver = orm_view("sqlite"); config.options = &filename; config.option_count = 1u;
      exec_database *database = &databases[i];
      check_equal(orm_connect(&config, &database->handle, &error), ORM_STATUS_OK);
      database->context = database->handle->backend.context;
      database->native = *database->handle->backend.ops;
      database->observed = database->native;
      database->observed.open_cursor = observed_open;
      database->observed.execute_command = observed_command;
      database->observed.begin_transaction = observed_begin;
      database->observed.destroy = observed_close;
      database->handle->backend.ops = &database->observed;
      check_equal(orm_raw(database->handle, orm_view("create table cursor_execution_probe(id integer)"),
                          &database->command, &error), ORM_STATUS_OK);
      check_equal(orm_query_open_command_flow(database->command, &database->publisher, &error), ORM_STATUS_OK);
    }
    for (unsigned i = 0u; i < 3u; ++i) {
      orm_error_t error; orm_error_init(&error);
      cursors[i].database = i == 2u ? 1u : 0u;
      check_equal(orm_raw(databases[cursors[i].database].handle, orm_view("select 7 as id"),
                          &cursors[i].query, &error), ORM_STATUS_OK);
      check_equal(open_rows(i, cursors[i].query, &error), ORM_STATUS_OK);
    }
  }
  after_each() {
    cleanup_probe = NULL;
    armed = EXEC_NONE;
    for (unsigned i = 0u; i < EXEC_CURSORS; ++i) drop(&cursors[i].publisher);
    for (unsigned i = 0u; i < EXEC_DATABASES; ++i) drop(&databases[i].publisher);
    orm_transaction_release(nested_transaction);
    orm_transaction_release(outer_transaction);
    for (unsigned i = 0u; i < EXEC_CURSORS; ++i) orm_query_release(cursors[i].query);
    for (unsigned i = 0u; i < EXEC_DATABASES; ++i) {
      orm_query_release(databases[i].command);
      orm_connection_release(databases[i].handle);
    }
  }
  it("blocks commands during native next") { scenario(EXEC_NEXT, EXEC_NEST_COMMAND, 1u); }
  it("allows independent commands during native next") { scenario(EXEC_NEXT, EXEC_NEST_COMMAND, 2u); }
  it("keeps admission through real CBind reader consumption") { scenario(EXEC_READER, EXEC_NEST_COMMAND, 1u); }
  it("allows independent commands during real row decoding") { scenario(EXEC_READER, EXEC_NEST_COMMAND, 2u); }
  it("blocks BEGIN during native next") { scenario(EXEC_NEXT, EXEC_NEST_BEGIN, 1u); }
  it("allows independent BEGIN during native next") { scenario(EXEC_NEXT, EXEC_NEST_BEGIN, 2u); }
  it("blocks cursor construction during native next") { scenario(EXEC_NEXT, EXEC_NEST_OPEN, 1u); }
  it("allows independent cursor construction during native next") { scenario(EXEC_NEXT, EXEC_NEST_OPEN, 2u); }
  it("blocks commands during native cancellation") { scenario(EXEC_CANCEL, EXEC_NEST_COMMAND, 1u); }
  it("allows independent commands during native cancellation") { scenario(EXEC_CANCEL, EXEC_NEST_COMMAND, 2u); }
  it("blocks commands during ordinary native disposal") { scenario(EXEC_DESTROY, EXEC_NEST_COMMAND, 1u); }
  it("allows independent commands during ordinary native disposal") { scenario(EXEC_DESTROY, EXEC_NEST_COMMAND, 2u); }
  it("rejects next inside a command without entering native cleanup") { scenario(EXEC_COMMAND, EXEC_NEST_NEXT, 1u); }
  it("allows independent next inside a command") { scenario(EXEC_COMMAND, EXEC_NEST_NEXT, 2u); }
  it("rejects next inside BEGIN without entering native cleanup") { scenario(EXEC_BEGIN, EXEC_NEST_NEXT, 1u); }
  it("allows independent next inside BEGIN") { scenario(EXEC_BEGIN, EXEC_NEST_NEXT, 2u); }
  it("defers sibling cancel until command callback return") { scenario(EXEC_COMMAND, EXEC_NEST_CANCEL, 1u); }
  it("allows independent cancel inside a command") { scenario(EXEC_COMMAND, EXEC_NEST_CANCEL, 2u); }
  it("defers sibling disposal until command callback return") { scenario(EXEC_COMMAND, EXEC_NEST_DESTROY, 1u); }
  it("allows independent disposal inside a command") { scenario(EXEC_COMMAND, EXEC_NEST_DESTROY, 2u); }
  it("defers sibling cancel until native next returns") { scenario(EXEC_NEXT, EXEC_NEST_CANCEL, 1u); }
  it("allows independent cancel inside native next") { scenario(EXEC_NEXT, EXEC_NEST_CANCEL, 2u); }
  it("defers sibling disposal until native next returns") { scenario(EXEC_NEXT, EXEC_NEST_DESTROY, 1u); }
  it("allows independent disposal inside native next") { scenario(EXEC_NEXT, EXEC_NEST_DESTROY, 2u); }
  it("defers sibling disposal until row reader consumption completes") { scenario(EXEC_READER, EXEC_NEST_DESTROY, 1u); }
  it("allows independent disposal during row reader consumption") { scenario(EXEC_READER, EXEC_NEST_DESTROY, 2u); }
  it("releases admission between sequential real row resumes") {
    for (unsigned i = 0u; i < 2u; ++i) {
      exec_row row = {0};
      check_equal(cflow_publisher_resume(&cursors[i].publisher, NULL, &row).kind, CFLOW_STEP_VALUE);
      check_equal(row.id, 7);
      check_false(databases[0].handle->native_active);
    }
  }
  it("rejects exhausted completion capacity before next or native cleanup") {
    orm_owner *owner = &databases[0].handle->owner;
    const uint32_t budget = owner->max_dependents, before = owner->dependents;
    owner->max_dependents = before;
    exec_row row = {0};
    const cflow_step result = cflow_publisher_resume(&cursors[0].publisher, NULL, &row);
    owner->max_dependents = budget; /* Restore before any assertion/teardown. */
    check_equal(result.kind, CFLOW_STEP_ERROR);
    check_equal(cursors[0].nexts, 0u);
    check_equal(cursors[0].cancels, 0u);
    check_equal(cursors[0].destroys, 0u);
    check_equal(owner->dependents, before);
    check_equal(cursors[0].query->owner.dependents, 1u);
    check_false(databases[0].handle->native_active);
  }
  it("coalesces repeated deferred cancellation and disposal without losing the hold") {
    cleanup_stage = EXEC_COMMAND; cleanup_probe = cancel_then_drop_sibling;
    orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
    check_equal(cflow_publisher_resume(&databases[0].publisher, NULL, &result).kind,
                CFLOW_STEP_VALUE_AND_DONE);
    check_equal(cleanup_inside, 0u);
    check_equal(held_during_cleanup, 1u);
    check_equal(cursors[1].cancels, 1u);
    check_equal(cursors[1].destroys, 1u);
    check_equal(cursors[1].query->owner.dependents, 0u);
    check_false(databases[0].handle->native_active);
  }
  it("accepts destruction while a previously deferred cancellation is running") {
    cleanup_stage = EXEC_COMMAND; cleanup_probe = queue_cancel_then_observe_it;
    orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
    check_equal(cflow_publisher_resume(&databases[0].publisher, NULL, &result).kind,
                CFLOW_STEP_VALUE_AND_DONE);
    check_null(cleanup_probe);
    check_equal(cleanup_inside, 0u);
    check_equal(held_during_cleanup, 1u);
    check_equal(cursors[1].cancels, 1u);
    check_equal(cursors[1].destroys, 1u);
    check_equal(cursors[1].query->owner.dependents, 0u);
  }
  it("drains cleanup enqueued by another cursor finalizer without native overlap") {
    cleanup_stage = EXEC_COMMAND; cleanup_probe = queue_destroy_then_observe_it;
    orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
    check_equal(cflow_publisher_resume(&databases[0].publisher, NULL, &result).kind,
                CFLOW_STEP_VALUE_AND_DONE);
    check_null(cleanup_probe);
    check_equal(cleanup_inside, 0u);
    for (unsigned i = 0u; i < 2u; ++i) {
      check_equal(cursors[i].cancels, 1u);
      check_equal(cursors[i].destroys, 1u);
      check_equal(cursors[i].query->owner.dependents, 0u);
    }
    check_false(databases[0].handle->native_active);
  }
  it("finishes idle cleanup when it owns the last query and connection holds") {
    drop(&databases[0].publisher);
    orm_query_release(databases[0].command); databases[0].command = NULL;
    drop(&cursors[1].publisher);
    orm_query_release(cursors[1].query); cursors[1].query = NULL;
    orm_query_release(cursors[0].query); cursors[0].query = NULL;
    orm_connection_release(databases[0].handle); databases[0].handle = NULL;
    check_equal(databases[0].destroys, 0u);
    drop(&cursors[0].publisher);
    check_equal(cursors[0].cancels, 1u);
    check_equal(cursors[0].destroys, 1u);
    check_equal(databases[0].destroys, 1u);
  }
  it("does not require another completion slot to dispose an admitted cursor") {
    orm_owner *owner = &databases[0].handle->owner;
    const uint32_t budget = owner->max_dependents;
    owner->max_dependents = owner->dependents;
    drop(&cursors[1].publisher);
    owner->max_dependents = budget;
    check_equal(cursors[1].cancels, 1u);
    check_equal(cursors[1].destroys, 1u);
    check_equal(cursors[1].query->owner.dependents, 0u);
    check_false(databases[0].handle->native_active);
  }
  it("permits required cursor cleanup after terminal connection failure") {
    databases[0].handle->failure = ORM_STATUS_CONNECTION_ERROR;
    drop(&cursors[1].publisher);
    check_equal(cursors[1].cancels, 1u);
    check_equal(cursors[1].destroys, 1u);
    check_equal(cursors[1].query->owner.dependents, 0u);
    check_equal(databases[0].handle->failure, ORM_STATUS_CONNECTION_ERROR);
    check_false(databases[0].handle->native_active);
  }
  it("defers final transaction rollback until a rejected database command returns") {
    orm_error_t error; orm_error_init(&error);
    check_equal(orm_transaction_begin(databases[0].handle, ORM_ISOLATION_SERIALIZABLE,
                  &outer_transaction, &error), ORM_STATUS_OK);
    observe_transaction();
    cleanup_stage = EXEC_COMMAND; cleanup_probe = release_transaction_in_callback;
    orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
    /* The real SQLite backend requires the transaction handle while active.
     * Deferred rollback must not clear that state before this command checks it. */
    const cflow_step step = cflow_publisher_resume(&databases[0].publisher, NULL, &result);
    check_equal(step.kind, CFLOW_STEP_ERROR);
    check_not_null(step.error);
    check_equal(strcmp(step.error, "use the SQLite transaction handle while active"), 0);
    check_equal(cleanup_inside, 0u);
    check_equal(tx_rollbacks, 1u);
    check_equal(tx_destroys, 1u);
    check_false(databases[0].handle->native_active);
  }
  it("defers final transaction cleanup through synchronous row reader consumption") {
    orm_error_t error; orm_error_init(&error);
    check_equal(orm_transaction_begin(databases[0].handle, ORM_ISOLATION_SERIALIZABLE,
                  &outer_transaction, &error), ORM_STATUS_OK);
    observe_transaction();
    cleanup_stage = EXEC_READER; cleanup_probe = release_transaction_in_callback;
    exec_row row = {0};
    check_equal(cflow_publisher_resume(&cursors[0].publisher, NULL, &row).kind,
                CFLOW_STEP_VALUE);
    check_equal(row.id, 7);
    check_equal(cleanup_inside, 0u);
    check_equal(tx_rollbacks, 1u);
    check_equal(tx_destroys, 1u);
    check_false(databases[0].handle->native_active);
  }
  it("returns BUSY for checked transaction close inside another native operation") {
    orm_error_t error; orm_error_init(&error);
    check_equal(orm_transaction_begin(databases[0].handle, ORM_ISOLATION_SERIALIZABLE,
                  &outer_transaction, &error), ORM_STATUS_OK);
    check_equal(orm_transaction_commit(outer_transaction, &error), ORM_STATUS_OK);
    observe_transaction();
    cleanup_stage = EXEC_COMMAND; cleanup_probe = close_transaction_in_callback;
    orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
    check_equal(cflow_publisher_resume(&databases[0].publisher, NULL, &result).kind,
                CFLOW_STEP_VALUE_AND_DONE);
    check_equal(nested_status, ORM_STATUS_BUSY);
    check_equal(nested_error, ORM_STATUS_BUSY);
    check_equal(cleanup_inside, 0u);
    check_equal(outer_transaction->owner.phase, ORM_OWNER_OPEN);
    check_equal(orm_transaction_close(outer_transaction, &error), ORM_STATUS_OK);
    check_equal(tx_destroys, 1u);
  }
  it("blocks same-connection command reentry during final transaction rollback") {
    orm_error_t error; orm_error_init(&error);
    check_equal(orm_transaction_begin(databases[0].handle, ORM_ISOLATION_SERIALIZABLE,
                  &outer_transaction, &error), ORM_STATUS_OK);
    observe_transaction();
    target = 1u; operation = EXEC_NEST_COMMAND; armed = EXEC_TX_ROLLBACK;
    orm_transaction_release(outer_transaction); outer_transaction = NULL;
    check_equal(probes, 1u);
    check_equal(nested_entries, 0u);
    check_equal(nested_step, CFLOW_STEP_ERROR);
    check_equal(tx_rollbacks, 1u);
    check_equal(tx_destroys, 1u);
    check_false(databases[0].handle->native_active);
  }
  it("allows independent commands during final transaction rollback") {
    orm_error_t error; orm_error_init(&error);
    check_equal(orm_transaction_begin(databases[0].handle, ORM_ISOLATION_SERIALIZABLE,
                  &outer_transaction, &error), ORM_STATUS_OK);
    observe_transaction();
    target = 2u; operation = EXEC_NEST_COMMAND; armed = EXEC_TX_ROLLBACK;
    orm_transaction_release(outer_transaction); outer_transaction = NULL;
    check_equal(probes, 1u);
    check_equal(nested_entries, 1u);
    check_equal(nested_step, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(tx_rollbacks, 1u);
    check_equal(tx_destroys, 1u);
    check_false(databases[0].handle->native_active);
  }
}

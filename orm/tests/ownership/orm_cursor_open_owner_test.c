/* Shares the checked-owner runner; all database work uses real SQLite. */
#define TINYTEST_NO_MAIN
#include "orm_internal.h"
#include <cmeta/struct.h>
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

enum { OPEN_CONNECTIONS = 2u };
typedef enum open_stage { OPEN_NONE, OPEN_CURSOR, OPEN_SHAPE, OPEN_DISPOSE,
                         OPEN_COMMAND, OPEN_BEGIN } open_stage;
typedef enum open_nested { OPEN_NEST_COMMAND, OPEN_NEST_BEGIN, OPEN_NEST_ROWS } open_nested;
typedef struct open_connection {
  orm_connection_t *handle;
  void *native_context;
  orm_backend_ops native, observed;
  orm_query_t *row, *other_row, *command;
  cflow_publisher command_publisher;
  unsigned open_calls, command_calls, begin_calls, close_calls;
} open_connection;
static open_connection connections[OPEN_CONNECTIONS];
static orm_transaction_t *outer_transaction, *nested_transaction;
static orm_transaction_backend_ops transaction_native, transaction_observed;
static orm_row_cursor_ops cursor_native, cursor_observed;
static cflow_publisher outer_publisher, nested_publisher;
static orm_flow_config_t flow_config;
static open_stage armed, release_stage;
static open_nested nested_operation;
static unsigned nested_target, probe_calls, nested_entries, cursor_destroys;
static unsigned closes_inside_callback;
static orm_status_t nested_status, nested_error, shape_failure;
static cflow_step_kind nested_step;
static bool invalid_cursor;

Struct(open_row, (int, id));
static const cmeta_type_identity row_identity = CMETA_TYPE_ID_ATOM_INIT("orm.open.Row");
static const cmeta_type_traits row_traits = {
  .flags = CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY
};
static const cmeta_type_desc row_type = {
  .name = "open_row", .size = sizeof(open_row), .align = _Alignof(open_row),
  .kind = CMETA_T_OBJECT, .traits = &row_traits, .identity = &row_identity
};
static const cmeta_data_field_desc row_fields[] = {
  {"orm.open.Row.id", "id", offsetof(open_row, id), &cmeta_data_int}
};
static const cmeta_data_struct_shape row_shape = {
  .layout = StructMeta(open_row), .fields = row_fields, .field_count = 1u
};
static const cmeta_data_desc row_data = {
  .struct_size = offsetof(cmeta_data_desc, shape) + sizeof(void *),
  .abi_version = CMETA_DATA_DESC_ABI_VERSION, .stable_id = "orm.open.Row.data",
  .display_name = "open row", .kind = CMETA_DATA_STRUCT,
  .storage_type = &row_type, .shape = &row_shape
};

static open_connection *connection_for(void *context) {
  for (unsigned i = 0; i < OPEN_CONNECTIONS; ++i)
    if (connections[i].native_context == context) return &connections[i];
  abort(); /* A test observer received a context it never installed. */
}
static void drop_publisher(cflow_publisher *publisher) {
  if (cflow_publisher_valid(publisher)) cflow_publisher_destroy(publisher);
  memset(publisher, 0, sizeof(*publisher));
}
static void release_external(open_connection *connection) {
  orm_query_release(connection->row); connection->row = NULL;
  orm_query_release(connection->other_row); connection->other_row = NULL;
  orm_query_release(connection->command); connection->command = NULL;
  orm_connection_release(connection->handle); connection->handle = NULL;
}
static orm_status_t open_rows(unsigned target, bool outer, orm_error_t *error) {
  orm_query_t *query = outer ? connections[target].row : connections[target].other_row;
  cflow_publisher *publisher = outer ? &outer_publisher : &nested_publisher;
  if (target == 0u && outer_transaction != NULL)
    return orm_query_open_flow_in_transaction(query, outer_transaction,
                                               &flow_config, publisher, error);
  return orm_query_open_flow(query, &flow_config, publisher, error);
}
static void probe(open_stage stage) {
  if (release_stage == stage) {
    release_stage = OPEN_NONE;
    release_external(&connections[0]);
    closes_inside_callback = connections[0].close_calls;
  }
  if (armed != stage) return;
  armed = OPEN_NONE; /* Never recursively dispatch the same Publisher. */
  ++probe_calls;
  open_connection *target = &connections[nested_target];
  orm_error_t error;
  orm_error_init(&error);
  unsigned before;
  switch (nested_operation) {
    case OPEN_NEST_COMMAND: {
      before = target->command_calls;
      orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
      nested_step = cflow_publisher_resume(&target->command_publisher, NULL, &result).kind;
      nested_entries = target->command_calls - before;
      break;
    }
    case OPEN_NEST_BEGIN:
      before = target->begin_calls;
      nested_status = orm_transaction_begin(target->handle, ORM_ISOLATION_SERIALIZABLE,
                                             &nested_transaction, &error);
      nested_entries = target->begin_calls - before;
      break;
    case OPEN_NEST_ROWS:
      before = target->open_calls;
      nested_status = open_rows(nested_target, false, &error);
      nested_entries = target->open_calls - before;
      break;
  }
  nested_error = error.status;
  /* No assertion or longjmp may escape a native callback. Unexpected successful
   * outputs stay owned by the fixture and are cleaned after the callback exits. */
}
static void observed_close(void *context) {
  open_connection *connection = connection_for(context);
  ++connection->close_calls;
  connection->native.destroy(context);
}
static void observed_cursor_destroy(void *context) {
  ++cursor_destroys;
  cursor_native.destroy(context);
  probe(OPEN_DISPOSE);
}
static orm_status_t observed_shape(void *context, const cmeta_data_desc *shape,
                                    orm_error_t *error) {
  orm_status_t status = cursor_native.configure_shape != NULL
      ? cursor_native.configure_shape(context, shape, error) : ORM_STATUS_OK;
  probe(OPEN_SHAPE);
  if (status == ORM_STATUS_OK && shape_failure != ORM_STATUS_OK) {
    orm_error_set(error, shape_failure, "test shape callback rejected the row");
    return shape_failure;
  }
  return status;
}
static void observe_cursor(orm_row_cursor *cursor, orm_status_t status) {
  if (status == ORM_STATUS_OK) {
    cursor_native = *cursor->ops;
    cursor_observed = cursor_native;
    cursor_observed.configure_shape = observed_shape;
    cursor_observed.destroy = observed_cursor_destroy;
    if (invalid_cursor) cursor_observed.name = NULL;
    cursor->ops = &cursor_observed;
  }
  probe(OPEN_CURSOR);
}
static orm_status_t observed_open(void *context, const orm_query_plan *plan,
    const orm_limits *limits, orm_row_cursor *cursor, orm_error_t *error) {
  open_connection *connection = connection_for(context);
  ++connection->open_calls;
  const orm_status_t status = connection->native.open_cursor(context, plan, limits, cursor, error);
  observe_cursor(cursor, status);
  return status;
}
static orm_status_t observed_command(void *context, const orm_query_plan *plan,
    const orm_limits *limits, uint64_t *affected, orm_error_t *error) {
  open_connection *connection = connection_for(context);
  ++connection->command_calls;
  const orm_status_t status = connection->native.execute_command(context, plan, limits, affected, error);
  probe(OPEN_COMMAND);
  return status;
}
static orm_status_t observed_begin(void *context, orm_isolation_t isolation,
    orm_transaction_backend *transaction, orm_error_t *error) {
  open_connection *connection = connection_for(context);
  ++connection->begin_calls;
  const orm_status_t status = connection->native.begin_transaction(context, isolation, transaction, error);
  probe(OPEN_BEGIN);
  return status;
}
static orm_status_t transaction_open(void *context, const orm_query_plan *plan,
    const orm_limits *limits, orm_row_cursor *cursor, orm_error_t *error) {
  ++connections[0].open_calls;
  const orm_status_t status = transaction_native.open_cursor(context, plan, limits, cursor, error);
  observe_cursor(cursor, status);
  return status;
}
static orm_status_t transaction_command(void *context, const orm_query_plan *plan,
    const orm_limits *limits, uint64_t *affected, orm_error_t *error) {
  ++connections[0].command_calls;
  return transaction_native.execute_command(context, plan, limits, affected, error);
}
static void begin_outer(void) {
  orm_error_t error;
  orm_error_init(&error);
  check_equal(orm_transaction_begin(connections[0].handle, ORM_ISOLATION_SERIALIZABLE,
                                    &outer_transaction, &error), ORM_STATUS_OK);
  transaction_native = *outer_transaction->backend.ops;
  transaction_observed = transaction_native;
  transaction_observed.open_cursor = transaction_open;
  transaction_observed.execute_command = transaction_command;
  outer_transaction->backend.ops = &transaction_observed;
}
static void scenario(open_stage stage, open_nested operation, unsigned target) {
  orm_error_t error;
  orm_error_init(&error);
  nested_operation = operation;
  nested_target = target;
  if (operation == OPEN_NEST_COMMAND) {
    orm_status_t status = target == 0u && outer_transaction != NULL
        ? orm_query_open_command_flow_in_transaction(connections[target].command,
            outer_transaction, &connections[target].command_publisher, &error)
        : orm_query_open_command_flow(connections[target].command,
            &connections[target].command_publisher, &error);
    check_equal(status, ORM_STATUS_OK);
  }
  if (stage == OPEN_DISPOSE) shape_failure = ORM_STATUS_TYPE_ERROR;
  armed = stage;
  if (stage == OPEN_COMMAND) {
    check_equal(orm_query_open_command_flow(connections[0].command,
                  &connections[0].command_publisher, &error), ORM_STATUS_OK);
    orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
    check_equal(cflow_publisher_resume(&connections[0].command_publisher, NULL, &result).kind,
                CFLOW_STEP_VALUE_AND_DONE);
  } else if (stage == OPEN_BEGIN) {
    check_equal(orm_transaction_begin(connections[0].handle, ORM_ISOLATION_SERIALIZABLE,
                  &outer_transaction, &error), ORM_STATUS_OK);
  } else {
    check_equal(open_rows(0u, true, &error), shape_failure);
  }
  check_equal(probe_calls, 1u);
  check_equal(nested_entries, target == 0u ? 0u : 1u);
  if (operation == OPEN_NEST_COMMAND) {
    check_equal(nested_step, target == 0u ? CFLOW_STEP_ERROR : CFLOW_STEP_VALUE_AND_DONE);
  } else {
    check_equal(nested_status, target == 0u ? ORM_STATUS_BUSY : ORM_STATUS_OK);
    check_equal(nested_error, nested_status);
    if (target == 0u) {
      check_null(nested_transaction);
      check_false(cflow_publisher_valid(&nested_publisher));
    }
  }
  check_false(connections[0].handle->native_active);
  check_equal(connections[0].handle->failure, ORM_STATUS_OK);
}

spec("native cursor construction shares the connection reservation") {
  before_each() {
    memset(connections, 0, sizeof(connections));
    memset(&outer_publisher, 0, sizeof(outer_publisher));
    memset(&nested_publisher, 0, sizeof(nested_publisher));
    outer_transaction = nested_transaction = NULL;
    armed = release_stage = OPEN_NONE;
    nested_target = probe_calls = nested_entries = cursor_destroys = 0u;
    closes_inside_callback = 0u;
    nested_status = nested_error = shape_failure = ORM_STATUS_OK;
    nested_step = CFLOW_STEP_DONE; invalid_cursor = false;
    orm_flow_config(&flow_config, &row_data);
    for (unsigned i = 0; i < OPEN_CONNECTIONS; ++i) {
      orm_config_t config;
      orm_error_t error;
      orm_config(&config); orm_error_init(&error);
      const orm_option_t filename = {orm_view("filename"), orm_view(":memory:")};
      config.driver = orm_view("sqlite"); config.options = &filename; config.option_count = 1u;
      open_connection *connection = &connections[i];
      check_equal(orm_connect(&config, &connection->handle, &error), ORM_STATUS_OK);
      connection->native_context = connection->handle->backend.context;
      connection->native = *connection->handle->backend.ops;
      connection->observed = connection->native;
      connection->observed.open_cursor = observed_open;
      connection->observed.execute_command = observed_command;
      connection->observed.begin_transaction = observed_begin;
      connection->observed.destroy = observed_close;
      connection->handle->backend.ops = &connection->observed;
      check_equal(orm_raw(connection->handle, orm_view("select 7 as id"), &connection->row, &error), ORM_STATUS_OK);
      check_equal(orm_raw(connection->handle, orm_view("select 8 as id"), &connection->other_row, &error), ORM_STATUS_OK);
      check_equal(orm_raw(connection->handle, orm_view("create table cursor_open_probe(id integer)"),
                          &connection->command, &error), ORM_STATUS_OK);
    }
  }
  after_each() {
    armed = release_stage = OPEN_NONE;
    drop_publisher(&nested_publisher); drop_publisher(&outer_publisher);
    for (unsigned i = 0; i < OPEN_CONNECTIONS; ++i)
      drop_publisher(&connections[i].command_publisher);
    orm_transaction_release(nested_transaction);
    orm_transaction_release(outer_transaction);
    for (unsigned i = 0; i < OPEN_CONNECTIONS; ++i) release_external(&connections[i]);
  }
  it("blocks same-connection commands inside cursor open") { scenario(OPEN_CURSOR, OPEN_NEST_COMMAND, 0u); }
  it("allows independent commands inside cursor open") { scenario(OPEN_CURSOR, OPEN_NEST_COMMAND, 1u); }
  it("blocks same-connection BEGIN inside cursor open") { scenario(OPEN_CURSOR, OPEN_NEST_BEGIN, 0u); }
  it("allows independent BEGIN inside cursor open") { scenario(OPEN_CURSOR, OPEN_NEST_BEGIN, 1u); }
  it("blocks same-connection recursive cursor open") { scenario(OPEN_CURSOR, OPEN_NEST_ROWS, 0u); }
  it("allows independent cursor open") { scenario(OPEN_CURSOR, OPEN_NEST_ROWS, 1u); }
  it("keeps the reservation through row shape configuration") { scenario(OPEN_SHAPE, OPEN_NEST_COMMAND, 0u); }
  it("allows independent commands during row shape configuration") { scenario(OPEN_SHAPE, OPEN_NEST_COMMAND, 1u); }
  it("keeps the reservation through failed construction disposal") { scenario(OPEN_DISPOSE, OPEN_NEST_COMMAND, 0u); }
  it("allows independent commands during failed construction disposal") { scenario(OPEN_DISPOSE, OPEN_NEST_COMMAND, 1u); }
  it("rejects cursor open from an active command callback") { scenario(OPEN_COMMAND, OPEN_NEST_ROWS, 0u); }
  it("allows independent cursor open from a command callback") { scenario(OPEN_COMMAND, OPEN_NEST_ROWS, 1u); }
  it("rejects cursor open from an active BEGIN callback") { scenario(OPEN_BEGIN, OPEN_NEST_ROWS, 0u); }
  it("allows independent cursor open from a BEGIN callback") { scenario(OPEN_BEGIN, OPEN_NEST_ROWS, 1u); }
  it("shares transaction cursor construction with transaction commands") {
    begin_outer(); scenario(OPEN_CURSOR, OPEN_NEST_COMMAND, 0u);
  }
  it("does not reserve the connection for an idle cursor lifetime") {
    orm_error_t error; orm_error_init(&error);
    check_equal(open_rows(0u, true, &error), ORM_STATUS_OK);
    check_equal(open_rows(0u, false, &error), ORM_STATUS_OK);
    open_row first = {0}, second = {0};
    check_equal(cflow_publisher_resume(&outer_publisher, NULL, &first).kind, CFLOW_STEP_VALUE);
    check_equal(cflow_publisher_resume(&nested_publisher, NULL, &second).kind, CFLOW_STEP_VALUE);
    check_equal(first.id, 7); check_equal(second.id, 8);
    check_false(connections[0].handle->native_active);
  }
  it("rejects exhausted completion capacity without a native call or leaked hold") {
    orm_error_t error; orm_error_init(&error);
    orm_owner *owner = &connections[0].handle->owner;
    const uint32_t budget = owner->max_dependents, before = owner->dependents;
    owner->max_dependents = before;
    const orm_status_t status = open_rows(0u, true, &error);
    owner->max_dependents = budget;
    check_equal(status, ORM_STATUS_LIMIT_EXCEEDED);
    check_equal(connections[0].open_calls, 0u);
    check_equal(owner->dependents, before);
    check_equal(connections[0].row->owner.dependents, 0u);
    check_false(connections[0].handle->native_active);
    check_equal(open_rows(0u, true, &error), ORM_STATUS_OK);
  }
  it("releases the reservation after a SQL prepare failure") {
    orm_error_t error; orm_error_init(&error);
    orm_query_release(connections[0].row); connections[0].row = NULL;
    check_equal(orm_raw(connections[0].handle, orm_view("select * from absent_open_table"),
                        &connections[0].row, &error), ORM_STATUS_OK);
    check_equal(open_rows(0u, true, &error), ORM_STATUS_SQL_ERROR);
    check_false(connections[0].handle->native_active);
    check_equal(open_rows(0u, false, &error), ORM_STATUS_OK);
  }
  it("records connection failure from shape setup before admitting sibling work") {
    orm_error_t error; orm_error_init(&error);
    shape_failure = ORM_STATUS_CONNECTION_ERROR;
    check_equal(open_rows(0u, true, &error), ORM_STATUS_CONNECTION_ERROR);
    shape_failure = ORM_STATUS_OK;
    check_equal(connections[0].handle->failure, ORM_STATUS_CONNECTION_ERROR);
    check_equal(cursor_destroys, 1u);
    check_equal(open_rows(0u, false, &error), ORM_STATUS_INVALID_STATE);
    check_equal(connections[0].open_calls, 1u);
  }
  it("retains the parent through failed shape cleanup after external release") {
    orm_error_t error; orm_error_init(&error);
    shape_failure = ORM_STATUS_TYPE_ERROR; release_stage = OPEN_DISPOSE;
    check_equal(open_rows(0u, true, &error), ORM_STATUS_TYPE_ERROR);
    check_equal(closes_inside_callback, 0u);
    check_equal(cursor_destroys, 1u); check_equal(connections[0].close_calls, 1u);
    check_false(cflow_publisher_valid(&outer_publisher));
  }
  it("transfers ownership to a successful Publisher after external release in shape setup") {
    orm_error_t error; orm_error_init(&error);
    release_stage = OPEN_SHAPE;
    check_equal(open_rows(0u, true, &error), ORM_STATUS_OK);
    check_equal(closes_inside_callback, 0u); check_equal(connections[0].close_calls, 0u);
    open_row row = {0};
    check_equal(cflow_publisher_resume(&outer_publisher, NULL, &row).kind, CFLOW_STEP_VALUE);
    check_equal(row.id, 7);
    drop_publisher(&outer_publisher); check_equal(connections[0].close_calls, 1u);
  }
  it("keeps the reservation through invalid-cursor cleanup") {
    orm_error_t error; orm_error_init(&error);
    check_equal(orm_query_open_command_flow(connections[0].command,
                  &connections[0].command_publisher, &error), ORM_STATUS_OK);
    invalid_cursor = true; armed = OPEN_DISPOSE; nested_operation = OPEN_NEST_COMMAND;
    check_equal(open_rows(0u, true, &error), ORM_STATUS_INTERNAL_ERROR);
    check_equal(probe_calls, 1u); check_equal(nested_entries, 0u);
    check_equal(cursor_destroys, 1u); check_false(connections[0].handle->native_active);
  }
}

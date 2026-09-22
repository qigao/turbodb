#include "orm_driver_backend_bridge.h"

#include <tinytest.h>

#include <string.h>

#define H(T) {(uint32_t)sizeof(T), ORM_DRIVER_ABI_VERSION}
#define TABLE(p) {(p), (uint32_t)sizeof(*(p)), 0u}

typedef struct adapter_plan {
  orm_driver_plan_meta_v1 meta;
  orm_driver_ordering_v1 ordering;
  orm_driver_assignment_v1 assignment;
} adapter_plan;

typedef struct fake_backend_state {
  uint32_t destroy_calls;
  uint32_t open_calls;
  uint32_t command_calls;
  uint32_t begin_calls;
  uint32_t cursor_destroy_calls;
  uint32_t cursor_cancel_calls;
  uint32_t cursor_shape_calls;
  uint32_t transaction_destroy_calls;
  uint32_t transaction_commit_calls;
  uint32_t transaction_rollback_calls;
  uint32_t transaction_savepoint_calls;
  uint64_t last_affected;
  char last_sql[64];
  char last_savepoint[32];
} fake_backend_state;

typedef struct fake_cursor {
  fake_backend_state *state;
  uint32_t emitted;
} fake_cursor;

typedef struct fake_transaction {
  fake_backend_state *state;
} fake_transaction;

static orm_status_t ORM_DRIVER_CALL plan_describe(
    const void *context, orm_driver_plan_meta_v1 *out,
    orm_error_t *error) {
  (void)error;
  *out = ((const adapter_plan *)context)->meta;
  return ORM_STATUS_OK;
}

static orm_status_t ORM_DRIVER_CALL plan_column(
    const void *context, uint64_t index,
    orm_driver_bytes_v1 *out, orm_error_t *error) {
  (void)context; (void)index; (void)out; (void)error;
  return ORM_STATUS_OUT_OF_RANGE;
}

static orm_status_t ORM_DRIVER_CALL plan_ordering(
    const void *context, orm_driver_ordering_v1 *out,
    orm_error_t *error) {
  (void)error;
  *out = ((const adapter_plan *)context)->ordering;
  return ORM_STATUS_OK;
}

static orm_status_t ORM_DRIVER_CALL plan_assignment(
    const void *context, uint64_t index,
    orm_driver_assignment_v1 *out, orm_error_t *error) {
  (void)error;
  if (index != 0u) return ORM_STATUS_OUT_OF_RANGE;
  *out = ((const adapter_plan *)context)->assignment;
  return ORM_STATUS_OK;
}

static orm_status_t ORM_DRIVER_CALL plan_predicate(
    const void *context, uint64_t index,
    orm_driver_predicate_v1 *out, orm_error_t *error) {
  (void)context; (void)index; (void)out; (void)error;
  return ORM_STATUS_OUT_OF_RANGE;
}

static orm_status_t ORM_DRIVER_CALL plan_parameter(
    const void *context, uint64_t index,
    orm_driver_value_v1 *out, orm_error_t *error) {
  (void)context; (void)index; (void)out; (void)error;
  return ORM_STATUS_OUT_OF_RANGE;
}

static const orm_driver_plan_metadata_ops_v1 plan_metadata_ops = {
    H(orm_driver_plan_metadata_ops_v1),
    plan_describe, plan_column, plan_ordering};
static const orm_driver_plan_value_ops_v1 plan_value_ops = {
    H(orm_driver_plan_value_ops_v1),
    plan_assignment, plan_predicate, plan_parameter};

static orm_driver_limits_v1 adapter_limits(void) {
  const orm_driver_limits_v1 limits = {
      H(orm_driver_limits_v1),
      8u, 8u, 8u, 8u, 1024u, 1024u, 16u, 4096u};
  return limits;
}

static orm_driver_plan_view_v1 adapter_view(adapter_plan *plan) {
  const orm_driver_plan_view_v1 view = {
      H(orm_driver_plan_view_v1), plan,
      TABLE(&plan_metadata_ops), TABLE(&plan_value_ops)};
  return view;
}

static void adapter_plan_raw(adapter_plan *plan, const char *sql) {
  memset(plan, 0, sizeof(*plan));
  plan->meta.header = (orm_driver_header_v1)H(orm_driver_plan_meta_v1);
  plan->meta.kind = ORM_DRIVER_PLAN_RAW_SQL;
  plan->meta.raw_sql = (orm_driver_bytes_v1){sql, strlen(sql)};
  plan->ordering.header =
      (orm_driver_header_v1)H(orm_driver_ordering_v1);
}

static void adapter_plan_insert(adapter_plan *plan) {
  memset(plan, 0, sizeof(*plan));
  plan->meta.header = (orm_driver_header_v1)H(orm_driver_plan_meta_v1);
  plan->meta.kind = ORM_DRIVER_PLAN_INSERT;
  plan->meta.table = (orm_driver_bytes_v1){"items", 5u};
  plan->meta.assignment_count = 1u;
  plan->ordering.header =
      (orm_driver_header_v1)H(orm_driver_ordering_v1);
  plan->assignment.header =
      (orm_driver_header_v1)H(orm_driver_assignment_v1);
  plan->assignment.column = (orm_driver_bytes_v1){"value", 5u};
  plan->assignment.value.header =
      (orm_driver_header_v1)H(orm_driver_value_v1);
  plan->assignment.value.kind = ORM_VALUE_INT64;
  plan->assignment.value.data.sint = 7;
}

static cserde_status fake_reader_next(void *context, cserde_token *out) {
  (void)context;
  memset(out, 0, sizeof(*out));
  return CSERDE_DONE;
}

static const cserde_reader_ops fake_reader_ops = {
    sizeof(cserde_reader_ops), CSERDE_READER_OPS_ABI_VERSION,
    fake_reader_next};

static orm_row_cursor_step fake_cursor_next(
    void *context, cserde_reader *reader) {
  fake_cursor *cursor = (fake_cursor *)context;
  orm_row_cursor_step step = ORM_ROW_CURSOR_STEP_INIT;
  if (cursor->emitted == 0u) {
    check_equal(cserde_reader_init(reader, &fake_reader_ops, cursor),
                CSERDE_OK);
    cursor->emitted = 1u;
    step.kind = ORM_ROW_CURSOR_ROW_AND_DONE;
  } else {
    step.kind = ORM_ROW_CURSOR_DONE;
  }
  return step;
}

static void fake_cursor_cancel(void *context) {
  fake_cursor *cursor = (fake_cursor *)context;
  ++cursor->state->cursor_cancel_calls;
}

static void fake_cursor_destroy(void *context) {
  fake_cursor *cursor = (fake_cursor *)context;
  ++cursor->state->cursor_destroy_calls;
  free(cursor);
}

static orm_status_t fake_cursor_shape(
    void *context, const cmeta_data_desc *shape,
    orm_error_t *error) {
  fake_cursor *cursor = (fake_cursor *)context;
  if (shape == NULL) return ORM_STATUS_INVALID_ARGUMENT;
  ++cursor->state->cursor_shape_calls;
  orm_error_init(error);
  return ORM_STATUS_OK;
}

static orm_status_t fake_cursor_columns(void *context, uint64_t *out) {
  (void)context;
  if (out == NULL) return ORM_STATUS_INVALID_ARGUMENT;
  *out = 2u;
  return ORM_STATUS_OK;
}

static const orm_row_cursor_ops fake_cursor_ops = {
    sizeof(orm_row_cursor_ops), ORM_ROW_CURSOR_OPS_ABI_VERSION,
    "fake", fake_cursor_next, fake_cursor_cancel, fake_cursor_destroy,
    fake_cursor_shape, fake_cursor_columns};

static orm_status_t fake_open_cursor(
    void *context, const orm_query_plan *plan,
    const orm_limits *limits, orm_row_cursor *out,
    orm_error_t *error) {
  fake_backend_state *state = (fake_backend_state *)context;
  (void)limits;
  if (out != NULL) memset(out, 0, sizeof(*out));
  if (state == NULL || plan == NULL || out == NULL ||
      plan->kind != ORM_QUERY_RAW)
    return ORM_STATUS_INVALID_ARGUMENT;
  ++state->open_calls;
  (void)snprintf(state->last_sql, sizeof(state->last_sql), "%s",
                 plan->raw_sql != NULL ? plan->raw_sql : "");
  fake_cursor *cursor = (fake_cursor *)calloc(1u, sizeof(*cursor));
  if (cursor == NULL) return ORM_STATUS_OUT_OF_MEMORY;
  cursor->state = state;
  out->ops = &fake_cursor_ops;
  out->context = cursor;
  orm_error_init(error);
  return ORM_STATUS_OK;
}

static orm_status_t fake_execute_command(
    void *context, const orm_query_plan *plan,
    const orm_limits *limits, uint64_t *affected,
    orm_error_t *error) {
  fake_backend_state *state = (fake_backend_state *)context;
  (void)limits;
  if (state == NULL || plan == NULL || affected == NULL)
    return ORM_STATUS_INVALID_ARGUMENT;
  ++state->command_calls;
  state->last_affected = 11u;
  *affected = 11u;
  orm_error_init(error);
  return ORM_STATUS_OK;
}

static void fake_transaction_destroy(void *context) {
  fake_transaction *transaction = (fake_transaction *)context;
  ++transaction->state->transaction_destroy_calls;
  free(transaction);
}

static orm_status_t fake_transaction_open(
    void *context, const orm_query_plan *plan,
    const orm_limits *limits, orm_row_cursor *out,
    orm_error_t *error) {
  fake_transaction *transaction = (fake_transaction *)context;
  return fake_open_cursor(
      transaction->state, plan, limits, out, error);
}

static orm_status_t fake_transaction_execute(
    void *context, const orm_query_plan *plan,
    const orm_limits *limits, uint64_t *affected,
    orm_error_t *error) {
  fake_transaction *transaction = (fake_transaction *)context;
  const orm_status_t status = fake_execute_command(
      transaction->state, plan, limits, affected, error);
  if (status == ORM_STATUS_OK) {
    *affected = 22u;
    transaction->state->last_affected = 22u;
  }
  return status;
}

static orm_status_t fake_transaction_commit(
    void *context, orm_error_t *error) {
  fake_transaction *transaction = (fake_transaction *)context;
  ++transaction->state->transaction_commit_calls;
  orm_error_init(error);
  return ORM_STATUS_OK;
}

static orm_status_t fake_transaction_rollback(
    void *context, orm_error_t *error) {
  fake_transaction *transaction = (fake_transaction *)context;
  ++transaction->state->transaction_rollback_calls;
  orm_error_init(error);
  return ORM_STATUS_OK;
}

static orm_status_t fake_savepoint(
    void *context, vstr name, orm_error_t *error) {
  fake_transaction *transaction = (fake_transaction *)context;
  ++transaction->state->transaction_savepoint_calls;
  const size_t n =
      name.len < sizeof(transaction->state->last_savepoint) - 1u
          ? name.len : sizeof(transaction->state->last_savepoint) - 1u;
  memcpy(transaction->state->last_savepoint, name.data, n);
  transaction->state->last_savepoint[n] = '\0';
  orm_error_init(error);
  return ORM_STATUS_OK;
}

static const orm_transaction_backend_ops fake_transaction_ops = {
    sizeof(orm_transaction_backend_ops),
    ORM_TRANSACTION_BACKEND_OPS_ABI_VERSION,
    fake_transaction_destroy,
    fake_transaction_open,
    fake_transaction_execute,
    fake_transaction_commit,
    fake_transaction_rollback,
    fake_savepoint,
    fake_savepoint,
    fake_savepoint};

static orm_status_t fake_begin_transaction(
    void *context, orm_isolation_t isolation,
    orm_transaction_backend *out, orm_error_t *error) {
  fake_backend_state *state = (fake_backend_state *)context;
  if (out != NULL) memset(out, 0, sizeof(*out));
  if (state == NULL || out == NULL ||
      isolation != ORM_ISOLATION_SERIALIZABLE)
    return ORM_STATUS_INVALID_ARGUMENT;
  ++state->begin_calls;
  fake_transaction *transaction =
      (fake_transaction *)calloc(1u, sizeof(*transaction));
  if (transaction == NULL) return ORM_STATUS_OUT_OF_MEMORY;
  transaction->state = state;
  out->ops = &fake_transaction_ops;
  out->context = transaction;
  orm_error_init(error);
  return ORM_STATUS_OK;
}

static void fake_backend_destroy(void *context) {
  fake_backend_state *state = (fake_backend_state *)context;
  ++state->destroy_calls;
}

static const orm_backend_ops fake_backend_ops = {
    sizeof(orm_backend_ops), ORM_BACKEND_OPS_ABI_VERSION,
    fake_backend_destroy, fake_open_cursor,
    fake_execute_command, fake_begin_transaction};

static fake_backend_state backend_state;

static orm_status_t fake_factory(
    const orm_config_t *config, const orm_limits *limits,
    orm_backend *out, orm_error_t *error) {
  (void)config;
  (void)limits;
  if (out == NULL) return ORM_STATUS_INVALID_ARGUMENT;
  memset(&backend_state, 0, sizeof(backend_state));
  out->ops = &fake_backend_ops;
  out->context = &backend_state;
  orm_error_init(error);
  return ORM_STATUS_OK;
}

static orm_driver_connection_v1 connection(void) {
  orm_driver_connection_v1 out;
  orm_config_t config;
  orm_driver_limits_v1 limits = adapter_limits();
  orm_error_t error;
  memset(&out, 0, sizeof(out));
  orm_config(&config);
  config.driver = orm_view("fake");
  check_equal(orm_driver_backend_connection_create(
                  fake_factory, &config, &limits, &out, &error),
              ORM_STATUS_OK);
  return out;
}

spec("Driver backend DTO bridge") {
  it("wraps command execution with a materialized local plan") {
    orm_driver_connection_v1 c = connection();
    const orm_driver_connection_ops_v1 *ops =
        (const orm_driver_connection_ops_v1 *)c.ops.data;
    adapter_plan plan;
    adapter_plan_insert(&plan);
    orm_driver_plan_view_v1 view = adapter_view(&plan);
    orm_driver_limits_v1 limits = adapter_limits();
    orm_error_t error;
    uint64_t affected = 0u;

    check_equal(ops->execute_command(
                    c.context, &view, &limits, &affected, &error),
                ORM_STATUS_OK);
    check_equal(affected, UINT64_C(11));
    check_equal(backend_state.command_calls, 1u);
    ops->destroy(c.context);
    check_equal(backend_state.destroy_calls, 1u);
  }

  it("maps private row cursors into Driver cursor DTOs") {
    orm_driver_connection_v1 c = connection();
    const orm_driver_connection_ops_v1 *ops =
        (const orm_driver_connection_ops_v1 *)c.ops.data;
    adapter_plan plan;
    adapter_plan_raw(&plan, "select 1");
    orm_driver_plan_view_v1 view = adapter_view(&plan);
    orm_driver_limits_v1 limits = adapter_limits();
    orm_driver_cursor_v1 cursor;
    orm_error_t error;
    memset(&cursor, 0, sizeof(cursor));

    check_equal(ops->open_cursor(
                    c.context, &view, &limits, &cursor, &error),
                ORM_STATUS_OK);
    const orm_driver_cursor_ops_v1 *cursor_ops =
        (const orm_driver_cursor_ops_v1 *)cursor.ops.data;
    orm_driver_step_v1 step;
    memset(&step, 0, sizeof(step));
    step.header = (orm_driver_header_v1)H(orm_driver_step_v1);
    cserde_reader reader;
    memset(&reader, 0, sizeof(reader));
    check_equal(cursor_ops->next(
                    cursor.context, &reader, &step, &error),
                ORM_STATUS_OK);
    check_equal(step.kind, ORM_DRIVER_STEP_ROW_AND_DONE);
    check_true(cserde_reader_valid(&reader));

    uint64_t columns = 0u;
    check_equal(cursor_ops->column_count(
                    cursor.context, &columns, &error),
                ORM_STATUS_OK);
    check_equal(columns, UINT64_C(2));
    cursor_ops->cancel(cursor.context);
    check_equal(backend_state.cursor_cancel_calls, 1u);
    cursor_ops->destroy(cursor.context);
    check_equal(backend_state.cursor_destroy_calls, 1u);
    ops->destroy(c.context);
  }

  it("wraps transaction command savepoint commit and destruction") {
    orm_driver_connection_v1 c = connection();
    const orm_driver_connection_ops_v1 *ops =
        (const orm_driver_connection_ops_v1 *)c.ops.data;
    orm_driver_transaction_v1 transaction;
    orm_error_t error;
    memset(&transaction, 0, sizeof(transaction));

    check_equal(ops->begin_transaction(
                    c.context, ORM_ISOLATION_SERIALIZABLE,
                    &transaction, &error),
                ORM_STATUS_OK);
    const orm_driver_transaction_ops_v1 *tx_ops =
        (const orm_driver_transaction_ops_v1 *)transaction.ops.data;

    adapter_plan plan;
    adapter_plan_insert(&plan);
    orm_driver_plan_view_v1 view = adapter_view(&plan);
    orm_driver_limits_v1 limits = adapter_limits();
    uint64_t affected = 0u;
    check_equal(tx_ops->execute_command(
                    transaction.context, &view, &limits,
                    &affected, &error),
                ORM_STATUS_OK);
    check_equal(affected, UINT64_C(22));

    const orm_driver_bytes_v1 name = {"sp1", 3u};
    check_equal(tx_ops->savepoint(
                    transaction.context, name, &error),
                ORM_STATUS_OK);
    check_equal(strcmp(backend_state.last_savepoint, "sp1"), 0);
    check_equal(tx_ops->commit(transaction.context, &error),
                ORM_STATUS_OK);
    check_equal(backend_state.transaction_commit_calls, 1u);
    tx_ops->destroy(transaction.context);
    check_equal(backend_state.transaction_destroy_calls, 1u);
    ops->destroy(c.context);
  }

  it("clears connection output when the backend factory fails") {
    orm_driver_connection_v1 out;
    orm_driver_limits_v1 limits = adapter_limits();
    orm_config_t config;
    orm_error_t error;
    memset(&out, 0xa5, sizeof(out));
    orm_config(&config);
    config.driver = orm_view("fake");

    check_equal(orm_driver_backend_connection_create(
                    NULL, &config, &limits, &out, &error),
                ORM_STATUS_INVALID_ARGUMENT);
    const orm_driver_connection_v1 zero = {0};
    check_equal(memcmp(&out, &zero, sizeof(out)), 0);
  }
}

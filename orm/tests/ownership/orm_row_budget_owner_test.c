/* #58 consumer contract: real SQLite/ORM ownership with observed native calls.
 * Nested shapes are tested at preparation; SQLite need not support nested rows
 * to prove they are rejected before dispatch. Successful reads use a flat row. */
#define TINYTEST_NO_MAIN
#include "orm_internal.h"
#include "orm_row_allocator_probe.h"
#include <tinytest.h>
#include <string.h>

typedef struct budget_shape {
  cmeta_type_identity identity;
  cmeta_type_desc type;
  cmeta_field_desc fields[9];
  cmeta_struct_desc layout;
  cmeta_data_field_desc values[9];
  cmeta_data_struct_shape record;
  cmeta_data_desc data;
} budget_shape;

static void budget_record(budget_shape *out, const char *name, size_t count,
                           const cmeta_data_desc *child) {
  static const char *const names[] = {"id", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8"};
  memset(out, 0, sizeof(*out));
  out->identity = (cmeta_type_identity)CMETA_TYPE_ID_ATOM_INIT(name);
  out->type = (cmeta_type_desc){
      .name = name, .size = count != 0u ? count * child->storage_type->size : 1u,
      .align = count != 0u ? child->storage_type->align : 1u,
      .kind = CMETA_T_OBJECT, .identity = &out->identity};
  for (size_t i = 0u; i < count; ++i) {
    const size_t offset = i * child->storage_type->size;
    out->fields[i] = (cmeta_field_desc){
        .name = names[i], .type_name = child->storage_type->name, .offset = offset,
        .size = child->storage_type->size, .align = child->storage_type->align,
        .type = child->storage_type};
    out->values[i] = (cmeta_data_field_desc){names[i], names[i], offset, child};
  }
  out->layout = (cmeta_struct_desc){name, out->type.size, out->type.align, out->fields, count};
  out->record = (cmeta_data_struct_shape){&out->layout, out->values, count};
  out->data = (cmeta_data_desc){
      .struct_size = sizeof(cmeta_data_desc), .abi_version = CMETA_DATA_DESC_ABI_VERSION,
      .stable_id = name, .display_name = name, .kind = CMETA_DATA_STRUCT,
      .storage_type = &out->type, .shape = &out->record};
}

static budget_shape flat, nested, empty, nested_empty, siblings, wide;
static orm_connection_t *connection;
static orm_query_t *query;
static orm_transaction_t *transaction;
static cflow_publisher publisher;
static orm_row_publisher_prepared *prepared;
static orm_backend_ops native_backend, observed_backend;
static orm_transaction_backend_ops native_transaction, observed_transaction;
static orm_row_cursor_ops native_cursor, observed_cursor;
static size_t opens, configures, nexts, cancels, destroys;

static orm_status_t observe_configure(void *context, const cmeta_data_desc *shape,
                                      orm_error_t *error) {
  ++configures;
  return native_cursor.configure_shape != NULL
      ? native_cursor.configure_shape(context, shape, error) : ORM_STATUS_OK;
}
static orm_row_cursor_step observe_next(void *context, cserde_reader *reader) {
  ++nexts;
  return native_cursor.next(context, reader);
}
static void observe_cancel(void *context) { ++cancels; native_cursor.cancel(context); }
static void observe_destroy(void *context) { ++destroys; native_cursor.destroy(context); }
static void observe_cursor(orm_row_cursor *cursor, orm_status_t status) {
  if (status != ORM_STATUS_OK) return;
  native_cursor = *cursor->ops;
  observed_cursor = native_cursor;
  observed_cursor.configure_shape = observe_configure;
  observed_cursor.next = observe_next;
  observed_cursor.cancel = observe_cancel;
  observed_cursor.destroy = observe_destroy;
  cursor->ops = &observed_cursor;
}
static orm_status_t observe_open(void *context, const orm_query_plan *plan,
    const orm_limits *limits, orm_row_cursor *cursor, orm_error_t *error) {
  ++opens;
  const orm_status_t status = native_backend.open_cursor(context, plan, limits, cursor, error);
  observe_cursor(cursor, status);
  return status;
}
static orm_status_t observe_transaction_open(void *context, const orm_query_plan *plan,
    const orm_limits *limits, orm_row_cursor *cursor, orm_error_t *error) {
  ++opens;
  const orm_status_t status = native_transaction.open_cursor(context, plan, limits, cursor, error);
  observe_cursor(cursor, status);
  return status;
}
static void begin_transaction(void) {
  orm_error_t error;
  orm_error_init(&error);
  check_equal(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                    &transaction, &error), ORM_STATUS_OK);
  native_transaction = *transaction->backend.ops;
  observed_transaction = native_transaction;
  observed_transaction.open_cursor = observe_transaction_open;
  transaction->backend.ops = &observed_transaction;
}
static orm_status_t open_rows(const orm_flow_config_t *config, orm_error_t *error) {
  return transaction != NULL
      ? orm_query_open_flow_in_transaction(query, transaction, config, &publisher, error)
      : orm_query_open_flow(query, config, &publisher, error);
}
static orm_flow_config_t make_config(const cmeta_data_desc *shape, size_t scratch,
                                     size_t depth) {
  orm_flow_config_t config;
  orm_flow_config(&config, shape);
  config.scratch_bytes = scratch;
  config.max_depth = depth;
  config.max_container_items = 0u; /* Static fields are not dynamic collection items. */
  config.max_buffer_bytes = 0u;    /* These rows own no payload. */
  return config;
}
static void check_usable(void) {
  orm_error_t error;
  orm_flow_config_t config = make_config(&flat.data, 1u, 1u);
  int row = 0;
  orm_error_init(&error);
  check_equal(open_rows(&config, &error), ORM_STATUS_OK);
  check_equal(opens, 1u);
  check_equal(configures, 1u);
  check_equal(cflow_publisher_resume(&publisher, NULL, &row).kind, CFLOW_STEP_VALUE);
  check_equal(row, 7);
  check_equal(nexts, 1u);
  cflow_publisher_destroy(&publisher);
  memset(&publisher, 0, sizeof(publisher));
  check_equal(destroys, 1u);
  check_equal(cancels, 1u);
  check_false(connection->native_active);
  check_equal(connection->failure, ORM_STATUS_OK);
  check_equal(query->owner.dependents, 0u);
  if (transaction != NULL) check_equal(transaction->owner.dependents, 0u);
}

static void require_refusal(const cmeta_data_desc *shape, size_t scratch,
                            size_t depth, size_t fail_at, orm_status_t expected) {
  const uint32_t connection_holds = connection->owner.dependents;
  const uint32_t query_refs = query->owner.references;
  const uint32_t transaction_refs = transaction != NULL ? transaction->owner.references : 0u;
  orm_flow_config_t config = make_config(shape, scratch, depth);
  orm_error_t error;
  orm_error_init(&error);
  orm_row_allocator_arm(fail_at);
  const orm_status_t status = open_rows(&config, &error);
  orm_row_allocator_disarm(); /* Disable before assertions or any cleanup. */
  const orm_row_allocator_observation seen = orm_row_allocator_observe();
  check_equal(status, expected);
  check_equal(error.status, expected);
  check_true(error.message[0] != '\0');
  check_null(publisher.self);
  check_equal(opens, 0u);
  check_equal(configures, 0u);
  check_equal(nexts, 0u);
  check_equal(cancels, 0u);
  check_equal(destroys, 0u);
  check_equal(seen.live, 0u);
  check_equal(seen.failures, fail_at != 0u ? 1u : 0u);
  if (fail_at != 0u) check_equal(seen.calls, fail_at);
  check_equal(connection->owner.dependents, connection_holds);
  check_equal(connection->owner.phase, ORM_OWNER_OPEN);
  check_equal(connection->failure, ORM_STATUS_OK);
  check_false(connection->native_active);
  check_null(connection->cleanup_head);
  check_equal(query->owner.references, query_refs);
  check_equal(query->owner.dependents, 0u);
  check_equal(query->owner.phase, ORM_OWNER_OPEN);
  if (transaction != NULL) {
    check_equal(transaction->owner.references, transaction_refs);
    check_equal(transaction->owner.dependents, 0u);
    check_equal(transaction->owner.phase, ORM_OWNER_OPEN);
    check_equal(transaction->state, ORM_TRANSACTION_ACTIVE);
  }
  /* A later, explicitly requested valid operation proves refusal did not poison
   * the connection or strand its reservation. This is not retrying failed CI. */
  check_usable();
  check_equal(connection->owner.dependents, connection_holds);
}

static void require_prepared(const cmeta_data_desc *shape, size_t scratch, size_t depth) {
  orm_row_publisher_config config = ORM_ROW_PUBLISHER_CONFIG_INIT(shape, scratch, depth, 0u, 0u);
  orm_error_t error;
  orm_error_init(&error);
  orm_row_allocator_arm(0u);
  const orm_status_t status = orm_row_publisher_prepare(&config, &prepared, &error);
  orm_row_allocator_disarm();
  check_equal(status, ORM_STATUS_OK);
  check_equal(error.status, ORM_STATUS_OK);
  check_not_null(prepared);
  const orm_row_allocator_observation seen = orm_row_allocator_observe();
  check_equal(seen.calls, 3u); /* Probe, prepared state, aligned decode workspace. */
  check_equal(seen.failures, 0u);
  check_equal(seen.live, 2u); /* Probe has already been freed. */
  check_equal(opens + configures + nexts + cancels + destroys, 0u);
  orm_row_publisher_prepared_destroy(prepared);
  prepared = NULL;
  check_equal(orm_row_allocator_observe().live, 0u);
}

spec("ORM row preparation rejects budgets and allocation failure before native dispatch") {
  before_each() {
    connection = NULL; query = NULL; transaction = NULL; prepared = NULL;
    memset(&publisher, 0, sizeof(publisher));
    opens = configures = nexts = cancels = destroys = 0u;
    budget_record(&flat, "budget.Flat", 1u, &cmeta_data_int);
    budget_record(&nested, "budget.Nested", 1u, &flat.data);
    budget_record(&empty, "budget.Empty", 0u, &cmeta_data_int);
    budget_record(&nested_empty, "budget.NestedEmpty", 1u, &empty.data);
    budget_record(&siblings, "budget.Siblings", 2u, &flat.data);
    budget_record(&wide, "budget.Wide", 9u, &cmeta_data_int);
    orm_config_t config;
    orm_error_t error;
    const orm_option_t filename = {orm_view("filename"), orm_view(":memory:")};
    orm_config(&config); orm_error_init(&error);
    config.driver = orm_view("sqlite"); config.options = &filename; config.option_count = 1u;
    check_equal(orm_connect(&config, &connection, &error), ORM_STATUS_OK);
    native_backend = *connection->backend.ops;
    observed_backend = native_backend;
    observed_backend.open_cursor = observe_open;
    connection->backend.ops = &observed_backend;
    check_equal(orm_raw(connection, orm_view("select 7 as id"), &query, &error), ORM_STATUS_OK);
  }
  after_each() {
    orm_row_allocator_disarm();
    orm_row_publisher_prepared_destroy(prepared); prepared = NULL;
    if (cflow_publisher_valid(&publisher)) cflow_publisher_destroy(&publisher);
    memset(&publisher, 0, sizeof(publisher));
    orm_query_release(query); query = NULL;
    orm_transaction_release(transaction); transaction = NULL;
    orm_connection_release(connection); connection = NULL;
    check_equal(orm_row_allocator_observe().live, 0u);
  }
  it("rejects nested active bitmaps one byte above caller scratch") {
    require_refusal(&nested.data, 1u, 2u, 0u, ORM_STATUS_LIMIT_EXCEEDED);
  }
  it("rejects one extra container level before backend open") {
    require_refusal(&nested.data, 2u, 1u, 0u, ORM_STATUS_LIMIT_EXCEEDED);
  }
  it("counts empty nested containers even without scalar leaves") {
    require_refusal(&nested_empty.data, 1u, 1u, 0u, ORM_STATUS_LIMIT_EXCEEDED);
  }
  it("rejects a ninth field exceeding one caller bitmap byte") {
    require_refusal(&wide.data, 1u, 1u, 0u, ORM_STATUS_LIMIT_EXCEEDED);
  }
  it("accepts exact nested scratch and container depth with no native I/O") {
    require_prepared(&nested.data, 2u, 2u);
  }
  it("uses peak rather than summed sibling bitmap scratch") {
    require_prepared(&siblings.data, 2u, 2u);
  }
  it("accepts an empty child at its exact container limit") {
    require_prepared(&nested_empty.data, 1u, 2u);
  }
  it("accepts a scalar root with zero bitmap and collection budgets") {
    require_prepared(&cmeta_data_int, 0u, 1u);
  }
  it("rejects a transaction nested budget without leaking transaction holds") {
    begin_transaction();
    require_refusal(&nested.data, 1u, 2u, 0u, ORM_STATUS_LIMIT_EXCEEDED);
  }
  it("rejects depth addition overflow without native dispatch") {
    require_refusal(&flat.data, 1u, SIZE_MAX, 0u, ORM_STATUS_LIMIT_EXCEEDED);
    check_equal(orm_row_allocator_observe().calls, 0u);
  }
  it("rejects traversal probe allocation failure and retains usable owners") {
    require_refusal(&flat.data, 1u, 1u, 1u, ORM_STATUS_OUT_OF_MEMORY);
  }
  it("releases the probe after prepared-state allocation failure") {
    require_refusal(&flat.data, 1u, 1u, 2u, ORM_STATUS_OUT_OF_MEMORY);
  }
  it("releases prepared state after decode-workspace allocation failure") {
    require_refusal(&flat.data, 1u, 1u, 3u, ORM_STATUS_OUT_OF_MEMORY);
  }
  it("unwinds transaction admission after probe allocation failure") {
    begin_transaction();
    require_refusal(&flat.data, 1u, 1u, 1u, ORM_STATUS_OUT_OF_MEMORY);
  }
  it("unwinds transaction admission after state allocation failure") {
    begin_transaction();
    require_refusal(&flat.data, 1u, 1u, 2u, ORM_STATUS_OUT_OF_MEMORY);
  }
  it("unwinds transaction admission after workspace allocation failure") {
    begin_transaction();
    require_refusal(&flat.data, 1u, 1u, 3u, ORM_STATUS_OUT_OF_MEMORY);
  }
}

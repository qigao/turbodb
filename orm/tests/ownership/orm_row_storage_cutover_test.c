/* #52 consumer contract: real SQLite and ORM, no alternate binder in the test. */
#define TINYTEST_NO_MAIN
#include "orm_internal.h"
#include <cmeta/struct.h>
#include <salts_cmeta_data.h>
#include <data_bind.h>
#include <tinytest.h>
#include <stdio.h>
#include <string.h>

enum { ROW_ZERO_TAG = 0x6b29, ROW_COUNT = 2, ROW_PAYLOAD_LIMIT = 64 };
typedef struct RowBuffer { tstr text; unsigned tag; } RowBuffer;
typedef struct CutoverRow { RowBuffer payload; int id; } CutoverRow;
static CutoverRow rows[ROW_COUNT];
static size_t assignments, allocations, releases;
static size_t fail_assignment, injected_failures;
static orm_connection_t *connection;
static orm_query_t *query;
static cflow_publisher source;

static bool row_buffer_zero(const void *object) {
  const RowBuffer *buffer = (const RowBuffer *)object;
  return buffer != NULL && buffer->tag == ROW_ZERO_TAG &&
         salts_tstr_cmeta_buffer_ops.is_zero(&buffer->text);
}
static cmeta_status row_buffer_init(void *object) {
  RowBuffer *buffer = (RowBuffer *)object;
  if (buffer == NULL) return CMETA_INVALID_ARGUMENT;
  const cmeta_status status = salts_tstr_cmeta_buffer_ops.init_zero(&buffer->text);
  if (status == CMETA_OK) buffer->tag = ROW_ZERO_TAG;
  return status;
}
static cmeta_status row_buffer_assign(void *object, const unsigned char *data,
                                      size_t size, size_t max_bytes) {
  RowBuffer *buffer = (RowBuffer *)object;
  if (!row_buffer_zero(buffer)) return CMETA_INVALID_ARGUMENT;
  ++assignments;
  if (fail_assignment != 0u && assignments == fail_assignment) {
    ++injected_failures;
    return CMETA_OUT_OF_MEMORY;
  }
  const cmeta_status status = salts_tstr_cmeta_buffer_ops.assign(
      &buffer->text, data, size, max_bytes);
  if (!salts_tstr_cmeta_buffer_ops.is_zero(&buffer->text)) ++allocations;
  return status;
}
static void row_buffer_restore(void *object) {
  RowBuffer *buffer = (RowBuffer *)object;
  if (buffer == NULL) return;
  if (!salts_tstr_cmeta_buffer_ops.is_zero(&buffer->text)) ++releases;
  salts_tstr_cmeta_buffer_ops.restore_zero(&buffer->text);
  buffer->tag = ROW_ZERO_TAG;
}
static cmeta_status row_buffer_read(const void *object, const unsigned char **data,
                                    size_t *size) {
  const RowBuffer *buffer = (const RowBuffer *)object;
  if (buffer == NULL || buffer->tag != ROW_ZERO_TAG) return CMETA_INVALID_ARGUMENT;
  return salts_tstr_cmeta_buffer_ops.read(&buffer->text, data, size);
}
static void row_buffer_move(void *destination, void *source_) {
  RowBuffer *to = (RowBuffer *)destination;
  RowBuffer *from = (RowBuffer *)source_;
  salts_tstr_cmeta_buffer_ops.move(&to->text, &from->text);
  to->tag = from->tag = ROW_ZERO_TAG;
}
static void row_move(void *destination, void *source_) {
  CutoverRow *to = (CutoverRow *)destination;
  CutoverRow *from = (CutoverRow *)source_;
  /* move_construct receives raw destination storage. */
  to->payload = (RowBuffer){NULL, ROW_ZERO_TAG};
  row_buffer_move(&to->payload, &from->payload);
  to->id = from->id;
  from->id = 0;
}
static void row_destroy(void *object) {
  CutoverRow *row = (CutoverRow *)object;
  row_buffer_restore(&row->payload);
  row->id = 0;
}

static const cmeta_type_identity buffer_identity =
    CMETA_TYPE_ID_ATOM_INIT("orm.cutover.TaggedBuffer");
static const cmeta_type_desc buffer_type = {
    .name = "RowBuffer", .size = sizeof(RowBuffer), .align = _Alignof(RowBuffer),
    .kind = CMETA_T_OBJECT, .identity = &buffer_identity};
static const cmeta_data_buffer_ops buffer_ops = {
    .struct_size = sizeof(cmeta_data_buffer_ops),
    .abi_version = CMETA_DATA_BUFFER_OPS_ABI_VERSION,
    .storage_type = &buffer_type, .ownership = CMETA_DATA_BUFFER_OWNED,
    .is_zero = row_buffer_zero, .assign = row_buffer_assign,
    .restore_zero = row_buffer_restore, .read = row_buffer_read,
    .init_zero = row_buffer_init, .move = row_buffer_move};
static const cmeta_type_identity row_identity =
    CMETA_TYPE_ID_ATOM_INIT("orm.cutover.Row");
static const cmeta_type_traits row_traits = {
    .flags = CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY,
    .move_construct = row_move, .destroy = row_destroy};
static const cmeta_type_desc row_type = {
    .name = "CutoverRow", .size = sizeof(CutoverRow), .align = _Alignof(CutoverRow),
    .kind = CMETA_T_OBJECT, .traits = &row_traits, .identity = &row_identity};
static cmeta_data_desc payload_data;
static cmeta_field_desc layout_fields[2];
static cmeta_data_field_desc data_fields[2];
static const cmeta_struct_desc row_layout = {
    "CutoverRow", sizeof(CutoverRow), _Alignof(CutoverRow), layout_fields, 2u};
static const cmeta_data_struct_shape row_shape = {&row_layout, data_fields, 2u};
static const cmeta_data_desc row_data = {
    .struct_size = sizeof(cmeta_data_desc), .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orm.cutover.Row.data", .display_name = "CutoverRow",
    .kind = CMETA_DATA_STRUCT, .storage_type = &row_type, .shape = &row_shape};
static const unsigned char first_payload[] = {'A', 0u, 'B'};
static const unsigned char second_payload[] = {'C', 'D'};

static void set_payload_kind(cmeta_data_kind kind) {
  /* Imported Salts addresses are bound at runtime, including on Windows. */
  payload_data = salts_tstr_cmeta_data;
  payload_data.kind = kind;
  payload_data.stable_id = kind == CMETA_DATA_STRING ? "orm.cutover.text" : "orm.cutover.bytes";
  payload_data.storage_type = &buffer_type;
  payload_data.buffer_ops = &buffer_ops;
  layout_fields[0] = (cmeta_field_desc){
      .name = "payload", .type_name = "RowBuffer", .offset = offsetof(CutoverRow, payload),
      .size = sizeof(RowBuffer), .align = _Alignof(RowBuffer), .type = &buffer_type};
  layout_fields[1] = (cmeta_field_desc){
      .name = "id", .type_name = "int", .offset = offsetof(CutoverRow, id),
      .size = sizeof(int), .align = _Alignof(int), .type = cmeta_data_int.storage_type};
  data_fields[0] = (cmeta_data_field_desc){
      "orm.cutover.Row.payload", "payload", offsetof(CutoverRow, payload), &payload_data};
  data_fields[1] = (cmeta_data_field_desc){
      "orm.cutover.Row.id", "id", offsetof(CutoverRow, id), &cmeta_data_int};
  check_true(cmeta_data_desc_valid(&payload_data));
  check_true(cmeta_data_buffer_ops_of(&payload_data) == &buffer_ops);
  check_true(cmeta_data_desc_valid(&row_data));
}
static void close_database(void) {
  if (cflow_publisher_valid(&source)) cflow_publisher_destroy(&source);
  memset(&source, 0, sizeof(source));
  orm_query_release(query); query = NULL;
  orm_connection_release(connection); connection = NULL;
}
static void open_query(cmeta_data_kind kind, const char *sql, size_t max_bytes) {
  orm_error_t error;
  orm_config_t config;
  orm_flow_config_t flow;
  orm_error_init(&error);
  orm_config(&config);
  const orm_option_t filename = {orm_view("filename"), orm_view(":memory:")};
  config.driver = orm_view("sqlite"); config.options = &filename; config.option_count = 1u;
  set_payload_kind(kind);
  check_equal(orm_connect(&config, &connection, &error), ORM_STATUS_OK);
  check_equal(orm_raw(connection, orm_view(sql), &query, &error), ORM_STATUS_OK);
  orm_flow_config(&flow, &row_data);
  flow.max_buffer_bytes = max_bytes;
  check_equal(orm_query_open_flow(query, &flow, &source, &error), ORM_STATUS_OK);
}
static void require_payload(const RowBuffer *buffer, const unsigned char *expected, size_t count) {
  const unsigned char *bytes = NULL;
  size_t size = 0u;
  check_equal(row_buffer_read(buffer, &bytes, &size), CMETA_OK);
  check_equal(size, count);
  check_not_null(bytes);
  check_equal(memcmp(bytes, expected, count), 0);
}
static void provider_control(cmeta_data_kind kind) {
  set_payload_kind(kind);
  check_equal(cmeta_data_buffer_init_zero(&payload_data, &rows[0].payload), CMETA_OK);
  check_equal(cmeta_data_buffer_init_zero(&payload_data, &rows[1].payload), CMETA_OK);
  check_true(row_buffer_zero(&rows[0].payload));
  check_equal(cmeta_data_buffer_assign(&payload_data, &rows[0].payload,
      first_payload, sizeof(first_payload), ROW_PAYLOAD_LIMIT), CMETA_OK);
  check_equal(cmeta_data_buffer_move(&payload_data, &rows[1].payload, &rows[0].payload), CMETA_OK);
  check_true(row_buffer_zero(&rows[0].payload));
  require_payload(&rows[1].payload, first_payload, sizeof(first_payload));
  check_equal(cmeta_data_buffer_restore_zero(&payload_data, &rows[1].payload), CMETA_OK);
  check_equal(cmeta_data_buffer_restore_zero(&payload_data, &rows[1].payload), CMETA_OK);
  check_true(row_buffer_zero(&rows[1].payload));
  check_equal(allocations, 1u);
  check_equal(releases, 1u);
}
static void row_lifetime(cmeta_data_kind kind) {
  const char *sql = kind == CMETA_DATA_STRING
      ? "select cast(x'410042' as text) as payload, 7 as id union all select cast(x'4344' as text), 9"
      : "select x'410042' as payload, 7 as id union all select x'4344', 9";
  open_query(kind, sql, ROW_PAYLOAD_LIMIT);
  /* Raw output slots deliberately are NOT initialized through the provider by
   * this fixture. Constructing a valid row is the Publisher's responsibility. */
  for (unsigned i = 0u; i < ROW_COUNT; ++i) {
    const cflow_step step = cflow_publisher_resume(&source, NULL, &rows[i]);
    (void)printf("ROW_CUTOVER kind=%d index=%u step=%d tag=%u error=%s\n",
        (int)kind, i, (int)step.kind, rows[i].payload.tag, step.error != NULL ? step.error : "");
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(rows[i].payload.tag, (unsigned)ROW_ZERO_TAG);
    check_equal(rows[i].id, i == 0u ? 7 : 9);
  }
  /* Caller-owned rows must survive both next() and teardown of every SQL owner. */
  close_database();
  require_payload(&rows[0].payload, first_payload, sizeof(first_payload));
  require_payload(&rows[1].payload, second_payload, sizeof(second_payload));
  check_equal(allocations - releases, (size_t)ROW_COUNT);
  for (unsigned i = 0u; i < ROW_COUNT; ++i) row_destroy(&rows[i]);
  check_equal(allocations, releases);
}
static void failed_row(cmeta_data_kind kind, bool limited) {
  const char *sql;
  if (limited)
    sql = kind == CMETA_DATA_STRING
        ? "select cast(x'410042' as text) as payload, 7 as id"
        : "select x'410042' as payload, 7 as id";
  else
    sql = kind == CMETA_DATA_STRING
        ? "select cast(x'410042' as text) as payload, 'not-an-int' as id"
        : "select x'410042' as payload, 'not-an-int' as id";
  open_query(kind, sql, limited ? sizeof(first_payload) - 1u : ROW_PAYLOAD_LIMIT);
  const cflow_step step = cflow_publisher_resume(&source, NULL, &rows[0]);
  (void)printf("ROW_CUTOVER_FAILURE kind=%d limited=%d step=%d tag=%u assign=%zu alloc=%zu free=%zu error=%s\n",
      (int)kind, (int)limited, (int)step.kind, rows[0].payload.tag, assignments,
      allocations, releases, step.error != NULL ? step.error : "");
  check_equal(step.kind, CFLOW_STEP_ERROR);
  check_not_null(step.error);
  check_true(row_buffer_zero(&rows[0].payload));
  check_equal(rows[0].id, 0);
  if (limited) check_equal(assignments, 0u);
  else check_true(assignments > 0u); /* The payload was actually constructed before the late error. */
  check_equal(allocations, releases);
  check_equal(connection->failure, ORM_STATUS_OK);
  const size_t before = assignments;
  check_equal(cflow_publisher_resume(&source, NULL, &rows[1]).kind, CFLOW_STEP_ERROR);
  check_equal(assignments, before);
  close_database();
  check_true(row_buffer_zero(&rows[0].payload));
}

spec("ORM row semantic-zero and owned storage before DataBind cutover") {
  before_each() {
    /* C aggregate initialization yields a safe raw tstr slot, not the provider's
     * tagged semantic zero. No missing production initialization is supplied. */
    for (unsigned i = 0u; i < ROW_COUNT; ++i) rows[i] = (CutoverRow){0};
    assignments = allocations = releases = 0u;
    connection = NULL; query = NULL;
    memset(&source, 0, sizeof(source));
  }
  after_each() {
    close_database();
    /* Runs after assertions, never fixes the state before checking it. */
    for (unsigned i = 0u; i < ROW_COUNT; ++i) row_destroy(&rows[i]);
  }
  it("validates the owned text provider without an ORM decoder") { provider_control(CMETA_DATA_STRING); }
  it("validates the owned bytes provider without an ORM decoder") { provider_control(CMETA_DATA_BYTES); }
  it("constructs owned text rows that survive SQLite advancement and teardown") { row_lifetime(CMETA_DATA_STRING); }
  it("constructs owned bytes rows that survive SQLite advancement and teardown") { row_lifetime(CMETA_DATA_BYTES); }
  it("restores tagged text semantic zero after a late scalar decode failure") { failed_row(CMETA_DATA_STRING, false); }
  it("restores tagged bytes semantic zero after a late scalar decode failure") { failed_row(CMETA_DATA_BYTES, false); }
  it("keeps tagged text semantic zero on a payload limit failure") { failed_row(CMETA_DATA_STRING, true); }
  it("keeps tagged bytes semantic zero on a payload limit failure") { failed_row(CMETA_DATA_BYTES, true); }
}

/* #59 extends the same tagged provider and runner, not a second binder. Failed
 * assignments leave semantic zero; successful ones still use real Salts tstr. */
typedef struct CutoverPair { RowBuffer left, right; int id; } CutoverPair;
static CutoverPair pair_rows[3];
static orm_transaction_t *pair_transaction;
static orm_backend_ops pair_backend_native, pair_backend_observed;
static orm_transaction_backend_ops pair_transaction_native, pair_transaction_observed;
static orm_row_cursor_ops pair_cursor_native, pair_cursor_observed;
static unsigned pair_next_calls, pair_cancel_calls, pair_destroy_calls;

static void pair_destroy(void *object) {
  CutoverPair *row = object;
  row_buffer_restore(&row->left);
  row_buffer_restore(&row->right);
  row->id = 0;
}
static void pair_move(void *destination, void *source_) {
  CutoverPair *to = destination, *from = source_;
  to->left = to->right = (RowBuffer){NULL, ROW_ZERO_TAG};
  row_buffer_move(&to->left, &from->left);
  row_buffer_move(&to->right, &from->right);
  to->id = from->id;
  from->id = 0;
}
static const cmeta_type_identity pair_identity = CMETA_TYPE_ID_ATOM_INIT("orm.cutover.Pair");
static const cmeta_type_traits pair_traits = {
    .flags = CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY,
    .move_construct = pair_move, .destroy = pair_destroy};
static const cmeta_type_desc pair_type = {
    .name = "CutoverPair", .size = sizeof(CutoverPair), .align = _Alignof(CutoverPair),
    .kind = CMETA_T_OBJECT, .traits = &pair_traits, .identity = &pair_identity};
static cmeta_field_desc pair_layout_fields[3];
static cmeta_data_field_desc pair_data_fields[3];
static const cmeta_struct_desc pair_layout = {
    "CutoverPair", sizeof(CutoverPair), _Alignof(CutoverPair), pair_layout_fields, 3u};
static const cmeta_data_struct_shape pair_shape = {&pair_layout, pair_data_fields, 3u};
static const cmeta_data_desc pair_data = {
    .struct_size = sizeof(cmeta_data_desc), .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orm.cutover.Pair.data", .display_name = "CutoverPair",
    .kind = CMETA_DATA_STRUCT, .storage_type = &pair_type, .shape = &pair_shape};

static orm_row_cursor_step pair_next(void *context, cserde_reader *reader) {
  ++pair_next_calls;
  return pair_cursor_native.next(context, reader);
}
static void pair_cancel(void *context) {
  ++pair_cancel_calls;
  pair_cursor_native.cancel(context);
}
static void pair_dispose(void *context) {
  ++pair_destroy_calls;
  pair_cursor_native.destroy(context);
}
static void pair_observe_cursor(orm_row_cursor *cursor, orm_status_t status) {
  if (status != ORM_STATUS_OK) return;
  pair_cursor_native = *cursor->ops;
  pair_cursor_observed = pair_cursor_native;
  pair_cursor_observed.next = pair_next;
  pair_cursor_observed.cancel = pair_cancel;
  pair_cursor_observed.destroy = pair_dispose;
  cursor->ops = &pair_cursor_observed;
}
static orm_status_t pair_open(void *context, const orm_query_plan *plan,
    const orm_limits *limits, orm_row_cursor *cursor, orm_error_t *error) {
  const orm_status_t status = pair_backend_native.open_cursor(context, plan, limits, cursor, error);
  pair_observe_cursor(cursor, status);
  return status;
}
static orm_status_t pair_open_transaction(void *context, const orm_query_plan *plan,
    const orm_limits *limits, orm_row_cursor *cursor, orm_error_t *error) {
  const orm_status_t status = pair_transaction_native.open_cursor(context, plan, limits, cursor, error);
  pair_observe_cursor(cursor, status);
  return status;
}
static orm_status_t pair_publish(orm_error_t *error) {
  orm_flow_config_t flow;
  orm_flow_config(&flow, &pair_data);
  flow.max_buffer_bytes = ROW_PAYLOAD_LIMIT;
  return pair_transaction != NULL
      ? orm_query_open_flow_in_transaction(query, pair_transaction, &flow, &source, error)
      : orm_query_open_flow(query, &flow, &source, error);
}
static void pair_open_query(cmeta_data_kind kind, bool transactional) {
  orm_error_t error;
  orm_config_t config;
  const orm_option_t filename = {orm_view("filename"), orm_view(":memory:")};
  const char *sql = kind == CMETA_DATA_STRING
      ? "select cast(x'410042' as text) as left, cast(x'4344' as text) as right, 7 as id "
        "union all select cast(x'410042' as text), cast(x'4344' as text), 9"
      : "select x'410042' as left, x'4344' as right, 7 as id union all select x'410042', x'4344', 9";
  set_payload_kind(kind);
  const char *names[] = {"left", "right"};
  const size_t offsets[] = {offsetof(CutoverPair, left), offsetof(CutoverPair, right)};
  for (size_t i = 0u; i < 2u; ++i) {
    pair_layout_fields[i] = (cmeta_field_desc){
        .name = names[i], .type_name = "RowBuffer", .offset = offsets[i],
        .size = sizeof(RowBuffer), .align = _Alignof(RowBuffer), .type = &buffer_type};
    pair_data_fields[i] = (cmeta_data_field_desc){names[i], names[i], offsets[i], &payload_data};
  }
  pair_layout_fields[2] = (cmeta_field_desc){
      .name = "id", .type_name = "int", .offset = offsetof(CutoverPair, id),
      .size = sizeof(int), .align = _Alignof(int), .type = cmeta_data_int.storage_type};
  pair_data_fields[2] = (cmeta_data_field_desc){
      "pair.id", "id", offsetof(CutoverPair, id), &cmeta_data_int};
  check_true(cmeta_data_desc_valid(&pair_data));
  orm_error_init(&error); orm_config(&config);
  config.driver = orm_view("sqlite"); config.options = &filename; config.option_count = 1u;
  check_equal(orm_connect(&config, &connection, &error), ORM_STATUS_OK);
  pair_backend_native = *connection->backend.ops;
  pair_backend_observed = pair_backend_native;
  pair_backend_observed.open_cursor = pair_open;
  connection->backend.ops = &pair_backend_observed;
  check_equal(orm_raw(connection, orm_view(sql), &query, &error), ORM_STATUS_OK);
  if (transactional) {
    check_equal(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                      &pair_transaction, &error), ORM_STATUS_OK);
    pair_transaction_native = *pair_transaction->backend.ops;
    pair_transaction_observed = pair_transaction_native;
    pair_transaction_observed.open_cursor = pair_open_transaction;
    pair_transaction->backend.ops = &pair_transaction_observed;
  }
  check_equal(pair_publish(&error), ORM_STATUS_OK);
}
static void pair_close_database(void) {
  if (cflow_publisher_valid(&source)) cflow_publisher_destroy(&source);
  memset(&source, 0, sizeof(source));
  orm_transaction_release(pair_transaction); pair_transaction = NULL;
  close_database();
}
static void pair_require_value(const CutoverPair *row, int id) {
  require_payload(&row->left, first_payload, sizeof(first_payload));
  require_payload(&row->right, second_payload, sizeof(second_payload));
  check_equal(row->id, id);
}
static void pair_require_zero(const CutoverPair *row) {
  check_true(row_buffer_zero(&row->left));
  check_true(row_buffer_zero(&row->right));
  check_equal(row->id, 0);
}
static void pair_require_owners(uint32_t connection_holds, uint32_t query_holds,
                                uint32_t transaction_holds) {
  check_equal(connection->owner.references, 1u);
  check_equal(connection->owner.dependents, connection_holds);
  check_equal(connection->owner.phase, ORM_OWNER_OPEN);
  check_equal(query->owner.references, 1u);
  check_equal(query->owner.dependents, query_holds);
  check_equal(query->owner.phase, ORM_OWNER_OPEN);
  check_equal(connection->failure, ORM_STATUS_OK);
  check_false(connection->native_active);
  check_null(connection->cleanup_head);
  check_null(connection->cleanup_tail);
  if (pair_transaction != NULL) {
    check_equal(pair_transaction->owner.references, 1u);
    check_equal(pair_transaction->owner.dependents, transaction_holds);
    check_equal(pair_transaction->owner.phase, ORM_OWNER_OPEN);
    check_equal(pair_transaction->state, ORM_TRANSACTION_ACTIVE);
    check_false(pair_transaction->operation_active);
  }
}
static void pair_failure(cmeta_data_kind kind, unsigned fail_field,
                          bool transactional, bool prior_row) {
  pair_open_query(kind, transactional);
  const uint32_t c_holds = connection->owner.dependents;
  const uint32_t q_holds = query->owner.dependents;
  const uint32_t t_holds = pair_transaction != NULL ? pair_transaction->owner.dependents : 0u;
  if (prior_row) {
    check_equal(cflow_publisher_resume(&source, NULL, &pair_rows[0]).kind, CFLOW_STEP_VALUE);
    pair_require_value(&pair_rows[0], 7);
  }
  const size_t prior_allocations = prior_row ? 2u : 0u;
  fail_assignment = assignments + fail_field;
  CutoverPair *failed = &pair_rows[prior_row ? 1u : 0u];
  const cflow_step step = cflow_publisher_resume(&source, NULL, failed);
  fail_assignment = 0u;
  check_equal(step.kind, CFLOW_STEP_ERROR);
  check_not_null(step.error);
  char expected_error[48], saved_error[ORM_C_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(expected_error, sizeof(expected_error), "databind=%d ", (int)DATA_BIND_ERR_OOM);
  check_not_null(strstr(step.error, expected_error));
  check_not_null(strstr(step.error, fail_field == 1u ? "CutoverPair.left" : "CutoverPair.right"));
  (void)snprintf(saved_error, sizeof(saved_error), "%s", step.error);
  check_equal(injected_failures, 1u);
  check_equal(assignments, prior_allocations + fail_field);
  check_equal(allocations, prior_allocations + fail_field - 1u);
  check_equal(releases, fail_field - 1u);
  pair_require_zero(failed);
  pair_require_owners(c_holds, q_holds, t_holds);
  check_equal(pair_next_calls, prior_row ? 2u : 1u);
  check_equal(pair_cancel_calls, 1u);
  check_equal(pair_destroy_calls, 0u);
  if (prior_row) pair_require_value(&pair_rows[0], 7);
  /* Terminal re-poll and cancel cannot reenter the reader or its failed provider. */
  const size_t before = assignments;
  const cflow_step again = cflow_publisher_resume(&source, NULL, &pair_rows[2]);
  check_equal(again.kind, CFLOW_STEP_ERROR);
  check_not_null(again.error);
  check_equal(strcmp(again.error, saved_error), 0);
  cflow_publisher_cancel(&source);
  check_equal(assignments, before);
  check_equal(pair_next_calls, prior_row ? 2u : 1u);
  check_equal(pair_cancel_calls, 1u);
  pair_require_owners(c_holds, q_holds, t_holds);
  cflow_publisher_destroy(&source); memset(&source, 0, sizeof(source));
  check_equal(pair_destroy_calls, 1u);
  pair_require_owners(c_holds, q_holds - 1u, t_holds != 0u ? t_holds - 1u : 0u);
  /* A fresh, explicit request on these same owners succeeds after disarming. */
  orm_error_t error; orm_error_init(&error);
  check_equal(pair_publish(&error), ORM_STATUS_OK);
  check_equal(cflow_publisher_resume(&source, NULL, &pair_rows[2]).kind, CFLOW_STEP_VALUE);
  pair_require_value(&pair_rows[2], 7);
  pair_close_database();
  check_equal(pair_cancel_calls, 2u);
  check_equal(pair_destroy_calls, 2u);
  if (prior_row) pair_require_value(&pair_rows[0], 7);
  pair_require_value(&pair_rows[2], 7);
  pair_require_zero(failed);
  check_equal(allocations - releases, prior_allocations + 2u);
  for (size_t i = 0u; i < 3u; ++i) pair_destroy(&pair_rows[i]);
  check_equal(allocations, releases);
}
static void pair_cancel_lifetime(cmeta_data_kind kind, bool transactional) {
  pair_open_query(kind, transactional);
  const uint32_t c_holds = connection->owner.dependents;
  const uint32_t q_holds = query->owner.dependents;
  const uint32_t t_holds = pair_transaction != NULL ? pair_transaction->owner.dependents : 0u;
  check_equal(cflow_publisher_resume(&source, NULL, &pair_rows[0]).kind, CFLOW_STEP_VALUE);
  pair_require_value(&pair_rows[0], 7);
  cflow_publisher_cancel(&source);
  cflow_publisher_cancel(&source);
  check_equal(pair_next_calls, 1u);
  check_equal(pair_cancel_calls, 1u);
  check_equal(pair_destroy_calls, 0u);
  pair_require_owners(c_holds, q_holds, t_holds);
  pair_require_value(&pair_rows[0], 7);
  pair_close_database();
  check_equal(pair_cancel_calls, 1u);
  check_equal(pair_destroy_calls, 1u);
  pair_require_value(&pair_rows[0], 7);
  check_equal(allocations - releases, 2u);
  pair_destroy(&pair_rows[0]);
  check_equal(allocations, releases);
}

spec("ORM owned row provider failure and explicit cancellation") {
  before_each() {
    for (size_t i = 0u; i < 3u; ++i) pair_rows[i] = (CutoverPair){0};
    assignments = allocations = releases = fail_assignment = injected_failures = 0u;
    pair_next_calls = pair_cancel_calls = pair_destroy_calls = 0u;
    connection = NULL; query = NULL; pair_transaction = NULL;
    memset(&source, 0, sizeof(source));
  }
  after_each() {
    fail_assignment = 0u;
    pair_close_database();
    for (size_t i = 0u; i < 3u; ++i) pair_destroy(&pair_rows[i]);
  }
  it("keeps tagged text zero when its first provider allocation fails") { pair_failure(CMETA_DATA_STRING, 1u, false, false); }
  it("rolls back earlier owned text when the second provider allocation fails") { pair_failure(CMETA_DATA_STRING, 2u, false, false); }
  it("retains transaction owners after first text provider allocation failure") { pair_failure(CMETA_DATA_STRING, 1u, true, false); }
  it("rolls back earlier owned text inside a transaction") { pair_failure(CMETA_DATA_STRING, 2u, true, false); }
  it("preserves a published text row when the next row cannot allocate") { pair_failure(CMETA_DATA_STRING, 2u, false, true); }
  it("preserves a published text row across transactional allocation failure") { pair_failure(CMETA_DATA_STRING, 2u, true, true); }
  it("keeps two owned text fields after repeated explicit cancel and teardown") { pair_cancel_lifetime(CMETA_DATA_STRING, false); }
  it("keeps owned text after transactional cancel and final rollback") { pair_cancel_lifetime(CMETA_DATA_STRING, true); }
  it("keeps tagged bytes zero when their first provider allocation fails") { pair_failure(CMETA_DATA_BYTES, 1u, false, false); }
  it("rolls back earlier owned bytes when the second provider allocation fails") { pair_failure(CMETA_DATA_BYTES, 2u, false, false); }
  it("retains transaction owners after first bytes provider allocation failure") { pair_failure(CMETA_DATA_BYTES, 1u, true, false); }
  it("rolls back earlier owned bytes inside a transaction") { pair_failure(CMETA_DATA_BYTES, 2u, true, false); }
  it("preserves a published bytes row when the next row cannot allocate") { pair_failure(CMETA_DATA_BYTES, 2u, false, true); }
  it("preserves a published bytes row across transactional allocation failure") { pair_failure(CMETA_DATA_BYTES, 2u, true, true); }
  it("keeps two owned bytes fields after repeated explicit cancel and teardown") { pair_cancel_lifetime(CMETA_DATA_BYTES, false); }
  it("keeps owned bytes after transactional cancel and final rollback") { pair_cancel_lifetime(CMETA_DATA_BYTES, true); }
}

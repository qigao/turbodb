/* #52 consumer contract: real SQLite and ORM, no alternate binder in the test. */
#define TINYTEST_NO_MAIN
#include "orm_internal.h"
#include <cmeta/struct.h>
#include <salts_cmeta_data.h>
#include <tinytest.h>
#include <stdio.h>
#include <string.h>

enum { ROW_ZERO_TAG = 0x6b29, ROW_COUNT = 2, ROW_PAYLOAD_LIMIT = 64 };
typedef struct RowBuffer { tstr text; unsigned tag; } RowBuffer;
typedef struct CutoverRow { RowBuffer payload; int id; } CutoverRow;
static CutoverRow rows[ROW_COUNT];
static size_t assignments, allocations, releases;
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

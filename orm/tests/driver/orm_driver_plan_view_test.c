#include "orm_driver_plan_view.h"
#include <tinytest.h>
#include <stddef.h>
#include <string.h>

#define REQUIRE_OK(call) check_equal((call), ORM_STATUS_OK)
#define INIT_DTO(value) do { memset(&(value), 0, sizeof(value)); \
  (value).header = test_header(sizeof(value)); } while (0)

enum { TEST_ITEMS = 16, TEST_BYTES = 1024, TEST_SENTINEL = 0xa5 };
static orm_query_plan plan;
static orm_limits limits;
static orm_error_t error;
static orm_driver_plan_view_v1 view;

static orm_driver_header_v1 test_header(size_t size) {
  orm_driver_header_v1 header = {(uint32_t)size, ORM_DRIVER_ABI_VERSION};
  return header;
}
static void init(orm_query_kind kind) {
  const char *input = kind == ORM_QUERY_RAW ? "select ?2, ?1, ?2" : "app.records";
  REQUIRE_OK(orm_plan_init(&plan, kind, orm_view(input), &limits, &error));
}
static void borrow(void) {
  REQUIRE_OK(orm_driver_plan_borrow(&plan, &view, &error));
  check_true(view.context == &plan);
  check_not_null(view.metadata.data); check_not_null(view.values.data);
}
static const orm_driver_plan_metadata_ops_v1 *metadata(void) { return view.metadata.data; }
static const orm_driver_plan_value_ops_v1 *values(void) { return view.values.data; }
static void bytes_equal(orm_driver_bytes_v1 bytes, const void *expected, size_t size) {
  check_equal(bytes.size, (uint64_t)size);
  if (size != 0u) {
    check_not_null(bytes.data);
    check_equal(memcmp(bytes.data, expected, size), 0);
  }
}
static void payload_zero(const void *object, size_t size) {
  const unsigned char *p = object;
  for (size_t i = sizeof(orm_driver_header_v1); i < size; ++i)
    check_equal(p[i], (unsigned char)0);
}
static void poison(void *object, size_t size) {
  const orm_driver_header_v1 header = test_header(size);
  memset(object, TEST_SENTINEL, size);
  memcpy(object, &header, sizeof(header));
}

spec("driver read-only native plan views") {
  (void)ttest_config__;
  before_each() {
    memset(&plan, 0, sizeof(plan)); memset(&view, 0, sizeof(view));
    limits.max_parameters = TEST_ITEMS; limits.max_columns = TEST_ITEMS;
    limits.max_predicates = TEST_ITEMS; limits.max_assignments = TEST_ITEMS;
    limits.max_query_bytes = TEST_BYTES; limits.max_parameter_bytes = TEST_BYTES;
    limits.max_result_rows = TEST_ITEMS; limits.max_result_bytes = TEST_BYTES;
    orm_error_init(&error);
  }
  after_each() { orm_plan_destroy(&plan); }

  it("rejects null borrow arguments and clears a stale view") {
    memset(&view, TEST_SENTINEL, sizeof(view));
    check_equal(orm_driver_plan_borrow(NULL, &view, &error), ORM_STATUS_INVALID_ARGUMENT);
    const unsigned char *p = (const unsigned char *)&view;
    for (size_t i = 0u; i < sizeof(view); ++i) check_equal(p[i], (unsigned char)0);
    init(ORM_QUERY_SELECT);
    check_equal(orm_driver_plan_borrow(&plan, NULL, NULL), ORM_STATUS_INVALID_ARGUMENT);
  }
  it("maps every query kind without exposing the private layout") {
    const orm_query_kind kinds[] = {ORM_QUERY_SELECT, ORM_QUERY_INSERT,
      ORM_QUERY_UPDATE, ORM_QUERY_DELETE, ORM_QUERY_RAW};
    const uint32_t expected[] = {ORM_DRIVER_PLAN_SELECT, ORM_DRIVER_PLAN_INSERT,
      ORM_DRIVER_PLAN_UPDATE, ORM_DRIVER_PLAN_DELETE, ORM_DRIVER_PLAN_RAW_SQL};
    for (size_t i = 0u; i < sizeof(kinds)/sizeof(kinds[0]); ++i) {
      init(kinds[i]); borrow();
      orm_driver_plan_meta_v1 meta; INIT_DTO(meta);
      REQUIRE_OK((metadata()->describe)(view.context, &meta, &error));
      check_equal(meta.kind, expected[i]); check_equal(meta.column_count, UINT64_C(0));
      if (kinds[i] == ORM_QUERY_RAW) {
        bytes_equal(meta.raw_sql, "select ?2, ?1, ?2", sizeof("select ?2, ?1, ?2")-1u);
        check_equal(meta.table.size, UINT64_C(0));
      } else bytes_equal(meta.table, "app.records", sizeof("app.records")-1u);
      check_equal(view.metadata.reserved, 0u); check_equal(view.values.reserved, 0u);
      check_equal(metadata()->header.abi_version, ORM_DRIVER_ABI_VERSION);
      check_equal(values()->header.struct_size, (uint32_t)sizeof(*values()));
      orm_plan_destroy(&plan);
    }
  }
  it("keeps raw SQL unchanged and supports repeated out-of-order binary parameters") {
    const unsigned char blob[] = {'a', 0, 'b'};
    init(ORM_QUERY_RAW);
    REQUIRE_OK(orm_plan_add_bind(&plan, orm_blob(blob, sizeof(blob)), &limits, &error));
    REQUIRE_OK(orm_plan_add_bind(&plan, orm_u64(UINT64_MAX), &limits, &error));
    const orm_query_plan snapshot = plan;
    borrow();
    orm_driver_value_v1 value; INIT_DTO(value);
    REQUIRE_OK(values()->raw_parameter_at(view.context, 1u, &value, &error));
    check_equal(value.kind, (uint32_t)ORM_VALUE_UINT64); check_equal(value.data.uint, UINT64_MAX);
    REQUIRE_OK(values()->raw_parameter_at(view.context, 0u, &value, &error));
    check_equal(value.kind, (uint32_t)ORM_VALUE_BLOB); bytes_equal(value.data.bytes, blob, sizeof(blob));
    const orm_owned_value *owned = vec_at_const(&plan.raw_parameters, 0u);
    check_true(value.data.bytes.data == owned->bytes);
    REQUIRE_OK(values()->raw_parameter_at(view.context, 1u, &value, NULL));
    check_equal(value.data.uint, UINT64_MAX);
    orm_driver_plan_meta_v1 meta; INIT_DTO(meta);
    REQUIRE_OK((metadata()->describe)(view.context, &meta, &error));
    check_equal(meta.raw_parameter_count, UINT64_C(2));
    bytes_equal(meta.raw_sql, "select ?2, ?1, ?2", sizeof("select ?2, ?1, ?2")-1u);
    check_equal(memcmp(&snapshot, &plan, sizeof(plan)), 0);
    check_equal(plan.parameter_bytes, sizeof(blob));
  }
  it("preserves all scalar types without lossy conversion") {
    init(ORM_QUERY_RAW);
    const orm_value_t inputs[] = {orm_null(), orm_i64(INT64_MIN), orm_u64(UINT64_MAX),
      orm_f64(1.25), orm_bool(0), orm_bool(1)};
    for (size_t i = 0u; i < sizeof(inputs)/sizeof(inputs[0]); ++i)
      REQUIRE_OK(orm_plan_add_bind(&plan, inputs[i], &limits, &error));
    borrow(); orm_driver_value_v1 value; INIT_DTO(value);
    for (size_t i = 0u; i < sizeof(inputs)/sizeof(inputs[0]); ++i) {
      REQUIRE_OK(values()->raw_parameter_at(view.context, i, &value, &error));
      check_equal(value.kind, (uint32_t)inputs[i].kind); check_equal(value.reserved, 0u);
      switch (inputs[i].kind) {
        case ORM_VALUE_NULL: check_equal(value.data.uint, UINT64_C(0)); break;
        case ORM_VALUE_INT64: check_equal(value.data.sint, INT64_MIN); break;
        case ORM_VALUE_UINT64: check_equal(value.data.uint, UINT64_MAX); break;
        case ORM_VALUE_DOUBLE: check_true(value.data.real == 1.25); break;
        default: check_equal(value.data.boolean, inputs[i].data.boolean_value); break;
      }
    }
  }
  it("distinguishes NULL empty text empty BLOB and embedded NUL text") {
    static const char text[] = {'a', 0, 'z'};
    const vstr text_view = {text, sizeof(text)};
    const orm_value_t inputs[] = {orm_null(), orm_text(""), orm_blob(NULL, 0u), orm_text_v(text_view)};
    init(ORM_QUERY_RAW);
    for (size_t i = 0u; i < sizeof(inputs)/sizeof(inputs[0]); ++i)
      REQUIRE_OK(orm_plan_add_bind(&plan, inputs[i], &limits, &error));
    borrow(); orm_driver_value_v1 value; INIT_DTO(value);
    for (size_t i = 0u; i < sizeof(inputs)/sizeof(inputs[0]); ++i) {
      REQUIRE_OK(values()->raw_parameter_at(view.context, i, &value, &error));
      check_equal(value.kind, (uint32_t)inputs[i].kind);
      if (i == 3u) bytes_equal(value.data.bytes, text, sizeof(text));
      else check_equal(value.data.bytes.size, UINT64_C(0));
    }
  }
  it("exposes ordered selected columns and select-all presence") {
    init(ORM_QUERY_SELECT);
    REQUIRE_OK(orm_plan_select_all(&plan, &error)); borrow();
    orm_driver_plan_meta_v1 meta; INIT_DTO(meta);
    REQUIRE_OK((metadata()->describe)(view.context, &meta, &error));
    check_true((meta.flags & ORM_DRIVER_PLAN_SELECT_ALL) != 0u);
    /* End this test borrow before building a new frozen view. */
    memset(&view, 0, sizeof(view));
    REQUIRE_OK(orm_plan_add_column(&plan, orm_view("id"), &limits, &error));
    REQUIRE_OK(orm_plan_add_column(&plan, orm_view("name"), &limits, &error));
    borrow(); REQUIRE_OK((metadata()->describe)(view.context, &meta, &error));
    check_equal(meta.column_count, UINT64_C(2));
    check_equal(meta.flags & ORM_DRIVER_PLAN_SELECT_ALL, 0u);
    orm_driver_bytes_v1 column = {0};
    REQUIRE_OK(metadata()->column_at(view.context, 1u, &column, &error)); bytes_equal(column, "name", 4u);
    REQUIRE_OK(metadata()->column_at(view.context, 0u, &column, &error)); bytes_equal(column, "id", 2u);
  }
  it("distinguishes absent limits from present zero and preserves uint64 bounds") {
    init(ORM_QUERY_SELECT);
    for (uint32_t flags = 0u; flags < 4u; ++flags) {
      plan.has_limit = (flags & 1u) != 0u; plan.has_offset = (flags & 2u) != 0u;
      plan.limit = flags == 0u ? UINT64_MAX : 0u; plan.offset = UINT64_MAX;
      borrow(); orm_driver_plan_meta_v1 meta; INIT_DTO(meta);
      REQUIRE_OK((metadata()->describe)(view.context, &meta, &error));
      check_equal(meta.flags & ORM_DRIVER_PLAN_HAS_LIMIT,
        plan.has_limit ? ORM_DRIVER_PLAN_HAS_LIMIT : 0u);
      check_equal(meta.flags & ORM_DRIVER_PLAN_HAS_OFFSET,
        plan.has_offset ? ORM_DRIVER_PLAN_HAS_OFFSET : 0u);
      check_equal(meta.limit, plan.limit); check_equal(meta.offset, plan.offset);
      memset(&view, 0, sizeof(view));
    }
  }
  it("maps absent ascending and descending ordering") {
    init(ORM_QUERY_SELECT); borrow();
    orm_driver_ordering_v1 order; INIT_DTO(order);
    REQUIRE_OK(metadata()->ordering(view.context, &order, &error)); check_equal(order.present, 0u);
    check_equal(order.column.size, UINT64_C(0));
    const orm_order_t directions[] = {ORM_ORDER_ASCENDING, ORM_ORDER_DESCENDING};
    for (size_t i = 0u; i < sizeof(directions)/sizeof(directions[0]); ++i) {
      memset(&view, 0, sizeof(view));
      REQUIRE_OK(orm_plan_set_order(&plan, orm_view("id"), directions[i], &limits, &error)); borrow();
      REQUIRE_OK(metadata()->ordering(view.context, &order, &error));
      check_equal(order.present, 1u); check_equal(order.order, (uint32_t)directions[i]);
      bytes_equal(order.column, "id", 2u);
    }
  }
  it("maps assignment columns and nested values") {
    init(ORM_QUERY_UPDATE);
    REQUIRE_OK(orm_plan_add_assignment(&plan, orm_view("id"), orm_u64(UINT64_MAX), &limits, &error));
    REQUIRE_OK(orm_plan_add_assignment(&plan, orm_view("name"), orm_null(), &limits, &error));
    borrow(); orm_driver_assignment_v1 assignment; INIT_DTO(assignment);
    REQUIRE_OK(values()->assignment_at(view.context, 0u, &assignment, &error));
    bytes_equal(assignment.column, "id", 2u); check_equal(assignment.value.data.uint, UINT64_MAX);
    check_equal(assignment.value.header.struct_size, (uint32_t)sizeof(assignment.value));
    REQUIRE_OK(values()->assignment_at(view.context, 1u, &assignment, &error));
    check_equal(assignment.value.kind, (uint32_t)ORM_VALUE_NULL);
    orm_driver_plan_meta_v1 meta; INIT_DTO(meta);
    REQUIRE_OK((metadata()->describe)(view.context, &meta, &error)); check_equal(meta.assignment_count, UINT64_C(2));
  }
  it("maps every comparison and nested predicate value") {
    const orm_compare_t comparisons[] = {ORM_COMPARE_EQUAL, ORM_COMPARE_NOT_EQUAL,
      ORM_COMPARE_LESS, ORM_COMPARE_LESS_EQUAL, ORM_COMPARE_GREATER,
      ORM_COMPARE_GREATER_EQUAL, ORM_COMPARE_LIKE, ORM_COMPARE_NOT_LIKE};
    init(ORM_QUERY_DELETE);
    for (size_t i = 0u; i < sizeof(comparisons)/sizeof(comparisons[0]); ++i)
      REQUIRE_OK(orm_plan_add_predicate(&plan, orm_view("id"), comparisons[i], orm_i64(-7), &limits, &error));
    borrow(); orm_driver_predicate_v1 predicate; INIT_DTO(predicate);
    for (size_t i = 0u; i < sizeof(comparisons)/sizeof(comparisons[0]); ++i) {
      REQUIRE_OK(values()->predicate_at(view.context, i, &predicate, &error));
      bytes_equal(predicate.column, "id", 2u); check_equal(predicate.comparison, (uint32_t)comparisons[i]);
      check_equal(predicate.reserved, 0u); check_equal(predicate.value.data.sint, INT64_C(-7));
    }
    orm_driver_plan_meta_v1 meta; INIT_DTO(meta);
    REQUIRE_OK((metadata()->describe)(view.context, &meta, &error)); check_equal(meta.predicate_count, UINT64_C(8));
  }
  it("rejects exact-end and UINT64_MAX indices and clears every indexed output") {
    init(ORM_QUERY_RAW); borrow();
    const uint64_t indices[] = {0u, UINT64_MAX};
    for (size_t i = 0u; i < sizeof(indices)/sizeof(indices[0]); ++i) {
      orm_driver_bytes_v1 column; memset(&column, TEST_SENTINEL, sizeof(column));
      check_equal(metadata()->column_at(view.context, indices[i], &column, &error), ORM_STATUS_OUT_OF_RANGE);
      check_null(column.data); check_equal(column.size, UINT64_C(0));
      orm_driver_assignment_v1 assignment; poison(&assignment, sizeof(assignment));
      check_equal(values()->assignment_at(view.context, indices[i], &assignment, &error), ORM_STATUS_OUT_OF_RANGE);
      payload_zero(&assignment, sizeof(assignment));
      orm_driver_predicate_v1 predicate; poison(&predicate, sizeof(predicate));
      check_equal(values()->predicate_at(view.context, indices[i], &predicate, &error), ORM_STATUS_OUT_OF_RANGE);
      payload_zero(&predicate, sizeof(predicate));
      orm_driver_value_v1 value; poison(&value, sizeof(value));
      check_equal(values()->raw_parameter_at(view.context, indices[i], &value, &error), ORM_STATUS_OUT_OF_RANGE);
      payload_zero(&value, sizeof(value)); check_equal(value.header.struct_size, (uint32_t)sizeof(value));
      check_equal(error.status, ORM_STATUS_OUT_OF_RANGE); check_true(error.message[0] != 0);
    }
  }
  it("rejects null context or output for every accessor") {
    init(ORM_QUERY_SELECT); borrow();
    orm_driver_plan_meta_v1 meta; INIT_DTO(meta);
    orm_driver_ordering_v1 order; INIT_DTO(order);
    orm_driver_assignment_v1 assignment; INIT_DTO(assignment);
    orm_driver_predicate_v1 predicate; INIT_DTO(predicate);
    orm_driver_value_v1 value; INIT_DTO(value);
    orm_driver_bytes_v1 column = {0};
    check_equal((metadata()->describe)(NULL, &meta, &error), ORM_STATUS_INVALID_ARGUMENT);
    check_equal((metadata()->describe)(view.context, NULL, NULL), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(metadata()->ordering(NULL, &order, &error), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(metadata()->ordering(view.context, NULL, NULL), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(metadata()->column_at(NULL, 0u, &column, &error), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(metadata()->column_at(view.context, 0u, NULL, NULL), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(values()->assignment_at(NULL, 0u, &assignment, &error), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(values()->assignment_at(view.context, 0u, NULL, NULL), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(values()->predicate_at(NULL, 0u, &predicate, &error), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(values()->predicate_at(view.context, 0u, NULL, NULL), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(values()->raw_parameter_at(NULL, 0u, &value, &error), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(values()->raw_parameter_at(view.context, 0u, NULL, NULL), ORM_STATUS_INVALID_ARGUMENT);
  }
  it("rejects invalid output headers without touching the caller object") {
    init(ORM_QUERY_RAW); borrow();
    const uint32_t sizes[] = {0u, 7u, (uint32_t)sizeof(orm_driver_value_v1)-1u,
                              ORM_DRIVER_DESCRIPTOR_MAX_BYTES+1u};
    for (size_t i = 0u; i < sizeof(sizes)/sizeof(sizes[0]); ++i) {
      orm_driver_value_v1 value; poison(&value, sizeof(value)); value.header.struct_size = sizes[i];
      const orm_driver_value_v1 snapshot = value;
      const orm_status_t expected = sizes[i] > ORM_DRIVER_DESCRIPTOR_MAX_BYTES ?
        ORM_STATUS_LIMIT_EXCEEDED : ORM_STATUS_ABI_MISMATCH;
      check_equal(values()->raw_parameter_at(view.context, 0u, &value, &error), expected);
      check_equal(memcmp(&value, &snapshot, sizeof(value)), 0);
    }
    orm_driver_plan_meta_v1 meta; poison(&meta, sizeof(meta)); meta.header.abi_version++;
    const orm_driver_plan_meta_v1 snapshot = meta;
    check_equal((metadata()->describe)(view.context, &meta, &error), ORM_STATUS_ABI_MISMATCH);
    check_equal(memcmp(&meta, &snapshot, sizeof(meta)), 0);
  }
  it("preserves a valid caller header and unknown tail on success and error") {
    struct extended_value { orm_driver_value_v1 value; unsigned char tail[TEST_ITEMS]; } extended;
    memset(&extended, TEST_SENTINEL, sizeof(extended));
    extended.value.header = test_header(sizeof(extended));
    init(ORM_QUERY_RAW); REQUIRE_OK(orm_plan_add_bind(&plan, orm_u64(UINT64_MAX), &limits, &error)); borrow();
    REQUIRE_OK(values()->raw_parameter_at(view.context, 0u, &extended.value, &error));
    check_equal(extended.value.header.struct_size, (uint32_t)sizeof(extended));
    check_equal(extended.value.data.uint, UINT64_MAX);
    check_equal(values()->raw_parameter_at(view.context, 1u, &extended.value, &error), ORM_STATUS_OUT_OF_RANGE);
    payload_zero(&extended.value, sizeof(extended.value));
    for (size_t i = 0u; i < sizeof(extended.tail); ++i)
      check_equal(extended.tail[i], (unsigned char)TEST_SENTINEL);
  }
  it("rejects unknown query kind without publishing a view") {
    init(ORM_QUERY_SELECT); plan.kind = (orm_query_kind)-1;
    memset(&view, TEST_SENTINEL, sizeof(view));
    check_equal(orm_driver_plan_borrow(&plan, &view, &error), ORM_STATUS_INVALID_ARGUMENT);
    check_null(view.context); check_null(view.metadata.data); check_null(view.values.data);
  }
  it("rejects unknown ordering and comparison instead of silently mapping them") {
    init(ORM_QUERY_SELECT);
    REQUIRE_OK(orm_plan_set_order(&plan, orm_view("id"), ORM_ORDER_ASCENDING, &limits, &error));
    REQUIRE_OK(orm_plan_add_predicate(&plan, orm_view("id"), ORM_COMPARE_EQUAL, orm_i64(1), &limits, &error));
    plan.ordering.order = -1;
    orm_predicate *owned = vec_at(&plan.predicates, 0u); owned->comparison = -1;
    borrow(); orm_driver_ordering_v1 order; poison(&order, sizeof(order));
    check_equal(metadata()->ordering(view.context, &order, &error), ORM_STATUS_INVALID_ARGUMENT);
    payload_zero(&order, sizeof(order));
    orm_driver_predicate_v1 predicate; poison(&predicate, sizeof(predicate));
    check_equal(values()->predicate_at(view.context, 0u, &predicate, &error), ORM_STATUS_INVALID_ARGUMENT);
    payload_zero(&predicate, sizeof(predicate));
  }
  it("rejects unknown value tags and malformed boolean values") {
    init(ORM_QUERY_RAW); REQUIRE_OK(orm_plan_add_bind(&plan, orm_bool(1), &limits, &error));
    orm_owned_value *owned = vec_at(&plan.raw_parameters, 0u);
    owned->kind = -1; borrow();
    orm_driver_value_v1 value; poison(&value, sizeof(value));
    check_equal(values()->raw_parameter_at(view.context, 0u, &value, &error), ORM_STATUS_INVALID_ARGUMENT);
    payload_zero(&value, sizeof(value));
    memset(&view, 0, sizeof(view)); owned->kind = ORM_VALUE_BOOLEAN; owned->data.boolean_value = 2u;
    borrow();
    check_equal(values()->raw_parameter_at(view.context, 0u, &value, &error), ORM_STATUS_INVALID_ARGUMENT);
    payload_zero(&value, sizeof(value));
  }
  it("clears old error diagnostics on successful repeat reads") {
    init(ORM_QUERY_RAW); REQUIRE_OK(orm_plan_add_bind(&plan, orm_i64(-1), &limits, &error)); borrow();
    orm_driver_value_v1 value; INIT_DTO(value);
    check_equal(values()->raw_parameter_at(view.context, UINT64_MAX, &value, &error), ORM_STATUS_OUT_OF_RANGE);
    REQUIRE_OK(values()->raw_parameter_at(view.context, 0u, &value, &error));
    check_equal(error.status, ORM_STATUS_OK); check_equal(error.message[0], '\0');
    const orm_driver_value_v1 snapshot = value;
    REQUIRE_OK(values()->raw_parameter_at(view.context, 0u, &value, NULL));
    check_equal(memcmp(&snapshot, &value, sizeof(value)), 0);
  }
}

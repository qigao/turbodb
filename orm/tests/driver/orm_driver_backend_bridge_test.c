#include "orm_driver_backend_bridge.h"

#include <tinytest.h>

#include <string.h>

#define H(T) {(uint32_t)sizeof(T), ORM_DRIVER_ABI_VERSION}
#define TABLE(p) {(p), (uint32_t)sizeof(*(p)), 0u}

typedef struct bridge_fixture {
  orm_driver_plan_meta_v1 meta;
  orm_driver_ordering_v1 ordering;
  orm_driver_bytes_v1 columns[3];
  orm_driver_assignment_v1 assignments[8];
  orm_driver_predicate_v1 predicates[3];
  orm_driver_value_v1 parameters[8];
  uint32_t describe_calls;
  uint32_t column_calls;
  uint32_t ordering_calls;
  uint32_t assignment_calls;
  uint32_t predicate_calls;
  uint32_t parameter_calls;
} bridge_fixture;

static orm_status_t ORM_DRIVER_CALL fake_describe(
    const void *context, orm_driver_plan_meta_v1 *out, orm_error_t *error) {
  bridge_fixture *fixture = (bridge_fixture *)context;
  ++fixture->describe_calls;
  (void)error;
  *out = fixture->meta;
  return ORM_STATUS_OK;
}

static orm_status_t ORM_DRIVER_CALL fake_column(
    const void *context, uint64_t index, orm_driver_bytes_v1 *out,
    orm_error_t *error) {
  bridge_fixture *fixture = (bridge_fixture *)context;
  ++fixture->column_calls;
  (void)error;
  if (index >= fixture->meta.column_count) return ORM_STATUS_OUT_OF_RANGE;
  *out = fixture->columns[index];
  return ORM_STATUS_OK;
}

static orm_status_t ORM_DRIVER_CALL fake_ordering(
    const void *context, orm_driver_ordering_v1 *out, orm_error_t *error) {
  bridge_fixture *fixture = (bridge_fixture *)context;
  ++fixture->ordering_calls;
  (void)error;
  *out = fixture->ordering;
  return ORM_STATUS_OK;
}

static orm_status_t ORM_DRIVER_CALL fake_assignment(
    const void *context, uint64_t index, orm_driver_assignment_v1 *out,
    orm_error_t *error) {
  bridge_fixture *fixture = (bridge_fixture *)context;
  ++fixture->assignment_calls;
  (void)error;
  if (index >= fixture->meta.assignment_count) return ORM_STATUS_OUT_OF_RANGE;
  *out = fixture->assignments[index];
  return ORM_STATUS_OK;
}

static orm_status_t ORM_DRIVER_CALL fake_predicate(
    const void *context, uint64_t index, orm_driver_predicate_v1 *out,
    orm_error_t *error) {
  bridge_fixture *fixture = (bridge_fixture *)context;
  ++fixture->predicate_calls;
  (void)error;
  if (index >= fixture->meta.predicate_count) return ORM_STATUS_OUT_OF_RANGE;
  *out = fixture->predicates[index];
  return ORM_STATUS_OK;
}

static orm_status_t ORM_DRIVER_CALL fake_parameter(
    const void *context, uint64_t index, orm_driver_value_v1 *out,
    orm_error_t *error) {
  bridge_fixture *fixture = (bridge_fixture *)context;
  ++fixture->parameter_calls;
  (void)error;
  if (index >= fixture->meta.raw_parameter_count) return ORM_STATUS_OUT_OF_RANGE;
  *out = fixture->parameters[index];
  return ORM_STATUS_OK;
}

static const orm_driver_plan_metadata_ops_v1 metadata_ops = {
    H(orm_driver_plan_metadata_ops_v1),
    fake_describe, fake_column, fake_ordering};
static const orm_driver_plan_value_ops_v1 value_ops = {
    H(orm_driver_plan_value_ops_v1),
    fake_assignment, fake_predicate, fake_parameter};

static orm_driver_limits_v1 limits(void) {
  const orm_driver_limits_v1 value = {
      H(orm_driver_limits_v1),
      16u, 16u, 16u, 16u,
      4096u, 4096u, 1024u, 65536u};
  return value;
}

static orm_driver_plan_view_v1 view(bridge_fixture *fixture) {
  const orm_driver_plan_view_v1 value = {
      H(orm_driver_plan_view_v1), fixture,
      TABLE(&metadata_ops), TABLE(&value_ops)};
  return value;
}

static orm_driver_value_v1 value_null(void) {
  const orm_driver_value_v1 value = {
      H(orm_driver_value_v1), ORM_VALUE_NULL, 0u, {0}};
  return value;
}
static orm_driver_value_v1 value_sint(int64_t v) {
  orm_driver_value_v1 value = {
      H(orm_driver_value_v1), ORM_VALUE_INT64, 0u, {0}};
  value.data.sint = v;
  return value;
}
static orm_driver_value_v1 value_uint(uint64_t v) {
  orm_driver_value_v1 value = {
      H(orm_driver_value_v1), ORM_VALUE_UINT64, 0u, {0}};
  value.data.uint = v;
  return value;
}
static orm_driver_value_v1 value_real(double v) {
  orm_driver_value_v1 value = {
      H(orm_driver_value_v1), ORM_VALUE_DOUBLE, 0u, {0}};
  value.data.real = v;
  return value;
}
static orm_driver_value_v1 value_bool(uint8_t v) {
  orm_driver_value_v1 value = {
      H(orm_driver_value_v1), ORM_VALUE_BOOLEAN, 0u, {0}};
  value.data.boolean = v;
  return value;
}
static orm_driver_value_v1 value_bytes(uint32_t kind, void *data, size_t size) {
  orm_driver_value_v1 value = {
      H(orm_driver_value_v1), kind, 0u, {0}};
  value.data.bytes.data = data;
  value.data.bytes.size = size;
  return value;
}

static void reset_fixture(bridge_fixture *fixture, uint32_t kind) {
  memset(fixture, 0, sizeof(*fixture));
  fixture->meta.header = (orm_driver_header_v1)H(orm_driver_plan_meta_v1);
  fixture->meta.kind = kind;
  fixture->ordering.header =
      (orm_driver_header_v1)H(orm_driver_ordering_v1);
}

static void set_assignment(bridge_fixture *fixture, size_t index,
                           const char *column, orm_driver_value_v1 value) {
  fixture->assignments[index].header =
      (orm_driver_header_v1)H(orm_driver_assignment_v1);
  fixture->assignments[index].column.data = column;
  fixture->assignments[index].column.size = strlen(column);
  fixture->assignments[index].value = value;
}

static void set_predicate(bridge_fixture *fixture, size_t index,
                          const char *column, uint32_t comparison,
                          orm_driver_value_v1 value) {
  fixture->predicates[index].header =
      (orm_driver_header_v1)H(orm_driver_predicate_v1);
  fixture->predicates[index].column.data = column;
  fixture->predicates[index].column.size = strlen(column);
  fixture->predicates[index].comparison = comparison;
  fixture->predicates[index].value = value;
}

static void check_kind(bridge_fixture *fixture, uint32_t driver_kind,
                       orm_query_kind expected) {
  orm_driver_limits_v1 lim = limits();
  orm_driver_plan_view_v1 v = view(fixture);
  orm_query_plan plan;
  orm_error_t error;
  fixture->meta.kind = driver_kind;
  check_equal(orm_driver_backend_plan_materialize(
                  &v, &lim, &plan, &error),
              ORM_STATUS_OK);
  check_equal(plan.kind, expected);
  orm_driver_backend_plan_destroy(&plan);
}

spec("Driver backend plan bridge") {
  it("materializes SELECT metadata through public callbacks and owns views") {
    bridge_fixture fixture;
    reset_fixture(&fixture, ORM_DRIVER_PLAN_SELECT);
    char table[] = "public.items";
    char first[] = "id";
    char second[] = "name";
    char order[] = "public.items.id";
    fixture.meta.table = (orm_driver_bytes_v1){table, strlen(table)};
    fixture.meta.column_count = 2u;
    fixture.meta.flags = ORM_DRIVER_PLAN_HAS_LIMIT | ORM_DRIVER_PLAN_HAS_OFFSET;
    fixture.meta.limit = 7u;
    fixture.meta.offset = 3u;
    fixture.columns[0] = (orm_driver_bytes_v1){first, strlen(first)};
    fixture.columns[1] = (orm_driver_bytes_v1){second, strlen(second)};
    fixture.ordering.present = 1u;
    fixture.ordering.order = ORM_ORDER_DESCENDING;
    fixture.ordering.column =
        (orm_driver_bytes_v1){order, strlen(order)};

    orm_driver_limits_v1 lim = limits();
    orm_driver_plan_view_v1 v = view(&fixture);
    orm_query_plan plan;
    orm_error_t error;
    check_equal(orm_driver_backend_plan_materialize(
                    &v, &lim, &plan, &error),
                ORM_STATUS_OK);
    check_equal(plan.kind, ORM_QUERY_SELECT);
    check_equal(strcmp(plan.table, "public.items"), 0);
    check_equal(vec_size(&plan.columns), (size_t)2u);
    check_equal(strcmp(*(tstr *)vec_at(&plan.columns, 0u), "id"), 0);
    check_equal(strcmp(*(tstr *)vec_at(&plan.columns, 1u), "name"), 0);
    check_true(plan.has_limit);
    check_true(plan.has_offset);
    check_equal(plan.limit, UINT64_C(7));
    check_equal(plan.offset, UINT64_C(3));
    check_true(plan.ordering.present);
    check_equal(plan.ordering.order, ORM_ORDER_DESCENDING);
    check_equal(strcmp(plan.ordering.column, "public.items.id"), 0);

    table[0] = 'X';
    first[0] = 'X';
    order[0] = 'X';
    check_equal(strcmp(plan.table, "public.items"), 0);
    check_equal(strcmp(*(tstr *)vec_at(&plan.columns, 0u), "id"), 0);
    check_equal(strcmp(plan.ordering.column, "public.items.id"), 0);
    check_equal(fixture.describe_calls, 1u);
    check_equal(fixture.column_calls, 2u);
    check_equal(fixture.ordering_calls, 1u);
    orm_driver_backend_plan_destroy(&plan);
  }

  it("materializes every plan kind through the public plan surface") {
    bridge_fixture fixture;
    char text[] = "x";

    reset_fixture(&fixture, ORM_DRIVER_PLAN_SELECT);
    fixture.meta.table = (orm_driver_bytes_v1){"items", 5u};
    fixture.meta.flags = ORM_DRIVER_PLAN_SELECT_ALL;
    check_kind(&fixture, ORM_DRIVER_PLAN_SELECT, ORM_QUERY_SELECT);

    reset_fixture(&fixture, ORM_DRIVER_PLAN_INSERT);
    fixture.meta.table = (orm_driver_bytes_v1){"items", 5u};
    fixture.meta.assignment_count = 1u;
    set_assignment(&fixture, 0u, "name",
                   value_bytes(ORM_VALUE_TEXT, text, 1u));
    check_kind(&fixture, ORM_DRIVER_PLAN_INSERT, ORM_QUERY_INSERT);

    reset_fixture(&fixture, ORM_DRIVER_PLAN_UPDATE);
    fixture.meta.table = (orm_driver_bytes_v1){"items", 5u};
    fixture.meta.assignment_count = 1u;
    fixture.meta.predicate_count = 1u;
    set_assignment(&fixture, 0u, "name",
                   value_bytes(ORM_VALUE_TEXT, text, 1u));
    set_predicate(&fixture, 0u, "id", ORM_COMPARE_EQUAL, value_sint(1));
    check_kind(&fixture, ORM_DRIVER_PLAN_UPDATE, ORM_QUERY_UPDATE);

    reset_fixture(&fixture, ORM_DRIVER_PLAN_DELETE);
    fixture.meta.table = (orm_driver_bytes_v1){"items", 5u};
    fixture.meta.predicate_count = 1u;
    set_predicate(&fixture, 0u, "id", ORM_COMPARE_EQUAL, value_sint(1));
    check_kind(&fixture, ORM_DRIVER_PLAN_DELETE, ORM_QUERY_DELETE);

    reset_fixture(&fixture, ORM_DRIVER_PLAN_RAW_SQL);
    fixture.meta.raw_sql = (orm_driver_bytes_v1){"select ?1", 9u};
    fixture.meta.raw_parameter_count = 1u;
    fixture.parameters[0] = value_sint(1);
    check_kind(&fixture, ORM_DRIVER_PLAN_RAW_SQL, ORM_QUERY_RAW);
  }

  it("copies all public value kinds and raw parameter bytes") {
    bridge_fixture fixture;
    reset_fixture(&fixture, ORM_DRIVER_PLAN_INSERT);
    char text[] = "hello";
    unsigned char blob[] = {0u, 1u, 2u, 3u};
    fixture.meta.table = (orm_driver_bytes_v1){"items", 5u};
    fixture.meta.assignment_count = 7u;
    set_assignment(&fixture, 0u, "n", value_null());
    set_assignment(&fixture, 1u, "i", value_sint(-7));
    set_assignment(&fixture, 2u, "u", value_uint(UINT64_C(9)));
    set_assignment(&fixture, 3u, "r", value_real(2.5));
    set_assignment(&fixture, 4u, "b", value_bool(1u));
    set_assignment(&fixture, 5u, "t",
                   value_bytes(ORM_VALUE_TEXT, text, strlen(text)));
    set_assignment(&fixture, 6u, "x",
                   value_bytes(ORM_VALUE_BLOB, blob, sizeof(blob)));

    orm_driver_limits_v1 lim = limits();
    orm_driver_plan_view_v1 v = view(&fixture);
    orm_query_plan plan;
    orm_error_t error;
    check_equal(orm_driver_backend_plan_materialize(
                    &v, &lim, &plan, &error),
                ORM_STATUS_OK);
    check_equal(vec_size(&plan.assignments), (size_t)7u);
    const orm_assignment *a =
        (const orm_assignment *)vec_at_const(&plan.assignments, 5u);
    const orm_assignment *x =
        (const orm_assignment *)vec_at_const(&plan.assignments, 6u);
    check_not_null(a);
    check_not_null(x);
    check_equal(a->value.kind, ORM_VALUE_TEXT);
    check_equal(strcmp(a->value.bytes, "hello"), 0);
    check_equal(x->value.kind, ORM_VALUE_BLOB);
    check_equal(tstr_len(x->value.bytes), sizeof(blob));
    check_equal(memcmp(x->value.bytes, blob, sizeof(blob)), 0);
    check_equal(plan.parameter_bytes, strlen(text) + sizeof(blob));

    text[0] = 'X';
    blob[0] = 9u;
    check_equal(strcmp(a->value.bytes, "hello"), 0);
    check_equal(((const unsigned char *)x->value.bytes)[0], 0u);
    orm_driver_backend_plan_destroy(&plan);
  }

  it("rejects short tables before invoking host callbacks") {
    bridge_fixture fixture;
    reset_fixture(&fixture, ORM_DRIVER_PLAN_SELECT);
    fixture.meta.table = (orm_driver_bytes_v1){"items", 5u};
    fixture.meta.flags = ORM_DRIVER_PLAN_SELECT_ALL;
    orm_driver_limits_v1 lim = limits();
    orm_driver_plan_view_v1 v = view(&fixture);
    orm_query_plan plan;
    orm_error_t error;

    v.metadata.bytes = ORM_DRIVER_HEADER_BYTES;
    check_equal(orm_driver_backend_plan_materialize(
                    &v, &lim, &plan, &error),
                ORM_STATUS_ABI_MISMATCH);
    check_equal(fixture.describe_calls, 0u);
    check_equal(vec_size(&plan.columns), (size_t)0u);
  }

  it("rejects metadata counts and parameter bytes before retaining host views") {
    bridge_fixture fixture;
    reset_fixture(&fixture, ORM_DRIVER_PLAN_RAW_SQL);
    char payload[] = "0123456789";
    fixture.meta.raw_sql = (orm_driver_bytes_v1){"select ?1", 9u};
    fixture.meta.raw_parameter_count = 17u;

    orm_driver_limits_v1 lim = limits();
    orm_driver_plan_view_v1 v = view(&fixture);
    orm_query_plan plan;
    orm_error_t error;
    check_equal(orm_driver_backend_plan_materialize(
                    &v, &lim, &plan, &error),
                ORM_STATUS_ABI_MISMATCH);
    check_equal(fixture.parameter_calls, 0u);

    fixture.meta.raw_parameter_count = 1u;
    fixture.parameters[0] =
        value_bytes(ORM_VALUE_BLOB, payload, sizeof(payload) - 1u);
    lim.max_parameter_bytes = 4u;
    check_equal(orm_driver_backend_plan_materialize(
                    &v, &lim, &plan, &error),
                ORM_STATUS_LIMIT_EXCEEDED);
    check_equal(fixture.parameter_calls, 1u);
    check_equal(vec_size(&plan.raw_parameters), (size_t)0u);
  }
}

#include "orm_internal.h"
#include "row.h"

#include <tinytest.h>

#include <string.h>

enum {
  TEST_MAX_FIELDS = 16u,
  TEST_MAX_BYTES = 4096u
};

static orm_owned_value test_i64(int64_t value) {
  orm_owned_value result = {0};
  result.kind = ORM_VALUE_INT64;
  result.data.int64_value = value;
  return result;
}

static orm_owned_value test_text(const char *value) {
  orm_owned_value result = {0};
  result.kind = ORM_VALUE_TEXT;
  result.bytes = tstr_dup(value);
  return result;
}

static orm_owned_value test_blob(const void *data, size_t size) {
  orm_owned_value result = {0};
  result.kind = ORM_VALUE_BLOB;
  result.bytes = tstr_new_len(data, size);
  return result;
}

static void test_value_destroy(orm_owned_value *value) {
  tstr_freep(&value->bytes);
}

static orm_status_t test_set(orm_tidesdb_row *row, const char *name,
                             const orm_owned_value *value,
                             orm_error_t *error) {
  orm_tidesdb_cell cell = {0};
  orm_status_t status = orm_tidesdb_cell_from_value(&cell, value, error);
  if (status == ORM_STATUS_OK)
    status = orm_tidesdb_row_set(row, vstr_from_cstr(name), &cell, error);
  tstr_freep(&cell.bytes);
  return status;
}

static orm_limits test_limits(void) {
  orm_limits limits = {0};
  limits.max_parameters = TEST_MAX_FIELDS;
  limits.max_columns = TEST_MAX_FIELDS;
  limits.max_predicates = TEST_MAX_FIELDS;
  limits.max_assignments = TEST_MAX_FIELDS;
  limits.max_query_bytes = TEST_MAX_BYTES;
  limits.max_parameter_bytes = TEST_MAX_BYTES;
  limits.max_result_rows = 128u;
  limits.max_result_bytes = TEST_MAX_BYTES;
  return limits;
}

spec("ORM pure C TidesDB row") {
  it("decodes borrowed field views for every stored scalar kind and clears truncated results") {
    enum { FIELD_COUNT = 7, COUNT_SENTINEL = 99 };
    const unsigned char payload[] = {'a', 0, 'b'};
    orm_owned_value values[] = {
      {.kind = ORM_VALUE_NULL}, {.kind = ORM_VALUE_INT64, .data.int64_value = INT64_MIN},
      {.kind = ORM_VALUE_UINT64, .data.uint64_value = UINT64_MAX},
      {.kind = ORM_VALUE_DOUBLE, .data.double_value = 1.5},
      {.kind = ORM_VALUE_BOOLEAN, .data.boolean_value = 1}, test_text("hello"), test_blob(payload, sizeof(payload))};
    const char *names[] = {"nil", "signed", "unsigned", "floating", "boolean", "text", "blob"};
    orm_error_t error; orm_error_init(&error);
    orm_tidesdb_row row = {0}; check_equal(orm_tidesdb_row_init(&row, FIELD_COUNT, &error), ORM_STATUS_OK);
    for (size_t i = 0; i < FIELD_COUNT; ++i) {
      check_equal(test_set(&row, names[i], &values[i], &error), ORM_STATUS_OK); test_value_destroy(&values[i]);
    }
    tstr encoded = NULL; check_equal(orm_tidesdb_row_encode(&row, TEST_MAX_BYTES, &encoded, &error), ORM_STATUS_OK);
    orm_tidesdb_row_destroy(&row);
    orm_tidesdb_field_view fields[FIELD_COUNT] = {0}; size_t count = COUNT_SENTINEL;
    const unsigned char *data = (const unsigned char *)encoded; const size_t size = tstr_len(encoded);
    check_equal(orm_tidesdb_row_decode_view(data, size, TEST_MAX_BYTES, fields, FIELD_COUNT, &count, &error), ORM_STATUS_OK);
    check_equal(count, FIELD_COUNT); check_equal(fields[0].value.kind, ORM_VALUE_NULL);
    check_equal(fields[1].value.data.int64_value, INT64_MIN); check_equal(fields[2].value.data.uint64_value, UINT64_MAX);
    check_equal(fields[3].value.data.double_value, 1.5); check_equal(fields[4].value.data.boolean_value, 1);
    check_equal(memcmp(fields[5].value.data.text_value.data, "hello", 5), 0);
    check_equal(memcmp(fields[6].value.data.blob_value.data, payload, sizeof(payload)), 0);
    check_equal(fields[5].value.data.text_value.data >= encoded, true);
    check_equal(fields[5].value.data.text_value.data < encoded + size, true);
    for (size_t truncated = 0; truncated < size; ++truncated) {
      count = COUNT_SENTINEL;
      check_equal(orm_tidesdb_row_decode_view(data, truncated, TEST_MAX_BYTES, fields, FIELD_COUNT, &count, &error), ORM_STATUS_DATASTORE_ERROR);
      check_equal(count, COUNT_SENTINEL);
      for (size_t i = 0; i < FIELD_COUNT; ++i) { check_null(fields[i].name.data); check_equal(fields[i].value.kind, ORM_VALUE_NULL); }
    }
    check_equal(orm_tidesdb_row_decode_view(data, size, size - 1, fields, FIELD_COUNT, &count, &error), ORM_STATUS_LIMIT_EXCEEDED);
    check_equal(orm_tidesdb_row_decode_view(data, size, TEST_MAX_BYTES, fields, FIELD_COUNT - 1, &count, &error), ORM_STATUS_LIMIT_EXCEEDED);
    tstr_free(encoded);
  }

  it("round-trips typed fields including null and embedded blob bytes") {
    static const unsigned char blob_bytes[] = {'A', 0u, 'B', 0u, 'C'};
    orm_tidesdb_row source = {0};
    orm_tidesdb_row decoded = {0};
    orm_owned_value id = test_i64(-7);
    orm_owned_value name = test_text("A:B");
    orm_owned_value blob = test_blob(blob_bytes, sizeof(blob_bytes));
    orm_owned_value null_value = {0};
    orm_error_t error;
    tstr encoded = NULL;
    const orm_tidesdb_cell *found;

    null_value.kind = ORM_VALUE_NULL;
    orm_error_init(&error);
    check_equal(orm_tidesdb_row_init(&source, TEST_MAX_FIELDS, &error),
                ORM_STATUS_OK);
    check_equal(test_set(&source, "id", &id, &error), ORM_STATUS_OK);
    check_equal(test_set(&source, "name", &name, &error), ORM_STATUS_OK);
    check_equal(test_set(&source, "note", &null_value, &error), ORM_STATUS_OK);
    check_equal(test_set(&source, "payload", &blob, &error), ORM_STATUS_OK);
    check_equal(orm_tidesdb_row_encode(&source, TEST_MAX_BYTES, &encoded,
                                       &error), ORM_STATUS_OK);
    check_equal(orm_tidesdb_row_decode((const unsigned char *)encoded,
                                       tstr_len(encoded), TEST_MAX_BYTES,
                                       TEST_MAX_FIELDS, &decoded, &error),
                ORM_STATUS_OK);

    found = orm_tidesdb_row_find(&decoded, vstr_from_cstr("id"));
    check_not_null(found);
    if (found != NULL) {
      check_equal(found->kind, ORM_VALUE_INT64);
      check_equal(tstr_len(found->bytes), (size_t)2u);
      check_equal(memcmp(found->bytes, "-7", 2u), 0);
    }
    found = orm_tidesdb_row_find(&decoded, vstr_from_cstr("note"));
    check_not_null(found);
    if (found != NULL)
      check_true(found->is_null);
    found = orm_tidesdb_row_find(&decoded, vstr_from_cstr("payload"));
    check_not_null(found);
    if (found != NULL) {
      check_equal(found->kind, ORM_VALUE_BLOB);
      check_equal(tstr_len(found->bytes), sizeof(blob_bytes));
      check_equal(memcmp(found->bytes, blob_bytes, sizeof(blob_bytes)), 0);
    }

    tstr_freep(&encoded);
    orm_tidesdb_row_destroy(&decoded);
    orm_tidesdb_row_destroy(&source);
    test_value_destroy(&blob);
    test_value_destroy(&name);
  }

  it("rejects an incompatible version and a truncated persisted row") {
    orm_tidesdb_row row = {0};
    orm_tidesdb_row decoded = {0};
    orm_owned_value id = test_i64(1);
    orm_error_t error;
    tstr encoded = NULL;

    orm_error_init(&error);
    check_equal(orm_tidesdb_row_init(&row, TEST_MAX_FIELDS, &error),
                ORM_STATUS_OK);
    check_equal(test_set(&row, "id", &id, &error), ORM_STATUS_OK);
    check_equal(orm_tidesdb_row_encode(&row, TEST_MAX_BYTES, &encoded, &error),
                ORM_STATUS_OK);
    encoded[6] = 2;
    check_equal(orm_tidesdb_row_decode((const unsigned char *)encoded,
                                       tstr_len(encoded), TEST_MAX_BYTES,
                                       TEST_MAX_FIELDS, &decoded, &error),
                ORM_STATUS_DATASTORE_ERROR);
    encoded[6] = 1;
    check_equal(orm_tidesdb_row_decode((const unsigned char *)encoded,
                                       tstr_len(encoded) - 1u,
                                       TEST_MAX_BYTES, TEST_MAX_FIELDS,
                                       &decoded, &error),
                ORM_STATUS_DATASTORE_ERROR);

    tstr_freep(&encoded);
    orm_tidesdb_row_destroy(&row);
  }

  it("evaluates flat predicates and projects missing fields as null") {
    orm_limits limits = test_limits();
    orm_tidesdb_row row = {0};
    orm_tidesdb_row projected = {0};
    orm_owned_value status = test_text("active");
    orm_query_plan plan;
    orm_error_t error;
    bool matched = false;
    const orm_tidesdb_cell *missing;

    orm_error_init(&error);
    check_equal(orm_tidesdb_row_init(&row, TEST_MAX_FIELDS, &error),
                ORM_STATUS_OK);
    check_equal(test_set(&row, "status", &status, &error), ORM_STATUS_OK);
    check_equal(orm_plan_init(&plan, ORM_QUERY_SELECT,
                              vstr_from_cstr("person"), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_column(&plan, vstr_from_cstr("status"), &limits,
                                    &error), ORM_STATUS_OK);
    check_equal(orm_plan_add_column(&plan, vstr_from_cstr("nickname"), &limits,
                                    &error), ORM_STATUS_OK);
    check_equal(orm_plan_add_predicate(&plan,
                                       vstr_from_cstr("person.status"),
                                       ORM_COMPARE_EQUAL, orm_text("active"),
                                       &limits, &error), ORM_STATUS_OK);
    check_equal(orm_tidesdb_row_matches(&row, &plan, &matched, &error),
                ORM_STATUS_OK);
    check_true(matched);
    check_equal(orm_tidesdb_row_project(&row, &plan, &limits, &projected,
                                        &error), ORM_STATUS_OK);
    missing = orm_tidesdb_row_find(&projected,
                                   vstr_from_cstr("nickname"));
    check_not_null(missing);
    if (missing != NULL)
      check_true(missing->is_null);

    orm_tidesdb_row_destroy(&projected);
    orm_plan_destroy(&plan);
    orm_tidesdb_row_destroy(&row);
    test_value_destroy(&status);
  }
}

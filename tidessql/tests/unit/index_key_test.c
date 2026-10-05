#include "index.h"
#include <tinytest.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static size_t reserves, resizes;
static bool forbid_allocation;
static stl_status key_test_reserve(vec_t *v, size_t n) {
  ++reserves; return forbid_allocation ? STL_OUT_OF_MEMORY : vec_reserve(v, n);
}
static stl_status key_test_resize(vec_t *v, size_t n) {
  ++resizes; return forbid_allocation ? STL_OUT_OF_MEMORY : vec_resize(v, n);
}
#define vec_reserve key_test_reserve
#define vec_resize key_test_resize
#include "../../src/work.c"
#undef vec_reserve
#undef vec_resize

enum { TEST_LIMIT = 1000000, TEST_WORK = 4 * 1024 * 1024, TEST_COLUMNS = 4,
       TEST_KEY_PARTS = 3, TEST_CELL = 9, TEST_KEY_BYTES = TEST_KEY_PARTS * TEST_CELL,
       TEST_SQL_BYTES = 128, TEST_SENTINEL = 0xa5 };
static const orm_sql_schema_column columns[] = {
  {{"id", 2}, {TURBODB_VALUE_INT64, false}}, {{"score", 5}, {TURBODB_VALUE_INT64, true}},
  {{"amount", 6}, {TURBODB_VALUE_UINT64, true}}, {{"weight", 6}, {TURBODB_VALUE_DOUBLE, true}}
};
static const orm_sql_table_schema schema = {{"items", 5}, columns, TEST_COLUMNS};
static const char basic_ddl[] = "CREATE UNIQUE INDEX ix ON items (score, amount DESC)";
static orm_tidesdb_sql_budget budget;
static orm_sql_budget_limits limits;
static orm_sql_index_definition definition;
static turbodb_error_t error;
static size_t key_size;

static void bind_key(const char *sql) {
  sqlparser_document *document = NULL; sqlparser_error parser_error;
  check_equal(sqlparser_parse_dialect(sql, strlen(sql), SQLPARSER_MYSQL, NULL, &document, &parser_error), SQLPARSER_OK);
  check_equal(orm_tidesdb_sql_index_bind_create(document, &schema, &budget, &definition, &error), TURBODB_STATUS_OK);
  sqlparser_document_destroy(document);
  check_equal(orm_tidesdb_sql_index_key_size(&definition, &key_size, &error), TURBODB_STATUS_OK);
}
static void double_key(bool descending, bool composite) {
  bind_key(composite ? descending ? "CREATE UNIQUE INDEX ix ON items (score,amount,weight DESC)" :
      "CREATE UNIQUE INDEX ix ON items (score,amount,weight)" :
      descending ? "CREATE UNIQUE INDEX ix ON items (weight DESC)" : "CREATE UNIQUE INDEX ix ON items (weight)");
}
static void reset(void) {
  forbid_allocation = false;
  check_equal(orm_tidesdb_sql_index_destroy(&definition, &error), TURBODB_STATUS_OK);
  check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
  check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_init(&budget, &limits, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
}
static bool encode(const turbodb_value_t row[TEST_COLUMNS], uint8_t *bytes) {
  bool contains_null = false;
  check_equal(orm_tidesdb_sql_index_key_encode(&definition, row, TEST_COLUMNS, bytes,
      TEST_KEY_BYTES, &contains_null, &error), TURBODB_STATUS_OK);
  return contains_null;
}
static void equal_value(turbodb_value_t actual, turbodb_value_t expected) {
  check_equal(actual.kind, expected.kind); check_equal(actual.reserved, 0u);
  if (expected.kind == TURBODB_VALUE_INT64) check_equal(actual.data.int64_value, expected.data.int64_value);
  if (expected.kind == TURBODB_VALUE_UINT64) check_equal(actual.data.uint64_value, expected.data.uint64_value);
  if (expected.kind == TURBODB_VALUE_DOUBLE) check_true(actual.data.double_value == expected.data.double_value);
}
static int sign(int value) { return value < 0 ? -1 : value > 0 ? 1 : 0; }
static int order(const turbodb_value_t *a, const turbodb_value_t *b) {
  for (size_t i = 0; i < vec_size(&definition.parts); ++i) {
    const orm_sql_index_part *part = vec_at_const(&definition.parts, i);
    int compared = orm_sql_value_order(&a[part->column], &b[part->column]);
    if (compared) return part->descending ? -compared : compared;
  }
  return 0;
}
static void remaining_steps(uint64_t amount) {
  const uint64_t available = budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS] -
      budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
  check_true(available >= amount);
  orm_sql_budget_amount charge = {0}; charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = available - amount;
  check_equal(orm_tidesdb_sql_budget_reserve(&budget, &charge, &error), TURBODB_STATUS_OK);
}
static void reject_bytes(const uint8_t *bytes, size_t size) {
  turbodb_value_t output[TEST_KEY_PARTS], before[TEST_KEY_PARTS];
  memset(output, TEST_SENTINEL, sizeof(output)); memcpy(before, output, sizeof(output));
  bool contains_null = false;
  check_equal(orm_tidesdb_sql_index_key_decode(&definition, bytes, size, output,
      TEST_KEY_PARTS, &contains_null, &error), TURBODB_STATUS_DATASTORE_ERROR);
  check_equal(memcmp(output, before, sizeof(output)), 0); check_false(contains_null);
}

spec("TidesDB numeric index key codec") {
  before_each() {
    reserves = resizes = 0; forbid_allocation = false;
    definition = (orm_sql_index_definition){0}; key_size = 0; tdsql_error_init(&error);
    limits = (orm_sql_budget_limits){0};
    for (size_t i = 0; i < ORM_SQL_BUDGET_RESOURCE_COUNT; ++i) limits.statement.value[i] = TEST_LIMIT;
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = TEST_WORK;
    limits.transaction = (orm_sql_transaction_budget_amount){TEST_LIMIT, TEST_LIMIT, TEST_LIMIT};
    check_equal(orm_tidesdb_sql_budget_init(&budget, &limits, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
  }
  after_each() {
    forbid_allocation = false;
    check_equal(orm_tidesdb_sql_index_destroy(&definition, &error), TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS], 0u);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
  }
  it("writes stable big endian composite bytes including sign and descending markers") {
    bind_key(basic_ddl);
    const uint8_t golden[] = {1,0x7f,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,
      0xfe,0xfe,0xfd,0xfc,0xfb,0xfa,0xf9,0xf8,0xf7};
    turbodb_value_t row[] = {turbodb_i64(7), turbodb_i64(-2), turbodb_u64(UINT64_C(0x0102030405060708)), turbodb_f64(3.0)};
    uint8_t bytes[TEST_KEY_BYTES]; memset(bytes, TEST_SENTINEL, sizeof(bytes));
    check_false(encode(row, bytes)); check_equal(key_size, sizeof(golden));
    check_equal(memcmp(bytes, golden, sizeof(golden)), 0);
    for (size_t i = key_size; i < sizeof(bytes); ++i) check_equal(bytes[i], TEST_SENTINEL);
    turbodb_value_t decoded[TEST_KEY_PARTS] = {turbodb_null(), turbodb_null(), turbodb_i64(91)}; bool contains_null = true;
    check_equal(orm_tidesdb_sql_index_key_decode(&definition, golden, sizeof(golden), decoded,
        TEST_KEY_PARTS, &contains_null, &error), TURBODB_STATUS_OK);
    equal_value(decoded[0], row[1]); equal_value(decoded[1], row[2]); equal_value(decoded[2], turbodb_i64(91));
    check_false(contains_null);
  }
  it("orders and round trips signed endpoints across every byte boundary") {
    const int64_t samples[] = {INT64_MIN, INT64_MIN+1, -INT64_C(0x100000000), -65536, -256, -1,
      0, 1, 255, 256, 65535, 65536, INT64_C(0xffffffff), INT64_C(0x100000000), INT64_MAX-1, INT64_MAX};
    for (unsigned descending = 0; descending < 2; ++descending) {
      reset(); bind_key(descending ? "CREATE INDEX ix ON items (score DESC)" : "CREATE INDEX ix ON items (score)");
      uint8_t previous[TEST_KEY_BYTES], current[TEST_KEY_BYTES];
      turbodb_value_t row[] = {turbodb_i64(1), turbodb_null(), turbodb_null(), turbodb_null()};
      check_true(encode(row, previous));
      for (size_t i = 0; i < sizeof(samples)/sizeof(samples[0]); ++i) {
        row[1] = turbodb_i64(samples[i]); check_false(encode(row, current));
        check_equal(sign(memcmp(previous, current, key_size)), descending ? 1 : -1);
        turbodb_value_t decoded; bool contains_null = true;
        check_equal(orm_tidesdb_sql_index_key_decode(&definition, current, key_size, &decoded,
            1, &contains_null, &error), TURBODB_STATUS_OK);
        equal_value(decoded, row[1]); check_false(contains_null); memcpy(previous, current, key_size);
      }
    }
  }
  it("orders and round trips unsigned values above INT64_MAX") {
    const uint64_t samples[] = {0, 1, 255, 256, 65535, 65536, UINT64_C(0xffffffff),
      UINT64_C(0x100000000), INT64_MAX, UINT64_C(0x8000000000000000), UINT64_MAX-1, UINT64_MAX};
    for (unsigned descending = 0; descending < 2; ++descending) {
      reset(); bind_key(descending ? "CREATE INDEX ix ON items (amount DESC)" : "CREATE INDEX ix ON items (amount)");
      uint8_t previous[TEST_KEY_BYTES], current[TEST_KEY_BYTES];
      turbodb_value_t row[] = {turbodb_i64(1), turbodb_null(), turbodb_null(), turbodb_null()};
      check_true(encode(row, previous));
      for (size_t i = 0; i < sizeof(samples)/sizeof(samples[0]); ++i) {
        row[2] = turbodb_u64(samples[i]); check_false(encode(row, current));
        check_equal(sign(memcmp(previous, current, key_size)), descending ? 1 : -1);
        turbodb_value_t decoded; bool contains_null = true;
        check_equal(orm_tidesdb_sql_index_key_decode(&definition, current, key_size, &decoded,
            1, &contains_null, &error), TURBODB_STATUS_OK);
        equal_value(decoded, row[2]); check_false(contains_null); memcpy(previous, current, key_size);
      }
    }
  }
  it("matches the SQL tuple comparator for all mixed directions and NULL combinations") {
    const turbodb_value_t signed_values[] = {turbodb_null(), turbodb_i64(INT64_MIN), turbodb_i64(-1), turbodb_i64(0), turbodb_i64(INT64_MAX)};
    const turbodb_value_t unsigned_values[] = {turbodb_null(), turbodb_u64(0), turbodb_u64(1), turbodb_u64(UINT64_C(0x8000000000000000)), turbodb_u64(UINT64_MAX)};
    enum { SAMPLE_COUNT = 5, ROW_COUNT = SAMPLE_COUNT * SAMPLE_COUNT };
    turbodb_value_t rows[ROW_COUNT][TEST_COLUMNS]; uint8_t bytes[ROW_COUNT][TEST_KEY_BYTES];
    for (unsigned direction = 0; direction < 4; ++direction) {
      reset(); char sql[TEST_SQL_BYTES];
      (void)snprintf(sql, sizeof(sql), "CREATE UNIQUE INDEX ix ON items (score %s,amount %s)",
          direction & 1 ? "DESC" : "ASC", direction & 2 ? "DESC" : "ASC");
      bind_key(sql);
      for (size_t i = 0; i < ROW_COUNT; ++i) {
        rows[i][0] = turbodb_i64((int64_t)i); rows[i][1] = signed_values[i / SAMPLE_COUNT];
        rows[i][2] = unsigned_values[i % SAMPLE_COUNT]; rows[i][3] = turbodb_null();
        check_equal(encode(rows[i], bytes[i]), rows[i][1].kind == TURBODB_VALUE_NULL || rows[i][2].kind == TURBODB_VALUE_NULL);
        turbodb_value_t decoded[TEST_KEY_PARTS]; bool contains_null = false;
        check_equal(orm_tidesdb_sql_index_key_decode(&definition, bytes[i], key_size, decoded,
            TEST_KEY_PARTS, &contains_null, &error), TURBODB_STATUS_OK);
        equal_value(decoded[0], rows[i][1]); equal_value(decoded[1], rows[i][2]);
      }
      for (size_t i = 0; i < ROW_COUNT; ++i) for (size_t j = 0; j < ROW_COUNT; ++j)
        check_equal(sign(memcmp(bytes[i], bytes[j], key_size)), order(rows[i], rows[j]));
    }
  }
  it("uses canonical NULL payloads independent of unused input union bytes") {
    bind_key(basic_ddl);
    turbodb_value_t row[] = {turbodb_i64(1), turbodb_null(), turbodb_null(), turbodb_null()};
    row[1].data.uint64_value = UINT64_MAX; row[2].data.uint64_value = UINT64_MAX;
    uint8_t bytes[TEST_KEY_BYTES]; check_true(encode(row, bytes));
    for (size_t i = 0; i < TEST_CELL; ++i) check_equal(bytes[i], 0);
    for (size_t i = TEST_CELL; i < key_size; ++i) check_equal(bytes[i], UINT8_MAX);
    turbodb_value_t decoded[TEST_KEY_PARTS]; bool contains_null = false;
    check_equal(orm_tidesdb_sql_index_key_decode(&definition, bytes, key_size, decoded,
        TEST_KEY_PARTS, &contains_null, &error), TURBODB_STATUS_OK);
    equal_value(decoded[0], turbodb_null()); equal_value(decoded[1], turbodb_null()); check_true(contains_null);
  }
  it("encodes equal unique tuples identically regardless of primary key and other columns") {
    bind_key(basic_ddl);
    turbodb_value_t a[] = {turbodb_i64(1), turbodb_i64(7), turbodb_u64(8), turbodb_f64(9.0)};
    turbodb_value_t b[] = {turbodb_i64(2), turbodb_i64(7), turbodb_u64(8), turbodb_text("not a key column")};
    uint8_t left[TEST_KEY_BYTES], right[TEST_KEY_BYTES];
    check_false(encode(a, left)); check_false(encode(b, right));
    check_equal(memcmp(left, right, key_size), 0);
    b[1] = turbodb_null(); check_true(encode(b, right));
    check_not_equal(memcmp(left, right, key_size), 0);
  }
  it("projects nonsequential key slots and leaves caller input unchanged") {
    bind_key("CREATE INDEX ix ON items (amount DESC,id ASC,score DESC)");
    turbodb_value_t row[] = {turbodb_i64(INT64_MIN), turbodb_i64(INT64_MAX), turbodb_u64(UINT64_MAX), turbodb_f64(1.0)};
    turbodb_value_t saved[TEST_COLUMNS]; memcpy(saved, row, sizeof(row));
    uint8_t bytes[TEST_KEY_BYTES]; check_false(encode(row, bytes)); check_equal(key_size, TEST_KEY_BYTES);
    check_equal(memcmp(row, saved, sizeof(row)), 0);
    memset(row, 0, sizeof(row));
    turbodb_value_t decoded[TEST_KEY_PARTS]; bool contains_null = true;
    check_equal(orm_tidesdb_sql_index_key_decode(&definition, bytes, key_size, decoded,
        TEST_KEY_PARTS, &contains_null, &error), TURBODB_STATUS_OK);
    equal_value(decoded[0], saved[2]); equal_value(decoded[1], saved[0]); equal_value(decoded[2], saved[1]);
  }
  it("rejects every truncation and a trailing byte without partial output") {
    bind_key(basic_ddl);
    turbodb_value_t row[] = {turbodb_i64(1), turbodb_i64(-1), turbodb_u64(UINT64_MAX), turbodb_null()};
    uint8_t bytes[TEST_KEY_BYTES]; check_false(encode(row, bytes));
    for (size_t size = 0; size < key_size; ++size) reject_bytes(bytes, size);
    reject_bytes(bytes, key_size + 1);
  }
  it("rejects every invalid marker and every noncanonical NULL payload byte") {
    bind_key(basic_ddl);
    turbodb_value_t row[] = {turbodb_i64(1), turbodb_null(), turbodb_null(), turbodb_null()};
    uint8_t bytes[TEST_KEY_BYTES], corrupted[TEST_KEY_BYTES]; check_true(encode(row, bytes));
    for (size_t slot = 0; slot < 2; ++slot) {
      for (unsigned marker = 2; marker <= UINT8_MAX; ++marker) {
        memcpy(corrupted, bytes, key_size);
        corrupted[slot * TEST_CELL] = slot ? (uint8_t)~marker : (uint8_t)marker;
        reject_bytes(corrupted, key_size);
      }
      for (size_t byte = 1; byte < TEST_CELL; ++byte) {
        memcpy(corrupted, bytes, key_size); corrupted[slot * TEST_CELL + byte] ^= 1;
        reject_bytes(corrupted, key_size);
      }
    }
  }
  it("rejects NULL in nonnullable keys in either direction") {
    for (unsigned descending = 0; descending < 2; ++descending) {
      reset(); bind_key(descending ? "CREATE INDEX ix ON items (id DESC)" : "CREATE INDEX ix ON items (id)");
      uint8_t bytes[TEST_KEY_BYTES]; memset(bytes, descending ? UINT8_MAX : 0, key_size);
      reject_bytes(bytes, key_size);
      turbodb_value_t row[] = {turbodb_null(), turbodb_null(), turbodb_null(), turbodb_null()}; bool contains_null = false;
      uint8_t before[TEST_KEY_BYTES]; memset(bytes, TEST_SENTINEL, sizeof(bytes)); memcpy(before, bytes, sizeof(bytes));
      check_equal(orm_tidesdb_sql_index_key_encode(&definition, row, TEST_COLUMNS, bytes,
          sizeof(bytes), &contains_null, &error), TURBODB_STATUS_TYPE_ERROR);
      check_equal(memcmp(bytes, before, sizeof(bytes)), 0); check_false(contains_null);
    }
  }
  it("preserves bytes and flags on late type or reserved-field errors") {
    bind_key(basic_ddl);
    turbodb_value_t bad[] = {turbodb_i64(9), turbodb_f64(9.0), turbodb_text("9"), turbodb_bool(true), turbodb_u64(9)};
    bad[sizeof(bad)/sizeof(bad[0])-1].reserved = 1;
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
      turbodb_value_t row[] = {turbodb_i64(1), turbodb_null(), bad[i], turbodb_null()};
      uint8_t bytes[TEST_KEY_BYTES], before[TEST_KEY_BYTES];
      memset(bytes, TEST_SENTINEL, sizeof(bytes)); memcpy(before, bytes, sizeof(bytes)); bool contains_null = false;
      check_equal(orm_tidesdb_sql_index_key_encode(&definition, row, TEST_COLUMNS, bytes,
          sizeof(bytes), &contains_null, &error), TURBODB_STATUS_TYPE_ERROR);
      check_equal(memcmp(bytes, before, sizeof(bytes)), 0); check_false(contains_null);
    }
  }
  it("enforces every encoder and decoder output capacity boundary") {
    bind_key(basic_ddl);
    turbodb_value_t row[] = {turbodb_i64(1), turbodb_i64(-1), turbodb_u64(2), turbodb_null()};
    uint8_t bytes[TEST_KEY_BYTES], before[TEST_KEY_BYTES];
    for (size_t capacity = 0; capacity < key_size; ++capacity) {
      memset(bytes, TEST_SENTINEL, sizeof(bytes)); memcpy(before, bytes, sizeof(bytes)); bool contains_null = true;
      check_equal(orm_tidesdb_sql_index_key_encode(&definition, row, TEST_COLUMNS, bytes,
          capacity, &contains_null, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(memcmp(bytes, before, sizeof(bytes)), 0); check_true(contains_null);
    }
    bool contains_null = true;
    check_equal(orm_tidesdb_sql_index_key_encode(&definition, row, TEST_COLUMNS, bytes,
        key_size, &contains_null, &error), TURBODB_STATUS_OK); check_false(contains_null);
    for (size_t capacity = 0; capacity < vec_size(&definition.parts); ++capacity) {
      turbodb_value_t out[TEST_KEY_PARTS], saved[TEST_KEY_PARTS];
      memset(out, TEST_SENTINEL, sizeof(out)); memcpy(saved, out, sizeof(out)); contains_null = true;
      check_equal(orm_tidesdb_sql_index_key_decode(&definition, bytes, key_size, out,
          capacity, &contains_null, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(memcmp(out, saved, sizeof(out)), 0); check_true(contains_null);
    }
  }
  it("preserves output at every encoder and decoder step boundary") {
    bind_key(basic_ddl);
    turbodb_value_t row[] = {turbodb_i64(1), turbodb_i64(-1), turbodb_u64(2), turbodb_null()};
    uint8_t encoded[TEST_KEY_BYTES]; const uint64_t start = budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_false(encode(row, encoded));
    const uint64_t encode_steps = budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS] - start;
    for (uint64_t capacity = 0; capacity <= encode_steps; ++capacity) {
      reset(); bind_key(basic_ddl); remaining_steps(capacity);
      uint8_t bytes[TEST_KEY_BYTES], before[TEST_KEY_BYTES];
      memset(bytes, TEST_SENTINEL, sizeof(bytes)); memcpy(before, bytes, sizeof(bytes)); bool contains_null = true;
      check_equal(orm_tidesdb_sql_index_key_encode(&definition, row, TEST_COLUMNS, bytes, sizeof(bytes),
          &contains_null, &error), capacity == encode_steps ? TURBODB_STATUS_OK : TURBODB_STATUS_LIMIT_EXCEEDED);
      if (capacity < encode_steps) { check_equal(memcmp(bytes, before, sizeof(bytes)), 0); check_true(contains_null); }
      else { check_equal(memcmp(bytes, encoded, key_size), 0); check_false(contains_null); }
    }
    reset(); bind_key(basic_ddl); const uint64_t decoding_start = budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    turbodb_value_t out[TEST_KEY_PARTS]; bool contains_null = true;
    check_equal(orm_tidesdb_sql_index_key_decode(&definition, encoded, key_size, out,
        TEST_KEY_PARTS, &contains_null, &error), TURBODB_STATUS_OK);
    const uint64_t decode_steps = budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS] - decoding_start;
    for (uint64_t capacity = 0; capacity <= decode_steps; ++capacity) {
      reset(); bind_key(basic_ddl); remaining_steps(capacity);
      turbodb_value_t before[TEST_KEY_PARTS]; memset(out, TEST_SENTINEL, sizeof(out)); memcpy(before, out, sizeof(out)); contains_null = true;
      check_equal(orm_tidesdb_sql_index_key_decode(&definition, encoded, key_size, out, TEST_KEY_PARTS,
          &contains_null, &error), capacity == decode_steps ? TURBODB_STATUS_OK : TURBODB_STATUS_LIMIT_EXCEEDED);
      if (capacity < decode_steps) { check_equal(memcmp(out, before, sizeof(out)), 0); check_true(contains_null); }
      else { equal_value(out[0], row[1]); equal_value(out[1], row[2]); check_false(contains_null); }
    }
  }
  it("runs after AST destruction with no workspace allocation or optional error object") {
    bind_key(basic_ddl); const size_t reserve_count = reserves, resize_count = resizes;
    const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; forbid_allocation = true;
    turbodb_value_t row[] = {turbodb_i64(1), turbodb_i64(INT64_MIN), turbodb_u64(UINT64_MAX), turbodb_null()}, out[TEST_KEY_PARTS];
    uint8_t bytes[TEST_KEY_BYTES]; bool contains_null = true;
    check_equal(orm_tidesdb_sql_index_key_encode(&definition, row, TEST_COLUMNS, bytes, sizeof(bytes), &contains_null, NULL), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_index_key_decode(&definition, bytes, key_size, out, TEST_KEY_PARTS, &contains_null, NULL), TURBODB_STATUS_OK);
    equal_value(out[0], row[1]); equal_value(out[1], row[2]);
    check_equal(reserves, reserve_count); check_equal(resizes, resize_count);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
  }
  it("rejects missing definitions pointers and absent row slots without publishing") {
    size_t size = SIZE_MAX; orm_sql_index_definition empty = {0};
    check_equal(orm_tidesdb_sql_index_key_size(NULL, &size, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_index_key_size(&empty, &size, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(size, SIZE_MAX); bind_key(basic_ddl);
    check_equal(orm_tidesdb_sql_index_key_size(&definition, NULL, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    turbodb_value_t row[] = {turbodb_i64(1), turbodb_i64(2), turbodb_u64(3), turbodb_null()};
    uint8_t bytes[TEST_KEY_BYTES], before[TEST_KEY_BYTES]; memset(bytes, TEST_SENTINEL, sizeof(bytes)); memcpy(before, bytes, sizeof(bytes));
    bool contains_null = true;
    check_equal(orm_tidesdb_sql_index_key_encode(&definition, row, 2, bytes, sizeof(bytes), &contains_null, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(memcmp(bytes, before, sizeof(bytes)), 0); check_true(contains_null);
    check_equal(orm_tidesdb_sql_index_key_encode(&definition, NULL, TEST_COLUMNS, bytes, sizeof(bytes), &contains_null, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_index_key_encode(&definition, row, TEST_COLUMNS, NULL, sizeof(bytes), &contains_null, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_index_key_encode(&definition, row, TEST_COLUMNS, bytes, sizeof(bytes), NULL, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    turbodb_value_t out[TEST_KEY_PARTS];
    check_equal(orm_tidesdb_sql_index_key_decode(&definition, NULL, key_size, out, TEST_KEY_PARTS, &contains_null, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_index_key_decode(&definition, bytes, key_size, NULL, TEST_KEY_PARTS, &contains_null, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_index_key_decode(&definition, bytes, key_size, out, TEST_KEY_PARTS, NULL, &error), TURBODB_STATUS_INVALID_ARGUMENT);
  }
  group("Private DOUBLE tuple encoding") {
    it("writes stable binary64 bytes for negative positive and canonical zero keys") {
      const double samples[] = {-1.0, -0.0, 0.0, 1.0};
      const uint8_t golden[][TEST_CELL] = {
        {1,0x40,0x0f,0xff,0xff,0xff,0xff,0xff,0xff},
        {1,0x80,0,0,0,0,0,0,0}, {1,0x80,0,0,0,0,0,0,0},
        {1,0xbf,0xf0,0,0,0,0,0,0}};
      for (unsigned descending = 0; descending < 2; ++descending) {
        reset(); double_key(descending != 0, false);
        for (size_t i = 0; i < sizeof(samples)/sizeof(samples[0]); ++i) {
          turbodb_value_t row[] = {turbodb_i64(1), turbodb_null(), turbodb_null(), turbodb_f64(samples[i])};
          uint8_t bytes[TEST_KEY_BYTES]; memset(bytes, TEST_SENTINEL, sizeof(bytes));
          check_false(encode(row, bytes)); check_equal(key_size, TEST_CELL);
          for (size_t j = 0; j < key_size; ++j)
            check_equal(bytes[j], descending ? (uint8_t)~golden[i][j] : golden[i][j]);
          for (size_t j = key_size; j < sizeof(bytes); ++j) check_equal(bytes[j], TEST_SENTINEL);
          turbodb_value_t decoded = turbodb_null(); bool null = true;
          check_equal(orm_tidesdb_sql_index_key_decode(&definition, bytes, key_size, &decoded, 1, &null, &error), TURBODB_STATUS_OK);
          equal_value(decoded, row[3]); check_false(null);
          if (samples[i] == 0.0) check_false(signbit(decoded.data.double_value));
        }
      }
    }
    it("orders and round trips every finite exponent boundary and both number signs") {
      enum { MANTISSA_BITS = 52, EXPONENT_FINITE_COUNT = 2047, SIGN_BIT = 63 };
      const uint64_t mantissas[] = {0, 1, (UINT64_C(1) << MANTISSA_BITS) - 1};
      for (unsigned descending = 0; descending < 2; ++descending) for (unsigned negative = 0; negative < 2; ++negative) {
        reset(); double_key(descending != 0, false);
        uint8_t previous[TEST_KEY_BYTES], current[TEST_KEY_BYTES];
        turbodb_value_t row[] = {turbodb_i64(1), turbodb_null(), turbodb_null(), turbodb_null()};
        check_true(encode(row, previous)); turbodb_value_t previous_value = turbodb_null();
        for (uint64_t exponent = 0; exponent < EXPONENT_FINITE_COUNT; ++exponent) {
          for (size_t m = 0; m < sizeof(mantissas)/sizeof(mantissas[0]); ++m) {
            const uint64_t raw = (exponent << MANTISSA_BITS) | mantissas[m] |
                (negative ? UINT64_C(1) << SIGN_BIT : 0);
            double value; memcpy(&value, &raw, sizeof(value)); check_true(isfinite(value));
            row[3] = turbodb_f64(value); check_false(encode(row, current));
            const int compared = orm_sql_value_order(&previous_value, &row[3]);
            check_equal(sign(memcmp(previous, current, key_size)), descending ? -compared : compared);
            turbodb_value_t decoded = turbodb_null(); bool null = true;
            check_equal(orm_tidesdb_sql_index_key_decode(&definition, current, key_size, &decoded, 1, &null, &error), TURBODB_STATUS_OK);
            equal_value(decoded, row[3]); check_false(null);
            if (value != 0.0) check_equal(memcmp(&decoded.data.double_value, &value, sizeof(value)), 0);
            else check_false(signbit(decoded.data.double_value));
            memcpy(previous, current, key_size); previous_value = row[3];
          }
        }
      }
    }
    it("matches tuple comparison for mixed integer DOUBLE directions and NULL values") {
      const turbodb_value_t integer_values[] = {turbodb_null(), turbodb_i64(INT64_MIN), turbodb_i64(0), turbodb_i64(INT64_MAX)};
      const turbodb_value_t real_values[] = {turbodb_null(), turbodb_f64(-DBL_MAX), turbodb_f64(-1.0),
        turbodb_f64(-0.0), turbodb_f64(0.0), turbodb_f64(DBL_TRUE_MIN), turbodb_f64(DBL_MAX)};
      enum { INTEGER_COUNT = 4, REAL_COUNT = 7, ROW_COUNT = INTEGER_COUNT * REAL_COUNT };
      turbodb_value_t rows[ROW_COUNT][TEST_COLUMNS]; uint8_t keys[ROW_COUNT][TEST_KEY_BYTES];
      for (unsigned directions = 0; directions < 4; ++directions) {
        reset(); double_key((directions & 2) != 0, true);
        ((orm_sql_index_part *)vec_at(&definition.parts, 0))->descending = (directions & 1) != 0;
        for (size_t i = 0; i < ROW_COUNT; ++i) {
          rows[i][0] = turbodb_i64((int64_t)i); rows[i][1] = integer_values[i / REAL_COUNT];
          rows[i][2] = turbodb_u64(UINT64_MAX); rows[i][3] = real_values[i % REAL_COUNT];
          check_equal(encode(rows[i], keys[i]), rows[i][1].kind == TURBODB_VALUE_NULL || rows[i][3].kind == TURBODB_VALUE_NULL);
        }
        for (size_t i = 0; i < ROW_COUNT; ++i) for (size_t j = 0; j < ROW_COUNT; ++j)
          check_equal(sign(memcmp(keys[i], keys[j], key_size)), order(rows[i], rows[j]));
      }
    }
    it("rejects nonfinite and noncanonical zero bytes after validating earlier tuple cells") {
      const uint64_t invalid_words[] = {UINT64_C(0xfff0000000000000), UINT64_C(0x000fffffffffffff),
        UINT64_C(0xfff8000000000000), UINT64_C(0x0007ffffffffffff),
        UINT64_C(0xfff0000000000001), UINT64_C(0x000ffffffffffffe), UINT64_C(0x7fffffffffffffff)};
      for (unsigned descending = 0; descending < 2; ++descending) {
        reset(); double_key(descending != 0, true);
        turbodb_value_t row[] = {turbodb_i64(1), turbodb_null(), turbodb_u64(2), turbodb_f64(1.0)};
        uint8_t bytes[TEST_KEY_BYTES]; check_true(encode(row, bytes));
        for (size_t i = 0; i < sizeof(invalid_words)/sizeof(invalid_words[0]); ++i) {
          uint64_t word = invalid_words[i];
          for (size_t byte = TEST_CELL; byte > 1; --byte) {
            bytes[2 * TEST_CELL + byte - 1] = descending ? (uint8_t)~word : (uint8_t)word;
            word >>= 8;
          }
          reject_bytes(bytes, key_size);
        }
      }
    }
    it("preserves bytes and NULL result on invalid late DOUBLE inputs") {
      turbodb_value_t invalid[] = {turbodb_f64(INFINITY), turbodb_f64(-INFINITY), turbodb_f64(NAN),
        turbodb_i64(1), turbodb_bool(true), turbodb_text("1.0"), turbodb_f64(1.0), turbodb_null()};
      invalid[sizeof(invalid)/sizeof(invalid[0])-2].reserved = 1;
      invalid[sizeof(invalid)/sizeof(invalid[0])-1].reserved = 1;
      double_key(false, true);
      for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        turbodb_value_t row[] = {turbodb_i64(1), turbodb_null(), turbodb_u64(2), invalid[i]};
        uint8_t bytes[TEST_KEY_BYTES], before[TEST_KEY_BYTES];
        memset(bytes, TEST_SENTINEL, sizeof(bytes)); memcpy(before, bytes, sizeof(bytes)); bool null = false;
        check_equal(orm_tidesdb_sql_index_key_encode(&definition, row, TEST_COLUMNS, bytes, sizeof(bytes), &null, &error), TURBODB_STATUS_TYPE_ERROR);
        check_equal(memcmp(bytes, before, sizeof(bytes)), 0); check_false(null);
      }
    }
    it("rejects truncated DOUBLE tuples and every insufficient output capacity") {
      double_key(true, true);
      turbodb_value_t row[] = {turbodb_i64(1), turbodb_i64(-1), turbodb_u64(2), turbodb_f64(-DBL_TRUE_MIN)};
      uint8_t valid[TEST_KEY_BYTES + 1]; bool null = true;
      check_equal(orm_tidesdb_sql_index_key_encode(&definition, row, TEST_COLUMNS, valid, sizeof(valid), &null, &error), TURBODB_STATUS_OK);
      for (size_t size = 0; size < key_size; ++size) reject_bytes(valid, size);
      reject_bytes(valid, key_size + 1);
      for (size_t capacity = 0; capacity < key_size; ++capacity) {
        uint8_t bytes[TEST_KEY_BYTES], before[TEST_KEY_BYTES];
        memset(bytes, TEST_SENTINEL, sizeof(bytes)); memcpy(before, bytes, sizeof(bytes)); null = true;
        check_equal(orm_tidesdb_sql_index_key_encode(&definition, row, TEST_COLUMNS, bytes, capacity, &null, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
        check_equal(memcmp(bytes, before, sizeof(bytes)), 0); check_true(null);
      }
      for (size_t capacity = 0; capacity < TEST_KEY_PARTS; ++capacity) {
        turbodb_value_t out[TEST_KEY_PARTS], before[TEST_KEY_PARTS];
        memset(out, TEST_SENTINEL, sizeof(out)); memcpy(before, out, sizeof(out)); null = true;
        check_equal(orm_tidesdb_sql_index_key_decode(&definition, valid, key_size, out, capacity, &null, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
        check_equal(memcmp(out, before, sizeof(out)), 0); check_true(null);
      }
    }
    it("enforces every DOUBLE encoder and decoder step boundary without partial output") {
      double_key(true, true);
      turbodb_value_t row[] = {turbodb_i64(1), turbodb_i64(-1), turbodb_u64(2), turbodb_f64(-0.0)};
      uint8_t encoded[TEST_KEY_BYTES]; const uint64_t start = budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      check_false(encode(row, encoded));
      const uint64_t encode_steps = budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS] - start;
      for (uint64_t capacity = 0; capacity <= encode_steps; ++capacity) {
        reset(); double_key(true, true); remaining_steps(capacity);
        uint8_t bytes[TEST_KEY_BYTES], before[TEST_KEY_BYTES]; bool null = true;
        memset(bytes, TEST_SENTINEL, sizeof(bytes)); memcpy(before, bytes, sizeof(bytes));
        check_equal(orm_tidesdb_sql_index_key_encode(&definition, row, TEST_COLUMNS, bytes, sizeof(bytes), &null, &error),
            capacity == encode_steps ? TURBODB_STATUS_OK : TURBODB_STATUS_LIMIT_EXCEEDED);
        if (capacity < encode_steps) { check_equal(memcmp(bytes, before, sizeof(bytes)), 0); check_true(null); }
        else { check_equal(memcmp(bytes, encoded, key_size), 0); check_false(null); }
      }
      reset(); double_key(true, true);
      turbodb_value_t out[TEST_KEY_PARTS]; bool null = true;
      const uint64_t decode_start = budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      check_equal(orm_tidesdb_sql_index_key_decode(&definition, encoded, key_size, out, TEST_KEY_PARTS, &null, &error), TURBODB_STATUS_OK);
      const uint64_t decode_steps = budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS] - decode_start;
      for (uint64_t capacity = 0; capacity <= decode_steps; ++capacity) {
        reset(); double_key(true, true); remaining_steps(capacity);
        turbodb_value_t before[TEST_KEY_PARTS]; memset(out, TEST_SENTINEL, sizeof(out)); memcpy(before, out, sizeof(out)); null = true;
        check_equal(orm_tidesdb_sql_index_key_decode(&definition, encoded, key_size, out, TEST_KEY_PARTS, &null, &error),
            capacity == decode_steps ? TURBODB_STATUS_OK : TURBODB_STATUS_LIMIT_EXCEEDED);
        if (capacity < decode_steps) { check_equal(memcmp(out, before, sizeof(out)), 0); check_true(null); }
        else { equal_value(out[2], row[3]); check_false(signbit(out[2].data.double_value)); check_false(null); }
      }
    }
    it("keeps DOUBLE round trips allocation free and caller inputs immutable") {
      double_key(false, true);
      const size_t reserve_count = reserves, resize_count = resizes;
      const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; forbid_allocation = true;
      turbodb_value_t row[] = {turbodb_i64(1), turbodb_i64(INT64_MIN), turbodb_u64(UINT64_MAX), turbodb_f64(DBL_MAX)}, saved[TEST_COLUMNS];
      memcpy(saved, row, sizeof(row)); uint8_t bytes[TEST_KEY_BYTES]; bool null = true;
      check_equal(orm_tidesdb_sql_index_key_encode(&definition, row, TEST_COLUMNS, bytes, sizeof(bytes), &null, NULL), TURBODB_STATUS_OK);
      check_equal(memcmp(row, saved, sizeof(row)), 0); memset(row, 0, sizeof(row));
      turbodb_value_t out[TEST_KEY_PARTS];
      check_equal(orm_tidesdb_sql_index_key_decode(&definition, bytes, key_size, out, TEST_KEY_PARTS, &null, NULL), TURBODB_STATUS_OK);
      equal_value(out[0], saved[1]); equal_value(out[1], saved[2]); equal_value(out[2], saved[3]); check_false(null);
      check_equal(reserves, reserve_count); check_equal(resizes, resize_count);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
    }
    it("separates NULL from numerical zero and rejects NULL marker payload corruption") {
      for (unsigned descending = 0; descending < 2; ++descending) {
        reset(); double_key(descending != 0, false);
        turbodb_value_t row[] = {turbodb_i64(1), turbodb_null(), turbodb_null(), turbodb_null()};
        row[3].data.uint64_value = UINT64_MAX;
        uint8_t bytes[TEST_KEY_BYTES], zero[TEST_KEY_BYTES]; check_true(encode(row, bytes));
        for (size_t byte = 0; byte < key_size; ++byte)
          check_equal(bytes[byte], descending ? UINT8_MAX : 0);
        turbodb_value_t out = turbodb_f64(1.0); bool null = false;
        check_equal(orm_tidesdb_sql_index_key_decode(&definition, bytes, key_size, &out, 1, &null, &error), TURBODB_STATUS_OK);
        equal_value(out, turbodb_null()); check_true(null);
        row[3] = turbodb_f64(-0.0); check_false(encode(row, zero));
        check_equal(sign(memcmp(bytes, zero, key_size)), descending ? 1 : -1);
        for (size_t byte = 1; byte < key_size; ++byte) {
          bytes[byte] ^= 1; reject_bytes(bytes, key_size); bytes[byte] ^= 1;
        }
        for (unsigned marker = 2; marker <= UINT8_MAX; ++marker) {
          bytes[0] = descending ? (uint8_t)~marker : (uint8_t)marker;
          reject_bytes(bytes, key_size);
        }
        bytes[0] = descending ? UINT8_MAX : 0;
        ((orm_sql_index_part *)vec_at(&definition.parts, 0))->type.nullable = false;
        reject_bytes(bytes, key_size); row[3] = turbodb_null(); null = false;
        uint8_t before[TEST_KEY_BYTES]; memset(bytes, TEST_SENTINEL, sizeof(bytes)); memcpy(before, bytes, sizeof(bytes));
        check_equal(orm_tidesdb_sql_index_key_encode(&definition, row, TEST_COLUMNS, bytes, sizeof(bytes), &null, &error), TURBODB_STATUS_TYPE_ERROR);
        check_equal(memcmp(bytes, before, sizeof(bytes)), 0); check_false(null);
      }
    }
  }
}

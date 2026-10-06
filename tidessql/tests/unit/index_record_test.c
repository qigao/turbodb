#include "index_record.h"
#include <tinytest.h>
#include <string.h>

static size_t reserves, resizes, fail_reserve, fail_resize;
static stl_status record_test_reserve(vec_t *v, size_t n) {
  return ++reserves == fail_reserve ? STL_OUT_OF_MEMORY : vec_reserve(v, n);
}
static stl_status record_test_resize(vec_t *v, size_t n) {
  return ++resizes == fail_resize ? STL_OUT_OF_MEMORY : vec_resize(v, n);
}
#define vec_reserve record_test_reserve
#define vec_resize record_test_resize
#include "../../src/work.c"
#undef vec_reserve
#undef vec_resize

enum { TEST_LIMIT = 65536, TEST_WORK = 4 * 1024 * 1024, TEST_KEY_BYTES = 18,
       TEST_HEADER = 40, TEST_NAME = 41, TEST_TABLE = 44, TEST_FIRST = 49, TEST_SECOND = 55,
       TEST_COUNT = 36, TEST_TYPE = 4, TEST_FLAGS = 5 };
static const orm_sql_schema_column columns[] = {
  {{"id", 2}, {TURBODB_VALUE_INT64, false}}, {{"score", 5}, {TURBODB_VALUE_INT64, true}},
  {{"amount", 6}, {TURBODB_VALUE_UINT64, true}}, {{"weight", 6}, {TURBODB_VALUE_DOUBLE, true}}
};
static const orm_sql_table_schema schema = {{"items", 5}, columns, sizeof(columns)/sizeof(columns[0])};
static const char basic_ddl[] = "CREATE UNIQUE INDEX ix ON items (score DESC,amount ASC)";
static const uint8_t golden[] = {'S','I',1,1,
  1,0,0,0,0,0,0,0, 2,0,0,0,0,0,0,0, 3,0,0,0,0,0,0,0, 4,0,0,0,0,0,0,0,
  2,0,0,0, 2,'i','x', 5,'i','t','e','m','s', 1,0,0,0,1,3, 2,0,0,0,2,1};
static orm_tidesdb_sql_budget budget;
static orm_sql_budget_limits limits;
static orm_sql_index_record record;
static vec_t bytes;
static size_t reserved;
static turbodb_error_t error;

static void bind_record_sql(const char *sql) {
  sqlparser_document *document = NULL; sqlparser_error parser_error;
  check_equal(sqlparser_parse_dialect(sql, strlen(sql), SQLPARSER_MYSQL, NULL, &document, &parser_error), SQLPARSER_OK);
  check_equal(orm_tidesdb_sql_index_bind_create(document, &schema, &budget, &record.definition, &error), TURBODB_STATUS_OK);
  sqlparser_document_destroy(document); record.identity = (orm_sql_index_identity){1,2,3,4};
}
static void bind_record(void) { bind_record_sql(basic_ddl); }
static turbodb_status_t encode_record(size_t max_bytes) {
  return orm_tidesdb_sql_index_record_encode(&record, &schema, max_bytes, &bytes, &reserved, &error);
}
static turbodb_status_t decode_record(const uint8_t *data, size_t size) {
  return orm_tidesdb_sql_index_record_decode(data, size, &schema, TEST_LIMIT, &budget, &record, &error);
}
static void release_bytes(void) {
  check_equal(orm_sql_work_release(&bytes, reserved, &budget, &error), TURBODB_STATUS_OK);
  bytes = (vec_t){0}; reserved = 0;
}
static void reset(void) {
  fail_reserve = fail_resize = 0; release_bytes();
  check_equal(orm_tidesdb_sql_index_record_destroy(&record, &error), TURBODB_STATUS_OK);
  check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
  check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_init(&budget, &limits, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
  reserves = resizes = 0;
}
static void reject_record(const uint8_t *data, size_t size, turbodb_status_t expected) {
  reset(); check_equal(decode_record(data, size), expected);
  check_null(record.definition.budget); check_equal(record.identity.index_id, 0u);
  check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
}
static void assert_definition(void) {
  check_equal(strcmp(record.definition.name, "ix"), 0); check_equal(strcmp(record.definition.table, "items"), 0);
  check_equal(record.definition.name_size, 2u); check_equal(record.definition.table_size, 5u);
  check_true(record.definition.unique); check_equal(vec_size(&record.definition.parts), 2u);
  const orm_sql_index_part *a = vec_at_const(&record.definition.parts, 0), *b = vec_at_const(&record.definition.parts, 1);
  check_equal(a->column, 1u); check_equal(a->type.kind, TURBODB_VALUE_INT64); check_true(a->type.nullable); check_true(a->descending);
  check_equal(b->column, 2u); check_equal(b->type.kind, TURBODB_VALUE_UINT64); check_true(b->type.nullable); check_false(b->descending);
}

spec("TidesDB index Catalog record codec") {
  before_each() {
    reserves = resizes = fail_reserve = fail_resize = 0;
    record = (orm_sql_index_record){0}; bytes = (vec_t){0}; reserved = 0; tdsql_error_init(&error);
    limits = (orm_sql_budget_limits){0};
    for (size_t i=0; i<ORM_SQL_BUDGET_RESOURCE_COUNT; ++i) limits.statement.value[i] = TEST_LIMIT;
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = TEST_WORK;
    limits.transaction = (orm_sql_transaction_budget_amount){TEST_LIMIT,TEST_LIMIT,TEST_LIMIT};
    check_equal(orm_tidesdb_sql_budget_init(&budget, &limits, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
  }
  after_each() {
    fail_reserve = fail_resize = 0; release_bytes();
    check_equal(orm_tidesdb_sql_index_record_destroy(&record, &error), TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS], 0u);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
  }
  it("encodes DOUBLE composite records as wire v2 and preserves integer wire v1 bytes") {
    bind_record_sql("CREATE UNIQUE INDEX ix ON items (weight DESC,amount ASC)");
    check_equal(encode_record(sizeof(golden)), TURBODB_STATUS_OK);
    uint8_t expected[sizeof(golden)]; memcpy(expected, golden, sizeof(expected));
    expected[2] = 2; expected[TEST_FIRST] = 3; expected[TEST_FIRST + TEST_TYPE] = 3;
    check_equal(vec_size(&bytes), sizeof(expected)); check_equal(memcmp(vec_data_const(&bytes), expected, sizeof(expected)), 0);
    reset(); check_equal(decode_record(expected, sizeof(expected)), TURBODB_STATUS_OK);
    const orm_sql_index_part *part = vec_at_const(&record.definition.parts, 0);
    check_equal(part->column, 3u); check_equal(part->type.kind, TURBODB_VALUE_DOUBLE);
    check_true(part->type.nullable); check_true(part->descending); check_true(record.definition.unique);
    check_equal(encode_record(sizeof(expected)), TURBODB_STATUS_OK);
    check_equal(memcmp(vec_data_const(&bytes), expected, sizeof(expected)), 0);
    reset(); bind_record(); check_equal(encode_record(sizeof(golden)), TURBODB_STATUS_OK);
    check_equal(memcmp(vec_data_const(&bytes), golden, sizeof(golden)), 0);
  }
  it("rejects record versions whose DOUBLE capability does not match their key types") {
    uint8_t input[sizeof(golden)]; memcpy(input, golden, sizeof(input));
    input[2] = 2; reject_record(input, sizeof(input), TURBODB_STATUS_DATASTORE_ERROR);
    input[2] = 1; input[TEST_FIRST] = 3; input[TEST_FIRST + TEST_TYPE] = 3;
    reject_record(input, sizeof(input), TURBODB_STATUS_DATASTORE_ERROR);
    input[2] = 2; check_equal(decode_record(input, sizeof(input)), TURBODB_STATUS_OK);
    for (size_t size = 0; size < sizeof(input); ++size) reject_record(input, size, TURBODB_STATUS_DATASTORE_ERROR);
  }
  it("refunds every allocation failure when decoding DOUBLE record parts") {
    uint8_t input[sizeof(golden)]; memcpy(input, golden, sizeof(input));
    input[2] = 2; input[TEST_FIRST] = 3; input[TEST_FIRST + TEST_TYPE] = 3;
    check_equal(decode_record(input, sizeof(input)), TURBODB_STATUS_OK);
    const size_t reserve_count = reserves, resize_count = resizes;
    for (unsigned mode = 0; mode < 2; ++mode) {
      const size_t count = mode ? resize_count : reserve_count;
      for (size_t failure = 1; failure <= count; ++failure) {
        reset(); if (mode) fail_resize = failure; else fail_reserve = failure;
        check_equal(decode_record(input, sizeof(input)), TURBODB_STATUS_OUT_OF_MEMORY);
        check_null(record.definition.budget); check_false(record.definition.parts.initialized);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
      }
    }
  }
  it("encodes a bound definition as canonical bytes and decodes every field") {
    bind_record(); check_equal(encode_record(sizeof(golden)), TURBODB_STATUS_OK);
    check_equal(vec_size(&bytes), sizeof(golden)); check_equal(memcmp(vec_data_const(&bytes), golden, sizeof(golden)), 0);
    reset(); check_equal(decode_record(golden, sizeof(golden)), TURBODB_STATUS_OK); assert_definition();
    check_equal(record.identity.table_id, 1u); check_equal(record.identity.table_generation, 2u);
    check_equal(record.identity.index_id, 3u); check_equal(record.identity.generation, 4u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_AST_NODES], 0u);
    check_equal(encode_record(sizeof(golden)), TURBODB_STATUS_OK); check_equal(memcmp(vec_data_const(&bytes), golden, sizeof(golden)), 0);
  }
  it("preserves maximum identity values and ordinary index flags") {
    bind_record(); record.identity = (orm_sql_index_identity){UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX};
    record.definition.unique = false; check_equal(encode_record(sizeof(golden)), TURBODB_STATUS_OK);
    uint8_t input[sizeof(golden)]; memcpy(input, vec_data_const(&bytes), sizeof(input)); reset();
    check_equal(decode_record(input, sizeof(input)), TURBODB_STATUS_OK); check_false(record.definition.unique);
    check_equal(record.identity.table_id, UINT64_MAX); check_equal(record.identity.table_generation, UINT64_MAX);
    check_equal(record.identity.index_id, UINT64_MAX); check_equal(record.identity.generation, UINT64_MAX);
  }
  it("owns decoded names and parts after input bytes and schema are overwritten") {
    uint8_t input[sizeof(golden)]; memcpy(input, golden, sizeof(input));
    orm_sql_schema_column mutable_columns[sizeof(columns)/sizeof(columns[0])]; memcpy(mutable_columns, columns, sizeof(columns));
    char table[] = "items";
    orm_sql_table_schema borrowed = {{table, sizeof(table)-1}, mutable_columns, sizeof(columns)/sizeof(columns[0])};
    check_equal(orm_tidesdb_sql_index_record_decode(input, sizeof(input), &borrowed, sizeof(input), &budget, &record, &error), TURBODB_STATUS_OK);
    memset(input, 0, sizeof(input)); memset(mutable_columns, 0, sizeof(mutable_columns)); memset(table, 0, sizeof(table));
    assert_definition();
    const turbodb_value_t row[] = {turbodb_i64(1), turbodb_i64(INT64_MIN), turbodb_u64(UINT64_MAX)};
    uint8_t key[TEST_KEY_BYTES]; bool contains_null = true;
    check_equal(orm_tidesdb_sql_index_key_encode(&record.definition, row, sizeof(row)/sizeof(row[0]),
        key, sizeof(key), &contains_null, &error), TURBODB_STATUS_OK); check_false(contains_null);
    turbodb_value_t values[2];
    check_equal(orm_tidesdb_sql_index_key_decode(&record.definition, key, sizeof(key), values, 2, &contains_null, &error), TURBODB_STATUS_OK);
    check_equal(values[0].data.int64_value, INT64_MIN); check_equal(values[1].data.uint64_value, UINT64_MAX);
  }
  it("rejects every truncation and extra bytes without publishing a partial owner") {
    for (size_t n=0; n<sizeof(golden); ++n) reject_record(golden, n, TURBODB_STATUS_DATASTORE_ERROR);
    uint8_t input[sizeof(golden)+1]; memcpy(input, golden, sizeof(golden)); input[sizeof(golden)] = 0;
    reject_record(input, sizeof(input), TURBODB_STATUS_DATASTORE_ERROR);
  }
  it("rejects corrupt headers identities names ordinals types and flags") {
    const struct { size_t offset; uint8_t value; turbodb_status_t status; } cases[] = {
      {0,0,TURBODB_STATUS_DATASTORE_ERROR}, {1,0,TURBODB_STATUS_DATASTORE_ERROR}, {2,3,TURBODB_STATUS_UNSUPPORTED},
      {3,2,TURBODB_STATUS_DATASTORE_ERROR}, {4,0,TURBODB_STATUS_DATASTORE_ERROR}, {12,0,TURBODB_STATUS_DATASTORE_ERROR},
      {20,0,TURBODB_STATUS_DATASTORE_ERROR}, {28,0,TURBODB_STATUS_DATASTORE_ERROR}, {TEST_COUNT,0,TURBODB_STATUS_DATASTORE_ERROR},
      {TEST_COUNT,255,TURBODB_STATUS_DATASTORE_ERROR}, {TEST_HEADER,0,TURBODB_STATUS_DATASTORE_ERROR},
      {TEST_HEADER,64,TURBODB_STATUS_DATASTORE_ERROR}, {TEST_NAME,'9',TURBODB_STATUS_DATASTORE_ERROR},
      {TEST_NAME,0,TURBODB_STATUS_DATASTORE_ERROR}, {TEST_TABLE-1,0,TURBODB_STATUS_DATASTORE_ERROR},
      {TEST_TABLE,'x',TURBODB_STATUS_DATASTORE_ERROR}, {TEST_FIRST,3,TURBODB_STATUS_DATASTORE_ERROR},
      {TEST_FIRST+TEST_TYPE,3,TURBODB_STATUS_DATASTORE_ERROR}, {TEST_FIRST+TEST_TYPE,2,TURBODB_STATUS_DATASTORE_ERROR},
      {TEST_FIRST+TEST_FLAGS,2,TURBODB_STATUS_DATASTORE_ERROR}, {TEST_SECOND+TEST_FLAGS,4,TURBODB_STATUS_DATASTORE_ERROR}};
    for (size_t i=0; i<sizeof(cases)/sizeof(cases[0]); ++i) {
      uint8_t input[sizeof(golden)]; memcpy(input, golden, sizeof(input)); input[cases[i].offset] = cases[i].value;
      reject_record(input, sizeof(input), cases[i].status);
    }
  }
  it("rejects duplicate key columns even when their persisted types match") {
    uint8_t input[sizeof(golden)]; memcpy(input, golden, sizeof(input));
    input[TEST_SECOND] = input[TEST_FIRST]; input[TEST_SECOND+TEST_TYPE] = input[TEST_FIRST+TEST_TYPE];
    reject_record(input, sizeof(input), TURBODB_STATUS_DATASTORE_ERROR); check_contains(error.message, "duplicate");
  }
  it("refuses a different table schema instead of coercing persisted index types") {
    orm_sql_schema_column changed[sizeof(columns)/sizeof(columns[0])];
    const orm_sql_table_schema other = {{"items",5},changed,sizeof(columns)/sizeof(columns[0])};
    for (unsigned mutation=0; mutation<3; ++mutation) {
      reset(); memcpy(changed, columns, sizeof(changed));
      if (mutation == 0) changed[1].type.kind = TURBODB_VALUE_DOUBLE;
      else if (mutation == 1) changed[1].type.nullable = false;
      else changed[2].type.kind = TURBODB_VALUE_INT64;
      check_equal(orm_tidesdb_sql_index_record_decode(golden,sizeof(golden),&other,sizeof(golden),&budget,&record,&error),TURBODB_STATUS_DATASTORE_ERROR);
      check_null(record.definition.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
    }
  }
  it("rejects invalid encoder identities names and mutated key definitions without allocating") {
    for (unsigned mutation=0; mutation<7; ++mutation) {
      reset(); bind_record(); const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      const size_t allocation_count = reserves;
      orm_sql_index_part *first = vec_at(&record.definition.parts,0), *second = vec_at(&record.definition.parts,1);
      switch (mutation) {
        case 0: record.identity.index_id=0; break;
        case 1: memcpy(record.definition.name,"PrImArY",7); record.definition.name_size=7; break;
        case 2: record.definition.name_size=ORM_SQL_SELECT_NAME_BYTES+1; break;
        case 3: record.definition.table[0]='x'; break;
        case 4: first->type.kind=TURBODB_VALUE_DOUBLE; break;
        case 5: first->column=schema.count; break;
        case 6: *second=*first; break;
      }
      check_equal(encode_record(TEST_LIMIT),TURBODB_STATUS_INVALID_ARGUMENT); check_false(bytes.initialized);
      check_equal(reserved,0u); check_equal(reserves,allocation_count);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }
  it("rejects persisted PRIMARY names with the same binder invariant") {
    uint8_t input[sizeof(golden)+5]; memcpy(input,golden,TEST_HEADER); input[TEST_HEADER]=7;
    memcpy(input+TEST_NAME,"PrImArY",7);
    memcpy(input+TEST_NAME+7,golden+TEST_NAME+2,sizeof(golden)-TEST_NAME-2);
    reject_record(input,sizeof(input),TURBODB_STATUS_DATASTORE_ERROR);
  }
  it("honors the record byte cap at every boundary") {
    bind_record(); const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for (size_t cap=1; cap<sizeof(golden); ++cap) {
      check_equal(encode_record(cap),TURBODB_STATUS_LIMIT_EXCEEDED); check_false(bytes.initialized); check_equal(reserved,0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
    check_equal(encode_record(sizeof(golden)),TURBODB_STATUS_OK); reset();
    for (size_t cap=1; cap<sizeof(golden); ++cap) {
      check_equal(orm_tidesdb_sql_index_record_decode(golden,sizeof(golden),&schema,cap,&budget,&record,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_null(record.definition.budget);
    }
    check_equal(orm_tidesdb_sql_index_record_decode(golden,sizeof(golden),&schema,sizeof(golden),&budget,&record,&error),TURBODB_STATUS_OK);
  }
  it("refunds all encoder and decoder allocation and resize failures") {
    bind_record(); reserves=resizes=0; check_equal(encode_record(TEST_LIMIT),TURBODB_STATUS_OK);
    const size_t encoder_reserves=reserves, encoder_resizes=resizes;
    for (unsigned resize=0; resize<2; ++resize) {
      const size_t calls=resize ? encoder_resizes : encoder_reserves; check_true(calls>0);
      for (size_t point=1; point<=calls; ++point) {
        reset(); bind_record(); reserves=resizes=0; const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
        if (resize) fail_resize=point; else fail_reserve=point;
        check_equal(encode_record(TEST_LIMIT),TURBODB_STATUS_OUT_OF_MEMORY); check_false(bytes.initialized); check_equal(reserved,0u);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
    }
    reset(); check_equal(decode_record(golden,sizeof(golden)),TURBODB_STATUS_OK);
    const size_t decoder_reserves=reserves, decoder_resizes=resizes;
    for (unsigned resize=0; resize<2; ++resize) {
      const size_t calls=resize ? decoder_resizes : decoder_reserves; check_true(calls>0);
      for (size_t point=1; point<=calls; ++point) {
        reset(); if (resize) fail_resize=point; else fail_reserve=point;
        check_equal(decode_record(golden,sizeof(golden)),TURBODB_STATUS_OUT_OF_MEMORY); check_null(record.definition.budget);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
      }
    }
  }
  it("enforces each decode PLAN WORK and STEP boundary with exact-capacity success") {
    check_equal(decode_record(golden,sizeof(golden)),TURBODB_STATUS_OK);
    const orm_sql_budget_amount used=budget.used; const orm_sql_budget_limits original=limits;
    const orm_sql_budget_resource resources[]={ORM_SQL_BUDGET_PLAN_NODES,ORM_SQL_BUDGET_WORK_BYTES,ORM_SQL_BUDGET_EXECUTION_STEPS};
    for (size_t r=0; r<sizeof(resources)/sizeof(resources[0]); ++r) {
      const orm_sql_budget_resource resource=resources[r];
      for (uint64_t cap=1; cap<=used.value[resource]; ++cap) {
        limits=original; limits.statement.value[resource]=cap; reset();
        check_equal(decode_record(golden,sizeof(golden)),cap==used.value[resource] ? TURBODB_STATUS_OK : TURBODB_STATUS_LIMIT_EXCEEDED);
        if (cap<used.value[resource]) { check_null(record.definition.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u); }
      }
    }
  }
  it("preserves occupied outputs and permits idempotent destruction with optional errors") {
    bind_record(); check_equal(encode_record(TEST_LIMIT),TURBODB_STATUS_OK);
    const void *encoded=vec_data_const(&bytes), *parts=vec_data_const(&record.definition.parts);
    const size_t receipt=reserved; const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(encode_record(TEST_LIMIT),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(decode_record(golden,sizeof(golden)),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(vec_data_const(&bytes),encoded); check_equal(reserved,receipt);
    check_equal(vec_data_const(&record.definition.parts),parts); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    release_bytes(); check_equal(orm_tidesdb_sql_index_record_destroy(&record,NULL),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_index_record_destroy(&record,NULL),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_index_record_destroy(NULL,NULL),TURBODB_STATUS_OK);
    check_equal(record.identity.table_id,0u); check_equal(record.identity.index_id,0u);
    check_equal(orm_tidesdb_sql_index_record_decode(golden,sizeof(golden),&schema,TEST_LIMIT,&budget,&record,NULL),TURBODB_STATUS_OK);
  }
  it("enforces encoder WORK and STEP admission without losing the bound definition") {
    bind_record(); const orm_sql_budget_amount base=budget.used;
    check_equal(encode_record(TEST_LIMIT),TURBODB_STATUS_OK); const orm_sql_budget_amount used=budget.used;
    const orm_sql_budget_limits original=limits;
    const orm_sql_budget_resource resources[]={ORM_SQL_BUDGET_WORK_BYTES,ORM_SQL_BUDGET_EXECUTION_STEPS};
    for (size_t r=0; r<sizeof(resources)/sizeof(resources[0]); ++r) {
      const orm_sql_budget_resource resource=resources[r];
      const uint64_t needed=used.value[resource]-base.value[resource];
      for (uint64_t cap=0; cap<=needed; ++cap) {
        limits=original; limits.statement.value[resource]=base.value[resource]+cap; reset(); bind_record();
        check_equal(encode_record(TEST_LIMIT),cap==needed ? TURBODB_STATUS_OK : TURBODB_STATUS_LIMIT_EXCEEDED);
        if (cap<needed) {
          check_false(bytes.initialized); check_equal(reserved,0u);
          check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],base.value[ORM_SQL_BUDGET_WORK_BYTES]);
        }
        assert_definition();
      }
    }
  }
}

#include "catalog.h"
#include "select.h"
#include <tinytest.h>
#include <stdio.h>
#include <string.h>

static size_t reserves, resizes, fail_reserve, fail_resize;
static stl_status catalog_test_reserve(vec_t *v, size_t n) {
  return ++reserves == fail_reserve ? STL_OUT_OF_MEMORY : vec_reserve(v, n);
}
static stl_status catalog_test_resize(vec_t *v, size_t n) {
  return ++resizes == fail_resize ? STL_OUT_OF_MEMORY : vec_resize(v, n);
}
#define vec_reserve catalog_test_reserve
#define vec_resize catalog_test_resize
#include "../../src/work.c"
#include "../../src/catalog.c"
#undef vec_reserve
#undef vec_resize

enum { TEST_LIMIT = 65536, TEST_WORK = 4 * 1024 * 1024, TEST_DEPTH = 32, TEST_SQL_BYTES = 512 };
static orm_tidesdb_sql_budget budget;
static orm_sql_budget_limits limits;
static orm_sql_table_definition definition;
static sqlparser_document *document;
static turbodb_error_t error;
static const char basic_ddl[] = "CREATE TABLE items (id BIGINT PRIMARY KEY, score BIGINT, weight DOUBLE NOT NULL)";
static const uint8_t wire_golden[] = {'S','C',1,0, 3,0,0,0, 0,0,0,0,
    1,'t', 2,1,0,'i','d', 1,2,1,'n', 1,3,1,'v'};
static const uint8_t wire_defaults[] = {'S','C',2,0, 4,0,0,0, 0,0,0,0,
    1,'t',
    2,1,0,'i','d', 0,0,0,0,0,0,0,0,0,
    1,1,0,'n', 2,0xf9,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    1,2,1,'u', 2,9,0,0,0,0,0,0,0,
    1,3,1,'v', 1,0,0,0,0,0,0,0,0};

static void parse_dialect(const char *sql, sqlparser_dialect dialect) {
  sqlparser_document_destroy(document); document = NULL;
  sqlparser_error parse_error;
  const sqlparser_options options = {dialect, false};
  check_equal(sqlparser_parse_with_options(sql, strlen(sql), &options, NULL, &document, &parse_error), SQLPARSER_OK);
}
static void parse(const char *sql) { parse_dialect(sql, SQLPARSER_MYSQL); }
static turbodb_status_t bind_create(void) { return orm_tidesdb_sql_catalog_bind_create(document, &budget, &definition, &error); }
static turbodb_status_t bind_default(size_t ordinal) {
  const sqlparser_node *root=sqlparser_get_node(document,sqlparser_statements(document).first);
  return orm_sql_catalog_default(&definition,document,ordinal,root->as.alter.value,&error);
}
static orm_sql_table_schema view(void) {
  orm_sql_table_schema schema = {0};
  check_equal(orm_tidesdb_sql_catalog_schema(&definition, &schema, &error), TURBODB_STATUS_OK); return schema;
}
static void reset(void) {
  fail_reserve = fail_resize = 0;
  check_equal(orm_tidesdb_sql_catalog_destroy(&definition, &error), TURBODB_STATUS_OK);
  check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
  check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_init(&budget, &limits, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
}
static void reject(const char *sql, turbodb_status_t expected) {
  reset(); parse(sql); check_equal(bind_create(), expected);
  check_contains(error.message, "at byte"); check_null(definition.budget);
  check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
  check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 0u);
  check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS], 0u);
}

spec("TidesDB Catalog CREATE definition") {
  before_each() {
    reserves = resizes = fail_reserve = fail_resize = 0;
    definition = (orm_sql_table_definition){0}; document = NULL; tdsql_error_init(&error);
    limits = (orm_sql_budget_limits){0};
    for (size_t i = 0; i < ORM_SQL_BUDGET_RESOURCE_COUNT; ++i) limits.statement.value[i] = TEST_LIMIT;
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = TEST_WORK;
    limits.transaction = (orm_sql_transaction_budget_amount){TEST_LIMIT, TEST_LIMIT, TEST_LIMIT};
    check_equal(orm_tidesdb_sql_budget_init(&budget, &limits, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
  }
  after_each() {
    fail_reserve = fail_resize = 0;
    check_equal(orm_tidesdb_sql_catalog_destroy(&definition, &error), TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document = NULL;
  }
  group("default assignment conversion") {
    it("owns normalized numeric values after converting boolean text and common expression results") {
      parse("CREATE TABLE t(id BIGINT PRIMARY KEY DEFAULT TRUE,n BIGINT DEFAULT("
          "CASE WHEN TRUE THEN 7 ELSE 0.5 END),s BIGINT DEFAULT '2.5',"
          "m BIGINT DEFAULT '-9223372036854775808.4',u BIGINT UNSIGNED DEFAULT '184467440737095516150e-1',"
          "v DOUBLE DEFAULT '7',w DOUBLE DEFAULT '999999999999999999999',z BIGINT DEFAULT NULL)");
      check_equal(bind_create(),TURBODB_STATUS_OK);
      sqlparser_document_destroy(document); document=NULL;
      const orm_sql_table_schema schema=view();
      check_equal(schema.defaults[0].value.data.int64_value,1);
      check_equal(schema.defaults[1].value.kind,TURBODB_VALUE_INT64); check_equal(schema.defaults[1].value.data.int64_value,7);
      check_equal(schema.defaults[2].value.data.int64_value,3); check_equal(schema.defaults[3].value.data.int64_value,INT64_MIN);
      check_equal(schema.defaults[4].value.kind,TURBODB_VALUE_UINT64); check_equal(schema.defaults[4].value.data.uint64_value,UINT64_MAX);
      check_equal(schema.defaults[5].value.kind,TURBODB_VALUE_DOUBLE); check_equal(schema.defaults[5].value.data.double_value,7.0);
      check_equal(schema.defaults[6].value.kind,TURBODB_VALUE_DOUBLE); check_equal(schema.defaults[6].value.data.double_value,1e21);
      check_true(schema.defaults[7].specified); check_equal(schema.defaults[7].value.kind,TURBODB_VALUE_NULL);
      vec_t encoded={0};size_t bytes=0;orm_sql_table_definition decoded={0};
      check_equal(orm_tidesdb_sql_catalog_encode(&definition,TEST_SQL_BYTES,&encoded,&bytes,&error),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_decode(vec_data_const(&encoded),vec_size(&encoded),TEST_SQL_BYTES,&budget,&decoded,&error),TURBODB_STATUS_OK);
      orm_sql_table_schema output;check_equal(orm_tidesdb_sql_catalog_schema(&decoded,&output,&error),TURBODB_STATUS_OK);
      check_equal(output.defaults[3].value.data.int64_value,INT64_MIN); check_equal(output.defaults[4].value.data.uint64_value,UINT64_MAX);
      check_equal(output.defaults[5].value.kind,TURBODB_VALUE_DOUBLE); check_equal(output.defaults[5].value.data.double_value,7.0);
      check_equal(orm_tidesdb_sql_catalog_destroy(&decoded,&error),TURBODB_STATUS_OK);
      check_equal(orm_sql_work_release(&encoded,bytes,&budget,&error),TURBODB_STATUS_OK);
    }
    it("converts ALTER fractions and exact decimal text without retaining the ALTER AST") {
      parse("CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT DEFAULT 7)"); check_equal(bind_create(),TURBODB_STATUS_OK);
      const char *sql[]={"ALTER TABLE t ALTER n SET DEFAULT TRUE","ALTER TABLE t ALTER n SET DEFAULT 1.5",
        "ALTER TABLE t ALTER n SET DEFAULT -2.5","ALTER TABLE t ALTER n SET DEFAULT '9007199254740993.0'",
        "ALTER TABLE t ALTER n SET DEFAULT '12.9e3'","ALTER TABLE t ALTER n SET DEFAULT 4503599627370497.0"};
      const int64_t expected[]={1,2,-3,INT64_C(9007199254740993),12900,INT64_C(4503599627370497)};
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        parse(sql[i]);check_equal(bind_default(1),TURBODB_STATUS_OK);
        sqlparser_document_destroy(document);document=NULL;
        check_equal(view().defaults[1].value.kind,TURBODB_VALUE_INT64);check_equal(view().defaults[1].value.data.int64_value,expected[i]);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
    }
    it("rejects invalid text and converted range failures without publishing CREATE metadata") {
      const char *sql[]={"CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT DEFAULT '2.5junk')",
        "CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT DEFAULT '')",
        "CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT DEFAULT 18446744073709551615)",
        "CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT DEFAULT '9223372036854775807.5')",
        "CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT UNSIGNED DEFAULT '-0.5')",
        "CREATE TABLE t(id BIGINT PRIMARY KEY,n DOUBLE DEFAULT '1e99999')"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i)
        reject(sql[i],i<2?TURBODB_STATUS_TYPE_ERROR:TURBODB_STATUS_OUT_OF_RANGE);
    }
    it("prepares positioned ADD conversion independently while preserving the source after failures") {
      parse("CREATE TABLE t(id BIGINT PRIMARY KEY DEFAULT 7)");check_equal(bind_create(),TURBODB_STATUS_OK);
      parse("ALTER TABLE t ADD n BIGINT NOT NULL DEFAULT '-2.5' FIRST");
      const sqlparser_node *root=sqlparser_get_node(document,sqlparser_statements(document).first);
      orm_sql_table_definition changed={0};
      check_equal(orm_sql_catalog_columns(&definition,document,root->as.alter.column,0,&changed,&error),TURBODB_STATUS_OK);
      sqlparser_document_destroy(document);document=NULL;
      orm_sql_table_schema output;check_equal(orm_tidesdb_sql_catalog_schema(&changed,&output,&error),TURBODB_STATUS_OK);
      check_equal(changed.primary_key,1u);check_equal(output.defaults[0].value.kind,TURBODB_VALUE_INT64);
      check_equal(output.defaults[0].value.data.int64_value,-3);check_equal(output.defaults[1].value.data.int64_value,7);
      check_equal(orm_tidesdb_sql_catalog_destroy(&changed,&error),TURBODB_STATUS_OK);
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      const char *sql[]={"ALTER TABLE t ADD n BIGINT DEFAULT '2.5junk'",
        "ALTER TABLE t ADD n BIGINT UNSIGNED DEFAULT '-0.5'"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        parse(sql[i]);root=sqlparser_get_node(document,sqlparser_statements(document).first);
        check_equal(orm_sql_catalog_columns(&definition,document,root->as.alter.column,0,&changed,&error),
            i?TURBODB_STATUS_OUT_OF_RANGE:TURBODB_STATUS_TYPE_ERROR);
        check_null(changed.budget);check_equal(view().count,1u);check_equal(view().defaults[0].value.data.int64_value,7);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
    }
    it("unwinds every converted CREATE workspace allocation and permits a clean retry") {
      const char sql[]="CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT DEFAULT '9007199254740993.0',"
        "u BIGINT UNSIGNED DEFAULT '184467440737095516150e-1',v DOUBLE DEFAULT '7')";
      parse(sql);reserves=resizes=0;check_equal(bind_create(),TURBODB_STATUS_OK);
      const size_t counts[]={reserves,resizes};
      for(size_t phase=0;phase<sizeof(counts)/sizeof(counts[0]);++phase) for(size_t point=1;point<=counts[phase];++point) {
        reset();reserves=resizes=0;if(phase)fail_resize=point;else fail_reserve=point;
        check_equal(bind_create(),TURBODB_STATUS_OUT_OF_MEMORY);check_null(definition.budget);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
        fail_reserve=fail_resize=0;check_equal(bind_create(),TURBODB_STATUS_OK);
        check_equal(view().defaults[1].value.data.int64_value,INT64_C(9007199254740993));
      }
    }
  }

  group("division defaults") {
    it("folds integer DIV modulo and DOUBLE division into the existing wire schema") {
      parse("CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT DEFAULT(7 DIV 3),"
          "u BIGINT UNSIGNED DEFAULT(18446744073709551615 DIV 2),"
          "v DOUBLE DEFAULT(7.5/2.0),r BIGINT DEFAULT(MOD(-7,3)))");
      check_equal(bind_create(),TURBODB_STATUS_OK);
      check_equal(view().defaults[1].value.data.int64_value,2);
      check_equal(view().defaults[2].value.data.uint64_value,(uint64_t)INT64_MAX);
      check_equal(view().defaults[3].value.data.double_value,3.75);
      check_equal(view().defaults[4].value.data.int64_value,-1);
      vec_t encoded={0}; size_t bytes=0; orm_sql_table_definition decoded={0};
      check_equal(orm_tidesdb_sql_catalog_encode(&definition,TEST_SQL_BYTES,&encoded,&bytes,&error),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_decode(vec_data_const(&encoded),vec_size(&encoded),TEST_SQL_BYTES,&budget,&decoded,&error),TURBODB_STATUS_OK);
      orm_sql_table_schema schema; check_equal(orm_tidesdb_sql_catalog_schema(&decoded,&schema,&error),TURBODB_STATUS_OK);
      check_equal(schema.defaults[4].value.data.int64_value,-1);
      check_equal(orm_tidesdb_sql_catalog_destroy(&decoded,&error),TURBODB_STATUS_OK);
      check_equal(orm_sql_work_release(&encoded,bytes,&budget,&error),TURBODB_STATUS_OK);
    }
    it("fails CREATE and ALTER zero defaults without changing the previous metadata") {
      reject("CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT DEFAULT(MOD(7,0)))",TURBODB_STATUS_SQL_ERROR);
      parse("CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT DEFAULT 7)"); check_equal(bind_create(),TURBODB_STATUS_OK);
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      parse("ALTER TABLE t ALTER n SET DEFAULT(7 DIV 0)"); check_equal(bind_default(1),TURBODB_STATUS_SQL_ERROR);
      check_equal(view().defaults[1].value.data.int64_value,7); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      parse("ALTER TABLE t ALTER n SET DEFAULT(NULL DIV 0)"); check_equal(bind_default(1),TURBODB_STATUS_OK);
      check_equal(view().defaults[1].value.kind,TURBODB_VALUE_NULL);
    }
  }

  it("folds common conditional result types and persists the converted default values") {
    parse("CREATE TABLE t(id BIGINT PRIMARY KEY,a DOUBLE DEFAULT(CASE WHEN TRUE THEN 7 ELSE 0.5 END),"
        "b DOUBLE DEFAULT(COALESCE(NULL,-1,18446744073709551615,0.0)),c DOUBLE DEFAULT(IFNULL(7,0.5)))");
    check_equal(bind_create(),TURBODB_STATUS_OK);
    check_equal(view().defaults[1].value.kind,TURBODB_VALUE_DOUBLE);check_equal(view().defaults[1].value.data.double_value,7.0);
    check_equal(view().defaults[2].value.kind,TURBODB_VALUE_DOUBLE);check_equal(view().defaults[2].value.data.double_value,-1.0);
    check_equal(view().defaults[3].value.kind,TURBODB_VALUE_DOUBLE);check_equal(view().defaults[3].value.data.double_value,7.0);
    parse("ALTER TABLE t ALTER a SET DEFAULT(IFNULL(9,2.5))");check_equal(bind_default(1),TURBODB_STATUS_OK);
    parse("ALTER TABLE t ALTER a SET DEFAULT(COALESCE(7/0.0,9))");check_equal(bind_default(1),TURBODB_STATUS_SQL_ERROR);
    check_equal(view().defaults[1].value.data.double_value,9.0);
    vec_t encoded={0};size_t bytes=0;orm_sql_table_definition decoded={0};
    check_equal(orm_tidesdb_sql_catalog_encode(&definition,TEST_SQL_BYTES,&encoded,&bytes,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_decode(vec_data_const(&encoded),vec_size(&encoded),TEST_SQL_BYTES,&budget,&decoded,&error),TURBODB_STATUS_OK);
    orm_sql_table_schema schema;check_equal(orm_tidesdb_sql_catalog_schema(&decoded,&schema,&error),TURBODB_STATUS_OK);
    check_equal(schema.defaults[1].value.data.double_value,9.0);check_equal(schema.defaults[2].value.data.double_value,-1.0);
    check_equal(schema.defaults[3].value.data.double_value,7.0);
    check_equal(orm_tidesdb_sql_catalog_destroy(&decoded,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_work_release(&encoded,bytes,&budget,&error),TURBODB_STATUS_OK);
  }

  it("folds real comparisons and common BETWEEN types while keeping default result kinds") {
    parse("CREATE TABLE t(id BIGINT PRIMARY KEY,a BIGINT DEFAULT(CASE WHEN "
        "9007199254740992 BETWEEN 9007199254740993 AND 9007199254740994.0 THEN 7 ELSE 0 END),"
        "b DOUBLE DEFAULT(NULLIF(7.0,8)),c DOUBLE DEFAULT(CASE 7 WHEN 7.0 THEN 3.5 ELSE 0.5 END))");
    check_equal(bind_create(),TURBODB_STATUS_OK);
    check_equal(view().defaults[1].value.kind,TURBODB_VALUE_INT64);check_equal(view().defaults[1].value.data.int64_value,7);
    check_equal(view().defaults[2].value.kind,TURBODB_VALUE_DOUBLE);check_equal(view().defaults[2].value.data.double_value,7.0);
    check_equal(view().defaults[3].value.data.double_value,3.5);
    parse("ALTER TABLE t ALTER a SET DEFAULT(CASE WHEN 9007199254740993=9007199254740992.0 THEN 9 ELSE 0 END)");
    check_equal(bind_default(1),TURBODB_STATUS_OK);check_equal(view().defaults[1].value.data.int64_value,9);
  }

  it("folds promoted defaults through CREATE ALTER and the existing catalog wire") {
    parse("CREATE TABLE t(id BIGINT PRIMARY KEY,a DOUBLE DEFAULT(7+0.5),"
        "b DOUBLE DEFAULT(7/2.0),c DOUBLE DEFAULT(MOD(-7,2.5)))");
    check_equal(bind_create(),TURBODB_STATUS_OK); const orm_sql_table_schema schema=view();
    check_equal(schema.defaults[1].value.data.double_value,7.5);
    check_equal(schema.defaults[2].value.data.double_value,3.5);
    check_equal(schema.defaults[3].value.data.double_value,-2.0);
    parse("ALTER TABLE t ALTER a SET DEFAULT(2*1.5)"); check_equal(bind_default(1),TURBODB_STATUS_OK);
    parse("ALTER TABLE t ALTER b SET DEFAULT(7/0.0)"); check_equal(bind_default(2),TURBODB_STATUS_SQL_ERROR);
    check_equal(view().defaults[2].value.data.double_value,3.5);
    vec_t encoded={0}; size_t bytes=0; orm_sql_table_definition decoded={0};
    check_equal(orm_tidesdb_sql_catalog_encode(&definition,TEST_SQL_BYTES,&encoded,&bytes,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_decode(vec_data_const(&encoded),vec_size(&encoded),TEST_SQL_BYTES,&budget,&decoded,&error),TURBODB_STATUS_OK);
    orm_sql_table_schema output; check_equal(orm_tidesdb_sql_catalog_schema(&decoded,&output,&error),TURBODB_STATUS_OK);
    check_equal(output.defaults[1].value.kind,TURBODB_VALUE_DOUBLE); check_equal(output.defaults[1].value.data.double_value,3.0);
    check_equal(output.defaults[2].value.data.double_value,3.5); check_equal(output.defaults[3].value.data.double_value,-2.0);
    check_equal(orm_tidesdb_sql_catalog_destroy(&decoded,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_work_release(&encoded,bytes,&budget,&error),TURBODB_STATUS_OK);
  }

  group("numeric function defaults") {
    it("folds typed CREATE defaults and decodes them without retaining the expressions") {
      parse("CREATE TABLE t(id BIGINT PRIMARY KEY DEFAULT (ABS(-7)),"
          "u BIGINT UNSIGNED DEFAULT (CEILING(18446744073709551615)),"
          "v DOUBLE DEFAULT (FLOOR(-1.25)),s BIGINT DEFAULT (SIGN(-1.25)))");
      check_equal(bind_create(),TURBODB_STATUS_OK);
      sqlparser_document_destroy(document); document=NULL;
      vec_t encoded={0}; size_t work=0; orm_sql_table_definition decoded={0};
      check_equal(orm_tidesdb_sql_catalog_encode(&definition,TEST_SQL_BYTES,&encoded,&work,&error),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_decode(vec_data_const(&encoded),vec_size(&encoded),TEST_SQL_BYTES,&budget,&decoded,&error),TURBODB_STATUS_OK);
      orm_sql_table_schema schema;
      check_equal(orm_tidesdb_sql_catalog_schema(&decoded,&schema,&error),TURBODB_STATUS_OK);
      check_equal(schema.defaults[0].value.data.int64_value,7);
      check_equal(schema.defaults[1].value.kind,TURBODB_VALUE_UINT64);
      check_equal(schema.defaults[1].value.data.uint64_value,UINT64_MAX);
      check_equal(schema.defaults[2].value.kind,TURBODB_VALUE_DOUBLE);
      check_equal(schema.defaults[2].value.data.double_value,-2.0);
      check_equal(schema.defaults[3].value.kind,TURBODB_VALUE_INT64);
      check_equal(schema.defaults[3].value.data.int64_value,-1);
      check_equal(orm_tidesdb_sql_catalog_destroy(&decoded,&error),TURBODB_STATUS_OK);
      check_equal(orm_sql_work_release(&encoded,work,&budget,&error),TURBODB_STATUS_OK);
    }
    it("keeps ALTER defaults after numeric overflow and permits NULL and nested functions") {
      parse("CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT DEFAULT 7,v DOUBLE)");
      check_equal(bind_create(),TURBODB_STATUS_OK);
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      parse("ALTER TABLE t ALTER n SET DEFAULT (ABS(-9223372036854775808))");
      check_equal(bind_default(1),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(view().defaults[1].value.data.int64_value,7);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      parse("ALTER TABLE t ALTER n SET DEFAULT (SIGN(ABS(-7)))");
      check_equal(bind_default(1),TURBODB_STATUS_OK);
      check_equal(view().defaults[1].value.data.int64_value,1);
      parse("ALTER TABLE t ALTER v SET DEFAULT (CEIL(NULL))");
      check_equal(bind_default(2),TURBODB_STATUS_OK);
      check_true(view().defaults[2].specified);
      check_equal(view().defaults[2].value.kind,TURBODB_VALUE_NULL);
    }
  }
  group("ALTER default preparation") {
    it("owns folded typed defaults independently of the ALTER AST") {
      parse("CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT,u BIGINT UNSIGNED,v DOUBLE)"); check_equal(bind_create(),TURBODB_STATUS_OK);
      parse("ALTER TABLE t ALTER n SET DEFAULT -9223372036854775808"); check_equal(bind_default(1),TURBODB_STATUS_OK);
      parse("ALTER TABLE t ALTER u SET DEFAULT 18446744073709551615"); check_equal(bind_default(2),TURBODB_STATUS_OK);
      parse("ALTER TABLE t ALTER v SET DEFAULT (2.5*4.0)"); check_equal(bind_default(3),TURBODB_STATUS_OK);
      sqlparser_document_destroy(document); document=NULL;
      const orm_sql_table_schema schema=view(); check_equal(schema.defaults[1].value.data.int64_value,INT64_MIN);
      check_equal(schema.defaults[2].value.data.uint64_value,UINT64_MAX); check_equal(schema.defaults[3].value.data.double_value,10.0);
      check_equal(schema.defaults[3].value.kind,TURBODB_VALUE_DOUBLE); check_equal(definition.primary_key,0u);
    }
    it("sets NULL and removes an explicit default without changing column attributes") {
      parse("CREATE TABLE t(id BIGINT PRIMARY KEY DEFAULT 7,n BIGINT DEFAULT 8)"); check_equal(bind_create(),TURBODB_STATUS_OK);
      parse("ALTER TABLE t ALTER COLUMN n SET DEFAULT NULL"); check_equal(bind_default(1),TURBODB_STATUS_OK);
      check_true(view().defaults[1].specified); check_equal(view().defaults[1].value.kind,TURBODB_VALUE_NULL);
      parse("ALTER TABLE t ALTER id DROP DEFAULT"); check_equal(bind_default(0),TURBODB_STATUS_OK);
      check_false(view().defaults[0].specified); check_false(view().columns[0].type.nullable);
      check_equal(bind_default(0),TURBODB_STATUS_OK); check_equal(view().columns[0].type.kind,TURBODB_VALUE_INT64);
    }
    it("preserves the previous default after type NULL expression and overflow failures") {
      parse("CREATE TABLE t(id BIGINT PRIMARY KEY DEFAULT 7)"); check_equal(bind_create(),TURBODB_STATUS_OK);
      const char *sql[]={"ALTER TABLE t ALTER id SET DEFAULT NULL","ALTER TABLE t ALTER id SET DEFAULT 'bad'",
        "ALTER TABLE t ALTER id SET DEFAULT '2.5junk'","ALTER TABLE t ALTER id SET DEFAULT (id+1)",
        "ALTER TABLE t ALTER id SET DEFAULT (UNSUPPORTED_FN(1))","ALTER TABLE t ALTER id SET DEFAULT (9223372036854775807+1)"};
      const turbodb_status_t statuses[]={TURBODB_STATUS_SQL_ERROR,TURBODB_STATUS_TYPE_ERROR,TURBODB_STATUS_TYPE_ERROR,
        TURBODB_STATUS_SQL_ERROR,TURBODB_STATUS_UNSUPPORTED,TURBODB_STATUS_LIMIT_EXCEEDED};
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        parse(sql[i]); check_equal(bind_default(0),statuses[i]); check_contains(error.message,"ALTER TABLE at byte");
        check_equal(view().defaults[0].value.data.int64_value,7); check_true(view().defaults[0].specified);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
    }
    it("reuses existing v2 defaults and restores exact v1 bytes after removing the last default") {
      parse("CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT UNSIGNED,v DOUBLE)"); check_equal(bind_create(),TURBODB_STATUS_OK);
      parse("ALTER TABLE t ALTER n SET DEFAULT 7"); check_equal(bind_default(1),TURBODB_STATUS_OK);
      vec_t encoded={0}; size_t work=0; orm_sql_table_definition decoded={0};
      check_equal(orm_tidesdb_sql_catalog_encode(&definition,TEST_SQL_BYTES,&encoded,&work,&error),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_decode(vec_data_const(&encoded),vec_size(&encoded),TEST_SQL_BYTES,&budget,&decoded,&error),TURBODB_STATUS_OK);
      orm_sql_table_schema schema; check_equal(orm_tidesdb_sql_catalog_schema(&decoded,&schema,&error),TURBODB_STATUS_OK);
      check_true(schema.defaults[1].specified); check_equal(schema.defaults[1].value.kind,TURBODB_VALUE_UINT64);
      check_equal(schema.defaults[1].value.data.uint64_value,7u); check_equal(orm_tidesdb_sql_catalog_destroy(&decoded,&error),TURBODB_STATUS_OK);
      check_equal(orm_sql_work_release(&encoded,work,&budget,&error),TURBODB_STATUS_OK); encoded=(vec_t){0}; work=0;
      parse("ALTER TABLE t ALTER n DROP DEFAULT"); check_equal(bind_default(1),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_encode(&definition,sizeof(wire_golden),&encoded,&work,&error),TURBODB_STATUS_OK);
      check_equal(vec_size(&encoded),sizeof(wire_golden)); check_equal(memcmp(vec_data_const(&encoded),wire_golden,sizeof(wire_golden)),0);
      check_equal(orm_sql_work_release(&encoded,work,&budget,&error),TURBODB_STATUS_OK);
    }
    it("refunds every intercepted expression allocation failure and retains the old default") {
      parse("CREATE TABLE t(id BIGINT PRIMARY KEY DEFAULT 7)"); check_equal(bind_create(),TURBODB_STATUS_OK);
      const char *sql[]={"ALTER TABLE t ALTER id SET DEFAULT (ABS(-2)+SIGN(3))",
        "ALTER TABLE t ALTER id SET DEFAULT '12.5e1'"};
      for(size_t op=0;op<sizeof(sql)/sizeof(sql[0]);++op) {
        parse(sql[op]); reserves=resizes=0; check_equal(bind_default(0),TURBODB_STATUS_OK);
        const size_t allocations[]={reserves,resizes}; const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
        for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=allocations[pass];++point) {
          ((orm_sql_column_default *)vec_at(&definition.defaults,0))->value=turbodb_i64(7);
          reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
          check_equal(bind_default(0),TURBODB_STATUS_OUT_OF_MEMORY); fail_reserve=fail_resize=0;
          check_equal(view().defaults[0].value.data.int64_value,7); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
        }
      }
    }
    it("checks every expression step boundary and rejects invalid definition inputs") {
      parse("CREATE TABLE t(id BIGINT PRIMARY KEY DEFAULT 7)"); check_equal(bind_create(),TURBODB_STATUS_OK);
      const char *sql[]={"ALTER TABLE t ALTER id SET DEFAULT (ABS(-2)+SIGN(3))",
        "ALTER TABLE t ALTER id SET DEFAULT '12.5e1'"};
      for(size_t op=0;op<sizeof(sql)/sizeof(sql[0]);++op) {
        parse(sql[op]); const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
        check_equal(bind_default(0),TURBODB_STATUS_OK); const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
        const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
        for(uint64_t point=0;point<steps;++point) {
          ((orm_sql_column_default *)vec_at(&definition.defaults,0))->value=turbodb_i64(7);
          budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
          check_equal(bind_default(0),TURBODB_STATUS_LIMIT_EXCEEDED); budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=TEST_LIMIT;
          check_equal(view().defaults[0].value.data.int64_value,7); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
        }
      }
      check_equal(orm_sql_catalog_default(NULL,document,0,0,&error),TURBODB_STATUS_INVALID_ARGUMENT);
      check_equal(orm_sql_catalog_default(&definition,NULL,0,0,&error),TURBODB_STATUS_INVALID_ARGUMENT);
      check_equal(bind_default(1),TURBODB_STATUS_INVALID_ARGUMENT);
      check_equal(orm_sql_catalog_default(&definition,document,0,UINT32_MAX,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    }
  }
  it("owns ordered names types and primary key independently of the DDL document") {
    parse(basic_ddl); check_equal(bind_create(), TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document = NULL;
    const orm_sql_table_schema schema = view();
    check_equal(schema.count, 3u); check_equal(schema.name.len, 5u);
    check_equal(memcmp(schema.name.data, "items", schema.name.len), 0);
    check_equal(memcmp(schema.columns[1].name.data, "score", schema.columns[1].name.len), 0);
    check_equal(definition.primary_key, 0u); check_false(definition.if_not_exists);
    check_equal(schema.columns[0].type.kind, TURBODB_VALUE_INT64); check_false(schema.columns[0].type.nullable);
    check_true(schema.columns[1].type.nullable); check_equal(schema.columns[2].type.kind, TURBODB_VALUE_DOUBLE);
    check_false(schema.columns[2].type.nullable);
    check_equal(budget.used.value[ORM_SQL_BUDGET_PLAN_NODES], 4u);
  }
  it("resolves forward table keys and preserves IF NOT EXISTS without checking existence") {
    parse("CREATE TABLE IF NOT EXISTS `items` (PRIMARY KEY (`id`), score DOUBLE, `id` bigint unsigned NULL)");
    check_equal(bind_create(), TURBODB_STATUS_OK); const orm_sql_table_schema schema = view();
    check_equal(definition.primary_key, 1u); check_true(definition.if_not_exists);
    check_equal(schema.columns[1].type.kind, TURBODB_VALUE_UINT64); check_false(schema.columns[1].type.nullable);
    check_true(schema.columns[0].type.nullable);
  }
  it("binds strict numeric and NULL column defaults") {
    parse("CREATE TABLE t(id BIGINT PRIMARY KEY DEFAULT 5,n BIGINT NOT NULL DEFAULT -7,"
        "u BIGINT UNSIGNED DEFAULT 9,v DOUBLE DEFAULT NULL)");
    check_equal(bind_create(),TURBODB_STATUS_OK);
    const orm_sql_table_schema schema=view();
    check_true(schema.defaults[0].specified);
    check_equal(schema.defaults[0].value.data.int64_value,5);
    check_true(schema.defaults[1].specified);
    check_equal(schema.defaults[1].value.kind,TURBODB_VALUE_INT64);
    check_equal(schema.defaults[1].value.data.int64_value,-7);
    check_true(schema.defaults[2].specified);
    check_equal(schema.defaults[2].value.kind,TURBODB_VALUE_UINT64);
    check_equal(schema.defaults[2].value.data.uint64_value,9u);
    check_true(schema.defaults[3].specified);
    check_equal(schema.defaults[3].value.kind,TURBODB_VALUE_NULL);
  }
  it("folds finite decimal and numeric constant default expressions") {
    parse("CREATE TABLE t(id BIGINT PRIMARY KEY DEFAULT (1+2),"
        "u BIGINT UNSIGNED DEFAULT (2*3),a DOUBLE DEFAULT 1.5,"
        "b DOUBLE NOT NULL DEFAULT (-2.5e1+5.0))");
    check_equal(bind_create(),TURBODB_STATUS_OK);
    const orm_sql_table_schema schema=view();
    check_equal(schema.defaults[0].value.data.int64_value,3);
    check_equal(schema.defaults[1].value.kind,TURBODB_VALUE_UINT64);
    check_equal(schema.defaults[1].value.data.uint64_value,6u);
    check_equal(schema.defaults[2].value.kind,TURBODB_VALUE_DOUBLE);
    check_equal(schema.defaults[2].value.data.double_value,1.5);
    check_equal(schema.defaults[3].value.data.double_value,-20.0);
  }
  it("makes primary keys nonnullable regardless of inline NULL attribute order") {
    const char *sql[] = {"CREATE TABLE t (id BIGINT NULL PRIMARY KEY)", "CREATE TABLE t (id BIGINT PRIMARY KEY NULL)",
      "CREATE TABLE t (id BIGINT KEY)", "CREATE TABLE t (id BIGINT, CONSTRAINT pk PRIMARY KEY(id))",
      "CREATE TABLE t (id BIGINT, CONSTRAINT PRIMARY KEY(id))"};
    for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
      reset(); parse(sql[i]); check_equal(bind_create(), TURBODB_STATUS_OK); check_false(view().columns[0].type.nullable);
    }
  }
  it("feeds SELECT binding and survives definition destruction before execution") {
    parse(basic_ddl); check_equal(bind_create(), TURBODB_STATUS_OK); orm_sql_table_schema schema = view();
    parse("SELECT id, score + 2 AS adjusted FROM items WHERE id > 1");
    orm_sql_select plan = {0}; orm_sql_select_run run = {0};
    check_equal(orm_tidesdb_sql_select_bind(document, &schema, TEST_DEPTH, &budget, &plan, &error), TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document = NULL;
    check_equal(orm_tidesdb_sql_catalog_destroy(&definition, &error), TURBODB_STATUS_OK); schema = (orm_sql_table_schema){0};
    const turbodb_value_t rows[] = {turbodb_i64(1), turbodb_i64(10), turbodb_f64(1.0), turbodb_i64(2), turbodb_i64(20), turbodb_f64(2.0)};
    check_equal(orm_tidesdb_sql_select_open(&plan, rows, 2, &run, &error), TURBODB_STATUS_OK);
    orm_sql_scan_row row = {0};
    check_equal(orm_tidesdb_sql_scan_next(&run.scan, &row, &error), TURBODB_STATUS_OK);
    check_equal(row.state, ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value, 2);
    check_equal(row.values[1].data.int64_value, 22);
    check_equal(orm_tidesdb_sql_scan_next(&run.scan, &row, &error), TURBODB_STATUS_OK); check_equal(row.state, ORM_SQL_SCAN_DONE);
    check_equal(orm_tidesdb_sql_select_close(&run, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_destroy(&plan, &error), TURBODB_STATUS_OK);
  }
  it("rejects duplicate columns multiple keys and unknown key references") {
    const char *sql[] = {
      "CREATE TABLE t (id BIGINT PRIMARY KEY, id BIGINT)",
      "CREATE TABLE t (id BIGINT PRIMARY KEY PRIMARY KEY)",
      "CREATE TABLE t (id BIGINT PRIMARY KEY, n BIGINT PRIMARY KEY)",
      "CREATE TABLE t (id BIGINT PRIMARY KEY, PRIMARY KEY(id))",
      "CREATE TABLE t (id BIGINT, PRIMARY KEY(absent))",
      "CREATE TABLE t (id BIGINT PRIMARY KEY, n DOUBLE NULL NOT NULL)"};
    for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) reject(sql[i], TURBODB_STATUS_SQL_ERROR);
  }
  it("rejects unenforced constraints and incomplete primary key definitions") {
    const char *sql[] = {
      "CREATE TABLE t (id BIGINT)", "CREATE TABLE t (id DOUBLE PRIMARY KEY)",
      "CREATE TABLE t (id BIGINT, n BIGINT, PRIMARY KEY(id,n))",
      "CREATE TABLE t (id BIGINT PRIMARY KEY AUTO_INCREMENT)",
      "CREATE TABLE t (id BIGINT PRIMARY KEY CHECK(id > 0))",
      "CREATE TABLE t (id BIGINT PRIMARY KEY UNIQUE)",
      "CREATE TABLE t (id BIGINT PRIMARY KEY REFERENCES other(id))",
      "CREATE TABLE t (id BIGINT, CONSTRAINT `bad-name` PRIMARY KEY(id))",
      "CREATE TABLE t (id BIGINT PRIMARY KEY, INDEX ix(id))",
      "CREATE TABLE t (id BIGINT PRIMARY KEY, UNIQUE(id))",
      "CREATE TABLE t (id BIGINT PRIMARY KEY, CONSTRAINT fk FOREIGN KEY child_idx(id) REFERENCES other(id) MATCH SIMPLE ON DELETE NO ACTION ON UPDATE RESTRICT)"};
    for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) reject(sql[i], TURBODB_STATUS_UNSUPPORTED);
  }
  it("rejects types whose bounds coercions or collation cannot be enforced") {
    const char *types[] = {"INT", "BOOL", "FLOAT", "DECIMAL(12,2)", "VARCHAR(20)", "TEXT", "BLOB",
      "BIGINT(20)", "BIGINT ZEROFILL", "DOUBLE UNSIGNED", "DOUBLE(10,2)"};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
      char sql[TEST_SQL_BYTES];
      (void)snprintf(sql, sizeof(sql), "CREATE TABLE t (id BIGINT PRIMARY KEY, n %s)", types[i]);
      reject(sql, TURBODB_STATUS_UNSUPPORTED);
    }
  }
  it("rejects ambiguous invalid and unsupported column defaults") {
    reject("CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT DEFAULT 1 DEFAULT 2)",
        TURBODB_STATUS_SQL_ERROR);
    reject("CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT NOT NULL DEFAULT NULL)",
        TURBODB_STATUS_SQL_ERROR);
    reject("CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT DEFAULT 'bad')",
        TURBODB_STATUS_TYPE_ERROR);
    reject("CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT DEFAULT '1.5junk')",
        TURBODB_STATUS_TYPE_ERROR);
    reject("CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT DEFAULT (id+1))",
        TURBODB_STATUS_SQL_ERROR);
    reject("CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT DEFAULT (UNSUPPORTED_FN(1)))",
        TURBODB_STATUS_UNSUPPORTED);
    reject("CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT UNSIGNED DEFAULT -1)",
        TURBODB_STATUS_OUT_OF_RANGE);
  }
  it("rejects table options temporary tables and alternate creation forms") {
    const char *sql[] = {
      "CREATE TEMPORARY TABLE t (id BIGINT PRIMARY KEY)", "CREATE TABLE t LIKE other",
      "CREATE TABLE t AS SELECT id FROM other", "CREATE TABLE t (id BIGINT PRIMARY KEY) ENGINE=InnoDB",
      "CREATE TABLE db.t (id BIGINT PRIMARY KEY)", "SELECT id FROM t",
      "CREATE TABLE t (id BIGINT PRIMARY KEY); CREATE TABLE u (id BIGINT PRIMARY KEY)"};
    for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) reject(sql[i], TURBODB_STATUS_UNSUPPORTED);
    reset(); parse_dialect("CREATE TABLE t (id BIGINT PRIMARY KEY)", SQLPARSER_SQLITE);
    check_equal(bind_create(), TURBODB_STATUS_UNSUPPORTED); check_null(definition.budget);
  }
  it("shares SELECT identifier restrictions and honors the name length boundary") {
    reject("CREATE TABLE `bad-name` (id BIGINT PRIMARY KEY)", TURBODB_STATUS_UNSUPPORTED);
    reject("CREATE TABLE t (`bad``name` BIGINT PRIMARY KEY)", TURBODB_STATUS_UNSUPPORTED);
    reject("CREATE TABLE t (`9id` BIGINT PRIMARY KEY)", TURBODB_STATUS_UNSUPPORTED);
    char name[ORM_SQL_SELECT_NAME_BYTES + 2]; memset(name, 'a', sizeof(name));
    name[ORM_SQL_SELECT_NAME_BYTES] = 0;
    char sql[TEST_SQL_BYTES]; (void)snprintf(sql, sizeof(sql), "CREATE TABLE `%s` (`%s` BIGINT PRIMARY KEY)", name, name);
    reset(); parse(sql); check_equal(bind_create(), TURBODB_STATUS_OK); check_equal(view().name.len, ORM_SQL_SELECT_NAME_BYTES);
    name[ORM_SQL_SELECT_NAME_BYTES] = 'a'; name[ORM_SQL_SELECT_NAME_BYTES + 1] = 0;
    (void)snprintf(sql, sizeof(sql), "CREATE TABLE `%s` (id BIGINT PRIMARY KEY)", name);
    reject(sql, TURBODB_STATUS_LIMIT_EXCEEDED);
  }
  it("refunds work at every definition allocation failure and permits retry") {
    parse(basic_ddl); reserves = resizes = 0; check_equal(bind_create(), TURBODB_STATUS_OK);
    const size_t reserve_count = reserves, resize_count = resizes;
    check_greater(reserve_count, 0u); check_greater(resize_count, 0u);
    for (unsigned pass = 0; pass < 2; ++pass) {
      for (size_t i = 1; i <= (pass ? resize_count : reserve_count); ++i) {
        reset(); reserves = resizes = 0;
        if (pass) fail_resize = i; else fail_reserve = i;
        check_equal(bind_create(), TURBODB_STATUS_OUT_OF_MEMORY); check_null(definition.budget);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
        check_greater(budget.used.value[ORM_SQL_BUDGET_AST_NODES], 0u);
        fail_reserve = fail_resize = 0; check_equal(bind_create(), TURBODB_STATUS_OK);
      }
    }
  }
  it("enforces AST plan step and workspace limits before publishing metadata") {
    const orm_sql_budget_resource resources[] = {ORM_SQL_BUDGET_AST_NODES, ORM_SQL_BUDGET_PLAN_NODES,
      ORM_SQL_BUDGET_EXECUTION_STEPS, ORM_SQL_BUDGET_WORK_BYTES};
    for (size_t i = 0; i < sizeof(resources) / sizeof(resources[0]); ++i) {
      const uint64_t previous = limits.statement.value[resources[i]];
      limits.statement.value[resources[i]] = 1;
      reset(); parse(basic_ddl); check_equal(bind_create(), TURBODB_STATUS_LIMIT_EXCEEDED);
      check_null(definition.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
      limits.statement.value[resources[i]] = previous;
    }
  }
  it("preserves occupied outputs and rejects invalid arguments or inactive budgets") {
    parse(basic_ddl); check_equal(bind_create(), TURBODB_STATUS_OK);
    const orm_sql_table_schema original = view(); const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(bind_create(), TURBODB_STATUS_INVALID_ARGUMENT); check_true(view().columns == original.columns);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
    check_equal(orm_tidesdb_sql_catalog_bind_create(NULL, &budget, &definition, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    reset(); check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    check_equal(bind_create(), TURBODB_STATUS_INVALID_STATE); check_null(definition.budget);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
    orm_sql_table_schema unchanged = {.count = TEST_LIMIT};
    check_equal(orm_tidesdb_sql_catalog_schema(&definition, &unchanged, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(unchanged.count, TEST_LIMIT);
    check_equal(orm_tidesdb_sql_catalog_destroy(NULL, &error), TURBODB_STATUS_OK);
  }
  it("encodes exact schema v1 bytes and decodes owned metadata") {
    parse("CREATE TABLE t (id BIGINT PRIMARY KEY, n BIGINT UNSIGNED, v DOUBLE)");
    check_equal(bind_create(), TURBODB_STATUS_OK); vec_t bytes = {0}; size_t work = 0;
    check_equal(orm_tidesdb_sql_catalog_encode(&definition, sizeof(wire_golden), &bytes, &work, &error), TURBODB_STATUS_OK);
    check_equal(vec_size(&bytes), sizeof(wire_golden));
    check_equal(memcmp(vec_data_const(&bytes), wire_golden, sizeof(wire_golden)), 0);
    check_equal(orm_sql_work_release(&bytes, work, &budget, &error), TURBODB_STATUS_OK);
    reset(); uint8_t input[sizeof(wire_golden)]; memcpy(input, wire_golden, sizeof(input));
    check_equal(orm_tidesdb_sql_catalog_decode(input, sizeof(input), sizeof(input), &budget, &definition, &error), TURBODB_STATUS_OK);
    memset(input, 0, sizeof(input)); const orm_sql_table_schema schema = view();
    check_equal(schema.name.data[0], 't'); check_equal(schema.count, 3u);
    check_not_null(schema.defaults); check_false(schema.defaults[0].specified);
    check_equal(schema.columns[1].type.kind, TURBODB_VALUE_UINT64);
    check_equal(schema.columns[2].type.kind, TURBODB_VALUE_DOUBLE); check_false(schema.columns[0].type.nullable);
  }
  it("encodes schema v2 defaults and decodes an independent snapshot") {
    parse("CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT NOT NULL DEFAULT -7,"
        "u BIGINT UNSIGNED DEFAULT 9,v DOUBLE DEFAULT NULL)");
    check_equal(bind_create(),TURBODB_STATUS_OK); vec_t bytes={0}; size_t work=0;
    check_equal(orm_tidesdb_sql_catalog_encode(&definition,sizeof(wire_defaults),
        &bytes,&work,&error),TURBODB_STATUS_OK);
    check_equal(vec_size(&bytes),sizeof(wire_defaults));
    check_equal(memcmp(vec_data_const(&bytes),wire_defaults,sizeof(wire_defaults)),0);
    check_equal(orm_sql_work_release(&bytes,work,&budget,&error),TURBODB_STATUS_OK);
    reset(); uint8_t input[sizeof(wire_defaults)];
    memcpy(input,wire_defaults,sizeof(input));
    check_equal(orm_tidesdb_sql_catalog_decode(input,sizeof(input),sizeof(input),
        &budget,&definition,&error),TURBODB_STATUS_OK);
    memset(input,0,sizeof(input)); const orm_sql_table_schema schema=view();
    check_equal(schema.defaults[1].value.data.int64_value,-7);
    check_equal(schema.defaults[2].value.data.uint64_value,9u);
    check_equal(schema.defaults[3].value.kind,TURBODB_VALUE_NULL);
  }
  it("rejects every truncated schema v2 prefix and malformed defaults") {
    for(size_t n=0;n<sizeof(wire_defaults);++n) {
      reset(); check_equal(orm_tidesdb_sql_catalog_decode(wire_defaults,n,
          sizeof(wire_defaults),&budget,&definition,&error),
          TURBODB_STATUS_DATASTORE_ERROR);
      check_null(definition.budget);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
    }
    const struct mutation { size_t offset; uint8_t value; } changes[]={
      {19,3},{20,1},{32,1}};
    for(size_t i=0;i<sizeof(changes)/sizeof(changes[0]);++i) {
      reset(); uint8_t data[sizeof(wire_defaults)];
      memcpy(data,wire_defaults,sizeof(data));
      data[changes[i].offset]=changes[i].value;
      check_equal(orm_tidesdb_sql_catalog_decode(data,sizeof(data),sizeof(data),
          &budget,&definition,&error),TURBODB_STATUS_DATASTORE_ERROR);
      check_null(definition.budget);
    }
    reset(); uint8_t nonfinite[sizeof(wire_defaults)];
    memcpy(nonfinite,wire_defaults,sizeof(nonfinite)); nonfinite[58]=2;
    nonfinite[65]=0xf0; nonfinite[66]=0x7f;
    check_equal(orm_tidesdb_sql_catalog_decode(nonfinite,sizeof(nonfinite),
        sizeof(nonfinite),&budget,&definition,&error),TURBODB_STATUS_DATASTORE_ERROR);
  }
  it("rejects every truncated prefix and trailing schema bytes without leaking") {
    for (size_t n = 0; n < sizeof(wire_golden); ++n) {
      reset(); check_equal(orm_tidesdb_sql_catalog_decode(wire_golden, n, sizeof(wire_golden),
          &budget, &definition, &error), TURBODB_STATUS_DATASTORE_ERROR);
      check_null(definition.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
    uint8_t extra[sizeof(wire_golden) + 1]; memcpy(extra, wire_golden, sizeof(wire_golden)); extra[sizeof(wire_golden)] = 0;
    reset(); check_equal(orm_tidesdb_sql_catalog_decode(extra, sizeof(extra), sizeof(extra), &budget, &definition, &error), TURBODB_STATUS_DATASTORE_ERROR);
  }
  it("rejects unknown versions malformed fields duplicate names and invalid primary keys") {
    struct mutation { size_t offset; uint8_t value; turbodb_status_t status; } cases[] = {
      {0, 0, TURBODB_STATUS_DATASTORE_ERROR}, {2, 3, TURBODB_STATUS_UNSUPPORTED}, {3, 1, TURBODB_STATUS_DATASTORE_ERROR},
      {4, 0, TURBODB_STATUS_DATASTORE_ERROR}, {4, 255, TURBODB_STATUS_DATASTORE_ERROR}, {8, 3, TURBODB_STATUS_DATASTORE_ERROR},
      {12, 0, TURBODB_STATUS_DATASTORE_ERROR}, {13, '9', TURBODB_STATUS_DATASTORE_ERROR},
      {15, 3, TURBODB_STATUS_DATASTORE_ERROR}, {16, 1, TURBODB_STATUS_DATASTORE_ERROR},
      {20, 99, TURBODB_STATUS_DATASTORE_ERROR}, {21, 2, TURBODB_STATUS_DATASTORE_ERROR},
      {26, 'n', TURBODB_STATUS_DATASTORE_ERROR}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      reset(); uint8_t data[sizeof(wire_golden)]; memcpy(data, wire_golden, sizeof(data)); data[cases[i].offset] = cases[i].value;
      check_equal(orm_tidesdb_sql_catalog_decode(data, sizeof(data), sizeof(data), &budget, &definition, &error), cases[i].status);
      check_null(definition.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
  }
  it("enforces codec byte limits and refunds each allocation failure") {
    parse(basic_ddl); check_equal(bind_create(), TURBODB_STATUS_OK);
    const uint64_t base = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; vec_t bytes = {0}; size_t work = 0;
    check_equal(orm_tidesdb_sql_catalog_encode(&definition, 1, &bytes, &work, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    for (unsigned pass = 0; pass < 2; ++pass) {
      reserves = resizes = 0; if (pass) fail_resize = 1; else fail_reserve = 1;
      check_equal(orm_tidesdb_sql_catalog_encode(&definition, TEST_LIMIT, &bytes, &work, &error), TURBODB_STATUS_OUT_OF_MEMORY);
      check_false(bytes.initialized); check_equal(work, 0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], base);
      fail_reserve = fail_resize = 0;
    }
    reset(); check_equal(orm_tidesdb_sql_catalog_decode(wire_golden, sizeof(wire_golden), sizeof(wire_golden) - 1,
        &budget, &definition, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    for (unsigned pass = 0; pass < 2; ++pass) for (size_t point = 1; point <= 2; ++point) {
      reset(); reserves = resizes = 0; if (pass) fail_resize = point; else fail_reserve = point;
      check_equal(orm_tidesdb_sql_catalog_decode(wire_golden, sizeof(wire_golden), sizeof(wire_golden),
          &budget, &definition, &error), TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(definition.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
  }
}

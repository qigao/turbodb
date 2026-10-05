#include "../row_fixture.h"
#include <orm_runtime.h>
#include <tinytest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <math.h>

static orm_runtime_t *runtime;
static orm_connection_t *connection;
static orm_query_t *query;
static orm_transaction_t *transaction;
static cflow_publisher publisher;
static orm_error_t error;
static char *directory;
static orm_result_t *result;
static orm_config_t config;
static const char ddl[] = "CREATE TABLE items(id BIGINT PRIMARY KEY,score BIGINT)";
static const char recursive_option[] = "sql_max_recursive_iterations";
static const char found_rows_option[] = "sql_client_found_rows";

static void raw_query(const char *sql) {
  orm_query_destroy(query);
  query = NULL;
  check_equal(orm_raw(connection, orm_view(sql), &query, &error), ORM_STATUS_OK);
}
static void parameter(orm_value_t input) {
  check_equal(orm_query_bind(query, input, &error), ORM_STATUS_OK);
}

static orm_status_t open_command(void) {
  return transaction != NULL
      ? orm_query_open_command_flow_in_transaction(query, transaction, &publisher, &error)
      : orm_query_open_command_flow(query, &publisher, &error);
}

static void command(uint64_t expected) {
  orm_command_result_t command_result = ORM_COMMAND_RESULT_INIT;
  orm_status_t status = open_command();
  if (status != ORM_STATUS_OK)
    fprintf(stderr, "open command failed: status=%d error=%s\n", (int)status, error.message);
  check_equal(status, ORM_STATUS_OK);
  const cflow_step step = cflow_publisher_resume(&publisher, NULL, &command_result);
  if (step.kind == CFLOW_STEP_ERROR) info("command failed: %s", step.error);
  check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
  check_equal(command_result.affected_rows, expected);
  cflow_publisher_destroy(&publisher);
  publisher = (cflow_publisher){0};
}

static void open_rows(void) {
  orm_flow_config_t flow;
  orm_flow_config(&flow, &orm_tides_public_row_data);
  orm_status_t status = transaction != NULL
      ? orm_query_open_flow_in_transaction(query, transaction, &flow, &publisher, &error)
      : orm_query_open_flow(query, &flow, &publisher, &error);
  check_equal(status, ORM_STATUS_OK);
}

static void row(long id, long score) {
  orm_tides_public_row value = {0};
  check_equal(cflow_publisher_resume(&publisher, NULL, &value).kind, CFLOW_STEP_VALUE);
  check_equal(value.id, id);
  check_equal(value.score, score);
}

static void end_rows(void) {
  orm_tides_public_row value = {0};
  check_equal(cflow_publisher_resume(&publisher, NULL, &value).kind, CFLOW_STEP_DONE);
  cflow_publisher_destroy(&publisher);
  publisher = (cflow_publisher){0};
}

static orm_status_t connect_profile(const char *profile, const char *initialize,
    const orm_option_t *extra, size_t count) {
  enum { OPTION_CAPACITY = 12 };
  orm_option_t options[OPTION_CAPACITY] = {
    {orm_view("path"), orm_view(directory)}, {orm_view("column_family"), orm_view("rel")},
    {orm_view("sql_profile"), orm_view(profile)}, {orm_view("sql_initialize"), orm_view(initialize)}
  };
  check_true(count <= OPTION_CAPACITY - 4);
  for (size_t i = 0; i < count; ++i) options[i + 4] = extra[i];
  config.driver = orm_view("tidesdb"); config.options = options; config.option_count = (uint32_t)(4 + count);
  const orm_status_t status = orm_runtime_connect(runtime, &config, &connection, &error);
  config.options = NULL; config.option_count = 0;
  return status;
}
static void disconnect(void) {
  orm_result_destroy(result); result = NULL;
  orm_query_destroy(query); query = NULL;
  orm_disconnect(connection); connection = NULL;
}
static orm_status_t execute(void) {
  orm_result_destroy(result); result = NULL;
  const orm_status_t status = transaction ? orm_query_execute_in_transaction(query, transaction, &result, &error)
                                        : orm_query_execute(query, &result, &error);
  if (status != ORM_STATUS_OK) info("execute status %d: %s",status,error.message);
  return status;
}
static void seed(void) {
  raw_query(ddl); command(0);
  raw_query("INSERT INTO items(id,score) VALUES(1,10),(2,20),(3,30)"); command(3);
}
static void text_at(uint64_t r, uint64_t c, const char *expected) {
  vstr value = {0}; check_equal(orm_result_get_text(result, r, c, &value, &error), ORM_STATUS_OK);
  check_equal(value.len, strlen(expected)); check_equal(memcmp(value.data, expected, value.len), 0);
}
typedef enum structured_kind { STRUCT_SELECT, STRUCT_INSERT, STRUCT_UPDATE, STRUCT_DELETE } structured_kind;
static void structured(structured_kind kind, const char *table) {
  orm_query_destroy(query); query = NULL;
  orm_status_t status = ORM_STATUS_INTERNAL_ERROR;
  switch (kind) {
    case STRUCT_SELECT: status = orm_query_create(connection,orm_view(table),&query,&error); break;
    case STRUCT_INSERT: status = orm_insert(connection,orm_view(table),&query,&error); break;
    case STRUCT_UPDATE: status = orm_update(connection,orm_view(table),&query,&error); break;
    case STRUCT_DELETE: status = orm_delete(connection,orm_view(table),&query,&error); break;
  }
  check_equal(status,ORM_STATUS_OK);
}
static void assign(const char *column, orm_value_t value) {
  check_equal(orm_query_set(query,orm_view(column),value,&error),ORM_STATUS_OK);
}
static void where(const char *column, orm_compare_t comparison, orm_value_t value) {
  check_equal(orm_query_where(query,orm_view(column),comparison,value,&error),ORM_STATUS_OK);
}
static void project(const char *column) {
  check_equal(orm_query_add_column(query,orm_view(column),&error),ORM_STATUS_OK);
}
static void count_is(uint64_t expected) {
  uint64_t count = 0;
  check_equal(orm_result_row_count(result,&count,&error),ORM_STATUS_OK); check_equal(count,expected);
}
static void integer_at(uint64_t r, uint64_t c, int64_t expected) {
  int64_t value=0; check_equal(orm_result_get_int64(result,r,c,&value,&error),ORM_STATUS_OK);
  check_equal(value,expected);
}
static void unsigned_at(uint64_t r, uint64_t c, uint64_t expected) {
  uint64_t value=0;
  check_equal(orm_result_get_uint64(result,r,c,&value,&error),ORM_STATUS_OK);
  check_equal(value,expected);
}
static void double_at(uint64_t r, uint64_t c, double expected) {
  double value=0; check_equal(orm_result_get_double(result,r,c,&value,&error),ORM_STATUS_OK);
  check_equal(value,expected);
  orm_value_kind_t kind; check_equal(orm_result_value_kind(result,r,c,&kind,&error),ORM_STATUS_OK);
  check_equal(kind,ORM_VALUE_DOUBLE);
}
static void seed_doubles(void) {
  raw_query("CREATE TABLE metrics(id BIGINT PRIMARY KEY,score DOUBLE)"); command(0);
  raw_query("INSERT INTO metrics(id,score) VALUES(1,?),(2,?),(3,?),(4,NULL)");
  parameter(orm_f64(1.5)); parameter(orm_f64(2.5)); parameter(orm_f64(8.0)); command(4);
}
static void recursive_connection(const char *iterations) {
  disconnect(); const orm_option_t option={orm_view(recursive_option),orm_view(iterations)};
  check_equal(connect_profile("relational","false",&option,1),ORM_STATUS_OK);
}

spec("TidesDB relational profile through the real plugin") {
  before_each() {
    runtime = NULL; connection = NULL; query = NULL; transaction = NULL; result = NULL;
    publisher = (cflow_publisher){0}; orm_error_init(&error); orm_config(&config);
    directory = tt_make_temp_dir("orm-relational"); check_not_null(directory);
    orm_runtime_config_t settings; orm_runtime_config_init(&settings);
    check_equal(orm_runtime_create(&settings, &runtime, &error), ORM_STATUS_OK);
    const char *module = getenv("ORM_TIDESDB_PLUGIN"); check_not_null(module);
    orm_driver_load_config_t load = {0}; load.struct_size = sizeof(load); load.abi_version = ORM_RUNTIME_ABI_VERSION;
    load.module_path = orm_view(module); load.expected_driver_id = orm_view("tidesdb");
    check_equal(orm_runtime_load_driver(runtime, &load, &error), ORM_STATUS_OK);
    check_equal(connect_profile("relational", "true", NULL, 0), ORM_STATUS_OK);
  }
  after_each() {
    if (cflow_publisher_valid(&publisher)) cflow_publisher_destroy(&publisher);
    orm_transaction_destroy(transaction); transaction = NULL;
    disconnect();
    if (runtime) { check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK); orm_runtime_release(runtime); }
    if (directory) { check_equal(tt_remove_tree(directory), 0); free(directory); }
  }

  group("session system variable reads") {
    it("reads defaults and SESSION LOCAL aliases with native result kinds") {
      raw_query("SELECT @@autocommit,@@SESSION.transaction_read_only,@@LOCAL.transaction_isolation");
      check_equal(execute(),ORM_STATUS_OK); count_is(1); integer_at(0,0,1); integer_at(0,1,0); text_at(0,2,"SERIALIZABLE");
      raw_query("SELECT @@SeSsIoN . `AuToCoMmIt` AS id,@@transaction_read_only AS score");
      open_rows(); row(1,0); end_rows();
      raw_query("BEGIN"); command(0);
      raw_query("SELECT @@autocommit"); check_equal(execute(),ORM_STATUS_OK); integer_at(0,0,1);
      raw_query("ROLLBACK"); command(0);
    }
    it("reads session defaults independently of active and next transaction characteristics") {
      seed(); raw_query("SET SESSION TRANSACTION READ ONLY"); command(0);
      raw_query("START TRANSACTION READ WRITE"); command(0);
      raw_query("SELECT @@transaction_read_only"); check_equal(execute(),ORM_STATUS_OK); integer_at(0,0,1);
      raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      raw_query("SET SESSION TRANSACTION READ WRITE"); command(0);
      raw_query("SELECT @@transaction_read_only"); check_equal(execute(),ORM_STATUS_OK); integer_at(0,0,0);
      raw_query("ROLLBACK"); command(0);
      raw_query("SET TRANSACTION READ ONLY"); command(0); raw_query("BEGIN"); command(0);
      raw_query("SELECT @@transaction_read_only"); check_equal(execute(),ORM_STATUS_OK); integer_at(0,0,0);
      raw_query("UPDATE items SET score=50 WHERE id=1"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("ROLLBACK"); command(0);
      raw_query("SET LOCAL TRANSACTION READ ONLY"); command(0);
      raw_query("SELECT @@LOCAL.transaction_read_only"); check_equal(execute(),ORM_STATUS_OK); integer_at(0,0,1);
      disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("SELECT @@transaction_read_only,@@autocommit"); check_equal(execute(),ORM_STATUS_OK); integer_at(0,0,0); integer_at(0,1,1);
    }
    it("uses the old snapshot for autocommit self assignment and commits only a successful zero to one transition") {
      seed(); raw_query("SET autocommit=0"); command(0);
      raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      raw_query("SET autocommit=@@LOCAL.autocommit"); command(0);
      raw_query("SELECT @@autocommit"); check_equal(execute(),ORM_STATUS_OK); integer_at(0,0,0);
      raw_query("SET autocommit=@@unknown"); check_equal(execute(),ORM_STATUS_UNSUPPORTED);
      raw_query("ROLLBACK"); command(0);
      raw_query("SELECT score FROM items WHERE id=1"); check_equal(execute(),ORM_STATUS_OK); integer_at(0,0,10);
      raw_query("UPDATE items SET score=41 WHERE id=1"); command(1);
      raw_query("SET autocommit=@@autocommit+1"); command(0);
      raw_query("ROLLBACK"); command(0);
      raw_query("SELECT @@autocommit AS ac,score FROM items WHERE id=1"); check_equal(execute(),ORM_STATUS_OK); integer_at(0,0,1); integer_at(0,1,41);
    }
    it("propagates one session snapshot through CTE derived compound scalar group and window plans") {
      seed(); raw_query("SET autocommit=0"); command(0);
      raw_query("WITH s AS (SELECT @@autocommit AS n) SELECT d.n+(SELECT @@autocommit) AS n FROM (SELECT n FROM s) d UNION ALL SELECT @@LOCAL.autocommit");
      check_equal(execute(),ORM_STATUS_OK); count_is(2); integer_at(0,0,0); integer_at(1,0,0);
      raw_query("SELECT @@SESSION.autocommit AS ac,COUNT(*) AS n FROM items GROUP BY @@autocommit");
      check_equal(execute(),ORM_STATUS_OK); count_is(1); integer_at(0,0,0); integer_at(0,1,3);
      raw_query("SELECT id,ROW_NUMBER() OVER (ORDER BY id+@@autocommit) AS rn FROM items WHERE @@autocommit=0 ORDER BY id");
      check_equal(execute(),ORM_STATUS_OK); count_is(3); integer_at(0,0,1); integer_at(0,1,1);
      raw_query("EXPLAIN SELECT id FROM items WHERE @@autocommit=0"); check_equal(execute(),ORM_STATUS_OK);
      raw_query("ROLLBACK"); command(0); raw_query("SET autocommit=1"); command(0);
    }
    it("propagates the snapshot through values duplicate assignments insert select and update delete dependencies") {
      seed(); raw_query("INSERT INTO items(id,score) VALUES(4,@@autocommit+10)"); command(1);
      raw_query("INSERT INTO items(id,score) VALUES(4,99) ON DUPLICATE KEY UPDATE score=score+@@LOCAL.autocommit"); command(2);
      raw_query("INSERT INTO items(id,score) SELECT 5,@@autocommit"); command(1);
      raw_query("UPDATE items SET score=(SELECT @@autocommit)+score WHERE id=5 AND @@autocommit=1"); command(1);
      raw_query("SELECT score FROM items WHERE id>=4 ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(2); integer_at(0,0,12); integer_at(1,0,2);
      raw_query("DELETE FROM items WHERE id=5 AND @@SESSION.autocommit IN (SELECT @@autocommit)"); command(1);
      raw_query("SET autocommit=0"); command(0);
      check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      raw_query("SELECT @@autocommit"); check_equal(execute(),ORM_STATUS_OK); integer_at(0,0,0);
      check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
      raw_query("SET autocommit=1"); command(0);
    }
    it("enumerates supported SHOW VARIABLES values and matches ASCII names with LIKE") {
      raw_query("SHOW VARIABLES"); check_equal(execute(),ORM_STATUS_OK); count_is(3);
      text_at(0,0,"autocommit"); text_at(0,1,"ON"); text_at(1,0,"transaction_isolation"); text_at(1,1,"SERIALIZABLE");
      text_at(2,0,"transaction_read_only"); text_at(2,1,"OFF");
      raw_query("SET autocommit=0"); command(0); raw_query("SET SESSION TRANSACTION READ ONLY"); command(0);
      raw_query("SHOW SESSION VARIABLES LIKE 'AUTO%' "); check_equal(execute(),ORM_STATUS_OK); count_is(1); text_at(0,0,"autocommit"); text_at(0,1,"OFF");
      raw_query("SHOW LOCAL VARIABLES LIKE 'TRANSACTION_READ_O_L%'"); check_equal(execute(),ORM_STATUS_OK); count_is(1); text_at(0,1,"ON");
      raw_query("SHOW VARIABLES LIKE 'transaction%' "); check_equal(execute(),ORM_STATUS_OK); count_is(2);
      raw_query("SHOW VARIABLES LIKE 'missing%' "); check_equal(execute(),ORM_STATUS_OK); count_is(0);
      raw_query("ROLLBACK"); command(0); raw_query("SET autocommit=1"); command(0);
      raw_query("SET SESSION TRANSACTION READ WRITE"); command(0);
    }
    it("rejects unsupported variable scopes names and SHOW filters before writing or committing") {
      seed(); raw_query("BEGIN"); command(0); raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      const char *const unsupported[]={"SELECT @@GLOBAL.autocommit","SELECT @autocommit","SELECT @@sql_mode",
        "SELECT CASE WHEN FALSE THEN @@unknown ELSE 1 END LIMIT 0",
        "SELECT id FROM items WHERE @@missing=1 LIMIT 0", "SHOW GLOBAL VARIABLES", "SHOW VARIABLES WHERE @@unknown=1",
        "SHOW VARIABLES LIKE 'é%'", "UPDATE items SET score=@@unknown WHERE id=1",
        "INSERT INTO items(id,score) VALUES(4,@@unknown)","SET autocommit=@@GLOBAL.autocommit"};
      for (size_t i=0;i<sizeof(unsupported)/sizeof(unsupported[0]);++i) {
        raw_query(unsupported[i]); check_equal(execute(),ORM_STATUS_UNSUPPORTED); check_null(result);
      }
      raw_query("SHOW VARIABLES"); parameter(orm_i64(1)); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("ROLLBACK"); command(0); raw_query("SELECT score FROM items WHERE id=1"); check_equal(execute(),ORM_STATUS_OK); integer_at(0,0,10);
      raw_query("CREATE TABLE bad_defaults(id BIGINT PRIMARY KEY,n BIGINT DEFAULT(@@autocommit))"); check_equal(execute(),ORM_STATUS_UNSUPPORTED);
    }
  }
  group("SET transaction system variables through the native plugin") {
    it("sets session read only through every supported name form and reads the same source") {
      seed();
      const char *const on[]={"SET TrAnSaCtIoN_ReAd_OnLy=ON","SET SESSION `transaction_read_only`=TRUE",
        "SET LOCAL transaction_read_only='oN'","SET @@SESSION.transaction_read_only=1",
        "SET @@LOCAL.`transaction_read_only`=1+0"};
      const char *const off[]={"SET transaction_read_only=OFF","SET LOCAL transaction_read_only=FALSE",
        "SET SESSION transaction_read_only='oFf'","SET @@SESSION.transaction_read_only=0",
        "SET @@LOCAL.transaction_read_only=DEFAULT"};
      for(size_t i=0;i<sizeof(on)/sizeof(on[0]);++i) {
        raw_query(on[i]);command(0);
        raw_query("SELECT @@transaction_read_only,@@SESSION.transaction_read_only");
        check_equal(execute(),ORM_STATUS_OK);integer_at(0,0,1);integer_at(0,1,1);
        raw_query("SHOW VARIABLES WHERE Variable_name='transaction_read_only'");
        check_equal(execute(),ORM_STATUS_OK);count_is(1);text_at(0,1,"ON");
        raw_query("UPDATE items SET score=40 WHERE id=1");check_equal(execute(),ORM_STATUS_SQL_ERROR);
        raw_query(off[i]);command(0);
        raw_query("UPDATE items SET score=score+1 WHERE id=1");command(1);
      }
      raw_query("SET transaction_read_only=CASE WHEN ? THEN @@transaction_read_only=0 ELSE ? END");
      parameter(orm_bool(true));parameter(orm_bool(false));command(0);
      raw_query("SET transaction_read_only=@@LOCAL.transaction_read_only");command(0);
      raw_query("SELECT @@transaction_read_only");check_equal(execute(),ORM_STATUS_OK);integer_at(0,0,1);
      raw_query("SET transaction_read_only=COALESCE(?,?)");parameter(orm_null());parameter(orm_u64(0));command(0);
    }
    it("treats bare at at transaction variables as next only while reads retain the session value") {
      seed();raw_query("SET @@transaction_read_only=ON");command(0);
      raw_query("START TRANSACTION");command(0);
      raw_query("SELECT @@transaction_read_only,@@autocommit");check_equal(execute(),ORM_STATUS_OK);
      integer_at(0,0,0);integer_at(0,1,1);
      raw_query("UPDATE items SET score=40 WHERE id=1");check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("SET @@transaction_read_only=OFF");check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("SET @@transaction_isolation='SERIALIZABLE'");check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("ROLLBACK");command(0);
      raw_query("UPDATE items SET score=41 WHERE id=1");command(1);
      raw_query("SET transaction_read_only=ON");command(0);
      raw_query("SET @@transaction_read_only=DEFAULT");command(0);raw_query("BEGIN");command(0);
      raw_query("SELECT @@transaction_read_only");check_equal(execute(),ORM_STATUS_OK);integer_at(0,0,1);
      raw_query("UPDATE items SET score=42 WHERE id=1");command(1);raw_query("COMMIT");command(0);
      raw_query("UPDATE items SET score=43 WHERE id=1");check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("SET transaction_read_only=DEFAULT");command(0);
      raw_query("SELECT score FROM items WHERE id=1");check_equal(execute(),ORM_STATUS_OK);integer_at(0,0,42);
    }
    it("changes only the named characteristic and preserves frozen active access through savepoints") {
      seed();raw_query("SET @@transaction_read_only=ON");command(0);
      raw_query("SET transaction_isolation='serializable'");command(0);
      raw_query("SET @@transaction_isolation=3");command(0);raw_query("BEGIN");command(0);
      raw_query("SET SESSION transaction_read_only=OFF");command(0);
      raw_query("UPDATE items SET score=40 WHERE id=1");check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("ROLLBACK");command(0);
      raw_query("SET @@transaction_read_only=ON");command(0);
      raw_query("SET transaction_read_only=0");command(0);raw_query("BEGIN");command(0);
      raw_query("SAVEPOINT before_setting");command(0);
      raw_query("SET transaction_read_only=ON");command(0);
      raw_query("UPDATE items SET score=41 WHERE id=1");command(1);
      raw_query("ROLLBACK TO before_setting");command(0);raw_query("ROLLBACK");command(0);
      raw_query("BEGIN");command(0);raw_query("UPDATE items SET score=42 WHERE id=1");check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("SET transaction_read_only=0");command(0);raw_query("COMMIT AND CHAIN");command(0);
      raw_query("DELETE FROM items");check_equal(execute(),ORM_STATUS_SQL_ERROR);raw_query("ROLLBACK");command(0);
      raw_query("UPDATE items SET score=43 WHERE id=1");command(1);
    }
    it("accepts SERIALIZABLE text defaults and enum ordinal parameters while preserving pending access") {
      seed();const char *const sql[]={"SET transaction_isolation='SERIALIZABLE'",
        "SET SESSION transaction_isolation=DEFAULT","SET LOCAL transaction_isolation=3",
        "SET @@SESSION.transaction_isolation=@@transaction_isolation",
        "SET @@LOCAL.`transaction_isolation`='serializable'","SET @@transaction_isolation=DEFAULT"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {raw_query(sql[i]);command(0);}
      raw_query("SET transaction_isolation=COALESCE(?,@@transaction_isolation)");parameter(orm_null());command(0);
      raw_query("SET transaction_isolation=?");parameter(orm_u64(3));command(0);
      raw_query("SET @@transaction_read_only=ON");command(0);
      raw_query("SET transaction_isolation=CASE WHEN ? THEN ? ELSE @@transaction_isolation END");
      parameter(orm_bool(true));parameter(orm_text("sErIaLiZaBlE"));command(0);
      raw_query("BEGIN");command(0);
      raw_query("SELECT @@transaction_isolation");check_equal(execute(),ORM_STATUS_OK);text_at(0,0,"SERIALIZABLE");
      raw_query("UPDATE items SET score=40 WHERE id=1");check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("ROLLBACK");command(0);
    }
    it("shares session writes with ORM handles without changing their frozen access or lifecycle") {
      seed();check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      raw_query("SET @@SESSION.transaction_read_only=?");parameter(orm_bool(true));command(0);
      raw_query("SET LOCAL transaction_isolation=@@transaction_isolation");command(0);
      raw_query("UPDATE items SET score=40 WHERE id=1");command(1);
      raw_query("SET @@transaction_read_only=0");check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("SET @@transaction_isolation=3");check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("SET autocommit=0");check_equal(execute(),ORM_STATUS_UNSUPPORTED);
      check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);orm_transaction_destroy(transaction);transaction=NULL;
      check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      raw_query("SET transaction_read_only=DEFAULT");command(0);
      raw_query("DELETE FROM items");check_equal(execute(),ORM_STATUS_SQL_ERROR);
      check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);orm_transaction_destroy(transaction);transaction=NULL;
      raw_query("UPDATE items SET score=41 WHERE id=1");command(1);
    }
    it("rejects invalid values missing parameters and unsupported variables without ending pending writes") {
      seed();raw_query("BEGIN");command(0);raw_query("UPDATE items SET score=40 WHERE id=1");command(1);
      const char *const invalid[]={"SET transaction_read_only=2","SET transaction_read_only=-1",
        "SET transaction_read_only=1.0","SET transaction_read_only=NULL","SET transaction_read_only='1'",
        "SET transaction_isolation=NULL","SET transaction_isolation='bad'","SET transaction_isolation=4",
        "SET transaction_isolation=-1","SET transaction_isolation=3.0","SET transaction_isolation='3'",
        "SET transaction_read_only=?","SET transaction_read_only=1/0.0"};
      for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        raw_query(invalid[i]);check_equal(execute(),ORM_STATUS_SQL_ERROR);check_null(result);
      }
      const char *const unsupported[]={"SET GLOBAL transaction_read_only=1","SET @@GLOBAL.transaction_isolation=3",
        "SET @transaction_read_only=1","SET transaction_read_only=1,autocommit=0",
        "SET transaction_isolation='READ-COMMITTED'","SET transaction_isolation=0",
        "SET transaction_isolation=1","SET transaction_isolation=2",
        "SET transaction_read_only=CASE WHEN TRUE THEN 0 ELSE @@unknown END",
        "SET transaction_read_only=(SELECT 1)"};
      for(size_t i=0;i<sizeof(unsupported)/sizeof(unsupported[0]);++i) {
        raw_query(unsupported[i]);check_equal(execute(),ORM_STATUS_UNSUPPORTED);check_null(result);
      }
      raw_query("SET transaction_isolation=3");parameter(orm_i64(3));check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("SELECT @@transaction_read_only AS mode,score FROM items WHERE id=1");
      check_equal(execute(),ORM_STATUS_OK);integer_at(0,0,0);integer_at(0,1,40);
      raw_query("ROLLBACK");command(0);
      raw_query("SELECT score FROM items WHERE id=1");check_equal(execute(),ORM_STATUS_OK);integer_at(0,0,10);
    }
    it("publishes only on resume and resets defaults when reconnecting") {
      seed();raw_query("SET transaction_read_only=ON");check_equal(open_command(),ORM_STATUS_OK);
      cflow_publisher_cancel(&publisher);cflow_publisher_destroy(&publisher);publisher=(cflow_publisher){0};
      raw_query("UPDATE items SET score=40 WHERE id=1");command(1);
      raw_query("SET transaction_read_only=?");parameter(orm_text("ON"));check_equal(open_command(),ORM_STATUS_OK);
      orm_query_destroy(query);query=NULL;orm_command_result_t output=ORM_COMMAND_RESULT_INIT;
      check_equal(cflow_publisher_resume(&publisher,NULL,&output).kind,CFLOW_STEP_VALUE_AND_DONE);check_equal(output.affected_rows,0u);
      cflow_publisher_destroy(&publisher);publisher=(cflow_publisher){0};
      raw_query("UPDATE items SET score=41 WHERE id=1");check_equal(execute(),ORM_STATUS_SQL_ERROR);
      disconnect();check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("SELECT @@transaction_read_only,@@transaction_isolation,@@autocommit");
      check_equal(execute(),ORM_STATUS_OK);integer_at(0,0,0);text_at(0,1,"SERIALIZABLE");integer_at(0,2,1);
    }
  }
  group("SHOW filtering through the native plugin") {
    it("matches table and column LIKE without losing metadata columns") {
      seed(); raw_query("CREATE TABLE others(id BIGINT PRIMARY KEY)"); command(0);
      raw_query("SHOW FULL TABLES LIKE 'it%'"); check_equal(execute(),ORM_STATUS_OK); count_is(1);
      text_at(0,0,"items"); text_at(0,1,"BASE TABLE");
      raw_query("SHOW TABLES LIKE 'IT%'"); check_equal(execute(),ORM_STATUS_OK); count_is(0);
      raw_query("SHOW FIELDS IN items LIKE 'SCO%'"); check_equal(execute(),ORM_STATUS_OK); count_is(1);
      text_at(0,0,"score"); text_at(0,1,"bigint");
    }
    it("filters table and column rows using case insensitive displayed labels and parameters") {
      seed(); raw_query("SHOW FULL TABLES WHERE Tables_IN_rel=? AND table_TYPE='BASE TABLE'");
      parameter(orm_text("items")); check_equal(execute(),ORM_STATUS_OK); count_is(1); text_at(0,0,"items");
      raw_query("SHOW COLUMNS FROM items WHERE fIeLd IN ('id','score') AND `Null`='YES' AND `Default` IS NULL");
      check_equal(execute(),ORM_STATUS_OK); count_is(1); text_at(0,0,"score");
      raw_query("SHOW COLUMNS FROM items WHERE Field LIKE 's%' AND `Key`<> 'PRI'");
      check_equal(execute(),ORM_STATUS_OK); count_is(1); text_at(0,0,"score");
      raw_query("SHOW COLUMNS FROM items WHERE NULL"); check_equal(execute(),ORM_STATUS_OK); count_is(0);
      raw_query("SELECT ID FROM items"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
    }
    it("filters index parts by numeric NULL and text metadata") {
      seed(); raw_query("CREATE INDEX pair ON items(score DESC,id ASC)"); command(0);
      raw_query("SHOW KEYS IN items WHERE non_unique=1 AND seq_IN_index>=? AND Cardinality IS NULL");
      parameter(orm_i64(2)); check_equal(execute(),ORM_STATUS_OK); count_is(1);
      text_at(0,2,"pair"); integer_at(0,3,2); text_at(0,4,"id");
      raw_query("SHOW INDEX FROM items WHERE Key_name='PRIMARY' OR `Collation`='D'");
      check_equal(execute(),ORM_STATUS_OK); count_is(2);
    }
    it("filters connection variables using the statement snapshot and value parameters") {
      raw_query("SET autocommit=0"); command(0); raw_query("SET SESSION TRANSACTION READ ONLY"); command(0);
      raw_query("SHOW LOCAL VARIABLES WHERE vAlUe=? AND @@SESSION.autocommit=0"); parameter(orm_text("ON"));
      check_equal(execute(),ORM_STATUS_OK); count_is(1); text_at(0,0,"transaction_read_only");
      raw_query("SHOW VARIABLES WHERE Variable_name='autocommit' OR Value='SERIALIZABLE'");
      check_equal(execute(),ORM_STATUS_OK); count_is(2);
      raw_query("ROLLBACK"); command(0);
    }
    it("rejects invalid empty source bindings and expressions without ending an active transaction") {
      raw_query("SHOW TABLES WHERE missing=1"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      seed(); raw_query("BEGIN"); command(0); raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      const char *const sql[]={"SHOW COLUMNS FROM items WHERE id=1", "SHOW FULL TABLES WHERE Table_type=?"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        raw_query(sql[i]); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
      }
      raw_query("SHOW COLUMNS FROM items WHERE FALSE AND @@unknown=1"); check_equal(execute(),ORM_STATUS_UNSUPPORTED);
      raw_query("SHOW INDEX FROM items WHERE EXISTS(SELECT 1)"); check_equal(execute(),ORM_STATUS_UNSUPPORTED);
      raw_query("ROLLBACK"); command(0);
      raw_query("SELECT score FROM items WHERE id=1"); check_equal(execute(),ORM_STATUS_OK); integer_at(0,0,10);
    }
  }
  group("numeric CAST through the native plugin") {
    it("preserves FLOAT rounding in group keys sets and alias identity") {
      raw_query("SELECT CAST(16777217 AS FLOAT),CAST(16777217 AS FLOAT(24)),"
        "CAST(16777217 AS FLOAT(25)),CAST(16777217 AS DOUBLE PRECISION)");
      check_equal(execute(),ORM_STATUS_OK); double_at(0,0,16777216.0); double_at(0,1,16777216.0);
      double_at(0,2,16777217.0); double_at(0,3,16777217.0);
      raw_query("CREATE TABLE float_groups(id BIGINT PRIMARY KEY,n BIGINT)"); command(0);
      raw_query("INSERT INTO float_groups VALUES(1,16777216),(2,16777217),(3,16777218)"); command(3);
      raw_query("SELECT CAST(n AS FLOAT(24)) AS v,COUNT(*) AS c FROM float_groups GROUP BY CAST(n AS FLOAT) ORDER BY v");
      check_equal(execute(),ORM_STATUS_OK); count_is(2);
      double_at(0,0,16777216.0); integer_at(0,1,2); double_at(1,0,16777218.0); integer_at(1,1,1);
      raw_query("SELECT CAST(n AS FLOAT) AS v,CAST(n AS DOUBLE) AS v FROM float_groups ORDER BY v");
      check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("SELECT CAST(16777216 AS FLOAT) AS v UNION SELECT CAST(16777217 AS FLOAT(24))");
      check_equal(execute(),ORM_STATUS_OK); count_is(1); double_at(0,0,16777216.0);
      raw_query("SELECT CAST(16777216 AS FLOAT(25)) AS v UNION SELECT CAST(16777217 AS FLOAT(53))");
      check_equal(execute(),ORM_STATUS_OK); count_is(2); double_at(0,0,16777216.0); double_at(1,0,16777217.0);
      raw_query("SELECT CAST('0.1junk' AS FLOAT)"); check_equal(execute(),ORM_STATUS_OK);
      double_at(0,0,0.100000001490116119384765625);
      raw_query("SHOW WARNINGS"); check_equal(execute(),ORM_STATUS_OK); count_is(1); integer_at(0,1,1292);
    }
    it("persists FLOAT defaults and backfill while rolling back later default changes") {
      raw_query("CREATE TABLE float_values(id BIGINT PRIMARY KEY,a DOUBLE DEFAULT(CAST(16777217 AS FLOAT(24))),"
        "b DOUBLE DEFAULT(CAST(16777217 AS FLOAT(25))))"); command(0);
      raw_query("INSERT INTO float_values(id) VALUES(1)"); command(1);
      raw_query("INSERT INTO float_values VALUES(2,CAST('0.1' AS FLOAT),CAST('0.1' AS DOUBLE PRECISION))"); command(1);
      raw_query("ALTER TABLE float_values ALTER a SET DEFAULT(CAST(16777219 AS FLOAT))"); command(0);
      raw_query("INSERT INTO float_values(id) VALUES(3)"); command(1);
      raw_query("ALTER TABLE float_values ADD c DOUBLE NOT NULL DEFAULT(CAST('0.1' AS FLOAT(24)))"); command(0);
      check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      raw_query("SAVEPOINT float_default"); command(0);
      raw_query("ALTER TABLE float_values ALTER a SET DEFAULT(CAST(16777219 AS FLOAT(53)))"); command(0);
      raw_query("INSERT INTO float_values(id) VALUES(4)"); command(1);
      raw_query("ROLLBACK TO SAVEPOINT float_default"); command(0);
      check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
      orm_transaction_destroy(transaction); transaction=NULL; disconnect();
      check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("SELECT a,b,c FROM float_values ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(3);
      double_at(0,0,16777216.0); double_at(0,1,16777217.0);
      double_at(1,0,0.100000001490116119384765625); double_at(1,1,0.1);
      double_at(2,0,16777220.0); double_at(2,1,16777217.0);
      for(uint64_t i=0;i<3;++i) double_at(i,2,0.100000001490116119384765625);
      raw_query("INSERT INTO float_values(id) VALUES(5)"); command(1);
      raw_query("SELECT a FROM float_values WHERE id=5"); check_equal(execute(),ORM_STATUS_OK); double_at(0,0,16777220.0);
    }
    it("rejects FLOAT overflow in strict and IGNORE statements without partial writes") {
      seed_doubles(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      raw_query("INSERT INTO metrics VALUES(9,9.0)"); command(1);
      const char *sql[]={"INSERT INTO metrics VALUES(10,10.0),(11,CAST(1e39 AS FLOAT))",
        "INSERT IGNORE INTO metrics VALUES(10,10.0),(11,CAST(1e39 AS FLOAT))",
        "UPDATE metrics SET score=CAST(CASE WHEN id=2 THEN 1e39 ELSE 8.0 END AS FLOAT) ORDER BY id",
        "UPDATE IGNORE metrics SET score=CAST(CASE WHEN id=2 THEN 1e39 ELSE 8.0 END AS FLOAT) ORDER BY id",
        "REPLACE INTO metrics VALUES(1,CAST(1e39 AS FLOAT))",
        "INSERT INTO metrics VALUES(1,2.0) ON DUPLICATE KEY UPDATE score=CAST(1e39 AS FLOAT)"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        raw_query(sql[i]); check_equal(execute(),ORM_STATUS_OUT_OF_RANGE); check_null(result);
      }
      raw_query("SELECT id,score FROM metrics ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(5);
      double_at(0,1,1.5); double_at(1,1,2.5); double_at(2,1,8.0); double_at(4,1,9.0);
      check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
      orm_transaction_destroy(transaction); transaction=NULL;
      raw_query("SELECT CAST(1e39 AS FLOAT(25))"); check_equal(execute(),ORM_STATUS_OK); double_at(0,0,1e39);
      raw_query("SELECT CAST(NULL AS FLOAT(54)) LIMIT 0"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("ALTER TABLE metrics ALTER score SET DEFAULT(CAST(1 AS FLOAT(54)))"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("SELECT id FROM metrics ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(4);
    }
    it("returns exact prefixes and complements with bounded query diagnostics") {
      disconnect(); const orm_option_t option={orm_view("sql_max_warnings"),orm_view("2")};
      check_equal(connect_profile("relational","false",&option,1),ORM_STATUS_OK);
      raw_query("SELECT CAST('-1.9' AS UNSIGNED INTEGER) AS u,CAST('12.9e3' AS SIGNED) AS i,"
        "CAST(18446744073709551615 AS SIGNED INTEGER) AS n,CAST('18446744073709551615' AS SIGNED) AS t,"
        "CAST('12.9e3' AS DOUBLE) AS d,CAST(TRUE AS REAL) AS b");
      check_equal(execute(),ORM_STATUS_OK); count_is(1);
      unsigned_at(0,0,UINT64_MAX); integer_at(0,1,12); integer_at(0,2,-1); integer_at(0,3,-1);
      double_at(0,4,12900.0); double_at(0,5,1.0);
      raw_query("SHOW COUNT(*) WARNINGS"); check_equal(execute(),ORM_STATUS_OK); unsigned_at(0,0,4);
      raw_query("SHOW WARNINGS"); check_equal(execute(),ORM_STATUS_OK); count_is(2);
      integer_at(0,1,1292); integer_at(1,1,1105);
      raw_query("SELECT COALESCE(7,CAST('bad' AS SIGNED)),CASE WHEN TRUE THEN 8 ELSE CAST('bad' AS SIGNED) END");
      check_equal(execute(),ORM_STATUS_OK); integer_at(0,0,7); integer_at(0,1,8);
      raw_query("SHOW COUNT(*) WARNINGS"); check_equal(execute(),ORM_STATUS_OK); unsigned_at(0,0,0);
    }
    it("rolls back strict CAST failures without losing earlier transaction writes") {
      seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      raw_query("INSERT INTO items VALUES(4,40)"); command(1);
      const char *sql[]={"INSERT INTO items VALUES(5,50),(6,CAST('bad' AS SIGNED))",
        "UPDATE items SET score=CAST(CASE WHEN id=2 THEN 'bad' ELSE '40' END AS SIGNED) ORDER BY id",
        "INSERT INTO items SELECT id+10,CAST('2.5' AS SIGNED) FROM items",
        "REPLACE INTO items VALUES(1,CAST('bad' AS SIGNED))",
        "INSERT INTO items VALUES(1,11) ON DUPLICATE KEY UPDATE score=CAST('bad' AS SIGNED)"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        raw_query(sql[i]); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
      }
      raw_query("SELECT id,score FROM items ORDER BY id"); open_rows(); row(1,10); row(2,20); row(3,30); row(4,40); end_rows();
      check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
      orm_transaction_destroy(transaction); transaction=NULL; disconnect();
      check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("SELECT id,score FROM items ORDER BY id"); open_rows(); row(1,10); row(2,20); row(3,30); row(4,40); end_rows();
    }
    it("preserves CAST versus assignment semantics in IGNORE writes and persists numeric defaults") {
      raw_query("CREATE TABLE cast_items(id BIGINT PRIMARY KEY,n BIGINT DEFAULT(CAST('12' AS SIGNED INTEGER)),"
        "u BIGINT UNSIGNED,d DOUBLE DEFAULT(CAST('12.9e3' AS DOUBLE)))"); command(0);
      raw_query("INSERT INTO cast_items(id,u) VALUES(1,CAST('-1' AS UNSIGNED))"); command(1);
      raw_query("SHOW WARNINGS"); check_equal(execute(),ORM_STATUS_OK); count_is(1); integer_at(0,1,1105);
      raw_query("INSERT IGNORE INTO cast_items(id,n) VALUES(2,'2.5'),(3,CAST('2.5' AS SIGNED)),(4,CAST('bad' AS SIGNED))"); command(3);
      raw_query("SHOW COUNT(*) WARNINGS"); check_equal(execute(),ORM_STATUS_OK); unsigned_at(0,0,3);
      raw_query("UPDATE IGNORE cast_items SET n=CAST('7.5' AS SIGNED) WHERE id=4"); command(1);
      raw_query("SHOW WARNINGS"); check_equal(execute(),ORM_STATUS_OK); count_is(1); integer_at(0,1,1292);
      raw_query("ALTER TABLE cast_items ALTER n SET DEFAULT(CAST('2.5' AS SIGNED))"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("INSERT INTO cast_items(id) VALUES(5)"); command(1);
      raw_query("SELECT n,u,d FROM cast_items ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(5);
      integer_at(0,0,12); unsigned_at(0,1,UINT64_MAX); double_at(0,2,12900.0);
      integer_at(1,0,3); integer_at(2,0,2); integer_at(3,0,7); integer_at(4,0,12);
      disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("SELECT n,d FROM cast_items WHERE id=5"); check_equal(execute(),ORM_STATUS_OK); integer_at(0,0,12); double_at(0,1,12900.0);
    }
    it("binds CAST children across grouped window correlated recursive and compound queries") {
      seed();
      raw_query("SELECT CAST(score AS SIGNED) AS n,CAST(COUNT(*) AS DOUBLE) AS c FROM items "
        "GROUP BY CAST(score AS SIGNED) HAVING CAST(COUNT(*) AS SIGNED)>0 ORDER BY n");
      check_equal(execute(),ORM_STATUS_OK); count_is(3);
      for(uint64_t i=0;i<3;++i) { integer_at(i,0,(int64_t)(i+1)*10); double_at(i,1,1.0); }
      raw_query("SELECT CAST(ROW_NUMBER() OVER(ORDER BY id) AS SIGNED) AS n FROM items ORDER BY n");
      check_equal(execute(),ORM_STATUS_OK); count_is(3); for(uint64_t i=0;i<3;++i) integer_at(i,0,(int64_t)i+1);
      raw_query("SELECT CAST((SELECT b.score FROM items b WHERE b.id=a.id) AS UNSIGNED) AS n FROM items a ORDER BY a.id");
      check_equal(execute(),ORM_STATUS_OK); count_is(3); for(uint64_t i=0;i<3;++i) unsigned_at(i,0,(i+1)*10);
      recursive_connection("4");
      raw_query("WITH RECURSIVE c(n) AS(SELECT CAST('1' AS SIGNED) UNION ALL "
        "SELECT CAST(n+1 AS SIGNED) FROM c WHERE n<3) SELECT CAST(n AS DOUBLE) AS n FROM c ORDER BY n");
      check_equal(execute(),ORM_STATUS_OK); count_is(3); for(uint64_t i=0;i<3;++i) double_at(i,0,(double)i+1.0);
      raw_query("SELECT CAST(1 AS DOUBLE) AS n UNION SELECT CAST(2 AS SIGNED) ORDER BY n");
      check_equal(execute(),ORM_STATUS_OK); count_is(2); double_at(0,0,1.0); double_at(1,0,2.0);
      raw_query("SELECT CASE WHEN TRUE THEN 1 ELSE CAST(1 AS CHAR) END"); check_equal(execute(),ORM_STATUS_UNSUPPORTED);
    }
  }

  group("converted numeric defaults through the native plugin") {
    it("persists converted CREATE ALTER and ADD defaults and reuses them in every write form") {
      raw_query("CREATE TABLE converted(id BIGINT PRIMARY KEY DEFAULT FALSE,n BIGINT NOT NULL DEFAULT '2.5',"
          "u BIGINT UNSIGNED DEFAULT '184467440737095516150e-1',v DOUBLE DEFAULT '7')");command(0);
      raw_query("INSERT INTO converted() VALUES()");command(1);
      raw_query("INSERT INTO converted(id) VALUES(1)");command(1);
      raw_query("INSERT INTO converted VALUES(2,DEFAULT,DEFAULT,DEFAULT)");command(1);
      raw_query("INSERT INTO converted SET id=3");command(1);
      raw_query("INSERT INTO converted(id) SELECT 4");command(1);
      raw_query("CREATE INDEX n_key ON converted(n)");command(0);
      raw_query("ALTER TABLE converted ADD extra BIGINT NOT NULL DEFAULT '-2.5' AFTER u");command(0);
      raw_query("ALTER TABLE converted ALTER n SET DEFAULT TRUE");command(0);
      raw_query("INSERT INTO converted(id) VALUES(5)");command(1);
      raw_query("REPLACE INTO converted(id) VALUES(6)");command(1);
      raw_query("UPDATE converted SET n=DEFAULT WHERE id=1");command(1);
      raw_query("INSERT INTO converted(id,n) VALUES(0,99) ON DUPLICATE KEY UPDATE n=DEFAULT");command(2);
      disconnect();check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("SELECT id,n,u,v,extra FROM converted ORDER BY id");check_equal(execute(),ORM_STATUS_OK);count_is(7);
      for(uint64_t i=0;i<7;++i) {
        integer_at(i,0,(int64_t)i);integer_at(i,1,i<2||i>4?1:3);unsigned_at(i,2,UINT64_MAX);
        double_at(i,3,7.0);integer_at(i,4,-3);
      }
      raw_query("SHOW COLUMNS FROM converted");check_equal(execute(),ORM_STATUS_OK);count_is(5);
      text_at(1,4,"1");text_at(2,4,"18446744073709551615");text_at(3,4,"-3");text_at(4,4,"7");
      raw_query("EXPLAIN SELECT id FROM converted WHERE n=3");check_equal(execute(),ORM_STATUS_OK);text_at(0,5,"n_key");
      raw_query("SELECT id FROM converted WHERE n=3 ORDER BY id");check_equal(execute(),ORM_STATUS_OK);count_is(3);
      for(uint64_t i=0;i<3;++i)integer_at(i,0,(int64_t)i+2);
    }
    it("keeps prior rows and defaults after invalid CREATE ALTER and ADD conversions") {
      seed();raw_query("ALTER TABLE items ALTER score SET DEFAULT 7");command(0);
      const char *sql[]={"CREATE TABLE bad(id BIGINT PRIMARY KEY,n BIGINT DEFAULT '2.5junk')",
        "ALTER TABLE items ALTER score SET DEFAULT '2.5junk'",
        "ALTER TABLE items ALTER score SET DEFAULT '9223372036854775807.5'",
        "ALTER TABLE items ADD extra BIGINT UNSIGNED NOT NULL DEFAULT '-0.5'"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        raw_query(sql[i]);check_equal(execute(),i<2?ORM_STATUS_TYPE_ERROR:ORM_STATUS_OUT_OF_RANGE);check_null(result);
        raw_query("SHOW COLUMNS FROM items");check_equal(execute(),ORM_STATUS_OK);count_is(2);text_at(1,4,"7");
      }
      raw_query("SHOW TABLES");check_equal(execute(),ORM_STATUS_OK);count_is(1);text_at(0,0,"items");
      raw_query("INSERT INTO items(id) VALUES(4)");command(1);
      disconnect();check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("SELECT score FROM items ORDER BY id");check_equal(execute(),ORM_STATUS_OK);count_is(4);
      integer_at(0,0,10);integer_at(1,0,20);integer_at(2,0,30);integer_at(3,0,7);
    }
    it("rolls back converted defaults backfilled columns and later rows at one savepoint") {
      seed();raw_query("ALTER TABLE items ALTER score SET DEFAULT '2.5'");command(0);
      check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      check_equal(orm_transaction_savepoint(transaction,orm_view("before_convert"),&error),ORM_STATUS_OK);
      raw_query("ALTER TABLE items ALTER score SET DEFAULT '12.9e3'");command(0);
      raw_query("ALTER TABLE items ADD extra DOUBLE NOT NULL DEFAULT '7'");command(0);
      raw_query("INSERT INTO items(id) VALUES(4)");command(1);
      raw_query("SELECT score,extra FROM items WHERE id=4");check_equal(execute(),ORM_STATUS_OK);integer_at(0,0,12900);double_at(0,1,7.0);
      orm_result_destroy(result);result=NULL;orm_query_destroy(query);query=NULL;
      check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("before_convert"),&error),ORM_STATUS_OK);
      raw_query("SHOW COLUMNS FROM items");check_equal(execute(),ORM_STATUS_OK);count_is(2);text_at(1,4,"3");
      raw_query("INSERT INTO items(id) VALUES(4)");command(1);
      orm_result_destroy(result);result=NULL;orm_query_destroy(query);query=NULL;
      check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);orm_transaction_destroy(transaction);transaction=NULL;
      disconnect();check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("SELECT score FROM items ORDER BY id");check_equal(execute(),ORM_STATUS_OK);count_is(4);integer_at(3,0,3);
    }
    it("uses the same decimal conversion for defaults raw writes and structured assignments") {
      raw_query("CREATE TABLE assignments(id BIGINT PRIMARY KEY,n BIGINT DEFAULT '12.9e3',v DOUBLE DEFAULT '7')");command(0);
      raw_query("INSERT INTO assignments(id) VALUES(1)");command(1);
      raw_query("INSERT INTO assignments VALUES(2,'12.9e3','7')");command(1);
      structured(STRUCT_INSERT,"assignments");assign("id",orm_i64(3));assign("n",orm_text("12.9e3"));assign("v",orm_text("7"));command(1);
      raw_query("SELECT n,v FROM assignments ORDER BY id");check_equal(execute(),ORM_STATUS_OK);count_is(3);
      for(uint64_t i=0;i<3;++i){integer_at(i,0,12900);double_at(i,1,7.0);}
      raw_query("UPDATE assignments SET n='2.5',v='999999999999999999999' WHERE id=2");command(1);
      raw_query("SELECT n,v FROM assignments WHERE id=2");check_equal(execute(),ORM_STATUS_OK);integer_at(0,0,3);double_at(0,1,1e21);
      raw_query("INSERT INTO assignments VALUES(4,'2.5','7'),(5,'2.5junk','7')");check_equal(execute(),ORM_STATUS_TYPE_ERROR);check_null(result);
      raw_query("SELECT id FROM assignments ORDER BY id");check_equal(execute(),ORM_STATUS_OK);count_is(3);
    }
  }

  group("decimal-place rounding through the native plugin") {
    it("combines canonical grouping window correlated recursive and set expressions") {
      seed();
      raw_query("SELECT ROUND(score) AS n,COUNT(*) AS c FROM items GROUP BY ROUND(score,0) ORDER BY n");
      check_equal(execute(),ORM_STATUS_OK); count_is(3);
      for(uint64_t i=0;i<3;++i) { integer_at(i,0,(int64_t)(i+1)*10); integer_at(i,1,1); }
      raw_query("SELECT ROUND(score+5,-1) AS n,COUNT(*) AS c FROM items GROUP BY ROUND(score+5,-1) ORDER BY n");
      check_equal(execute(),ORM_STATUS_OK); count_is(3);
      for(uint64_t i=0;i<3;++i) { integer_at(i,0,(int64_t)(i+2)*10); integer_at(i,1,1); }
      raw_query("SELECT ROUND(AVG(score+0.0),-1) AS n FROM items"); check_equal(execute(),ORM_STATUS_OK); double_at(0,0,20.0);
      raw_query("SELECT ROUND(ROW_NUMBER() OVER(ORDER BY id),-1) AS n,"
        "TRUNCATE(SUM(score+0.0) OVER(ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW),-1) AS s FROM items ORDER BY id");
      check_equal(execute(),ORM_STATUS_OK); count_is(3); const double sums[]={10.0,30.0,60.0};
      for(uint64_t i=0;i<3;++i) { integer_at(i,0,0); double_at(i,1,sums[i]); }
      raw_query("SELECT ROUND((SELECT b.score FROM items b WHERE b.id=a.id),-1) AS n FROM items a ORDER BY a.id");
      check_equal(execute(),ORM_STATUS_OK); count_is(3); for(uint64_t i=0;i<3;++i) integer_at(i,0,(int64_t)(i+1)*10);
      raw_query("SELECT ROUND(25,-1) AS n UNION SELECT ROUND(25e0,-1) ORDER BY n");
      check_equal(execute(),ORM_STATUS_OK); count_is(2); double_at(0,0,20.0); double_at(1,0,30.0);
      raw_query("SELECT ROUND(2.5e0) AS n UNION SELECT TRUNCATE(2.9e0,0)");
      check_equal(execute(),ORM_STATUS_OK); count_is(1); double_at(0,0,2.0);
      recursive_connection("4");
      raw_query("WITH RECURSIVE c(n) AS(SELECT 5 UNION ALL SELECT n+10 FROM c WHERE n<25) SELECT ROUND(n,-1) AS r FROM c ORDER BY r");
      check_equal(execute(),ORM_STATUS_OK); count_is(3); for(uint64_t i=0;i<3;++i) integer_at(i,0,(int64_t)(i+1)*10);
    }
    it("returns typed parameter values and rejects invalid precision even for unexecuted rows") {
      raw_query("SELECT ROUND(?) AS r,ROUND(?,?) AS p,TRUNCATE(?,?) AS t,ROUND(NULL,?) AS n");
      parameter(orm_f64(2.5)); parameter(orm_f64(1.375)); parameter(orm_i64(2));
      parameter(orm_f64(-1.375)); parameter(orm_i64(2)); parameter(orm_i64(7));
      check_equal(execute(),ORM_STATUS_OK); count_is(1); double_at(0,0,2.0); double_at(0,1,1.38); double_at(0,2,-1.37);
      uint8_t null=0; check_equal(orm_result_is_null(result,0,3,&null,&error),ORM_STATUS_OK); check_equal(null,1);
      raw_query("SELECT ROUND(?,?)"); parameter(orm_i64(25)); parameter(orm_u64(UINT64_MAX));
      check_equal(execute(),ORM_STATUS_OK); integer_at(0,0,25);
      const struct { const char *sql; orm_status_t expected; } rejected[]={
        {"SELECT ROUND(1,2,3) LIMIT 0",ORM_STATUS_SQL_ERROR},
        {"SELECT TRUNCATE(1) LIMIT 0",ORM_STATUS_SQL_ERROR},
        {"SELECT CASE WHEN TRUE THEN 1 ELSE ROUND(1,1.5) END",ORM_STATUS_UNSUPPORTED},
        {"SELECT ROUND(NULL,TRUE) LIMIT 0",ORM_STATUS_UNSUPPORTED},
        {"SELECT ROUND(1) OVER()",ORM_STATUS_SQL_ERROR}
      };
      for(size_t i=0;i<sizeof(rejected)/sizeof(rejected[0]);++i) { raw_query(rejected[i].sql); check_equal(execute(),rejected[i].expected); check_null(result); }
    }
    it("persists folded rounding defaults and backfill while restoring savepoint metadata") {
      raw_query("CREATE TABLE rounded(id BIGINT PRIMARY KEY,n BIGINT DEFAULT(ROUND(25,-1)),"
        "v DOUBLE DEFAULT(TRUNCATE(-1.375e0,2)),u BIGINT UNSIGNED DEFAULT(TRUNCATE(18446744073709551615,-1)))"); command(0);
      raw_query("INSERT INTO rounded(id) VALUES(1)"); command(1);
      raw_query("ALTER TABLE rounded ALTER n SET DEFAULT(TRUNCATE(-25,-1))"); command(0);
      raw_query("INSERT INTO rounded(id) VALUES(2)"); command(1);
      raw_query("ALTER TABLE rounded ADD c DOUBLE NOT NULL DEFAULT(ROUND(3.5e0))"); command(0);
      check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      raw_query("SAVEPOINT rounding_default"); command(0);
      raw_query("ALTER TABLE rounded ALTER n SET DEFAULT(ROUND(9223372036854775807,-1))");
      check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
      raw_query("ALTER TABLE rounded ALTER v SET DEFAULT(ROUND(3.5e0))"); command(0);
      raw_query("INSERT INTO rounded(id) VALUES(3)"); command(1);
      raw_query("ROLLBACK WORK TO rounding_default"); command(0);
      check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
      orm_transaction_destroy(transaction); transaction=NULL; disconnect();
      check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("SELECT n,v,u,c FROM rounded ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(2);
      integer_at(0,0,30); integer_at(1,0,-20);
      for(uint64_t i=0;i<2;++i) { double_at(i,1,-1.37); unsigned_at(i,2,UINT64_C(18446744073709551610)); double_at(i,3,4.0); }
      raw_query("INSERT INTO rounded(id) VALUES(4)"); command(1);
      raw_query("SELECT n,v FROM rounded WHERE id=4"); check_equal(execute(),ORM_STATUS_OK); integer_at(0,0,-20); double_at(0,1,-1.37);
      raw_query("UPDATE rounded SET v=ROUND(v,1)"); command(3);
      disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("SELECT v FROM rounded ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(3);
      for(uint64_t i=0;i<3;++i) double_at(i,0,-1.4);
    }
    it("rolls back rounding overflow in every write form and keeps prior transaction writes") {
      seed(); raw_query("UPDATE items SET score=15 WHERE id=1"); command(1);
      raw_query("UPDATE items SET score=9223372036854775807 WHERE id=3"); command(1);
      check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      raw_query("INSERT INTO items VALUES(9,90)"); command(1);
      const char *sql[]={"INSERT INTO items VALUES(10,ROUND(15,-1)),(11,ROUND(9223372036854775807,-1))",
        "INSERT IGNORE INTO items VALUES(10,ROUND(15,-1)),(11,ROUND(9223372036854775807,-1))",
        "UPDATE items SET score=ROUND(score,-1) ORDER BY id",
        "UPDATE IGNORE items SET score=ROUND(score,-1) ORDER BY id",
        "REPLACE INTO items VALUES(1,ROUND(9223372036854775807,-1))",
        "INSERT INTO items VALUES(1,1) ON DUPLICATE KEY UPDATE score=ROUND(9223372036854775807,-1)",
        "INSERT INTO items SELECT id+100,ROUND(score,-1) FROM items"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) { raw_query(sql[i]); check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result); }
      raw_query("SELECT id,score FROM items ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(4);
      integer_at(0,1,15); integer_at(1,1,20); integer_at(2,1,INT64_MAX); integer_at(3,0,9); integer_at(3,1,90);
      check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
      orm_transaction_destroy(transaction); transaction=NULL;
      raw_query("UPDATE items SET score=TRUNCATE(score,-1) ORDER BY id"); command(2);
      disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("SELECT score FROM items ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(3);
      integer_at(0,0,10); integer_at(1,0,20); integer_at(2,0,INT64_C(9223372036854775800));
    }
  }

  group("numeric scalar functions through the native plugin") {
    it("returns typed parameter results and streams numeric computed rows") {
      raw_query("SELECT ABS(?) AS a,SIGN(?) AS s,FLOOR(?) AS f,CEILING(?) AS c,ABS(NULL) AS n");
      parameter(orm_i64(-7)); parameter(orm_u64(UINT64_MAX)); parameter(orm_f64(-1.25)); parameter(orm_f64(1.25));
      check_equal(execute(),ORM_STATUS_OK); count_is(1);
      integer_at(0,0,7); integer_at(0,1,1); double_at(0,2,-2.0); double_at(0,3,2.0);
      uint8_t missing=0; check_equal(orm_result_is_null(result,0,4,&missing,&error),ORM_STATUS_OK); check_equal(missing,1);
      seed(); raw_query("SELECT ABS(-id) AS id,SIGN(score) AS score FROM items ORDER BY id");
      open_rows(); row(1,1); row(2,1); row(3,1); end_rows();
    }
    it("persists CREATE ALTER and ADD folded function defaults across reconnect") {
      raw_query("CREATE TABLE numbers(id BIGINT PRIMARY KEY,v DOUBLE DEFAULT (FLOOR(-1.25)),s BIGINT DEFAULT (SIGN(-1.25)))"); command(0);
      raw_query("INSERT INTO numbers(id) VALUES(1)"); command(1);
      raw_query("ALTER TABLE numbers ADD u BIGINT UNSIGNED NOT NULL DEFAULT (ABS(18446744073709551615))"); command(0);
      raw_query("ALTER TABLE numbers ALTER v SET DEFAULT (CEILING(1.25))"); command(0);
      disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("INSERT INTO numbers(id) VALUES(2)"); command(1);
      raw_query("SELECT id,v,s,u FROM numbers ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(2);
      double_at(0,1,-2.0); double_at(1,1,2.0);
      for(uint64_t i=0;i<2;++i) { integer_at(i,2,-1); unsigned_at(i,3,UINT64_MAX); }
      raw_query("ALTER TABLE numbers ALTER s SET DEFAULT (ABS(-9223372036854775808))");
      check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
      raw_query("INSERT INTO numbers(id) VALUES(3)"); command(1);
      raw_query("SELECT s FROM numbers WHERE id=3"); check_equal(execute(),ORM_STATUS_OK); integer_at(0,0,-1);
    }
    it("rolls back function writes with savepoints and leaves overflow statements atomic") {
      seed();
      check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      check_equal(orm_transaction_savepoint(transaction,orm_view("before_functions"),&error),ORM_STATUS_OK);
      raw_query("UPDATE items SET score=ABS(-score) + SIGN(score) WHERE id<=2"); command(2);
      raw_query("INSERT INTO items SELECT id+3,ABS(-score) FROM items"); command(3);
      raw_query("SELECT score FROM items WHERE id<=2 ORDER BY id"); check_equal(execute(),ORM_STATUS_OK);
      integer_at(0,0,11); integer_at(1,0,21);
      orm_result_destroy(result); result=NULL; orm_query_destroy(query); query=NULL;
      check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("before_functions"),&error),ORM_STATUS_OK);
      raw_query("SELECT score FROM items ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(3);
      integer_at(0,0,10); integer_at(1,0,20); integer_at(2,0,30);
      raw_query("INSERT INTO items VALUES(4,ABS(-4)),(5,ABS(-9223372036854775808))");
      check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
      raw_query("SELECT id FROM items ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(3);
      raw_query("UPDATE items SET score=SIGN(-id) WHERE id=1"); command(1);
      raw_query("DELETE FROM items WHERE ABS(score)=1"); command(1);
      check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
    }
  }
  group("common JOIN columns through the native plugin") {
    it("returns merged columns and original qualified NULL values with the documented star order") {
      seed(); const char *sql[]={
        "SELECT * FROM items a LEFT JOIN (SELECT 2 AS id,200 AS y UNION ALL SELECT 4,400) b USING(id) ORDER BY id",
        "SELECT * FROM items a NATURAL RIGHT JOIN (SELECT 2 AS id,200 AS y UNION ALL SELECT 4,400) b ORDER BY id"};
      for(size_t mode=0;mode<sizeof(sql)/sizeof(sql[0]);++mode) {
        raw_query(sql[mode]); check_equal(execute(),ORM_STATUS_OK); count_is(mode?2:3);
        for(size_t i=0;i<(mode?2u:3u);++i) integer_at(i,0,mode?(int64_t)(i+1)*2:(int64_t)i+1);
        uint8_t missing=0; check_equal(orm_result_is_null(result,mode?1:0,2,&missing,&error),ORM_STATUS_OK); check_equal(missing,1);
        integer_at(mode?0:1,1,mode?200:20); integer_at(mode?0:1,2,mode?20:200);
      }
      raw_query("SELECT id,a.id AS aid,b.id AS bid FROM items a RIGHT JOIN (SELECT 4 AS id) b USING(id)");
      check_equal(execute(),ORM_STATUS_OK); count_is(1); integer_at(0,0,4); integer_at(0,2,4);
      uint8_t missing=0; check_equal(orm_result_is_null(result,0,1,&missing,&error),ORM_STATUS_OK); check_equal(missing,1);
    }
    it("streams common names into typed row flows and preserves them across reopen") {
      seed(); raw_query("SELECT id,score FROM items a JOIN (SELECT id FROM items) b USING(id) ORDER BY id");
      open_rows(); row(1,10); row(2,20); row(3,30); end_rows();
      disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("WITH c AS (SELECT id FROM items) SELECT id,score FROM items a NATURAL JOIN c b ORDER BY id");
      open_rows(); row(1,10); row(2,20); row(3,30); end_rows();
    }
    it("keeps INSERT SELECT common joins in the transaction and restores their writes on savepoint rollback") {
      seed(); raw_query("CREATE TABLE copied(id BIGINT PRIMARY KEY,score BIGINT)"); command(0);
      check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      check_equal(orm_transaction_savepoint(transaction,orm_view("before_common"),&error),ORM_STATUS_OK);
      raw_query("INSERT INTO copied SELECT id,score FROM items a JOIN (SELECT id FROM items) b USING(id)"); command(3);
      raw_query("SELECT id,score FROM copied a NATURAL JOIN items b ORDER BY id"); open_rows(); row(1,10); row(2,20); row(3,30); end_rows();
      check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("before_common"),&error),ORM_STATUS_OK);
      raw_query("SELECT id FROM copied"); check_equal(execute(),ORM_STATUS_OK); count_is(0);
      check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
    }
  }
  group("ALTER defaults through the native plugin") {
    it("preserves old values and applies changed defaults to every later write form") {
      seed(); raw_query("ALTER TABLE items ALTER COLUMN score SET DEFAULT (2+5)"); command(0);
      raw_query("SELECT score FROM items ORDER BY id"); check_equal(execute(), ORM_STATUS_OK);
      count_is(3); integer_at(0,0,10); integer_at(1,0,20); integer_at(2,0,30);
      raw_query("INSERT INTO items(id) VALUES(4)"); command(1);
      raw_query("INSERT INTO items SET id=5"); command(1);
      raw_query("INSERT INTO items(id) SELECT 6"); command(1);
      raw_query("REPLACE INTO items(id) VALUES(7)"); command(1);
      raw_query("INSERT INTO items VALUES(1,99) ON DUPLICATE KEY UPDATE score=DEFAULT"); command(2);
      raw_query("UPDATE items SET score=DEFAULT WHERE id=2"); command(1);
      raw_query("SHOW COLUMNS FROM items"); check_equal(execute(), ORM_STATUS_OK); text_at(1,4,"7");
      raw_query("SELECT score FROM items WHERE id<>3 ORDER BY id"); check_equal(execute(), ORM_STATUS_OK); count_is(6);
      for (uint64_t i=0; i<6; ++i) integer_at(i,0,7);
      raw_query("ALTER TABLE items ALTER score DROP DEFAULT"); command(0);
      raw_query("INSERT INTO items(id) VALUES(8)"); command(1);
      raw_query("SELECT id FROM items WHERE score IS NULL"); check_equal(execute(), ORM_STATUS_OK); count_is(1); integer_at(0,0,8);
      raw_query("SELECT score FROM items WHERE id=4"); check_equal(execute(), ORM_STATUS_OK); integer_at(0,0,7);
      raw_query("SHOW CREATE TABLE items"); check_equal(execute(), ORM_STATUS_OK);
      text_at(0,1,"CREATE TABLE `items` (\n  `id` bigint NOT NULL,\n  `score` bigint NULL,\n  PRIMARY KEY (`id`)\n)");
    }
    it("fills positioned NOT NULL columns with typed defaults and preserves indexed access after reopen") {
      seed(); raw_query("CREATE UNIQUE INDEX score_key ON items(score)"); command(0);
      raw_query("ALTER TABLE items ADD u BIGINT UNSIGNED NOT NULL DEFAULT 18446744073709551615 FIRST"); command(0);
      raw_query("ALTER TABLE items ADD v DOUBLE NOT NULL DEFAULT (1.25*2.0) AFTER id"); command(0);
      raw_query("ALTER TABLE items ADD n BIGINT DEFAULT NULL"); command(0);
      raw_query("INSERT INTO items(id,score) VALUES(4,40)"); command(1);
      disconnect(); check_equal(connect_profile("relational","false",NULL,0), ORM_STATUS_OK);
      raw_query("SELECT id,u,v FROM items ORDER BY id"); check_equal(execute(), ORM_STATUS_OK); count_is(4);
      for (uint64_t i=0; i<4; ++i) { integer_at(i,0,(int64_t)i+1); unsigned_at(i,1,UINT64_MAX); double_at(i,2,2.5); }
      raw_query("SELECT id FROM items WHERE n IS NULL ORDER BY id"); check_equal(execute(), ORM_STATUS_OK); count_is(4);
      raw_query("EXPLAIN SELECT id FROM items WHERE score=20"); check_equal(execute(), ORM_STATUS_OK); text_at(0,5,"score_key");
      raw_query("INSERT INTO items(id,score) VALUES(5,20)"); check_equal(execute(), ORM_STATUS_CONSTRAINT); check_null(result);
      raw_query("ALTER TABLE items ALTER COLUMN v SET DEFAULT (3.0*2.0)"); command(0);
      raw_query("UPDATE items SET v=DEFAULT WHERE score=20"); command(1);
      raw_query("SELECT id,v FROM items WHERE score=20"); check_equal(execute(), ORM_STATUS_OK); integer_at(0,0,2); double_at(0,1,6.0);
    }
    it("restores added columns changed defaults and inserted rows together on savepoint rollback") {
      seed(); raw_query("ALTER TABLE items ALTER score SET DEFAULT 5"); command(0);
      check_equal(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE, &transaction, &error), ORM_STATUS_OK);
      check_equal(orm_transaction_savepoint(transaction, orm_view("defaults_before"), &error), ORM_STATUS_OK);
      raw_query("ALTER TABLE items ALTER score DROP DEFAULT"); command(0);
      raw_query("ALTER TABLE items ALTER id SET DEFAULT 8"); command(0);
      raw_query("ALTER TABLE items ADD extra BIGINT NOT NULL DEFAULT -7 AFTER score"); command(0);
      raw_query("INSERT INTO items() VALUES()"); command(1);
      raw_query("SELECT id,extra FROM items ORDER BY id"); check_equal(execute(), ORM_STATUS_OK); count_is(4); integer_at(3,0,8); integer_at(3,1,-7);
      orm_result_destroy(result); result=NULL; orm_query_destroy(query); query=NULL;
      check_equal(orm_transaction_rollback_to_savepoint(transaction, orm_view("defaults_before"), &error), ORM_STATUS_OK);
      raw_query("SHOW COLUMNS FROM items"); check_equal(execute(), ORM_STATUS_OK); count_is(2); text_at(1,4,"5");
      raw_query("INSERT INTO items(id) VALUES(4)"); command(1);
      raw_query("SELECT score FROM items WHERE id=4"); check_equal(execute(), ORM_STATUS_OK); integer_at(0,0,5);
      orm_result_destroy(result); result=NULL; orm_query_destroy(query); query=NULL;
      check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
      disconnect(); check_equal(connect_profile("relational","false",NULL,0), ORM_STATUS_OK);
      raw_query("SHOW COLUMNS FROM items"); check_equal(execute(), ORM_STATUS_OK); count_is(2); text_at(1,4,"5");
      raw_query("SELECT id FROM items ORDER BY id"); check_equal(execute(), ORM_STATUS_OK); count_is(4); integer_at(3,0,4);
    }
    it("rejects invalid default changes atomically and enforces strict missing NOT NULL values after DROP") {
      raw_query("CREATE TABLE strict_values(id BIGINT PRIMARY KEY,n BIGINT NOT NULL DEFAULT 7)"); command(0);
      raw_query("INSERT INTO strict_values(id) VALUES(1)"); command(1);
      const char *invalid[]={"ALTER TABLE strict_values ALTER n SET DEFAULT NULL",
        "ALTER TABLE strict_values ALTER n SET DEFAULT '1.5junk'", "ALTER TABLE strict_values ALTER n SET DEFAULT (id+1)",
        "ALTER TABLE strict_values ALTER n SET DEFAULT (9223372036854775807+1)",
        "ALTER TABLE strict_values ADD extra BIGINT NOT NULL DEFAULT NULL",
        "ALTER TABLE strict_values ALTER missing DROP DEFAULT"};
      const orm_status_t statuses[]={ORM_STATUS_SQL_ERROR,ORM_STATUS_TYPE_ERROR,ORM_STATUS_SQL_ERROR,
        ORM_STATUS_LIMIT_EXCEEDED,ORM_STATUS_SQL_ERROR,ORM_STATUS_SQL_ERROR};
      for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        raw_query(invalid[i]); check_equal(execute(),statuses[i]); check_null(result);
        raw_query("SHOW COLUMNS FROM strict_values"); check_equal(execute(),ORM_STATUS_OK); count_is(2); text_at(1,4,"7");
      }
      raw_query("ALTER TABLE strict_values ALTER n DROP DEFAULT"); command(0);
      raw_query("INSERT INTO strict_values(id) VALUES(2)"); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
      raw_query("SELECT id,n FROM strict_values"); check_equal(execute(),ORM_STATUS_OK); count_is(1); integer_at(0,0,1); integer_at(0,1,7);
      raw_query("ALTER TABLE strict_values ALTER n SET DEFAULT -9"); command(0);
      raw_query("INSERT INTO strict_values(id) VALUES(2)"); command(1);
      raw_query("SELECT n FROM strict_values WHERE id=2"); check_equal(execute(),ORM_STATUS_OK); integer_at(0,0,-9);
    }
    it("keeps an active row publisher valid when ALTER default is refused as BUSY") {
      seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      raw_query("SELECT id,score FROM items ORDER BY id"); open_rows();
      orm_query_t *alter=NULL;
      check_equal(orm_raw(connection,orm_view("ALTER TABLE items ALTER score SET DEFAULT 7"),&alter,&error),ORM_STATUS_OK);
      orm_result_t *failed=NULL; check_equal(orm_query_execute_in_transaction(alter,transaction,&failed,&error),ORM_STATUS_BUSY);
      check_null(failed); orm_query_destroy(alter); row(1,10); row(2,20); row(3,30); end_rows();
      raw_query("ALTER TABLE items ALTER score SET DEFAULT 7"); command(0);
      orm_query_destroy(query); query=NULL; check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
    }
  }
  group("common numeric set results through the native plugin") {
    it("materializes all six set modes after real conversion and preserves NULL equality") {
      const char *sql[]={
        "(SELECT ? AS n UNION ALL SELECT ?) UNION ALL SELECT ?",
        "(SELECT ? AS n UNION ALL SELECT ?) UNION SELECT ?",
        "(SELECT ? AS n UNION ALL SELECT ?) INTERSECT ALL SELECT ?",
        "(SELECT ? AS n UNION ALL SELECT ?) INTERSECT SELECT ?",
        "(SELECT ? AS n UNION ALL SELECT ?) EXCEPT ALL SELECT ?",
        "(SELECT ? AS n UNION ALL SELECT ?) EXCEPT SELECT ?"};
      const uint64_t counts[]={3,1,1,1,1,0};
      for(size_t mode=0;mode<sizeof(sql)/sizeof(sql[0]);++mode) {
        raw_query(sql[mode]); parameter(orm_i64(INT64_C(9007199254740992)));
        parameter(orm_i64(INT64_C(9007199254740993))); parameter(orm_f64(9007199254740992.0));
        check_equal(execute(),ORM_STATUS_OK); count_is(counts[mode]);
        for(uint64_t row=0;row<counts[mode];++row) double_at(row,0,9007199254740992.0);
      }
      raw_query("SELECT NULL AS n UNION SELECT 0 UNION SELECT ?"); parameter(orm_f64(-0.0));
      check_equal(execute(),ORM_STATUS_OK); count_is(2);
      orm_value_kind_t kind; check_equal(orm_result_value_kind(result,0,0,&kind,&error),ORM_STATUS_OK);
      check_equal(kind,ORM_VALUE_NULL); double_at(1,0,0.0);
    }
    it("applies late real chain types before DISTINCT pagination and CTE membership") {
      raw_query("SELECT 9007199254740992 AS n UNION SELECT 9007199254740993 UNION ALL SELECT ? ORDER BY n LIMIT 1 OFFSET 1");
      parameter(orm_f64(9007199254740992.0)); check_equal(execute(),ORM_STATUS_OK); count_is(1); double_at(0,0,9007199254740992.0);
      raw_query("SELECT EXISTS(SELECT 9007199254740992 UNION SELECT 9007199254740993 UNION ALL SELECT ? LIMIT 1 OFFSET 2) AS n");
      parameter(orm_f64(9007199254740992.0)); check_equal(execute(),ORM_STATUS_OK); count_is(1);
      uint8_t exists=1; check_equal(orm_result_get_boolean(result,0,0,&exists,&error),ORM_STATUS_OK); check_false(exists);
      raw_query("WITH c(n) AS(SELECT 9007199254740992 INTERSECT SELECT 9007199254740993 INTERSECT SELECT ?) SELECT n FROM c");
      parameter(orm_f64(9007199254740992.0)); check_equal(execute(),ORM_STATUS_OK); count_is(1); double_at(0,0,9007199254740992.0);
      raw_query("SELECT ? AS n UNION ALL SELECT ? UNION ALL SELECT ?");
      parameter(orm_i64(-1)); parameter(orm_u64(UINT64_MAX)); parameter(orm_f64(0.5));
      check_equal(execute(),ORM_STATUS_OK); count_is(3);
      double_at(0,0,-1.0); double_at(1,0,18446744073709551616.0); double_at(2,0,0.5);
      raw_query("SELECT ? AS n UNION ALL (SELECT 9007199254740992 INTERSECT SELECT 9007199254740993)");
      parameter(orm_f64(0.5)); check_equal(execute(),ORM_STATUS_OK); count_is(2);
      double_at(0,0,0.5); double_at(1,0,9007199254740992.0);
      raw_query("(SELECT 9007199254740992 AS n UNION SELECT 9007199254740993 LIMIT 2) UNION ALL SELECT ?");
      parameter(orm_f64(0.5)); check_equal(execute(),ORM_STATUS_OK); count_is(2);
      double_at(0,0,9007199254740992.0); double_at(1,0,0.5);
    }
    it("persists converted INSERT SELECT rows and rolls back a later BIGINT range failure") {
      seed(); raw_query("CREATE TABLE reals(id BIGINT PRIMARY KEY,n DOUBLE)"); command(0);
      raw_query("INSERT INTO reals SELECT 1,7 UNION ALL SELECT 2,18446744073709551615 UNION ALL SELECT 3,?");
      parameter(orm_f64(0.5)); command(3);
      disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("SELECT id,n FROM reals ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(3);
      double_at(0,1,7.0); double_at(1,1,18446744073709551616.0); double_at(2,1,0.5);
      raw_query("INSERT INTO items SELECT 4,10 UNION ALL SELECT 5,18446744073709551615 UNION ALL SELECT 6,?");
      parameter(orm_f64(0.5)); check_equal(execute(),ORM_STATUS_OUT_OF_RANGE); check_null(result);
      raw_query("SELECT id,score FROM items ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(3);
      for(uint64_t row=0;row<3;++row) { integer_at(row,0,(int64_t)row+1); integer_at(row,1,((int64_t)row+1)*10); }
    }
  }
  group("INTERSECT and EXCEPT through the native plugin") {
    it("materializes numeric multiset counts and DISTINCT membership") {
      seed(); raw_query("INSERT INTO items VALUES(4,10),(5,10)"); command(2);
      const char *sql[]={"SELECT score FROM items INTERSECT ALL SELECT score FROM items WHERE id=1",
        "SELECT score FROM items INTERSECT DISTINCT SELECT score FROM items WHERE id=1",
        "SELECT score FROM items WHERE score=10 EXCEPT ALL SELECT score FROM items WHERE id=1",
        "SELECT score FROM items WHERE score=10 EXCEPT SELECT score FROM items WHERE id=1"};
      const uint64_t counts[]={1,1,2,0};
      for(size_t mode=0;mode<sizeof(sql)/sizeof(sql[0]);++mode) {
        raw_query(sql[mode]); check_equal(execute(),ORM_STATUS_OK); count_is(counts[mode]);
        for(uint64_t i=0;i<counts[mode];++i) integer_at(i,0,10);
      }
    }
    it("streams complete EXCEPT tuples and supports cancellation and connection reuse") {
      seed(); raw_query("SELECT id,score FROM items EXCEPT ALL SELECT id,score FROM items WHERE id=2 ORDER BY id");
      open_rows(); row(1,10); row(3,30); end_rows();
      raw_query("SELECT id,score FROM items INTERSECT SELECT id,score FROM items");
      open_rows(); cflow_publisher_cancel(&publisher); cflow_publisher_cancel(&publisher);
      cflow_publisher_destroy(&publisher); publisher=(cflow_publisher){0};
      raw_query("SELECT COUNT(*) AS n FROM items"); check_equal(execute(),ORM_STATUS_OK); integer_at(0,0,3);
    }
    it("uses the explicit transaction snapshot and restores set reads on savepoint rollback") {
      seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      check_equal(orm_transaction_savepoint(transaction,orm_view("before_set"),&error),ORM_STATUS_OK);
      raw_query("INSERT INTO items VALUES(4,40)"); command(1);
      raw_query("SELECT id FROM items EXCEPT SELECT 2 ORDER BY id"); check_equal(execute(),ORM_STATUS_OK);
      count_is(3); integer_at(2,0,4);
      orm_result_destroy(result); result=NULL; orm_query_destroy(query); query=NULL;
      check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("before_set"),&error),ORM_STATUS_OK);
      raw_query("SELECT id FROM items EXCEPT SELECT 2 ORDER BY id"); check_equal(execute(),ORM_STATUS_OK);
      count_is(2); integer_at(0,0,1); integer_at(1,0,3);
      orm_result_destroy(result); result=NULL; orm_query_destroy(query); query=NULL;
      check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
    }
    it("executes correlated EXISTS and set CTE derived sources") {
      seed(); raw_query("WITH c(n) AS (SELECT id FROM items EXCEPT SELECT 3) SELECT d.n FROM (SELECT n FROM c INTERSECT SELECT 2) d");
      check_equal(execute(),ORM_STATUS_OK); count_is(1); integer_at(0,0,2);
      raw_query("SELECT o.id FROM items o WHERE EXISTS(SELECT o.id INTERSECT SELECT ?) ORDER BY o.id"); parameter(orm_i64(2));
      check_equal(execute(),ORM_STATUS_OK); count_is(1); integer_at(0,0,2);
    }
    it("uses set IN membership in reads UPDATE and DELETE with NULL semantics") {
      seed(); raw_query("SELECT id FROM items WHERE id IN (SELECT NULL UNION ALL SELECT 2 EXCEPT SELECT NULL)");
      check_equal(execute(),ORM_STATUS_OK); count_is(1); integer_at(0,0,2);
      raw_query("SELECT id FROM items WHERE id NOT IN (SELECT NULL INTERSECT SELECT NULL)");
      check_equal(execute(),ORM_STATUS_OK); count_is(0);
      raw_query("UPDATE items o SET score=(SELECT o.score INTERSECT SELECT 20) WHERE o.id IN (SELECT 2 INTERSECT SELECT 2)"); command(0);
      raw_query("DELETE FROM items WHERE id IN (SELECT 1 EXCEPT SELECT 2)"); command(1);
      raw_query("SELECT id,score FROM items ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(2);
      integer_at(0,0,2); integer_at(0,1,20); integer_at(1,0,3); integer_at(1,1,30);
    }
    it("explains set results without evaluating failing operand expressions") {
      seed(); raw_query("EXPLAIN SELECT id+9223372036854775807 AS n FROM items EXCEPT ALL SELECT 2");
      check_equal(execute(),ORM_STATUS_OK); count_is(3);
      text_at(0,1,"PRIMARY"); text_at(1,1,"EXCEPT"); text_at(2,1,"EXCEPT RESULT");
      vstr extra={0}; check_equal(orm_result_get_text(result,2,11,&extra,&error),ORM_STATUS_OK);
      check_greater_equal(extra.len,strlen("EXCEPT ALL"));
      check_equal(memcmp(extra.data,"EXCEPT ALL",strlen("EXCEPT ALL")),0);
    }
    it("publishes no materialized result prefix on operand failure and permits recovery") {
      seed(); raw_query("SELECT id FROM items INTERSECT ALL SELECT id+9223372036854775807 AS n FROM items");
      check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
      raw_query("SELECT id FROM items EXCEPT SELECT 2 ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(2);
      integer_at(0,0,1); integer_at(1,0,3);
    }
    it("executes a set seed with UNION recursion and rejects recursive set operators") {
      seed(); recursive_connection("8");
      raw_query("WITH RECURSIVE c(n) AS ((SELECT 1 INTERSECT SELECT 1) UNION ALL SELECT n+1 FROM c WHERE n<3) SELECT n FROM c EXCEPT SELECT 2 ORDER BY n");
      check_equal(execute(),ORM_STATUS_OK); count_is(2); integer_at(0,0,1); integer_at(1,0,3);
      raw_query("WITH RECURSIVE c(n) AS (SELECT 1 EXCEPT ALL SELECT n+1 FROM c) SELECT n FROM c");
      check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
      raw_query("SELECT id FROM items INTERSECT SELECT 2"); check_equal(execute(),ORM_STATUS_OK); count_is(1); integer_at(0,0,2);
    }
  }
  it("executes DISTINCT numeric tuple groups HAVING and output metadata through the plugin") {
    seed(); raw_query("INSERT INTO items VALUES(4,10),(5,NULL),(6,20)"); command(3);
    raw_query("SELECT COUNT(DISTINCT score,id>0) AS tuples,COUNT(DISTINCT score) AS unique_scores,COUNT(score) AS present FROM items");
    check_equal(execute(),ORM_STATUS_OK); count_is(1); integer_at(0,0,3); integer_at(0,1,3); integer_at(0,2,5);
    raw_query("SELECT score AS k,COUNT(DISTINCT id,id>0) AS n FROM items GROUP BY score HAVING n>1 ORDER BY k");
    check_equal(execute(),ORM_STATUS_OK); count_is(2); integer_at(0,0,10); integer_at(1,0,20);
    integer_at(0,1,2); integer_at(1,1,2);
    raw_query("SELECT COUNT(DISTINCT score,id) AS n FROM items WHERE FALSE");
    check_equal(execute(),ORM_STATUS_OK); count_is(1); integer_at(0,0,0);
  }
  it("executes distinct DOUBLE sums averages and MIN MAX no-op semantics") {
    seed_doubles(); raw_query("INSERT INTO metrics VALUES(5,?)"); parameter(orm_f64(2.5)); command(1);
    raw_query("SELECT SUM(DISTINCT score) AS s,AVG(DISTINCT score) AS a,MIN(DISTINCT score) AS low,MAX(DISTINCT score) AS high FROM metrics");
    check_equal(execute(),ORM_STATUS_OK); count_is(1); double_at(0,0,12.0); double_at(0,1,4.0);
    double_at(0,2,1.5); double_at(0,3,8.0);
    raw_query("SELECT COUNT(DISTINCT ?,score) AS n FROM metrics"); parameter(orm_u64(UINT64_MAX));
    check_equal(execute(),ORM_STATUS_OK); integer_at(0,0,3);
  }
  it("keeps DISTINCT reads in the transaction snapshot and restores results after rollback") {
    seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    raw_query("INSERT INTO items VALUES(4,40)"); command(1);
    raw_query("SELECT COUNT(DISTINCT score,id>0) AS n FROM items"); check_equal(execute(),ORM_STATUS_OK); integer_at(0,0,4);
    orm_result_destroy(result); result=NULL; orm_query_destroy(query); query=NULL;
    check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
    raw_query("SELECT COUNT(DISTINCT score,id>0) AS n FROM items"); check_equal(execute(),ORM_STATUS_OK); integer_at(0,0,3);
    raw_query("SELECT COUNT(DISTINCT score,id) OVER() AS n FROM items"); check_equal(execute(),ORM_STATUS_UNSUPPORTED); check_null(result);
    raw_query("SELECT COUNT(DISTINCT score) AS n FROM items"); check_equal(execute(),ORM_STATUS_OK); integer_at(0,0,3);
  }
  it("avoids duplicate DOUBLE overflow and recovers after unique intermediate overflow") {
    seed_doubles(); raw_query("UPDATE metrics SET score=? WHERE id<4"); parameter(orm_f64(DBL_MAX)); command(3);
    raw_query("SELECT SUM(DISTINCT score) AS s,AVG(DISTINCT score) AS a FROM metrics");
    check_equal(execute(),ORM_STATUS_OK); double_at(0,0,DBL_MAX); double_at(0,1,DBL_MAX);
    raw_query("UPDATE metrics SET score=? WHERE id=1"); parameter(orm_f64(-DBL_MAX)); command(1);
    raw_query("UPDATE metrics SET score=? WHERE id=2"); parameter(orm_f64(-DBL_MAX/2)); command(1);
    raw_query("SELECT SUM(DISTINCT score) AS s FROM metrics"); check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
    raw_query("SELECT COUNT(DISTINCT score,id>0) AS n FROM metrics"); check_equal(execute(),ORM_STATUS_OK); integer_at(0,0,3);
  }
  it("executes unsigned bit groups and empty identities through the real plugin") {
    seed(); raw_query("SELECT BIT_AND(score) AS a,BIT_OR(score) AS o,BIT_XOR(score) AS x FROM items");
    check_equal(execute(),ORM_STATUS_OK); count_is(1); unsigned_at(0,0,0); unsigned_at(0,1,30); unsigned_at(0,2,0);
    raw_query("SELECT BIT_AND(score) AS a,BIT_OR(score) AS o,BIT_XOR(score) AS x FROM items WHERE id<0");
    check_equal(execute(),ORM_STATUS_OK); count_is(1); unsigned_at(0,0,UINT64_MAX); unsigned_at(0,1,0); unsigned_at(0,2,0);
    raw_query("SELECT BIT_OR(-1) AS negative_bits,BIT_XOR(?) AS high_bits,BIT_AND(NULL) AS null_bits"); parameter(orm_u64(UINT64_MAX));
    check_equal(execute(),ORM_STATUS_OK); count_is(1);
    for(uint64_t i=0;i<3;++i) unsigned_at(0,i,UINT64_MAX);
  }
  it("executes rounded DOUBLE bit windows through native ROWS frames") {
    seed_doubles(); raw_query("SELECT id,BIT_AND(score) OVER w AS a,BIT_OR(score) OVER w AS o,BIT_XOR(score) OVER w AS x "
        "FROM metrics WINDOW w AS(ORDER BY id ROWS 1 PRECEDING) ORDER BY id");
    check_equal(execute(),ORM_STATUS_OK); count_is(4); const uint64_t expected[][3]={{2,2,2},{2,2,0},{0,10,10},{8,8,8}};
    for(uint64_t i=0;i<4;++i) {
      integer_at(i,0,(int64_t)i+1); for(uint64_t column=0;column<3;++column) unsigned_at(i,column+1,expected[i][column]);
    }
  }
  it("recovers after a bit conversion failure without returning a partial window result") {
    raw_query("CREATE TABLE extremes(id BIGINT PRIMARY KEY,score DOUBLE)"); command(0);
    raw_query("INSERT INTO extremes VALUES(1,?),(2,?)"); parameter(orm_f64(1.5)); parameter(orm_f64(DBL_MAX)); command(2);
    raw_query("SELECT id,BIT_OR(score) OVER(ORDER BY id) AS bits FROM extremes");
    check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
    raw_query("SELECT BIT_OR(score) AS bits FROM extremes WHERE id=1");
    check_equal(execute(),ORM_STATUS_OK); count_is(1); unsigned_at(0,0,2);
  }
  it("executes integer statistical groups and their aliases through the real plugin") {
    seed(); raw_query("SELECT VAR_POP(score) AS p,VAR_SAMP(score) AS s,STDDEV_POP(score) AS dp,"
        "STDDEV_SAMP(score) AS ds,VARIANCE(score) AS v,STD(score) AS a,STDDEV(score) AS b FROM items");
    check_equal(execute(),ORM_STATUS_OK); count_is(1);
    double_at(0,0,200.0/3.0); double_at(0,1,100); double_at(0,2,sqrt(200.0/3.0)); double_at(0,3,10);
    double_at(0,4,200.0/3.0); double_at(0,5,sqrt(200.0/3.0)); double_at(0,6,sqrt(200.0/3.0));
  }
  it("executes rolling statistical windows with nullable DOUBLE samples") {
    seed_doubles(); raw_query("SELECT id,VAR_POP(score) OVER w AS p,VAR_SAMP(score) OVER w AS s,"
        "STDDEV_POP(score) OVER w AS dp,STDDEV_SAMP(score) OVER w AS ds FROM metrics "
        "WINDOW w AS(ORDER BY id ROWS 1 PRECEDING) ORDER BY id");
    check_equal(execute(),ORM_STATUS_OK); count_is(4); const double variance[]={0,0.25,7.5625,0};
    for(uint64_t i=0;i<4;++i) {
      integer_at(i,0,(int64_t)i+1); double_at(i,1,variance[i]); double_at(i,3,sqrt(variance[i]));
      for(uint64_t column=2;column<5;column+=2) {
        if(i==0 || i==3) {
          orm_value_kind_t kind; check_equal(orm_result_value_kind(result,i,column,&kind,&error),ORM_STATUS_OK); check_equal(kind,ORM_VALUE_NULL);
        } else double_at(i,column,column==2?variance[i]*2:sqrt(variance[i]*2));
      }
    }
  }
  it("recovers from statistical window overflow and preserves singleton sample NULL metadata") {
    raw_query("CREATE TABLE extremes(id BIGINT PRIMARY KEY,score DOUBLE)"); command(0);
    raw_query("INSERT INTO extremes VALUES(1,?),(2,?)"); parameter(orm_f64(DBL_MAX)); parameter(orm_f64(-DBL_MAX)); command(2);
    raw_query("SELECT id,STDDEV(score) OVER(ORDER BY id) AS s FROM extremes");
    check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
    raw_query("SELECT VAR_POP(7) AS p,VAR_SAMP(7) AS s,STDDEV_SAMP(NULL) AS d");
    check_equal(execute(),ORM_STATUS_OK); count_is(1); double_at(0,0,0);
    for(uint64_t column=1;column<3;++column) {
      orm_value_kind_t kind; check_equal(orm_result_value_kind(result,0,column,&kind,&error),ORM_STATUS_OK); check_equal(kind,ORM_VALUE_NULL);
    }
  }
  it("executes window COUNT and extrema through native NULL peer frames") {
    seed(); raw_query("UPDATE items SET score=10 WHERE id=2"); command(1);
    raw_query("UPDATE items SET score=NULL WHERE id=3"); command(1);
    raw_query("SELECT id,COUNT(*) OVER w AS n,COUNT(score) OVER w AS present,MIN(score) OVER w AS low,MAX(score) OVER w AS high "
        "FROM items WINDOW w AS(ORDER BY score) ORDER BY id");
    check_equal(execute(),ORM_STATUS_OK); count_is(3);
    for(uint64_t i=0;i<3;++i) {
      integer_at(i,0,(int64_t)i+1); integer_at(i,1,i<2?3:1); integer_at(i,2,i<2?2:0);
      for(uint64_t column=3;column<5;++column) {
        if(i<2) integer_at(i,column,10);
        else { orm_value_kind_t kind; check_equal(orm_result_value_kind(result,i,column,&kind,&error),ORM_STATUS_OK); check_equal(kind,ORM_VALUE_NULL); }
      }
    }
  }
  it("executes rolling DOUBLE window SUM AVG in the real plugin") {
    seed_doubles();
    raw_query("SELECT id,SUM(score) OVER w AS total,AVG(score) OVER w AS mean FROM metrics "
        "WINDOW w AS(ORDER BY id ROWS 1 PRECEDING) ORDER BY id");
    check_equal(execute(),ORM_STATUS_OK); count_is(4);
    const double sums[]={1.5,4,10.5,8},means[]={1.5,2,5.25,8};
    for(uint64_t i=0;i<4;++i) { integer_at(i,0,(int64_t)i+1); double_at(i,1,sums[i]); double_at(i,2,means[i]); }
  }
  it("retains an aggregate window flow across disconnect and isolates later query errors") {
    seed(); raw_query("SELECT id,COUNT(*) OVER(ORDER BY id ROWS 1 PRECEDING) AS score FROM items ORDER BY id");
    open_rows(); row(1,1); disconnect(); row(2,2); row(3,2); end_rows();
    check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SELECT COUNT(DISTINCT id) OVER() AS n FROM items"); check_equal(execute(),ORM_STATUS_UNSUPPORTED); check_null(result);
    raw_query("SELECT SUM(score) OVER() AS n FROM items LIMIT 0"); check_equal(execute(),ORM_STATUS_UNSUPPORTED); check_null(result);
    raw_query("SELECT id,COUNT(*) OVER() AS score FROM items ORDER BY id"); open_rows(); row(1,3); row(2,3); row(3,3); end_rows();
  }
  it("executes grouped aggregates as window inputs in the real plugin") {
    seed(); raw_query("UPDATE items SET score=10 WHERE id=2"); command(1);
    raw_query("SELECT score,COUNT(*) AS n,MAX(COUNT(*)) OVER() AS largest,COUNT(*) OVER() AS groups_count "
        "FROM items GROUP BY score ORDER BY score");
    check_equal(execute(),ORM_STATUS_OK); count_is(2);
    integer_at(0,0,10); integer_at(0,1,2); integer_at(1,0,30); integer_at(1,1,1);
    for(uint64_t i=0;i<2;++i) { integer_at(i,2,2); integer_at(i,3,2); }
  }
  it("executes native default peer frames and preserves NULL frame values") {
    seed(); raw_query("UPDATE items SET score=10 WHERE id=2"); command(1);
    raw_query("UPDATE items SET score=NULL WHERE id=3"); command(1);
    raw_query("SELECT id,FIRST_VALUE(score) OVER w AS f,LAST_VALUE(id) OVER w AS l,NTH_VALUE(id,2) OVER w AS n "
        "FROM items WINDOW w AS(ORDER BY score) ORDER BY id");
    check_equal(execute(),ORM_STATUS_OK); count_is(3);
    for(uint64_t i=0;i<3;++i) {
      orm_value_kind_t kind; check_equal(orm_result_value_kind(result,i,1,&kind,&error),ORM_STATUS_OK);
      check_equal(kind,ORM_VALUE_NULL); integer_at(i,2,i<2 ? 2 : 3);
      if(i<2) integer_at(i,3,1);
      else { check_equal(orm_result_value_kind(result,i,3,&kind,&error),ORM_STATUS_OK); check_equal(kind,ORM_VALUE_NULL); }
    }
    raw_query("SELECT id,LAST_VALUE(score) OVER(ORDER BY score RANGE BETWEEN 0 PRECEDING AND 0 FOLLOWING) AS n "
        "FROM items ORDER BY id"); check_equal(execute(),ORM_STATUS_OK);
    integer_at(0,1,10); integer_at(1,1,10);
    orm_value_kind_t kind; check_equal(orm_result_value_kind(result,2,1,&kind,&error),ORM_STATUS_OK); check_equal(kind,ORM_VALUE_NULL);
  }
  it("retains rolling NTH_VALUE frames and markers after disconnect") {
    seed(); raw_query("SELECT id,NTH_VALUE(id,?) OVER w AS score FROM items WINDOW w AS "
        "(ORDER BY id ROWS BETWEEN ? PRECEDING AND ? FOLLOWING) ORDER BY id");
    parameter(orm_i64(2)); parameter(orm_u64(1)); parameter(orm_i64(1));
    open_rows(); disconnect(); check_equal(orm_runtime_close(runtime,&error),ORM_STATUS_BUSY);
    row(1,2); row(2,2); row(3,3); end_rows(); check_equal(orm_runtime_close(runtime,&error),ORM_STATUS_OK);
  }
  it("rejects native NTH_VALUE domains even under EXPLAIN and zero limit then recovers") {
    seed(); const char *sql[]={"SELECT NTH_VALUE(id,?) OVER() AS n FROM items LIMIT 0",
        "EXPLAIN SELECT NTH_VALUE(id,?) OVER() AS n FROM items"};
    const orm_value_t invalid[]={orm_null(),orm_i64(-1),orm_i64(0),orm_u64((uint64_t)INT64_MAX+1)};
    for(size_t sample=0;sample<sizeof(sql)/sizeof(sql[0]);++sample)
      for(size_t value=0;value<sizeof(invalid)/sizeof(invalid[0]);++value) {
        raw_query(sql[sample]); parameter(invalid[value]);
        check_equal(execute(),value<2 ? ORM_STATUS_TYPE_ERROR : ORM_STATUS_SQL_ERROR); check_null(result);
      }
    raw_query("EXPLAIN SELECT FIRST_VALUE(id+9223372036854775807) OVER(ORDER BY id ROWS CURRENT ROW) AS n FROM items");
    check_equal(execute(),ORM_STATUS_OK); count_is(1);
    raw_query("SELECT id,FIRST_VALUE(score) OVER(ORDER BY id ROWS 1 PRECEDING) AS score FROM items ORDER BY id");
    open_rows(); row(1,10); row(2,10); row(3,20); end_rows();
  }

  it("executes all eight nonframing functions with a native explicit current-row frame") {
    seed(); raw_query("SELECT id,ROW_NUMBER() OVER w AS rn,RANK() OVER w AS r,DENSE_RANK() OVER w AS d,"
        "PERCENT_RANK() OVER w AS p,CUME_DIST() OVER w AS c,NTILE(2) OVER w AS bucket,"
        "LAG(id,1,99) OVER w AS previous,LEAD(id,1,88) OVER w AS following FROM items "
        "WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND CURRENT ROW) ORDER BY id");
    check_equal(execute(),ORM_STATUS_OK); count_is(3);
    for(uint64_t i=0;i<3;++i) {
      for(uint64_t column=0;column<4;++column) integer_at(i,column,(int64_t)i+1);
      double_at(i,4,(double)i/2); double_at(i,5,(double)(i+1)/3);
      integer_at(i,6,i<2?1:2); integer_at(i,7,i?(int64_t)i:99); integer_at(i,8,i<2?(int64_t)i+2:88);
    }
  }
  it("retains explicit-frame parameters and the native window owner after disconnect") {
    seed(); raw_query("SELECT id,LAG(id,?,?) OVER w AS score FROM items WINDOW w AS "
        "(ORDER BY id ROWS BETWEEN ? PRECEDING AND ? FOLLOWING) ORDER BY id");
    parameter(orm_i64(1)); parameter(orm_i64(99)); parameter(orm_i64(0)); parameter(orm_u64(UINT64_MAX));
    open_rows(); disconnect(); check_equal(orm_runtime_close(runtime,&error),ORM_STATUS_BUSY);
    row(1,99); row(2,1); row(3,2); end_rows(); check_equal(orm_runtime_close(runtime,&error),ORM_STATUS_OK);
  }
  it("rejects frame domains under zero limit and EXPLAIN then recovers the native connection") {
    seed();
    const char *sql[]={"SELECT RANK() OVER(ORDER BY id ROWS ? PRECEDING) AS n FROM items LIMIT 0",
        "EXPLAIN SELECT RANK() OVER(ORDER BY id ROWS ? PRECEDING) AS n FROM items",
        "SELECT id FROM items WINDOW unused AS (ORDER BY id RANGE ? PRECEDING) LIMIT 0",
        "EXPLAIN SELECT id FROM items WINDOW unused AS (ORDER BY id RANGE ? PRECEDING)"};
    const orm_value_t invalid[]={orm_i64(-1),orm_null()};
    for(size_t sample=0;sample<sizeof(sql)/sizeof(sql[0]);++sample)
      for(size_t value=0;value<sizeof(invalid)/sizeof(invalid[0]);++value) {
        raw_query(sql[sample]); parameter(invalid[value]); check_equal(execute(),ORM_STATUS_TYPE_ERROR); check_null(result);
      }
    raw_query("EXPLAIN SELECT RANK() OVER w AS n FROM items WINDOW w AS "
        "(ORDER BY id+9223372036854775807 RANGE CURRENT ROW)");
    check_equal(execute(),ORM_STATUS_OK); count_is(1);
    raw_query("SELECT id,RANK() OVER(w) AS n FROM items WINDOW w AS(ROWS CURRENT ROW)");
    check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
    raw_query("SELECT id,RANK() OVER(ORDER BY id RANGE 0.5 PRECEDING) AS score FROM items ORDER BY id");
    open_rows(); row(1,1); row(2,2); row(3,3); end_rows();
  }
  it("executes named inheritance with native partitions and offset defaults") {
    seed(); raw_query("SELECT id,LAG(score,1,99) OVER(sorted) AS score FROM items "
        "WINDOW sorted AS (base ORDER BY id),base AS (PARTITION BY id>1) ORDER BY id");
    open_rows(); row(1,99); row(2,99); row(3,20); end_rows();
    raw_query("SELECT id,RANK() OVER W AS score FROM items WINDOW w AS (ORDER BY score DESC) ORDER BY id");
    open_rows(); row(1,3); row(2,2); row(3,1); end_rows();
  }
  it("retains a named-window flow and its marker snapshot after disconnect") {
    seed(); raw_query("SELECT id,ROW_NUMBER() OVER(w ORDER BY id DESC)+? AS score FROM items "
        "WINDOW w AS (PARTITION BY id>?) ORDER BY id");
    parameter(orm_i64(10)); parameter(orm_i64(1)); open_rows(); disconnect();
    check_equal(orm_runtime_close(runtime,&error),ORM_STATUS_BUSY);
    row(1,11); row(2,12); row(3,11); end_rows();
    check_equal(orm_runtime_close(runtime,&error),ORM_STATUS_OK);
  }
  it("explains named windows without key evaluation and recovers from invalid definitions") {
    seed(); raw_query("EXPLAIN SELECT RANK() OVER w AS n FROM items "
        "WINDOW w AS (ORDER BY id+9223372036854775807)");
    check_equal(execute(),ORM_STATUS_OK); count_is(1);
    vstr extra={0}; check_equal(orm_result_get_text(result,0,11,&extra,&error),ORM_STATUS_OK);
    check_contains(extra.data,"Window");
    const char *invalid[]={"SELECT id FROM items WINDOW w AS (x),x AS (w) LIMIT 0",
        "SELECT id FROM items WINDOW w AS (),W AS () LIMIT 0",
        "EXPLAIN SELECT id FROM items WINDOW w AS (ORDER BY missing)",
        "SELECT RANK() OVER (w PARTITION BY id) AS n FROM items WINDOW w AS ()"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
      raw_query(invalid[i]); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
    }
    raw_query("SELECT id,RANK() OVER w AS score FROM items WINDOW w AS (ORDER BY id) ORDER BY id");
    open_rows(); row(1,1); row(2,2); row(3,3); end_rows();
  }
  it("materializes native lag and lead values with partition defaults and target NULLs") {
    seed(); raw_query("INSERT INTO items VALUES(4,NULL)"); command(1);
    raw_query("SELECT id,LAG(score) OVER(ORDER BY id) AS previous,LEAD(score,1,id+100) OVER(ORDER BY id) AS following "
        "FROM items ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(4);
    for(uint64_t i=0;i<4;++i) {
      integer_at(i,0,(int64_t)i+1); uint8_t missing=0;
      if(!i) { check_equal(orm_result_is_null(result,i,1,&missing,&error),ORM_STATUS_OK); check_equal(missing,1); }
      else integer_at(i,1,(int64_t)i*10);
      if(i==2) { check_equal(orm_result_is_null(result,i,2,&missing,&error),ORM_STATUS_OK); check_equal(missing,1); }
      else integer_at(i,2,i==3?104:((int64_t)i+2)*10);
    }
  }
  it("streams offset marker snapshots after connection and query destruction") {
    seed(); raw_query("SELECT id,LAG(score,?,?) OVER(ORDER BY id) AS score FROM items ORDER BY id");
    parameter(orm_u64(1)); parameter(orm_i64(99)); open_rows(); disconnect();
    check_equal(orm_runtime_close(runtime,&error),ORM_STATUS_BUSY);
    row(1,99); row(2,10); row(3,20); end_rows(); check_equal(orm_runtime_close(runtime,&error),ORM_STATUS_OK);
  }
  it("materializes owned byte lead results without requiring a byte storage column") {
    seed(); raw_query("SELECT id,LEAD(IFNULL(NULL,?),1,?) OVER(ORDER BY id) AS label FROM items ORDER BY id");
    parameter(orm_text("value")); parameter(orm_text("last")); check_equal(execute(),ORM_STATUS_OK); count_is(3);
    text_at(0,1,"value"); text_at(1,1,"value"); text_at(2,1,"last");
  }
  it("evaluates correlated lag values and defaults in their original row scope") {
    seed(); raw_query("SELECT a.id,LAG((SELECT a.score),1,(SELECT a.id+100)) OVER(ORDER BY a.id) AS score "
        "FROM items a ORDER BY a.id"); open_rows(); row(1,101); row(2,10); row(3,20); end_rows();
    raw_query("SELECT a.id,LAG((SELECT a.id),1,(SELECT a.id+100)) OVER(ORDER BY a.id) AS score "
        "FROM items a GROUP BY a.id HAVING a.id>1 ORDER BY a.id");
    open_rows(); row(2,102); row(3,2); end_rows();
  }
  it("rejects invalid lead offsets under EXPLAIN and zero-limit consumers then recovers") {
    seed(); const char *sql[]={"EXPLAIN SELECT LEAD(score,?) OVER() AS n FROM items",
        "SELECT LEAD(score,?) OVER() AS n FROM items LIMIT 0"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      raw_query(sql[i]); parameter(orm_i64(-1)); check_equal(execute(),ORM_STATUS_TYPE_ERROR); check_null(result);
    }
    raw_query("SELECT id,LEAD(score,0) OVER(ORDER BY id) AS score FROM items ORDER BY id");
    open_rows(); row(1,10); row(2,20); row(3,30); end_rows();
  }
  it("materializes all six window ranks with peer groups and nullable native keys") {
    seed(); raw_query("INSERT INTO items VALUES(4,20),(5,NULL)"); command(2);
    raw_query("SELECT id,ROW_NUMBER() OVER(ORDER BY score,id) AS n,RANK() OVER(ORDER BY score) AS r,"
        "DENSE_RANK() OVER(ORDER BY score) AS d,PERCENT_RANK() OVER(ORDER BY score) AS p,"
        "CUME_DIST() OVER(ORDER BY score) AS c,NTILE(3) OVER(ORDER BY score,id) AS b FROM items ORDER BY id");
    check_equal(execute(),ORM_STATUS_OK); count_is(5);
    const int64_t numbers[]={2,3,5,4,1},ranks[]={2,3,5,3,1},dense[]={2,3,4,3,1},buckets[]={1,2,3,2,1};
    const double percent[]={0.25,0.5,1.0,0.5,0.0},cume[]={0.4,0.8,1.0,0.8,0.2};
    for(uint64_t i=0;i<5;++i) {
      integer_at(i,0,(int64_t)i+1); integer_at(i,1,numbers[i]); integer_at(i,2,ranks[i]);
      integer_at(i,3,dense[i]); double_at(i,4,percent[i]); double_at(i,5,cume[i]); integer_at(i,6,buckets[i]);
    }
  }
  it("retains a window flow after its query and connection are destroyed") {
    seed(); raw_query("SELECT id,ROW_NUMBER() OVER(ORDER BY score DESC)+? AS score FROM items ORDER BY id");
    parameter(orm_i64(10)); open_rows(); disconnect();
    check_equal(orm_runtime_close(runtime,&error),ORM_STATUS_BUSY);
    row(1,13); row(2,12); row(3,11); end_rows();
    check_equal(orm_runtime_close(runtime,&error),ORM_STATUS_OK);
  }
  it("executes windows after HAVING and consumes renamed window outputs through CTE and lateral scopes") {
    seed(); raw_query("SELECT score,RANK() OVER(ORDER BY COUNT(*) DESC,score DESC) AS r FROM items "
        "GROUP BY score HAVING score>=20 ORDER BY score");
    check_equal(execute(),ORM_STATUS_OK); count_is(2); integer_at(0,0,20); integer_at(0,1,2);
    integer_at(1,0,30); integer_at(1,1,1);
    raw_query("WITH c AS (SELECT id,ROW_NUMBER() OVER(ORDER BY score DESC) AS n FROM items) "
        "SELECT c.id,d.score FROM c,LATERAL (SELECT c.n+ROW_NUMBER() OVER() AS score) d ORDER BY c.id");
    open_rows(); row(1,4); row(2,3); row(3,2); end_rows();
  }
  it("preserves correlated scalar captures beside and inside window keys") {
    seed(); raw_query("SELECT a.id,ROW_NUMBER() OVER(ORDER BY (SELECT a.score) DESC)+(SELECT a.id) AS score "
        "FROM items a ORDER BY a.id");
    open_rows(); row(1,4); row(2,4); row(3,4); end_rows();
  }
  it("rolls back a correlated window scalar write through the existing transaction owner") {
    seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    raw_query("UPDATE items a SET score=(SELECT ROW_NUMBER() OVER()+a.score+?) WHERE a.id=2");
    parameter(orm_i64(5)); command(1);
    raw_query("SELECT id,score FROM items WHERE id=2"); open_rows(); row(2,26); end_rows();
    check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
    raw_query("SELECT id,score FROM items WHERE id=2"); open_rows(); row(2,20); end_rows();
  }
  it("explains windows without evaluating overflowing keys and recovers after invalid bucket counts") {
    seed(); raw_query("EXPLAIN SELECT ROW_NUMBER() OVER(ORDER BY id+9223372036854775807) AS n FROM items");
    check_equal(execute(),ORM_STATUS_OK); count_is(1);
    vstr extra={0}; check_equal(orm_result_get_text(result,0,11,&extra,&error),ORM_STATUS_OK);
    check_contains(extra.data,"Window");
    raw_query("SELECT ROW_NUMBER() OVER(ORDER BY id+9223372036854775807) AS n FROM items");
    check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
    const char *invalid[]={"SELECT NTILE(?) OVER() AS n FROM items LIMIT 0","EXPLAIN SELECT NTILE(?) OVER() AS n FROM items"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
      raw_query(invalid[i]); parameter(orm_i64(0)); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
    }
    raw_query("SELECT id,NTILE(?) OVER(ORDER BY id) AS score FROM items ORDER BY id");
    parameter(orm_i64(2)); open_rows(); row(1,1); row(2,1); row(3,2); end_rows();
  }
  it("streams renamed derived stars through the real plugin with owned query lifetime") {
    seed(); raw_query("SELECT d.id,d.score FROM (SELECT * FROM items) d(id,score) ORDER BY d.id");
    open_rows(); disconnect(); row(1,10); row(2,20); row(3,30); end_rows();
  }
  it("materializes positional aliases from repeated outputs and consecutive lateral sources") {
    seed(); raw_query("SELECT d.id,e.score FROM (SELECT id AS same,score AS same FROM items) d(id,amount),"
        "LATERAL (SELECT d.amount+?) q(value),LATERAL (SELECT q.value+d.id) e(score) ORDER BY d.id");
    parameter(orm_i64(5)); check_equal(execute(),ORM_STATUS_OK); count_is(3);
    for(uint64_t i=0;i<3;++i) {
      integer_at(i,0,(int64_t)i+1); integer_at(i,1,((int64_t)i+1)*11+5);
    }
  }
  it("rejects mismatched or repeated derived aliases and recovers on the same connection") {
    seed(); const char *invalid[]={
      "SELECT * FROM (SELECT id,score FROM items) d(id)",
      "SELECT * FROM (SELECT id,score FROM items) d(id,id)",
      "SELECT d.score FROM (SELECT id,score FROM items) d(id,amount)",
      "SELECT * FROM items a,LATERAL (SELECT a.id,a.score) d(id,id)"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
      raw_query(invalid[i]); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
    }
    raw_query("SELECT d.id,d.score FROM (SELECT id,score FROM items) d(id,score) ORDER BY d.id");
    open_rows(); row(1,10); row(2,20); row(3,30); end_rows();
  }
  it("uses renamed derived outputs in correlated writes within the existing transaction owner") {
    seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    raw_query("UPDATE items a SET score=(SELECT d.n FROM (SELECT a.score+?) d(n)) WHERE a.id=2");
    parameter(orm_i64(5)); command(1);
    raw_query("SELECT d.id,d.score FROM (SELECT id,score FROM items WHERE id=2) d(id,score)");
    open_rows(); row(2,25); end_rows();
    check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
    raw_query("SELECT d.id,d.score FROM (SELECT id,score FROM items WHERE id=2) d(id,score)");
    open_rows(); row(2,20); end_rows();
  }
  it("streams lateral rows through the real plugin with owned markers and connection lifetime") {
    seed(); raw_query("SELECT a.id AS id,d.score FROM items a,LATERAL (SELECT a.score+? AS score) d ORDER BY a.id");
    parameter(orm_i64(5)); open_rows(); disconnect();
    check_equal(orm_runtime_close(runtime,&error),ORM_STATUS_BUSY);
    row(1,15); row(2,25); row(3,35); end_rows();
    check_equal(orm_runtime_close(runtime,&error),ORM_STATUS_OK);
  }
  it("materializes consecutive nested lateral definitions and independent CTE readers") {
    seed(); raw_query("SELECT a.id AS id,e.score FROM items a,LATERAL "
        "(WITH q AS (SELECT a.score AS n) SELECT (SELECT n+1 FROM q) AS score FROM q) d,"
        "LATERAL (SELECT d.score+a.id AS score) e ORDER BY a.id");
    check_equal(execute(),ORM_STATUS_OK); count_is(3);
    for(uint64_t i=0;i<3;++i) { integer_at(i,0,(int64_t)i+1); integer_at(i,1,((int64_t)i+1)*11+1); }
  }
  it("materializes lateral LEFT nulls and groups dependent native row outputs") {
    seed(); raw_query("SELECT a.id AS id,d.score FROM items a LEFT JOIN LATERAL "
        "(SELECT a.score AS score WHERE a.id=2) d ON TRUE ORDER BY a.id");
    check_equal(execute(),ORM_STATUS_OK); count_is(3);
    for(uint64_t i=0;i<3;++i) {
      uint8_t missing=0; integer_at(i,0,(int64_t)i+1);
      check_equal(orm_result_is_null(result,i,1,&missing,&error),ORM_STATUS_OK); check_equal(missing,i==1?0:1);
    }
    integer_at(1,1,20);
    raw_query("SELECT d.id AS id,COUNT(*) AS score FROM items a JOIN LATERAL "
        "(SELECT b.id FROM items b WHERE b.id<=a.id) d ON TRUE "
        "GROUP BY d.id HAVING COUNT(*)>1 ORDER BY score DESC LIMIT 1 OFFSET 1");
    check_equal(execute(),ORM_STATUS_OK); count_is(1); integer_at(0,0,2); integer_at(0,1,2);
  }
  it("keeps lateral query leases through explicit cancellation and releases statement admission on close") {
    seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    raw_query("SELECT a.id AS id,d.score FROM items a,LATERAL (SELECT a.score AS score) d"); open_rows(); row(1,10);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_BUSY);
    cflow_publisher_cancel(&publisher); cflow_publisher_cancel(&publisher);
    orm_tides_public_row value={0}; check_equal(cflow_publisher_resume(&publisher,NULL,&value).kind,CFLOW_STEP_DONE);
    cflow_publisher_destroy(&publisher); publisher=(cflow_publisher){0};
    raw_query("UPDATE items SET score=99 WHERE id=2"); command(1);
    check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
    orm_transaction_destroy(transaction); transaction=NULL;
    raw_query("SELECT a.id AS id,d.score FROM items a,LATERAL (SELECT a.score AS score) d WHERE a.id=2");
    open_rows(); row(2,20); end_rows();
  }
  it("explains lateral dependencies and recovers from scope and later evaluation errors") {
    seed(); raw_query("EXPLAIN SELECT d.n FROM items a,LATERAL (SELECT a.id+9223372036854775807 AS n) d");
    check_equal(execute(),ORM_STATUS_OK);
    raw_query("SELECT d.n FROM items a RIGHT JOIN LATERAL (SELECT a.id AS n) d ON TRUE");
    check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
    raw_query("SELECT d.n FROM items a,LATERAL (SELECT a.id+9223372036854775807 AS n) d");
    check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
    raw_query("SELECT a.id AS id,d.score FROM items a,LATERAL (SELECT a.score AS score) d ORDER BY a.id");
    open_rows(); row(1,10); row(2,20); row(3,30); end_rows();
  }
  it("runs a fresh correlated recursive CTE inside each lateral row under the configured bound") {
    seed(); recursive_connection("4");
    raw_query("SELECT a.id AS id,d.n AS score FROM items a,LATERAL "
        "(WITH RECURSIVE c(n) AS (SELECT a.id UNION ALL SELECT n+1 FROM c WHERE n<a.id+1) "
        "SELECT MAX(n) AS n FROM c) d ORDER BY a.id");
    open_rows(); row(1,2); row(2,3); row(3,4); end_rows();
    raw_query("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT d.n FROM c,LATERAL "
        "(SELECT (SELECT c.n+1) AS n) d WHERE c.n<3) SELECT n FROM c");
    check_equal(execute(),ORM_STATUS_OK); count_is(3);
    for(uint64_t i=0;i<3;++i) integer_at(i,0,(int64_t)i+1);
  }
  it("executes DDL and bound multirow CRUD and persists across database reopen") {
    raw_query(ddl); command(0);
    raw_query("INSERT INTO items(score,id) VALUES(?,?),(?,?)");
    parameter(orm_i64(10)); parameter(orm_i64(1)); parameter(orm_i64(20)); parameter(orm_i64(2)); command(2);
    raw_query("UPDATE items SET id=id+1,score=score+? ORDER BY id DESC"); parameter(orm_i64(5)); command(2);
    raw_query("DELETE FROM items WHERE score=?"); parameter(orm_i64(15)); command(1);
    disconnect(); check_equal(connect_profile("relational", "false", NULL, 0), ORM_STATUS_OK);
    raw_query("SELECT id,score+? AS score FROM items WHERE id=? LIMIT ?");
    parameter(orm_i64(2)); parameter(orm_i64(3)); parameter(orm_i64(1)); open_rows(); row(3,27); end_rows();
  }
  it("exposes SHOW metadata and preserves NULL and scalar kinds in materialized results") {
    raw_query("SHOW FULL TABLES"); check_equal(execute(), ORM_STATUS_OK);
    uint64_t count = 99; check_equal(orm_result_column_count(result, &count, &error), ORM_STATUS_OK); check_equal(count, 2u);
    seed(); raw_query("SHOW FULL TABLES"); check_equal(execute(), ORM_STATUS_OK);
    text_at(0,0,"items"); text_at(0,1,"BASE TABLE");
    raw_query("SHOW COLUMNS FROM items"); check_equal(execute(), ORM_STATUS_OK);
    text_at(0,0,"id"); text_at(0,1,"bigint"); text_at(0,3,"PRI");
    uint8_t is_null = 0; check_equal(orm_result_is_null(result,0,4,&is_null,&error), ORM_STATUS_OK); check_equal(is_null,1);
    raw_query("SELECT id, NULL AS missing, id>0 AS yes, ? AS tag, ? AS unsigned_value, ? AS fraction FROM items LIMIT 1");
    parameter(orm_text("C:\\")); parameter(orm_u64(UINT64_MAX)); parameter(orm_f64(1.5));
    check_equal(execute(), ORM_STATUS_OK); text_at(0,3,"C:\\");
    check_equal(orm_result_is_null(result,0,1,&is_null,&error), ORM_STATUS_OK); check_equal(is_null,1);
    uint64_t unsigned_value = 0; double fraction = 0;
    check_equal(orm_result_get_uint64(result,0,4,&unsigned_value,&error), ORM_STATUS_OK); check_equal(unsigned_value,UINT64_MAX);
    check_equal(orm_result_get_double(result,0,5,&fraction,&error), ORM_STATUS_OK); check_true(fraction == 1.5);
    orm_value_kind_t kind; check_equal(orm_result_value_kind(result,0,2,&kind,&error),ORM_STATUS_OK); check_equal(kind,ORM_VALUE_BOOLEAN);
  }
  it("executes a qualified correlated scalar subquery through the relational driver") {
    seed();
    raw_query("SELECT o.id,(SELECT i.score FROM items i WHERE i.id=o.id) AS score "
        "FROM items o ORDER BY o.id");
    open_rows(); row(1,10); row(2,20); row(3,30); end_rows();
  }
  it("returns retained correlated TEXT through the relational driver") {
    seed();
    raw_query("SELECT o.id,(SELECT 'matched' WHERE o.id=2) AS label "
        "FROM items o ORDER BY o.id");
    check_equal(execute(),ORM_STATUS_OK);
    uint64_t count=0; check_equal(orm_result_row_count(result,&count,&error),
        ORM_STATUS_OK); check_equal(count,3u);
    uint8_t is_null=0; check_equal(orm_result_is_null(result,0,1,&is_null,&error),
        ORM_STATUS_OK); check_equal(is_null,1u);
    text_at(1,1,"matched");
    check_equal(orm_result_is_null(result,2,1,&is_null,&error),ORM_STATUS_OK);
    check_equal(is_null,1u);
  }
  it("resolves unqualified outer names in direct unit subqueries") {
    seed();
    raw_query("SELECT o.id,(SELECT id+100) AS shifted FROM items o ORDER BY o.id");
    check_equal(execute(),ORM_STATUS_OK); count_is(3);
    for(uint64_t i=0;i<3;++i) {
      integer_at(i,0,(int64_t)i+1);
      integer_at(i,1,(int64_t)i+101);
    }
  }
  it("executes joined correlated dependencies through the relational driver") {
    seed();
    raw_query("SELECT a.id,(SELECT a.id+b.id) AS score FROM items a JOIN items b "
        "ON EXISTS(SELECT 1 WHERE a.id=b.id) ORDER BY a.id");
    open_rows(); row(1,2); row(2,4); row(3,6); end_rows();
  }
  it("executes correlated subqueries with joined sources through the relational driver") {
    seed();
    raw_query("SELECT o.id,(SELECT COUNT(*) FROM items x JOIN items y "
        "ON x.id=y.id AND y.id=o.id) AS score FROM items o ORDER BY o.id");
    open_rows(); row(1,1); row(2,1); row(3,1); end_rows();
  }
  it("executes direct correlated compound subqueries through the relational driver") {
    seed();
    raw_query("SELECT o.id,(SELECT o.id+? UNION SELECT o.id+20 ORDER BY 1 LIMIT 1) AS score "
        "FROM items o WHERE o.id IN (SELECT o.id UNION ALL SELECT -1) AND EXISTS"
        "(SELECT 1 WHERE o.id=2 UNION ALL SELECT 1 WHERE o.id=3) ORDER BY o.id");
    parameter(orm_i64(10));
    open_rows(); row(2,12); row(3,13); end_rows();
  }
  it("reopens correlated derived inputs through the relational driver") {
    seed();
    raw_query("SELECT o.id,(SELECT d.x FROM "
        "(SELECT o.id AS x UNION SELECT -1) d JOIN items i ON i.id=d.x) AS score "
        "FROM items o ORDER BY o.id");
    open_rows(); row(1,1); row(2,2); row(3,3); end_rows();
  }
  it("rebuilds correlated CTE stores through the relational driver") {
    seed();
    raw_query("SELECT o.id,(WITH c(x) AS (SELECT o.id UNION SELECT -1) "
        "SELECT MAX(x) FROM c) AS score FROM items o ORDER BY o.id");
    open_rows(); row(1,1); row(2,2); row(3,3); end_rows();

    raw_query("WITH outer_rows AS (SELECT id FROM items) "
        "SELECT o.id,(SELECT o.id) AS score FROM outer_rows o ORDER BY o.id");
    open_rows(); row(1,1); row(2,2); row(3,3); end_rows();

    raw_query("WITH outer_rows AS (SELECT id FROM items) "
        "SELECT o.id,(SELECT o.id+i.id) AS score FROM outer_rows o "
        "JOIN items i ON o.id=i.id ORDER BY o.id");
    open_rows(); row(1,2); row(2,4); row(3,6); end_rows();
  }
  it("resolves unqualified outer columns after inner local schemas") {
    seed();
    raw_query("CREATE TABLE keys_only(k BIGINT PRIMARY KEY)"); command(0);
    raw_query("INSERT INTO keys_only(k) VALUES(1)"); command(1);
    raw_query("SELECT o.id,(SELECT id FROM keys_only WHERE k=1) AS score "
        "FROM items o ORDER BY o.id");
    open_rows(); row(1,1); row(2,2); row(3,3); end_rows();
  }
  it("captures parent and grandparent rows through the relational driver") {
    seed();
    raw_query("SELECT o.id,(SELECT (SELECT i.id+o.id+?) FROM items i "
        "WHERE i.id=2) AS score FROM items o ORDER BY o.id");
    parameter(orm_i64(10));
    open_rows(); row(1,13); row(2,14); row(3,15); end_rows();
    raw_query("SELECT o.id,o.score FROM items o WHERE EXISTS(SELECT 1 "
        "FROM items i WHERE i.id=o.id AND i.id IN (SELECT o.id WHERE i.id=2))");
    open_rows(); row(2,20); end_rows();
  }
  it("captures grouped keys through the relational driver") {
    seed();
    raw_query("SELECT o.id,(SELECT o.id+?) AS score FROM items o "
        "GROUP BY o.score,o.id HAVING EXISTS(SELECT 1 WHERE o.id>1) ORDER BY o.id");
    parameter(orm_i64(10));
    open_rows(); row(2,12); row(3,13); end_rows();
    raw_query("SELECT o.id,(SELECT (SELECT i.id+o.id) FROM items i "
        "GROUP BY i.id ORDER BY i.id LIMIT 1) AS score FROM items o ORDER BY o.id");
    open_rows(); row(1,2); row(2,3); row(3,4); end_rows();
  }
  it("captures through derived and dependent CTE definitions through the relational driver") {
    seed();
    raw_query("SELECT o.id,(SELECT d.x FROM (SELECT (SELECT i.id+o.id+?) AS x "
        "FROM items i WHERE i.id=2) d) AS score FROM items o GROUP BY o.id ORDER BY o.id");
    parameter(orm_i64(10));
    open_rows(); row(1,13); row(2,14); row(3,15); end_rows();
    raw_query("SELECT o.id,(WITH c AS (SELECT (SELECT i.id+o.id) AS x FROM items i WHERE i.id=2), "
        "d AS (SELECT (SELECT c.x+o.id) AS y FROM c) SELECT (SELECT y FROM d)) AS score "
        "FROM items o ORDER BY o.id");
    open_rows(); row(1,4); row(2,6); row(3,8); end_rows();
  }
  it("retains TEXT through nested definition readers through the relational driver") {
    seed();
    raw_query("SELECT o.id,(WITH c AS (SELECT (SELECT ? WHERE i.id=o.id) AS x "
        "FROM items i WHERE i.id=2) SELECT (SELECT x FROM c)) AS label FROM items o ORDER BY o.id");
    parameter(orm_text("captured")); check_equal(execute(),ORM_STATUS_OK); count_is(3);
    uint8_t is_null=0;
    check_equal(orm_result_is_null(result,0,1,&is_null,&error),ORM_STATUS_OK); check_equal(is_null,1u);
    text_at(1,1,"captured");
    check_equal(orm_result_is_null(result,2,1,&is_null,&error),ORM_STATUS_OK); check_equal(is_null,1u);
  }
  it("rolls back DDL and DML together and commits multiple statements on one owner") {
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    seed(); check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
    orm_transaction_destroy(transaction); transaction = NULL;
    raw_query("SHOW TABLES"); check_equal(execute(),ORM_STATUS_OK);
    uint64_t count = 99; check_equal(orm_result_row_count(result,&count,&error),ORM_STATUS_OK); check_equal(count,0u);
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    seed(); raw_query("UPDATE items SET score=score+1"); command(3);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
    orm_transaction_destroy(transaction); transaction = NULL;
    raw_query("SELECT id,score FROM items WHERE id=2"); open_rows(); row(2,21); end_rows();
  }
  it("keeps a completed transaction handle from releasing the next transaction admission") {
    seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
    orm_transaction_t *old = transaction; transaction = NULL;
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    orm_transaction_destroy(old);
    orm_transaction_t *other = NULL;
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&other,&error),ORM_STATUS_BUSY); check_null(other);
    raw_query("DELETE FROM items"); command(3); check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
    orm_transaction_destroy(transaction); transaction = NULL;
    raw_query("SELECT id,score FROM items LIMIT 1"); open_rows(); row(1,10); end_rows();
  }
  it("holds an explicit statement open until its cursor is destroyed") {
    seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM items LIMIT 1"); open_rows();
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_BUSY);
    check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_BUSY);
    raw_query("UPDATE items SET score=99"); check_equal(execute(),ORM_STATUS_BUSY); check_null(result);
    row(1,10); end_rows(); raw_query("UPDATE items SET score=99 WHERE id=1"); command(1);
    check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
  }
  it("retains auto query owners after query and connection handles are released") {
    seed(); raw_query("SELECT id,score FROM items WHERE id=?"); parameter(orm_i64(2)); open_rows();
    disconnect(); check_equal(orm_runtime_close(runtime,&error),ORM_STATUS_BUSY);
    row(2,20); end_rows(); check_equal(orm_runtime_close(runtime,&error),ORM_STATUS_OK);
  }
  it("defers abandoned explicit transaction rollback until the last query closes") {
    seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    raw_query("UPDATE items SET score=88 WHERE id=1"); command(1);
    raw_query("SELECT id,score FROM items WHERE id=1"); open_rows();
    orm_transaction_destroy(transaction); transaction = NULL;
    orm_query_destroy(query); query = NULL;
    row(1,88); end_rows(); raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,10); end_rows();
  }
  it("cancels a query idempotently and releases admission on destruction") {
    seed(); raw_query("SELECT id,score FROM items"); open_rows();
    cflow_publisher_cancel(&publisher); cflow_publisher_cancel(&publisher);
    orm_tides_public_row value = {0}; check_equal(cflow_publisher_resume(&publisher,NULL,&value).kind,CFLOW_STEP_DONE);
    cflow_publisher_destroy(&publisher); publisher = (cflow_publisher){0};
    raw_query("DELETE FROM items"); command(3);
  }
  it("rejects unsupported entries and failing statements without a partial write") {
    seed(); const char *sql[] = {
      "INSERT INTO items(id,score) VALUES(4,40),(1,99)", "UPDATE items SET id=id+1 ORDER BY id ASC",
      "INSERT INTO items(id,score) VALUES(4,?)", "INSERT INTO items(id,score) VALUES(4,40); DELETE FROM items",
      "DROP TEMPORARY TABLE items", "COMMIT RELEASE", "SELECT id AS x,score AS x FROM items"
    };
    for (size_t i=0; i<sizeof(sql)/sizeof(sql[0]); ++i) {
      raw_query(sql[i]); check_not_equal(execute(),ORM_STATUS_OK); check_null(result);
    }
    raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,10); end_rows();
    orm_query_destroy(query); query = NULL;
    check_equal(orm_query_create(connection,orm_view("items"),&query,&error),ORM_STATUS_OK);
    check_equal(execute(),ORM_STATUS_INVALID_STATE);
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SNAPSHOT,&transaction,&error),ORM_STATUS_UNSUPPORTED); check_null(transaction);
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("user"),&error),ORM_STATUS_OK);
    check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
  }
  group("common conditional result types through the plugin") {
    it("returns converted parameter branches and retains numeric NULL selection semantics") {
      const char *sql="SELECT CASE ? WHEN 0 THEN ? WHEN 1 THEN ? ELSE ? END AS n";
      for(size_t branch=0;branch<3;++branch) {
        raw_query(sql);parameter(orm_i64((int64_t)branch));
        parameter(orm_i64(INT64_C(9007199254740993)));parameter(orm_u64(UINT64_MAX));parameter(orm_f64(0.5));
        check_equal(execute(),ORM_STATUS_OK);count_is(1);
        const double expected[]={9007199254740992.0,18446744073709551616.0,0.5};double_at(0,0,expected[branch]);
      }
      raw_query("SELECT COALESCE(NULL,7,18446744073709551615,0.0) AS a,IFNULL(7,0.5) AS b,"
          "CASE WHEN FALSE THEN 7 WHEN FALSE THEN 0.5 END AS c,NULLIF(7,8.0) AS d");
      check_equal(execute(),ORM_STATUS_OK);count_is(1);double_at(0,0,7.0);double_at(0,1,7.0);
      uint8_t missing=0;check_equal(orm_result_is_null(result,0,2,&missing,&error),ORM_STATUS_OK);check_equal(missing,1u);
      integer_at(0,3,7);
    }
    it("deduplicates rounded real branches and feeds grouping CTEs and window SUM") {
      raw_query("CREATE TABLE numbers(id BIGINT UNSIGNED PRIMARY KEY,n DOUBLE)");command(0);
      raw_query("INSERT INTO numbers(id,n) VALUES(9007199254740992,0.0),(9007199254740993,0.0),(9007199254740994,2.0)");command(3);
      raw_query("SELECT DISTINCT CASE WHEN id<9007199254740994 THEN id ELSE n END AS v FROM numbers ORDER BY v");
      check_equal(execute(),ORM_STATUS_OK);count_is(2);double_at(0,0,2.0);double_at(1,0,9007199254740992.0);
      raw_query("SELECT CASE WHEN id<9007199254740994 THEN id ELSE n END AS v,COUNT(*) AS count "
          "FROM numbers GROUP BY CASE WHEN id<9007199254740994 THEN id ELSE n END ORDER BY v");
      check_equal(execute(),ORM_STATUS_OK);count_is(2);double_at(0,0,2.0);integer_at(0,1,1);
      double_at(1,0,9007199254740992.0);integer_at(1,1,2);
      seed();raw_query("WITH c AS(SELECT id,COALESCE(score,0.5) AS n FROM items) "
          "SELECT id,SUM(CASE WHEN id=1 THEN id ELSE n END) OVER(ORDER BY id ROWS UNBOUNDED PRECEDING) AS total "
          "FROM c ORDER BY id");
      check_equal(execute(),ORM_STATUS_OK);count_is(3);
      const double totals[]={1.0,21.0,51.0};for(size_t i=0;i<sizeof(totals)/sizeof(totals[0]);++i) double_at(i,1,totals[i]);
    }
    it("persists converted defaults and rolls back strict writes after a later branch fails") {
      raw_query("CREATE TABLE chosen(id BIGINT PRIMARY KEY,n DOUBLE DEFAULT(CASE WHEN TRUE THEN 7 ELSE 0.5 END))");command(0);
      raw_query("INSERT INTO chosen(id) VALUES(1)");command(1);
      raw_query("ALTER TABLE chosen ALTER n SET DEFAULT(COALESCE(NULL,9,18446744073709551615,0.0))");command(0);
      seed();raw_query("INSERT INTO chosen(id,n) SELECT id+10,IFNULL(score,0.5) AS n FROM items");command(3);
      raw_query("UPDATE items SET score=CASE WHEN id=1 THEN 7 ELSE 9223372036854775808.0 END ORDER BY id");
      check_equal(execute(),ORM_STATUS_OUT_OF_RANGE);check_null(result);
      raw_query("SELECT id,score FROM items ORDER BY id");open_rows();row(1,10);row(2,20);row(3,30);end_rows();
      raw_query("UPDATE items SET score=COALESCE(score,7/0.0) WHERE id=1");command(0);
      raw_query("SHOW WARNINGS");check_equal(execute(),ORM_STATUS_OK);count_is(0);
      raw_query("UPDATE items SET score=COALESCE(7/0.0,7) WHERE id=1");check_equal(execute(),ORM_STATUS_SQL_ERROR);check_null(result);
      disconnect();check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("INSERT INTO chosen(id) VALUES(2)");command(1);
      raw_query("SELECT n FROM chosen ORDER BY id");check_equal(execute(),ORM_STATUS_OK);count_is(5);
      const double values[]={7.0,9.0,10.0,20.0,30.0};for(size_t i=0;i<sizeof(values)/sizeof(values[0]);++i) double_at(i,0,values[i]);
    }
  }

  group("mixed real comparisons through the plugin") {
    it("preserves every rounded match when real equality and BETWEEN cannot use integer index keys") {
      raw_query("CREATE TABLE numbers(id BIGINT UNSIGNED PRIMARY KEY,score BIGINT)");command(0);
      raw_query("INSERT INTO numbers(id,score) VALUES(9007199254740992,1),(9007199254740993,2),"
          "(9007199254740994,3),(18446744073709551615,4)");command(4);
      raw_query("CREATE UNIQUE INDEX id_key ON numbers(id)");command(0);
      raw_query("SELECT score FROM numbers WHERE id=? ORDER BY score");parameter(orm_f64(9007199254740992.0));
      check_equal(execute(),ORM_STATUS_OK);count_is(2);integer_at(0,0,1);integer_at(1,0,2);
      raw_query("EXPLAIN SELECT score FROM numbers WHERE id=?");parameter(orm_f64(9007199254740992.0));
      check_equal(execute(),ORM_STATUS_OK);count_is(1);text_at(0,4,"ALL");
      raw_query("SELECT score FROM numbers WHERE id=? ORDER BY score");parameter(orm_u64(UINT64_C(9007199254740993)));
      check_equal(execute(),ORM_STATUS_OK);count_is(1);integer_at(0,0,2);
      raw_query("EXPLAIN SELECT score FROM numbers WHERE id=?");parameter(orm_u64(UINT64_C(9007199254740993)));
      check_equal(execute(),ORM_STATUS_OK);count_is(1);text_at(0,4,"const");
      raw_query("SELECT score FROM numbers WHERE id BETWEEN ? AND ? ORDER BY score");
      parameter(orm_u64(UINT64_C(9007199254740993)));parameter(orm_f64(9007199254740994.0));
      check_equal(execute(),ORM_STATUS_OK);count_is(3);for(uint64_t i=0;i<3;++i)integer_at(i,0,(int64_t)i+1);
      raw_query("SELECT score FROM numbers WHERE id=?");parameter(orm_f64(18446744073709551616.0));
      check_equal(execute(),ORM_STATUS_OK);count_is(1);integer_at(0,0,4);
      raw_query("SELECT score FROM numbers WHERE id IN(9007199254740992.0) ORDER BY score");
      check_equal(execute(),ORM_STATUS_OK);count_is(2);integer_at(0,0,1);integer_at(1,0,2);
    }
    it("uses mixed comparison in atomic UPDATE and DELETE with parameter predicates") {
      seed();raw_query("UPDATE items SET score=score+1 WHERE id BETWEEN ? AND ?");
      parameter(orm_f64(1.5));parameter(orm_i64(3));command(2);
      raw_query("DELETE FROM items WHERE id=?");parameter(orm_f64(2.0));command(1);
      raw_query("SELECT id,score FROM items ORDER BY id");open_rows();row(1,10);row(3,31);end_rows();
      raw_query("INSERT INTO items(id,score) VALUES(4,CASE 4 WHEN 4.0 THEN 40 ELSE 0 END)");command(1);
      raw_query("UPDATE items SET score=NULLIF(score,31.0) WHERE id=3.0");command(1);
      raw_query("SELECT id FROM items WHERE score IS NULL");check_equal(execute(),ORM_STATUS_OK);count_is(1);integer_at(0,0,3);
      raw_query("UPDATE items SET score=score+9223372036854775807 WHERE id>0.0");
      check_equal(open_command(),ORM_STATUS_OK);orm_command_result_t value=ORM_COMMAND_RESULT_INIT;
      check_equal(cflow_publisher_resume(&publisher,NULL,&value).kind,CFLOW_STEP_ERROR);
      cflow_publisher_destroy(&publisher);publisher=(cflow_publisher){0};
      raw_query("SELECT id,score FROM items WHERE score IS NOT NULL ORDER BY id");open_rows();row(1,10);row(4,40);end_rows();
    }
    it("executes mixed predicates across CTE JOIN and related IN using the same snapshot") {
      seed();raw_query("WITH c AS(SELECT id+0.0 AS n FROM items) SELECT a.id FROM items a "
          "JOIN c ON a.id=c.n WHERE a.id IN(SELECT c.n) ORDER BY a.id");
      check_equal(execute(),ORM_STATUS_OK);count_is(3);for(uint64_t i=0;i<3;++i)integer_at(i,0,(int64_t)i+1);
      raw_query("SELECT id FROM items WHERE id NOT IN(SELECT 2.0) ORDER BY id");
      check_equal(execute(),ORM_STATUS_OK);count_is(2);integer_at(0,0,1);integer_at(1,0,3);
      raw_query("SELECT NULL BETWEEN TRUE AND 2.0 AS n");check_equal(execute(),ORM_STATUS_OK);count_is(1);
      uint8_t is_null=0;check_equal(orm_result_is_null(result,0,0,&is_null,&error),ORM_STATUS_OK);check_equal(is_null,1);
    }
  }

  group("mixed real arithmetic through the plugin") {
    it("returns promoted parameter kinds and streams integer results of real expressions") {
      raw_query("SELECT ?+? AS a,?-? AS b,?*? AS c,?/? AS d,MOD(?,?) AS e,?%? AS f");
      const orm_value_t values[]={orm_i64(7),orm_f64(0.5),orm_f64(3.0),orm_i64(2),
        orm_u64(UINT64_MAX),orm_f64(0.5),orm_f64(7.5),orm_i64(2),orm_i64(-7),orm_f64(2.5),orm_u64(UINT64_MAX),orm_f64(2.0)};
      for(size_t i=0;i<sizeof(values)/sizeof(values[0]);++i) parameter(values[i]);
      check_equal(execute(),ORM_STATUS_OK); count_is(1);
      const double expected[]={7.5,1.0,9223372036854775808.0,3.75,-2.0,0.0};
      for(size_t i=0;i<sizeof(expected)/sizeof(expected[0]);++i) double_at(0,i,expected[i]);
      seed(); raw_query("SELECT id,SIGN(score+0.5) AS score FROM items ORDER BY id");
      open_rows(); row(1,1); row(2,1); row(3,1); end_rows();
    }
    it("uses promoted arithmetic in writes and reports existing integer assignment rounding") {
      seed_doubles(); raw_query("UPDATE metrics SET score=score+id WHERE id=1"); command(1);
      raw_query("INSERT INTO metrics(id,score) VALUES(5,7/2.0)"); command(1);
      raw_query("INSERT INTO metrics(id,score) VALUES(5,1.0) ON DUPLICATE KEY UPDATE score=score*2"); command(2);
      raw_query("SELECT score FROM metrics WHERE id IN(1,5) ORDER BY id"); check_equal(execute(),ORM_STATUS_OK);
      count_is(2); double_at(0,0,2.5); double_at(1,0,7.0);
      seed(); raw_query("UPDATE items SET score=score+0.5 WHERE id=1"); command(1);
      raw_query("SHOW WARNINGS"); check_equal(execute(),ORM_STATUS_OK); count_is(1); integer_at(0,1,1265);
      raw_query("INSERT INTO items(id,score) VALUES(4,MOD(7,2.5))"); command(1);
      raw_query("SELECT id,score FROM items WHERE id IN(1,4) ORDER BY id"); open_rows(); row(1,11); row(4,2); end_rows();
    }
    it("keeps strict overflow and mixed zero writes atomic while IGNORE records NULL") {
      seed_doubles();
      raw_query("INSERT INTO metrics(id,score) VALUES(5,1.0),(6,2*?)"); parameter(orm_f64(DBL_MAX));
      check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
      raw_query("UPDATE metrics SET score=?*id ORDER BY id"); parameter(orm_f64(DBL_MAX));
      check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
      raw_query("UPDATE metrics SET score=score/(id-2) WHERE id<=2 ORDER BY id");
      check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
      raw_query("SELECT score FROM metrics ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(4);
      double_at(0,0,1.5); double_at(1,0,2.5); double_at(2,0,8.0);
      raw_query("UPDATE IGNORE metrics SET score=score/(id-2) WHERE id<=2 ORDER BY id"); command(2);
      raw_query("SHOW WARNINGS"); check_equal(execute(),ORM_STATUS_OK); count_is(1); integer_at(0,1,1365);
      raw_query("SELECT score FROM metrics WHERE id<=2 ORDER BY id"); check_equal(execute(),ORM_STATUS_OK);
      double_at(0,0,-1.5); uint8_t missing=0; check_equal(orm_result_is_null(result,1,0,&missing,&error),ORM_STATUS_OK); check_equal(missing,1);
    }
    it("persists promoted defaults and INSERT SELECT values across connection reopen") {
      raw_query("CREATE TABLE promoted(id BIGINT PRIMARY KEY,n DOUBLE DEFAULT(7/2.0))"); command(0);
      raw_query("INSERT INTO promoted(id) VALUES(1)"); command(1);
      raw_query("ALTER TABLE promoted ALTER n SET DEFAULT(MOD(-7,2.5))"); command(0);
      seed(); raw_query("INSERT INTO promoted(id,n) WITH c AS(SELECT id+10 AS id,score*0.5 AS n FROM items) SELECT id,n FROM c"); command(3);
      disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("INSERT INTO promoted(id) VALUES(2)"); command(1);
      raw_query("SELECT n FROM promoted ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(5);
      const double expected[]={3.5,-2.0,5.0,10.0,15.0};
      for(size_t i=0;i<sizeof(expected)/sizeof(expected[0]);++i) double_at(i,0,expected[i]);
    }
  }

  group("division and modulo through the real plugin") {
    it("preserves integer precision and typed DOUBLE results through SQL parameters") {
      raw_query("SELECT ? DIV ? AS q,MOD(?,?) AS r,?/ ? AS v,? % ? AS u");
      const orm_value_t values[]={orm_i64(-7),orm_i64(3),orm_i64(-7),orm_u64(UINT64_MAX),
        orm_f64(7.5),orm_f64(2.0),orm_u64(UINT64_MAX),orm_i64(2)};
      for(size_t i=0;i<sizeof(values)/sizeof(values[0]);++i) parameter(values[i]);
      check_equal(execute(),ORM_STATUS_OK); count_is(1);
      integer_at(0,0,-2); integer_at(0,1,-7); double_at(0,2,3.75); unsigned_at(0,3,1);
      seed(); raw_query("SELECT id,MOD(score,7) AS score FROM items ORDER BY id"); open_rows();
      row(1,3); row(2,6); row(3,2); end_rows();
    }
    it("returns query NULLs and exposes retained and total division warnings") {
      disconnect(); const orm_option_t option={orm_view("sql_max_warnings"),orm_view("2")};
      check_equal(connect_profile("relational","false",&option,1),ORM_STATUS_OK); seed();
      raw_query("SELECT id DIV 0 AS q,MOD(score,0) AS r FROM items ORDER BY id");
      check_equal(execute(),ORM_STATUS_OK); count_is(3);
      for(uint64_t r=0;r<3;++r) for(uint64_t c=0;c<2;++c) {
        uint8_t missing=0; check_equal(orm_result_is_null(result,r,c,&missing,&error),ORM_STATUS_OK); check_equal(missing,1);
      }
      raw_query("SHOW COUNT(*) WARNINGS"); check_equal(execute(),ORM_STATUS_OK); unsigned_at(0,0,6);
      raw_query("SHOW WARNINGS"); check_equal(execute(),ORM_STATUS_OK); count_is(2);
      for(uint64_t r=0;r<2;++r) { text_at(r,0,"Warning"); integer_at(r,1,1365); text_at(r,2,"Division by 0"); }
      raw_query("SELECT CASE WHEN TRUE THEN 7 ELSE 1 DIV 0 END AS n,MOD(NULL,0) AS z"); check_equal(execute(),ORM_STATUS_OK);
      integer_at(0,0,7); raw_query("SHOW COUNT(*) WARNINGS"); check_equal(execute(),ORM_STATUS_OK); unsigned_at(0,0,0);
      raw_query("DELETE FROM items WHERE id DIV 0=0"); command(0);
      raw_query("SHOW COUNT(*) WARNINGS"); check_equal(execute(),ORM_STATUS_OK); unsigned_at(0,0,3);
      raw_query("SELECT id,score FROM items ORDER BY id"); open_rows(); row(1,10); row(2,20); row(3,30); end_rows();
    }
    it("rolls back strict zero failures while retaining earlier transaction writes") {
      seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      raw_query("INSERT INTO items(id,score) VALUES(4,40)"); command(1);
      const char *sql[]={"INSERT INTO items(id,score) VALUES(5,50),(6,MOD(7,0))",
        "UPDATE items SET score=MOD(score,id-2) ORDER BY id",
        "INSERT INTO items(id,score) SELECT id+10,MOD(score,id-2) FROM items",
        "REPLACE INTO items(id,score) VALUES(1,1 DIV 0)",
        "INSERT INTO items(id,score) VALUES(1,11) ON DUPLICATE KEY UPDATE score=MOD(score,0)"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        raw_query(sql[i]); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
      }
      raw_query("SELECT id,score FROM items ORDER BY id"); open_rows(); row(1,10); row(2,20); row(3,30); row(4,40); end_rows();
      check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
      orm_transaction_destroy(transaction); transaction=NULL;
      raw_query("SELECT id,score FROM items ORDER BY id"); open_rows(); row(1,10); row(2,20); row(3,30); end_rows();
    }
    it("keeps IGNORE mode through INSERT SELECT and UPDATE and records NULL adjustments") {
      seed(); raw_query("INSERT IGNORE INTO items(id,score) VALUES(4,MOD(7,0))"); command(1);
      raw_query("SHOW COUNT(*) WARNINGS"); check_equal(execute(),ORM_STATUS_OK); unsigned_at(0,0,1);
      raw_query("UPDATE IGNORE items SET score=id DIV 0 WHERE id=1"); command(1);
      raw_query("SHOW WARNINGS"); check_equal(execute(),ORM_STATUS_OK); count_is(1); integer_at(0,1,1365);
      raw_query("INSERT IGNORE INTO items(id,score) WITH c AS(SELECT 5 AS id,MOD(7,0) AS n) SELECT id,n FROM c"); command(1);
      raw_query("SHOW COUNT(*) WARNINGS"); check_equal(execute(),ORM_STATUS_OK); unsigned_at(0,0,1);
      raw_query("SELECT id,score FROM items WHERE id IN(1,4,5) ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(3);
      for(uint64_t r=0;r<3;++r) { uint8_t missing=0;
        check_equal(orm_result_is_null(result,r,1,&missing,&error),ORM_STATUS_OK); check_equal(missing,1); }
      raw_query("CREATE TABLE nonnull_items(id BIGINT PRIMARY KEY,n BIGINT NOT NULL)"); command(0);
      raw_query("INSERT IGNORE INTO nonnull_items(id,n) VALUES(1,1 DIV 0)"); command(1);
      raw_query("SHOW WARNINGS"); check_equal(execute(),ORM_STATUS_OK); count_is(2); integer_at(0,1,1365); integer_at(1,1,1048);
      raw_query("SELECT n FROM nonnull_items"); check_equal(execute(),ORM_STATUS_OK); integer_at(0,0,0);
    }
    it("inherits query warning receivers in recursion and avoids pruned expression evaluation") {
      disconnect(); const orm_option_t option={orm_view(recursive_option),orm_view("5")};
      check_equal(connect_profile("relational","false",&option,1),ORM_STATUS_OK);
      raw_query("WITH RECURSIVE c(n,z) AS(SELECT 1,MOD(7,0) UNION ALL SELECT n+1,MOD(n,0) FROM c WHERE n<3) SELECT z FROM c");
      check_equal(execute(),ORM_STATUS_OK); count_is(3);
      for(uint64_t r=0;r<3;++r) { uint8_t missing=0;
        check_equal(orm_result_is_null(result,r,0,&missing,&error),ORM_STATUS_OK); check_equal(missing,1); }
      raw_query("SHOW COUNT(*) WARNINGS"); check_equal(execute(),ORM_STATUS_OK); unsigned_at(0,0,3);
      const char *sql[]={"SELECT MOD(7,0) AS n LIMIT 0", "EXPLAIN SELECT 7 DIV 0 AS n",
        "SELECT EXISTS(SELECT MOD(7,0)) AS n"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        raw_query(sql[i]); check_equal(execute(),ORM_STATUS_OK);
        raw_query("SHOW COUNT(*) WARNINGS"); check_equal(execute(),ORM_STATUS_OK); unsigned_at(0,0,0);
      }
    }
    it("retains strict and IGNORE write modes while reopening recursive members") {
      recursive_connection("5"); seed();
      raw_query("INSERT INTO items(id,score) WITH RECURSIVE c(n,z) AS"
          "(SELECT 1,7 UNION ALL SELECT n+1,MOD(n,0) FROM c WHERE n<3) SELECT n+10,z FROM c");
      check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
      raw_query("SELECT id,score FROM items ORDER BY id"); open_rows(); row(1,10); row(2,20); row(3,30); end_rows();
      raw_query("INSERT IGNORE INTO items(id,score) WITH RECURSIVE c(n,z) AS"
          "(SELECT 1,7 UNION ALL SELECT n+1,MOD(n,0) FROM c WHERE n<3) SELECT n+10,z FROM c"); command(3);
      raw_query("SHOW COUNT(*) WARNINGS"); check_equal(execute(),ORM_STATUS_OK); unsigned_at(0,0,2);
      raw_query("SELECT score FROM items WHERE id>=11 ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(3);
      integer_at(0,0,7);
      for(uint64_t r=1;r<3;++r) { uint8_t missing=0;
        check_equal(orm_result_is_null(result,r,0,&missing,&error),ORM_STATUS_OK); check_equal(missing,1); }
    }
    it("persists folded division defaults and rejects zero default changes") {
      raw_query("CREATE TABLE division_defaults(id BIGINT PRIMARY KEY,n BIGINT DEFAULT(7 DIV 3),v DOUBLE DEFAULT(MOD(7.5,2.0)))"); command(0);
      raw_query("INSERT INTO division_defaults(id) VALUES(1)"); command(1);
      raw_query("ALTER TABLE division_defaults ALTER n SET DEFAULT(1 DIV 0)"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("ALTER TABLE division_defaults ALTER n SET DEFAULT(MOD(-7,3))"); command(0);
      disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("INSERT INTO division_defaults(id) VALUES(2)"); command(1);
      raw_query("SELECT n,v FROM division_defaults ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(2);
      integer_at(0,0,2); integer_at(1,0,-1); double_at(0,1,1.5); double_at(1,1,1.5);
    }
  }

  it("rejects unsupported exact division and noninteger DIV without partial writes") {
    seed();
    const char *sql[]={"SELECT id / 2 FROM items", "SELECT id DIV 2.0 FROM items",
      "INSERT INTO items(id,score) VALUES(4,40),(5,50 DIV 2.0)",
      "UPDATE items SET score=id / 2", "DELETE FROM items WHERE id DIV 2.0=0"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      raw_query(sql[i]); check_equal(execute(),ORM_STATUS_UNSUPPORTED); check_null(result);
    }
    raw_query("SELECT id,score FROM items ORDER BY id"); open_rows(); row(1,10); row(2,20); row(3,30); end_rows();
  }
  it("rejects unsupported index forms while retaining prior transaction writes") {
    seed();
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    raw_query("INSERT INTO items(id,score) VALUES(4,40)"); command(1);
    const char *sql[] = {"CREATE INDEX ix ON items (score(2))",
      "CREATE UNIQUE INDEX ux ON items ((score+id))"};
    for (size_t i=0; i<sizeof(sql)/sizeof(sql[0]); ++i) {
      raw_query(sql[i]); check_equal(execute(),ORM_STATUS_UNSUPPORTED); check_null(result);
    }
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
    orm_transaction_destroy(transaction); transaction=NULL;
    raw_query("SELECT id,score FROM items WHERE id=4"); open_rows(); row(4,40); end_rows();
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM items ORDER BY id"); open_rows();
    row(1,10); row(2,20); row(3,30); row(4,40); end_rows();
  }
  it("executes CREATE INDEX and maintains unique keys through raw and structured CRUD after reopen") {
    seed(); raw_query("CREATE INDEX ix ON items (score DESC)"); command(0);
    raw_query("CREATE UNIQUE INDEX ux ON items (score)"); command(0);
    raw_query("CREATE INDEX ix ON items (id)"); check_equal(execute(),ORM_STATUS_CONSTRAINT); check_null(result);
    structured(STRUCT_INSERT,"items"); assign("id",orm_i64(4)); assign("score",orm_i64(20));
    check_equal(execute(),ORM_STATUS_CONSTRAINT); check_null(result);
    structured(STRUCT_INSERT,"items"); assign("id",orm_i64(4)); assign("score",orm_i64(40)); command(1);
    structured(STRUCT_UPDATE,"items"); assign("id",orm_i64(5)); assign("score",orm_i64(45));
    where("id",ORM_COMPARE_EQUAL,orm_i64(4)); command(1);
    raw_query("UPDATE items SET score=45 WHERE id=1"); check_equal(execute(),ORM_STATUS_CONSTRAINT); check_null(result);
    structured(STRUCT_DELETE,"items"); where("id",ORM_COMPARE_EQUAL,orm_i64(5)); command(1);
    raw_query("INSERT INTO items(id,score) VALUES(6,45)"); command(1);
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("INSERT INTO items(id,score) VALUES(7,45)"); check_equal(execute(),ORM_STATUS_CONSTRAINT); check_null(result);
    raw_query("SELECT id,score FROM items ORDER BY id"); open_rows();
    row(1,10); row(2,20); row(3,30); row(6,45); end_rows();
  }
  it("executes MySQL duplicate-key assignments with sequential affected-row semantics") {
    seed();
    raw_query("INSERT INTO items(id,score) VALUES(1,99) ON DUPLICATE KEY UPDATE score=score+1"); command(2);
    raw_query("INSERT INTO items(id,score) VALUES(1,99) ON DUPLICATE KEY UPDATE score=score"); command(0);
    raw_query("INSERT INTO items(id,score) VALUES(4,40) ON DUPLICATE KEY UPDATE score=VALUES(score)"); command(1);
    raw_query("INSERT INTO items(id,score) VALUES(1,?) ON DUPLICATE KEY UPDATE score=VALUES(score)+?,score=score+1");
    parameter(orm_i64(50)); parameter(orm_i64(2)); command(2);
    raw_query("INSERT INTO items(id,score) VALUES(5,5),(5,7) ON DUPLICATE KEY UPDATE score=VALUES(score)"); command(3);
    raw_query("SELECT id,score FROM items ORDER BY id"); open_rows();
    row(1,53); row(2,20); row(3,30); row(4,40); row(5,7); end_rows();
  }
  it("opts into MySQL CLIENT_FOUND_ROWS reporting for UPDATE and duplicate keys") {
    seed(); disconnect();
    const orm_option_t option={orm_view(found_rows_option),orm_view("true")};
    check_equal(connect_profile("relational","false",&option,1),ORM_STATUS_OK);
    raw_query("UPDATE items SET score=score ORDER BY id LIMIT 2"); command(2);
    raw_query("INSERT INTO items VALUES(1,99) "
        "ON DUPLICATE KEY UPDATE score=score"); command(1);
    raw_query("INSERT INTO items VALUES(1,99) "
        "ON DUPLICATE KEY UPDATE score=score+1"); command(2);
    raw_query("DELETE FROM items WHERE id=2"); command(1);
  }
  it("uses MySQL INSERT row aliases for candidate values") {
    seed();
    raw_query("INSERT INTO items(id,score) VALUES(1,50) AS incoming(i,s) "
        "ON DUPLICATE KEY UPDATE score=incoming.s+1"); command(2);
    raw_query("INSERT INTO items SET id=1,score=70 AS incoming "
        "ON DUPLICATE KEY UPDATE score=incoming.score+1"); command(2);
    raw_query("SELECT id,score FROM items WHERE id=1"); open_rows();
    row(1,71); end_rows();
  }
  it("executes REPLACE and deletes every conflicting unique-key owner") {
    raw_query("CREATE TABLE replacements(id BIGINT PRIMARY KEY,score BIGINT NOT NULL,"
        "tag BIGINT NOT NULL)"); command(0);
    raw_query("CREATE UNIQUE INDEX by_score ON replacements(score)"); command(0);
    raw_query("CREATE UNIQUE INDEX by_tag ON replacements(tag)"); command(0);
    raw_query("INSERT INTO replacements VALUES(1,10,100),(2,20,200)"); command(2);
    raw_query("REPLACE INTO replacements VALUES(1,11,101)"); command(2);
    raw_query("REPLACE INTO replacements SET id=3,score=30,tag=300"); command(1);
    raw_query("REPLACE INTO replacements SELECT 4,20,101"); command(3);
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM replacements ORDER BY id"); open_rows();
    row(3,30); row(4,20); end_rows();
  }
  it("executes INSERT SET with parameters duplicate updates and IGNORE") {
    seed();
    raw_query("INSERT INTO items SET score=?,id=?");
    parameter(orm_i64(40)); parameter(orm_i64(4)); command(1);
    raw_query("INSERT INTO items SET id=?,score=? "
        "ON DUPLICATE KEY UPDATE score=VALUES(score)+1");
    parameter(orm_i64(4)); parameter(orm_i64(50)); command(2);
    raw_query("CREATE UNIQUE INDEX ux ON items(score)"); command(0);
    raw_query("INSERT IGNORE INTO items SET id=?,score=?");
    parameter(orm_i64(5)); parameter(orm_i64(20)); command(0);
    raw_query("SELECT id,score FROM items ORDER BY id"); open_rows();
    row(1,10); row(2,20); row(3,30); row(4,51); end_rows();
  }
  it("materializes INSERT SELECT including compound CTE and conflict paths") {
    seed();
    raw_query("INSERT INTO items(id,score) "
        "SELECT id+10,score+? FROM items WHERE id<=2 ORDER BY id");
    parameter(orm_i64(5)); command(2);
    raw_query("INSERT INTO items(id,score) SELECT id,score+? FROM items WHERE id=1 "
        "ON DUPLICATE KEY UPDATE score=VALUES(score)+?");
    parameter(orm_i64(10)); parameter(orm_i64(1)); command(2);
    raw_query("CREATE UNIQUE INDEX ux ON items(score)"); command(0);
    raw_query("INSERT IGNORE INTO items(id,score) "
        "SELECT id+20,score FROM items WHERE id<=2"); command(0);
    raw_query("INSERT INTO items(id,score) "
        "SELECT 50,500 UNION ALL SELECT 1,600");
    check_equal(execute(),ORM_STATUS_CONSTRAINT); check_null(result);
    raw_query("INSERT INTO items(id,score) WITH q AS "
        "(SELECT 60 AS id,600 AS score) SELECT id,score FROM q"); command(1);
    raw_query("SELECT id,score FROM items ORDER BY id"); open_rows();
    row(1,21); row(2,20); row(3,30); row(11,15); row(12,25); row(60,600); end_rows();
  }
  it("uses the configured recursive bound for INSERT SELECT atomically") {
    seed();
    const char bounded[] = "INSERT INTO items(id,score) WITH RECURSIVE c(n) AS ("
        "SELECT 4 UNION ALL SELECT n+1 FROM c WHERE n<6) SELECT n,n*10 FROM c";
    raw_query(bounded); check_equal(execute(),ORM_STATUS_UNSUPPORTED); check_null(result);
    recursive_connection("3"); raw_query(bounded); command(3);
    raw_query("INSERT INTO items(id,score) WITH RECURSIVE c(n) AS ("
        "SELECT 7 UNION ALL SELECT n+1 FROM c) SELECT n,n*10 FROM c");
    check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
    raw_query("SELECT id,score FROM items ORDER BY id"); open_rows();
    row(1,10); row(2,20); row(3,30); row(4,40); row(5,50); row(6,60); end_rows();
  }
  it("executes parameterized CTE UPDATE and DELETE through the relational driver") {
    seed();
    raw_query("WITH c(k) AS (SELECT id FROM items WHERE id>=?) "
        "UPDATE items SET score=id*10+? WHERE id IN (SELECT k FROM c)");
    parameter(orm_i64(2)); parameter(orm_i64(5)); command(2);
    raw_query("WITH c(v) AS (SELECT ?) UPDATE items "
        "SET score=(SELECT v FROM c) WHERE id=?");
    parameter(orm_i64(77)); parameter(orm_i64(1)); command(1);
    raw_query("WITH c(k) AS (SELECT id FROM items WHERE score=?) "
        "DELETE FROM items WHERE id IN (SELECT k FROM c)");
    parameter(orm_i64(25)); command(1);
    const char recursive[] = "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL "
        "SELECT n+1 FROM c WHERE n<2) UPDATE items SET score=score+1 "
        "WHERE id IN (SELECT n FROM c)";
    raw_query(recursive); check_equal(execute(),ORM_STATUS_UNSUPPORTED);
    check_null(result); recursive_connection("3"); raw_query(recursive); command(1);
    raw_query("SELECT id,score FROM items ORDER BY id"); open_rows();
    row(1,78); row(3,35); end_rows();
  }
  it("executes correlated UPDATE and DELETE through the relational driver") {
    seed();
    raw_query("UPDATE items SET score=(SELECT id*100) "
        "WHERE EXISTS(SELECT 1 WHERE id=2)"); command(1);
    raw_query("DELETE FROM items WHERE items.id IN "
        "(SELECT items.id WHERE items.id=3)"); command(1);
    raw_query("SELECT id,score FROM items ORDER BY id"); open_rows();
    row(1,10); row(2,200); end_rows();
  }
  it("executes write target aliases through the relational driver") {
    seed();
    raw_query("UPDATE items AS i SET score=(SELECT i.id*100) "
        "WHERE i.id>=2 ORDER BY i.id ASC LIMIT 1");
    command(1);
    raw_query("DELETE FROM items target WHERE EXISTS"
        "(SELECT 1 WHERE target.id=3)"); command(1);
    raw_query("SELECT id,score FROM items ORDER BY id"); open_rows();
    row(1,10); row(2,200); end_rows();
  }
  it("applies omitted partial and explicit DEFAULT values in strict mode") {
    seed();
    raw_query("INSERT INTO items VALUES(4,40)"); command(1);
    raw_query("INSERT INTO items(id) VALUES(5)"); command(1);
    raw_query("INSERT INTO items SET id=6"); command(1);
    raw_query("INSERT INTO items(id) SELECT 7"); command(1);
    raw_query("INSERT INTO items(id,score) VALUES(4,99) "
        "ON DUPLICATE KEY UPDATE score=DEFAULT"); command(2);
    raw_query("SELECT id,score FROM items WHERE id>=4 ORDER BY id");
    check_equal(execute(),ORM_STATUS_OK);
    uint64_t count=0; check_equal(orm_result_row_count(result,&count,&error),
        ORM_STATUS_OK); check_equal(count,4u);
    for (uint64_t i=0;i<count;++i) {
      int64_t id=0; uint8_t is_null=0;
      check_equal(orm_result_get_int64(result,i,0,&id,&error),ORM_STATUS_OK);
      check_equal(id,(int64_t)i+4);
      check_equal(orm_result_is_null(result,i,1,&is_null,&error),ORM_STATUS_OK);
      check_equal(is_null,1u);
    }
  }
  it("persists explicit numeric defaults and reuses them across INSERT forms") {
    raw_query("CREATE TABLE defaults(id BIGINT PRIMARY KEY DEFAULT 5,"
        "n BIGINT NOT NULL DEFAULT -7,u BIGINT UNSIGNED DEFAULT 9,"
        "v DOUBLE DEFAULT (1.25*2.0))"); command(0);
    raw_query("INSERT INTO defaults(id) VALUES(1)"); command(1);
    raw_query("INSERT INTO defaults VALUES(2,DEFAULT,DEFAULT,DEFAULT)"); command(1);
    raw_query("INSERT INTO defaults SET id=3"); command(1);
    raw_query("INSERT INTO defaults(id) SELECT 4"); command(1);
    raw_query("INSERT INTO defaults() VALUES()"); command(1);
    raw_query("UPDATE defaults SET n=5 WHERE id=1"); command(1);
    raw_query("UPDATE defaults SET n=DEFAULT,u=DEFAULT,v=DEFAULT WHERE id=1"); command(1);
    raw_query("UPDATE defaults SET n=5 WHERE id=1"); command(1);
    raw_query("INSERT INTO defaults(id,n,u,v) VALUES(1,5,DEFAULT,NULL) "
        "ON DUPLICATE KEY UPDATE n=DEFAULT,u=DEFAULT"); command(2);
    raw_query("SHOW COLUMNS FROM defaults");
    check_equal(execute(),ORM_STATUS_OK); count_is(4);
    text_at(1,4,"-7"); text_at(2,4,"9");
    raw_query("SHOW CREATE TABLE defaults");
    check_equal(execute(),ORM_STATUS_OK);
    text_at(0,1,"CREATE TABLE `defaults` (\n"
        "  `id` bigint NOT NULL DEFAULT 5,\n"
        "  `n` bigint NOT NULL DEFAULT -7,\n"
        "  `u` bigint unsigned NULL DEFAULT 9,\n"
        "  `v` double NULL DEFAULT 2.5,\n"
        "  PRIMARY KEY (`id`)\n)");
    raw_query("ALTER TABLE defaults ADD extra BIGINT"); command(0);
    raw_query("INSERT INTO defaults(id) VALUES(6)"); command(1);
    disconnect();
    check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SELECT id,n,u,v FROM defaults ORDER BY id");
    check_equal(execute(),ORM_STATUS_OK); count_is(6);
    for(uint64_t i=0;i<6;++i) {
      int64_t id=0,n=0; uint64_t u=0; double v=0.0;
      check_equal(orm_result_get_int64(result,i,0,&id,&error),ORM_STATUS_OK);
      check_equal(orm_result_get_int64(result,i,1,&n,&error),ORM_STATUS_OK);
      check_equal(orm_result_get_uint64(result,i,2,&u,&error),ORM_STATUS_OK);
      check_equal(orm_result_get_double(result,i,3,&v,&error),ORM_STATUS_OK);
      check_equal(id,(int64_t)i+1); check_equal(n,-7);
      check_equal(u,9u); check_equal(v,2.5);
    }
  }
  it("skips INSERT IGNORE key conflicts and keeps non-ignorable failures atomic") {
    seed(); raw_query("CREATE UNIQUE INDEX ux ON items(score)"); command(0);
    raw_query("INSERT IGNORE INTO items(id,score) VALUES(1,99),(4,10),(5,50),(6,50)"); command(1);
    raw_query("SELECT id,score FROM items ORDER BY id"); open_rows();
    row(1,10); row(2,20); row(3,30); row(5,50); end_rows();

    raw_query("INSERT IGNORE INTO items(id,score) VALUES(9,20) "
        "ON DUPLICATE KEY UPDATE score=30"); command(0);
    raw_query("SELECT id,score FROM items ORDER BY id"); open_rows();
    row(1,10); row(2,20); row(3,30); row(5,50); end_rows();

    raw_query("INSERT IGNORE INTO items(id,score) VALUES(7,70),(1,0) "
        "ON DUPLICATE KEY UPDATE score=score+9223372036854775807");
    check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
    raw_query("SELECT id,score FROM items ORDER BY id"); open_rows();
    row(1,10); row(2,20); row(3,30); row(5,50); end_rows();
  }
  it("reports bounded INSERT IGNORE adjustments through SHOW WARNINGS") {
    raw_query("CREATE TABLE warning_items(id BIGINT UNSIGNED PRIMARY KEY,"
        "score BIGINT NOT NULL)"); command(0);
    raw_query("INSERT IGNORE INTO warning_items(id,score) VALUES"
        "('invalid',NULL),(-1,2)"); command(1);

    raw_query("SHOW WARNINGS"); check_equal(execute(),ORM_STATUS_OK); count_is(4);
    text_at(0,0,"Warning"); integer_at(0,1,1366);
    text_at(0,2,"Incorrect numeric value for column 'id' at row 1");
    integer_at(1,1,1048); integer_at(2,1,1264); integer_at(3,1,1062);

    raw_query("SHOW WARNINGS LIMIT 1,2"); check_equal(execute(),ORM_STATUS_OK);
    count_is(2); integer_at(0,1,1048); integer_at(1,1,1264);
    raw_query("SHOW COUNT(*) WARNINGS"); check_equal(execute(),ORM_STATUS_OK);
    count_is(1); unsigned_at(0,0,4);

    raw_query("SELECT id,score FROM warning_items"); check_equal(execute(),ORM_STATUS_OK);
    count_is(1); unsigned_at(0,0,0); integer_at(0,1,0);
    raw_query("SHOW COUNT(*) WARNINGS"); check_equal(execute(),ORM_STATUS_OK);
    unsigned_at(0,0,0);

    raw_query("INSERT INTO warning_items(id,score) VALUES(2,2)"); command(1);
    raw_query("UPDATE warning_items SET score='4' WHERE id=2"); command(1);
    raw_query("CREATE UNIQUE INDEX warning_score ON warning_items(score)");
    command(0);
    raw_query("UPDATE IGNORE warning_items SET score="
        "CASE WHEN id=0 THEN '4x' ELSE '3' END ORDER BY id");
    command(1);
    raw_query("SHOW WARNINGS"); check_equal(execute(),ORM_STATUS_OK); count_is(2);
    integer_at(0,1,1265); integer_at(1,1,1062);
    text_at(0,2,"Data truncated for column 'score' at row 1");
    text_at(1,2,"Duplicate key ignored at row 1");
    raw_query("SELECT score FROM warning_items WHERE id=0");
    check_equal(execute(),ORM_STATUS_OK);
    count_is(1); integer_at(0,0,0);
    raw_query("SELECT score FROM warning_items WHERE id=2");
    check_equal(execute(),ORM_STATUS_OK);
    count_is(1); integer_at(0,0,3);

    disconnect();
    const orm_option_t warning_limit={orm_view("sql_max_warnings"),orm_view("2")};
    check_equal(connect_profile("relational","false",&warning_limit,1),ORM_STATUS_OK);
    raw_query("DELETE FROM warning_items"); command(2);
    raw_query("INSERT IGNORE INTO warning_items(id,score) VALUES"
        "('invalid',NULL),(-1,2)"); command(1);
    raw_query("SHOW WARNINGS"); check_equal(execute(),ORM_STATUS_OK); count_is(2);
    integer_at(0,1,1366); integer_at(1,1,1048);
    raw_query("SHOW COUNT(*) WARNINGS"); check_equal(execute(),ORM_STATUS_OK);
    unsigned_at(0,0,4);
  }
  it("uses unique-key owners and rolls back a partially applied duplicate-key statement") {
    seed(); raw_query("CREATE UNIQUE INDEX ux ON items(score)"); command(0);
    raw_query("INSERT INTO items(id,score) VALUES(9,20) ON DUPLICATE KEY UPDATE score=score+2"); command(2);
    raw_query("SELECT id,score FROM items ORDER BY id"); open_rows();
    row(1,10); row(2,22); row(3,30); end_rows();

    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    raw_query("INSERT INTO items(id,score) VALUES(8,80)"); command(1);
    raw_query("INSERT INTO items(id,score) VALUES(6,60),(7,22) ON DUPLICATE KEY UPDATE score=30");
    check_equal(execute(),ORM_STATUS_CONSTRAINT); check_null(result);
    raw_query("SELECT id,score FROM items ORDER BY id"); open_rows();
    row(1,10); row(2,22); row(3,30); row(8,80); end_rows();
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
    orm_transaction_destroy(transaction); transaction=NULL;
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM items ORDER BY id"); open_rows();
    row(1,10); row(2,22); row(3,30); row(8,80); end_rows();
  }
  it("rolls back index creation and rejects duplicate backfill without changing rows") {
    seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    raw_query("CREATE UNIQUE INDEX ux ON items (score)"); command(0);
    raw_query("INSERT INTO items(id,score) VALUES(4,10)"); check_equal(execute(),ORM_STATUS_CONSTRAINT); check_null(result);
    check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
    orm_transaction_destroy(transaction); transaction=NULL;
    raw_query("INSERT INTO items(id,score) VALUES(4,10)"); command(1);
    raw_query("CREATE UNIQUE INDEX ux ON items (score)"); check_equal(execute(),ORM_STATUS_CONSTRAINT); check_null(result);
    raw_query("SELECT id,score FROM items ORDER BY id"); open_rows();
    row(1,10); row(2,20); row(3,30); row(4,10); end_rows();
    raw_query("DELETE FROM items WHERE id=4"); command(1);
    raw_query("CREATE UNIQUE INDEX ux ON items (score)"); command(0);
    raw_query("INSERT INTO items(id,score) VALUES(4,10)"); check_equal(execute(),ORM_STATUS_CONSTRAINT); check_null(result);
  }
  it("drops indexes through SQL, restores them on rollback and persists deletion after reconnect") {
    seed(); raw_query("CREATE UNIQUE INDEX ux ON items (score)"); command(0);
    raw_query("CREATE INDEX ix ON items (id DESC)"); command(0);
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    raw_query("DROP INDEX ux ON items"); command(0);
    raw_query("INSERT INTO items(id,score) VALUES(4,10)"); command(1);
    check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
    orm_transaction_destroy(transaction); transaction=NULL;
    raw_query("INSERT INTO items(id,score) VALUES(4,10)"); check_equal(execute(),ORM_STATUS_CONSTRAINT); check_null(result);
    raw_query("DROP INDEX ux ON items"); command(0);
    raw_query("DROP INDEX ux ON items"); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
    structured(STRUCT_INSERT,"items"); assign("id",orm_i64(4)); assign("score",orm_i64(10)); command(1);
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM items ORDER BY id"); open_rows();
    row(1,10); row(2,20); row(3,30); row(4,10); end_rows();
    raw_query("CREATE UNIQUE INDEX ux ON items (score)"); check_equal(execute(),ORM_STATUS_CONSTRAINT); check_null(result);
    raw_query("DELETE FROM items WHERE id=4"); command(1);
    raw_query("CREATE UNIQUE INDEX ux ON items (score)"); command(0);
    raw_query("INSERT INTO items(id,score) VALUES(4,10)"); check_equal(execute(),ORM_STATUS_CONSTRAINT); check_null(result);
    raw_query("DROP INDEX ux ON items"); command(0); raw_query("DROP INDEX ix ON items"); command(0);
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("INSERT INTO items(id,score) VALUES(4,10)"); command(1);
  }
  it("truncates indexed tables through SQL and rolls back via user savepoints") {
    seed(); raw_query("CREATE UNIQUE INDEX ux ON items(score)"); command(0);
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("before_clear"),&error),ORM_STATUS_OK);
    raw_query("TRUNCATE TABLE items"); command(0);
    raw_query("SELECT id,score FROM items"); open_rows(); end_rows();
    raw_query("SHOW COLUMNS FROM items"); check_equal(execute(),ORM_STATUS_OK); count_is(2);
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("before_clear"),&error),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM items ORDER BY id"); open_rows(); row(1,10); row(2,20); row(3,30); end_rows();
    raw_query("TRUNCATE items"); command(0);
    structured(STRUCT_INSERT,"items"); assign("id",orm_i64(4)); assign("score",orm_i64(20)); command(1);
    raw_query("INSERT INTO items(id,score) VALUES(5,20)"); check_equal(execute(),ORM_STATUS_CONSTRAINT);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM items WHERE score=20"); open_rows(); row(4,20); end_rows();
    raw_query("EXPLAIN SELECT id FROM items WHERE score=20"); check_equal(execute(),ORM_STATUS_OK); text_at(0,6,"ux");
  }
  it("drops multiple indexed tables atomically and allows rollback or persistent same-name recreation") {
    seed(); raw_query("CREATE INDEX ix ON items(score DESC)"); command(0);
    raw_query("CREATE TABLE kept(id BIGINT PRIMARY KEY,score BIGINT,INDEX by_score(score))"); command(0);
    raw_query("INSERT INTO kept(id,score) VALUES(9,90)"); command(1);
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    raw_query("DROP TABLE items,kept"); command(0); raw_query("SHOW TABLES"); check_equal(execute(),ORM_STATUS_OK); count_is(0);
    raw_query("SELECT id FROM items"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
    check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
    raw_query("SELECT id,score FROM items WHERE score=20"); open_rows(); row(2,20); end_rows();
    raw_query("SELECT id,score FROM kept WHERE score=90"); open_rows(); row(9,90); end_rows();
    raw_query("DROP TABLE IF EXISTS missing,items,kept CASCADE"); command(0);
    raw_query("DROP TABLE IF EXISTS missing,items,kept RESTRICT"); command(0);
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SHOW TABLES"); check_equal(execute(),ORM_STATUS_OK); count_is(0);
    raw_query(ddl); command(0); raw_query("CREATE UNIQUE INDEX ix ON items(score)"); command(0);
    raw_query("INSERT INTO items(id,score) VALUES(1,100)"); command(1);
    raw_query("SELECT id,score FROM items WHERE score=100"); open_rows(); row(1,100); end_rows();
  }
  it("rejects unsupported or missing table clear targets without affecting an existing table") {
    seed(); const char *unsupported[] = {"DROP TEMPORARY TABLE items","TRUNCATE db.items"};
    for (size_t i=0;i<sizeof(unsupported)/sizeof(unsupported[0]);++i) {
      raw_query(unsupported[i]); check_equal(execute(),ORM_STATUS_UNSUPPORTED); check_null(result);
    }
    raw_query("DROP TABLE missing"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
    raw_query("DROP TABLE items,missing"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
    raw_query("TRUNCATE TABLE missing"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
    raw_query("DROP TABLE IF EXISTS missing"); command(0);
    raw_query("SELECT id,score FROM items ORDER BY id"); open_rows(); row(1,10); row(2,20); row(3,30); end_rows();
  }
  it("uses and explains equality indexes through raw SQL and structured predicates after reconnect") {
    seed(); raw_query("CREATE INDEX by_score ON items (score DESC)"); command(0);
    raw_query("INSERT INTO items(id,score) VALUES(4,20)"); command(1);
    raw_query("EXPLAIN SELECT id FROM items WHERE score=20"); check_equal(execute(),ORM_STATUS_OK); count_is(1);
    text_at(0,4,"ref"); text_at(0,5,"by_score"); text_at(0,6,"by_score"); text_at(0,7,"9"); text_at(0,8,"const");
    raw_query("SELECT id,score FROM items WHERE score=? ORDER BY id DESC"); parameter(orm_i64(20)); open_rows(); row(4,20); row(2,20); end_rows();
    structured(STRUCT_SELECT,"items"); project("id"); project("score"); where("score",ORM_COMPARE_EQUAL,orm_i64(20));
    open_rows(); row(2,20); row(4,20); end_rows();
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM items WHERE score=20 ORDER BY id"); open_rows(); row(2,20); row(4,20); end_rows();
    raw_query("CREATE UNIQUE INDEX by_id ON items (id)"); command(0);
    raw_query("EXPLAIN SELECT score FROM items WHERE id=2"); check_equal(execute(),ORM_STATUS_OK); text_at(0,4,"const"); text_at(0,6,"by_id");
    raw_query("DROP INDEX by_score ON items"); command(0);
    raw_query("EXPLAIN SELECT id FROM items WHERE score=20"); check_equal(execute(),ORM_STATUS_OK); text_at(0,4,"ALL");
    raw_query("SELECT id,score FROM items WHERE score=20 ORDER BY id"); open_rows(); row(2,20); row(4,20); end_rows();
  }
  it("executes and explains left-prefix and range access through the real driver") {
    seed(); raw_query("INSERT INTO items(id,score) VALUES(4,20),(5,NULL)"); command(2);
    raw_query("CREATE UNIQUE INDEX score_id ON items (score DESC,id ASC)"); command(0);
    raw_query("EXPLAIN SELECT id FROM items WHERE score=20"); check_equal(execute(),ORM_STATUS_OK);
    text_at(0,4,"ref"); text_at(0,6,"score_id"); text_at(0,7,"9");
    raw_query("EXPLAIN SELECT id FROM items WHERE score>=20 AND score<30"); check_equal(execute(),ORM_STATUS_OK);
    text_at(0,4,"range"); text_at(0,6,"score_id"); text_at(0,7,"9");
    uint8_t is_null = 0; check_equal(orm_result_is_null(result,0,8,&is_null,&error),ORM_STATUS_OK); check_equal(is_null,1);
    raw_query("SELECT id,score FROM items WHERE score BETWEEN ? AND ? ORDER BY id");
    parameter(orm_i64(20)); parameter(orm_i64(30)); open_rows(); row(2,20); row(3,30); row(4,20); end_rows();
    raw_query("SELECT id,score FROM items WHERE score=20 AND id>2"); open_rows(); row(4,20); end_rows();
    structured(STRUCT_SELECT,"items"); project("id"); project("score"); where("score",ORM_COMPARE_GREATER,orm_i64(20));
    open_rows(); row(3,30); end_rows();
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM items WHERE score>=20 AND score<30 ORDER BY id"); open_rows(); row(2,20); row(4,20); end_rows();
    raw_query("EXPLAIN SELECT id FROM items WHERE score=20 AND id>=2"); check_equal(execute(),ORM_STATUS_OK);
    text_at(0,4,"range"); text_at(0,7,"18");
  }
  it("executes deduplicated IN and OR index ranges through the real driver after reconnect") {
    seed(); raw_query("INSERT INTO items(id,score) VALUES(4,20),(5,NULL)"); command(2);
    raw_query("CREATE UNIQUE INDEX score_id ON items (score DESC,id ASC)"); command(0);
    raw_query("EXPLAIN SELECT id FROM items WHERE score IN (10,20) OR (score=20 AND id>2)");
    check_equal(execute(),ORM_STATUS_OK); text_at(0,4,"range"); text_at(0,6,"score_id"); text_at(0,7,"18");
    uint8_t is_null = 0; check_equal(orm_result_is_null(result,0,8,&is_null,&error),ORM_STATUS_OK); check_equal(is_null,1);
    raw_query("SELECT id,score FROM items WHERE score IN (?,?,?,NULL) OR (score>=20 AND score<30) ORDER BY id");
    parameter(orm_i64(10)); parameter(orm_i64(20)); parameter(orm_i64(10));
    open_rows(); row(1,10); row(2,20); row(4,20); end_rows();
    raw_query("SELECT COUNT(*) AS n FROM items WHERE score IN (10,20) OR score IN (20,30)");
    check_equal(execute(),ORM_STATUS_OK); count_is(1); int64_t count = 0;
    check_equal(orm_result_get_int64(result,0,0,&count,&error),ORM_STATUS_OK); check_equal(count,4);
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM items WHERE score=10 OR score=30 ORDER BY id"); open_rows(); row(1,10); row(3,30); end_rows();
    raw_query("SELECT id FROM items WHERE score IN (NULL) OR score IS NULL"); check_equal(execute(),ORM_STATUS_OK); count_is(1);
    int64_t id = 0; check_equal(orm_result_get_int64(result,0,0,&id,&error),ORM_STATUS_OK); check_equal(id,5);
  }
  it("shares rows between structured CRUD and raw SQL across reopen") {
    seed();
    structured(STRUCT_INSERT,"items"); assign("score",orm_i64(40)); assign("id",orm_i64(4)); command(1);
    structured(STRUCT_UPDATE,"items"); assign("score",orm_i64(45)); assign("id",orm_i64(5));
    where("items.id",ORM_COMPARE_EQUAL,orm_i64(4)); command(1);
    structured(STRUCT_DELETE,"items"); where("score",ORM_COMPARE_LESS_EQUAL,orm_i64(20)); command(2);
    structured(STRUCT_SELECT,"items"); check_equal(orm_query_select_all(query,&error),ORM_STATUS_OK);
    check_equal(execute(),ORM_STATUS_OK); count_is(2);
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM items"); open_rows(); row(3,30); row(5,45); end_rows();
  }
  it("applies qualified projections and pagination to structured row publishers") {
    seed(); structured(STRUCT_SELECT,"items"); project("items.id"); project("items.score");
    where("items.score",ORM_COMPARE_GREATER_EQUAL,orm_i64(10));
    check_equal(orm_query_set_offset(query,1,&error),ORM_STATUS_OK);
    check_equal(orm_query_set_limit(query,1,&error),ORM_STATUS_OK);
    open_rows(); row(2,20); end_rows();
    check_equal(orm_query_set_limit(query,0,&error),ORM_STATUS_OK); open_rows(); end_rows();
    structured(STRUCT_SELECT,"items"); project("id"); project("score");
    check_equal(orm_query_set_offset(query,2,&error),ORM_STATUS_OK); open_rows(); row(3,30); end_rows();
    check_equal(orm_query_set_offset(query,UINT64_MAX,&error),ORM_STATUS_OK); open_rows(); end_rows();
  }
  it("preserves comparison and NULL predicate semantics and parameter order") {
    seed(); raw_query("INSERT INTO items(id,score) VALUES(4,NULL)"); command(1);
    const orm_compare_t comparisons[]={ORM_COMPARE_EQUAL,ORM_COMPARE_NOT_EQUAL,ORM_COMPARE_LESS,
      ORM_COMPARE_LESS_EQUAL,ORM_COMPARE_GREATER,ORM_COMPARE_GREATER_EQUAL};
    const uint64_t counts[]={1,2,1,2,1,2};
    for(size_t i=0;i<sizeof(comparisons)/sizeof(comparisons[0]);++i) {
      structured(STRUCT_SELECT,"items"); project("id"); where("score",comparisons[i],orm_i64(20));
      check_equal(execute(),ORM_STATUS_OK); count_is(counts[i]);
    }
    structured(STRUCT_UPDATE,"items"); assign("score",orm_i64(44));
    where("score",ORM_COMPARE_EQUAL,orm_null()); where("id",ORM_COMPARE_EQUAL,orm_i64(4)); command(1);
    structured(STRUCT_UPDATE,"items"); assign("score",orm_null());
    where("id",ORM_COMPARE_EQUAL,orm_i64(2)); command(1);
    structured(STRUCT_SELECT,"items"); project("id"); where("score",ORM_COMPARE_NOT_EQUAL,orm_null());
    check_equal(execute(),ORM_STATUS_OK); count_is(3);
    raw_query("SELECT id,score FROM items WHERE id=4"); open_rows(); row(4,44); end_rows();
  }
  it("keeps keyword identifiers as columns rather than SQL literals") {
    raw_query("CREATE TABLE `select`(id BIGINT PRIMARY KEY,`true` BIGINT,`null` BIGINT)"); command(0);
    structured(STRUCT_INSERT,"select"); assign("id",orm_i64(1)); assign("true",orm_i64(42)); assign("null",orm_i64(73)); command(1);
    structured(STRUCT_SELECT,"select"); project("true"); project("select.null");
    where("true",ORM_COMPARE_EQUAL,orm_i64(42));
    check_equal(execute(),ORM_STATUS_OK); count_is(1);
    int64_t value=0; check_equal(orm_result_get_int64(result,0,0,&value,&error),ORM_STATUS_OK); check_equal(value,42);
    check_equal(orm_result_get_int64(result,0,1,&value,&error),ORM_STATUS_OK); check_equal(value,73);
  }
  it("rolls structured changes back through the same user savepoint as raw SQL") {
    seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("before"),&error),ORM_STATUS_OK);
    structured(STRUCT_UPDATE,"items"); assign("score",orm_i64(99)); where("id",ORM_COMPARE_EQUAL,orm_i64(1)); command(1);
    raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,99); end_rows();
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("before"),&error),ORM_STATUS_OK);
    structured(STRUCT_SELECT,"items"); project("id"); project("score"); where("id",ORM_COMPARE_EQUAL,orm_i64(1));
    open_rows(); row(1,10); end_rows();
    structured(STRUCT_DELETE,"items"); where("id",ORM_COMPARE_EQUAL,orm_i64(2)); command(1);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
    orm_transaction_destroy(transaction); transaction=NULL;
    raw_query("SELECT id,score FROM items"); open_rows(); row(1,10); row(3,30); end_rows();
  }
  it("converts compatible structured assignments and rejects unsupported expressions") {
    seed();
    structured(STRUCT_UPDATE,"items"); assign("score",orm_text("99"));
    command(3);
    structured(STRUCT_DELETE,"items"); where("score",ORM_COMPARE_LIKE,orm_text("%"));
    check_equal(execute(),ORM_STATUS_UNSUPPORTED); check_null(result);
    structured(STRUCT_SELECT,"items"); project("absent"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
    structured(STRUCT_SELECT,"missing.items"); project("id"); check_equal(execute(),ORM_STATUS_UNSUPPORTED);
    structured(STRUCT_SELECT,"items"); project("id");
    check_equal(orm_query_order_by(query,orm_view("score"),ORM_ORDER_DESCENDING,&error),ORM_STATUS_OK);
    check_equal(execute(),ORM_STATUS_OK); count_is(3);
    raw_query("SELECT id,score FROM items"); open_rows(); row(1,99); row(2,99); row(3,99); end_rows();
  }
  it("enforces the rendered SQL byte limit before writes and keeps the connection usable") {
    seed(); disconnect(); config.max_query_bytes=128;
    check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    structured(STRUCT_UPDATE,"items"); assign("score",orm_i64(99));
    for(size_t i=0;i<12;++i) where("id",ORM_COMPARE_EQUAL,orm_i64(1));
    check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
    raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,10); end_rows();
  }
  it("orders raw query expressions aliases and hidden keys before pagination") {
    seed(); raw_query("UPDATE items SET score=10 WHERE id=3"); command(1);
    raw_query("SELECT id,score+? AS score FROM items WHERE id>? ORDER BY score DESC,id DESC LIMIT ? OFFSET ?");
    parameter(orm_i64(1)); parameter(orm_i64(0)); parameter(orm_i64(2)); parameter(orm_i64(1));
    open_rows(); row(3,11); row(1,11); end_rows();
    raw_query("SELECT id,score FROM items ORDER BY -score ASC,id DESC LIMIT 2");
    open_rows(); row(2,20); row(3,10); end_rows();
    raw_query("SELECT id,score FROM items ORDER BY 2 DESC,1 DESC LIMIT 1");
    open_rows(); row(2,20); end_rows();
  }
  it("orders nullable keys with MySQL NULL placement through materialized results") {
    seed(); raw_query("UPDATE items SET score=NULL WHERE id=2"); command(1);
    const char *sql[]={"SELECT id FROM items ORDER BY score ASC", "SELECT id FROM items ORDER BY score DESC"};
    const int64_t expected[][3]={{2,1,3},{3,1,2}};
    for(size_t i=0;i<2;++i) {
      raw_query(sql[i]); check_equal(execute(),ORM_STATUS_OK); count_is(3);
      for(size_t j=0;j<3;++j) {
        int64_t id=0; check_equal(orm_result_get_int64(result,j,0,&id,&error),ORM_STATUS_OK);
        check_equal(id,expected[i][j]);
      }
    }
  }
  it("shares structured ordering with SQL and rejects sort capacity exhaustion without a prefix") {
    seed(); structured(STRUCT_SELECT,"items"); project("id"); project("score");
    check_equal(orm_query_order_by(query,orm_view("score"),ORM_ORDER_DESCENDING,&error),ORM_STATUS_OK);
    check_equal(orm_query_set_limit(query,1,&error),ORM_STATUS_OK);
    check_equal(orm_query_set_offset(query,1,&error),ORM_STATUS_OK); open_rows(); row(2,20); end_rows();
    disconnect(); const orm_option_t limit={orm_view("sql_max_materialized_rows"),orm_view("2")};
    check_equal(connect_profile("relational","false",&limit,1),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM items ORDER BY score DESC LIMIT 1");
    check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
    raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,10); end_rows();
  }
  it("streams raw SQL joins with bound ON parameters through the real plugin across reopen") {
    seed(); raw_query("CREATE TABLE extra(id BIGINT PRIMARY KEY,score BIGINT)"); command(0);
    raw_query("INSERT INTO extra(id,score) VALUES(2,200),(4,400)"); command(2);
    for(size_t pass=0;pass<2;++pass) {
      raw_query("SELECT a.id+? AS id,b.score FROM items a INNER JOIN extra b ON a.id+?=b.id WHERE a.id>? ORDER BY id");
      parameter(orm_i64(10)); parameter(orm_i64(1)); parameter(orm_i64(0)); open_rows(); row(11,200); row(13,400); end_rows();
      if(!pass) { disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK); }
    }
    raw_query("SELECT a.id AS aid,b.id AS bid FROM items a RIGHT JOIN extra b ON a.id=b.id ORDER BY bid");
    check_equal(execute(),ORM_STATUS_OK); count_is(2);
    int64_t value=0; check_equal(orm_result_get_int64(result,0,0,&value,&error),ORM_STATUS_OK); check_equal(value,2);
    uint8_t is_null=0; check_equal(orm_result_is_null(result,1,0,&is_null,&error),ORM_STATUS_OK); check_equal(is_null,1);
    check_equal(orm_result_get_int64(result,1,1,&value,&error),ORM_STATUS_OK); check_equal(value,4);
  }
  it("shares JOIN transaction snapshots savepoints and cursor admission") {
    seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("before_join"),&error),ORM_STATUS_OK);
    raw_query("UPDATE items SET score=99 WHERE id=2"); command(1);
    raw_query("SELECT a.id,b.score FROM items a JOIN items b ON a.id+1=b.id WHERE a.id=1"); open_rows();
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_BUSY);
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("before_join"),&error),ORM_STATUS_BUSY);
    row(1,99); end_rows();
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("before_join"),&error),ORM_STATUS_OK);
    raw_query("SELECT a.id,b.score FROM items a JOIN items b ON a.id+1=b.id WHERE a.id=1"); open_rows(); row(1,20); end_rows();
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
  }
  it("groups and deduplicates joined rows while preserving aliases and outer NULLs") {
    seed(); raw_query("SELECT a.id,COUNT(b.id) AS score FROM items a LEFT JOIN items b ON a.id<b.id "
      "GROUP BY a.id HAVING score>=0 ORDER BY a.id"); open_rows(); row(1,2); row(2,1); row(3,0); end_rows();
    raw_query("SELECT DISTINCT a.id,a.score FROM items a CROSS JOIN items b ORDER BY a.id DESC LIMIT 2");
    open_rows(); row(3,30); row(2,20); end_rows();
    raw_query("SELECT a.id FROM items a LEFT JOIN items b ON a.id+1=b.id WHERE b.id IS NULL");
    check_equal(execute(),ORM_STATUS_OK); count_is(1); int64_t value=0;
    check_equal(orm_result_get_int64(result,0,0,&value,&error),ORM_STATUS_OK); check_equal(value,3);
    raw_query("SELECT id FROM items a JOIN items b ON a.id=b.id"); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
    raw_query("SELECT a.id FROM items a JOIN items b USING(id)"); check_equal(execute(),ORM_STATUS_OK); count_is(3);
  }
  it("enforces configured JOIN pair bounds and retains usable data after query failure") {
    seed(); disconnect(); const orm_option_t limit={orm_view("sql_max_join_pairs"),orm_view("1")};
    check_equal(connect_profile("relational","false",&limit,1),ORM_STATUS_OK);
    raw_query("SELECT a.id FROM items a CROSS JOIN items b"); check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
    raw_query("SELECT id,score FROM items WHERE id=2"); open_rows(); row(2,20); end_rows();
    raw_query("SELECT a.id,a.score FROM items a CROSS JOIN items b LIMIT 1"); open_rows(); row(1,10); end_rows();
    raw_query("EXPLAIN SELECT a.id FROM items a RIGHT JOIN items b ON a.id=b.id");
    check_equal(execute(),ORM_STATUS_OK); count_is(2); text_at(0,2,"b"); text_at(1,2,"a");
  }
  it("materializes multi-table EXPLAIN rows and keeps JOIN sources alive after handles are released") {
    seed(); raw_query("EXPLAIN FORMAT=TRADITIONAL SELECT a.id FROM items a LEFT JOIN items b ON a.id=b.id "
      "JOIN items c ON b.id=c.id ORDER BY a.id");
    check_equal(execute(),ORM_STATUS_OK); count_is(3); text_at(0,2,"a"); text_at(1,2,"b"); text_at(2,2,"c");
    uint8_t is_null=0; check_equal(orm_result_is_null(result,0,9,&is_null,&error),ORM_STATUS_OK); check_equal(is_null,1);
    raw_query("SELECT a.id,b.score FROM items a JOIN items b ON a.id=b.id WHERE a.id=2"); open_rows();
    disconnect(); check_equal(orm_runtime_close(runtime,&error),ORM_STATUS_BUSY); row(2,20); end_rows();
    check_equal(orm_runtime_close(runtime,&error),ORM_STATUS_OK);
  }
  it("executes raw UNION ALL with first-branch metadata bound parameters and persistence") {
    seed(); for(size_t pass=0;pass<2;++pass) {
      raw_query("SELECT id+? AS id,score FROM items WHERE id=1 UNION ALL SELECT id+? AS other,score AS points FROM items WHERE id=2 ORDER BY id");
      parameter(orm_i64(10)); parameter(orm_i64(20)); open_rows(); row(11,10); row(22,20); end_rows();
      if(!pass) { disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK); }
    }
    raw_query("(SELECT id,score FROM items ORDER BY id DESC LIMIT 1) UNION ALL (SELECT id,score FROM items ORDER BY id LIMIT 1) ORDER BY id DESC");
    check_equal(open_command(),ORM_STATUS_INVALID_ARGUMENT); check_false(cflow_publisher_valid(&publisher));
    open_rows(); row(3,30); row(1,10); end_rows();
  }
  it("executes constants and unit CTE queries through the relational plugin without creating tables") {
    raw_query("SELECT 1+1, ? AS label"); parameter(orm_text("unit")); check_equal(execute(),ORM_STATUS_OK); count_is(1);
    int64_t value=0; check_equal(orm_result_get_int64(result,0,0,&value,&error),ORM_STATUS_OK); check_equal(value,2); text_at(0,1,"unit");
    raw_query("WITH c(id,score) AS (SELECT 1,10 UNION ALL SELECT 2,20) SELECT a.id,b.score FROM c a JOIN c b ON a.id=b.id ORDER BY a.id");
    open_rows(); row(1,10); row(2,20); end_rows();
    raw_query("EXPLAIN SELECT 9223372036854775807+1"); check_equal(execute(),ORM_STATUS_OK); count_is(1);
    uint8_t is_null=0; check_equal(orm_result_is_null(result,0,2,&is_null,&error),ORM_STATUS_OK); check_equal(is_null,1);
    text_at(0,11,"No tables used");
  }
  it("retains and releases transaction admission for constant streams and recovers from name errors") {
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    raw_query("SELECT ? AS id,? AS score"); parameter(orm_i64(4)); parameter(orm_i64(40)); open_rows();
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_BUSY); row(4,40); end_rows();
    raw_query("SELECT unit"); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
    raw_query("SELECT 1 AS id,10 AS score"); open_rows(); cflow_publisher_cancel(&publisher);
    orm_tides_public_row value={0}; check_equal(cflow_publisher_resume(&publisher,NULL,&value).kind,CFLOW_STEP_DONE);
    cflow_publisher_destroy(&publisher); publisher=(cflow_publisher){0};
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
  }
  it("returns distinct UNION NULLs merged types and mixed ALL semantics through materialized results") {
    seed(); raw_query("SELECT NULL AS id FROM items UNION SELECT id FROM items UNION ALL SELECT id FROM items WHERE id=2 ORDER BY id");
    check_equal(execute(),ORM_STATUS_OK); count_is(5); uint8_t is_null=0;
    check_equal(orm_result_is_null(result,0,0,&is_null,&error),ORM_STATUS_OK); check_equal(is_null,1);
    const int64_t expected[]={1,2,2,3};
    for(size_t i=0;i<4;++i) { int64_t value=0; check_equal(orm_result_get_int64(result,i+1,0,&value,&error),ORM_STATUS_OK); check_equal(value,expected[i]); }
    raw_query("SELECT id FROM items UNION ALL SELECT id FROM items UNION DISTINCT SELECT id FROM items ORDER BY id");
    check_equal(execute(),ORM_STATUS_OK); count_is(3);
    raw_query("SELECT ? AS label FROM items WHERE id=1 UNION ALL SELECT ? AS second FROM items WHERE id=2");
    parameter(orm_text("first")); parameter(orm_text("second")); check_equal(execute(),ORM_STATUS_OK); count_is(2);
    text_at(0,0,"first"); text_at(1,0,"second");
  }
  it("combines JOIN and aggregate UNION branches in one savepoint snapshot") {
    seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("before_union"),&error),ORM_STATUS_OK);
    raw_query("UPDATE items SET score=99 WHERE id=2"); command(1);
    for(size_t pass=0;pass<2;++pass) {
      raw_query("SELECT a.id,b.score FROM items a JOIN items b ON a.id+1=b.id WHERE a.id=1 UNION ALL SELECT COUNT(*) AS n,MAX(score) AS m FROM items ORDER BY id");
      open_rows(); check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_BUSY);
      row(1,pass?20:99); row(3,pass?30:99); end_rows();
      if(!pass) check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("before_union"),&error),ORM_STATUS_OK);
    }
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
  }
  it("rejects UNION materialization and type errors without losing connection admission") {
    seed(); disconnect(); const orm_option_t option={orm_view("sql_max_materialized_rows"),orm_view("2")};
    check_equal(connect_profile("relational","false",&option,1),ORM_STATUS_OK);
    raw_query("SELECT id FROM items UNION SELECT id FROM items"); check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
    raw_query("SELECT id,score FROM items UNION ALL SELECT id,score FROM items LIMIT 1"); open_rows(); row(1,10); end_rows();
    raw_query("SELECT id FROM items UNION ALL SELECT ? AS n FROM items"); parameter(orm_bool(true));
    check_equal(execute(),ORM_STATUS_UNSUPPORTED); check_null(result);
    raw_query("SELECT id FROM items UNION ALL SELECT id,score FROM items"); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
    raw_query("SELECT id,score FROM items WHERE id=2"); open_rows(); row(2,20); end_rows();
  }
  it("retains UNION branch owners after handles are released and releases cancelled query admission") {
    seed(); raw_query("SELECT id,score FROM items UNION ALL SELECT id,score FROM items"); open_rows();
    cflow_publisher_cancel(&publisher); orm_tides_public_row value={0};
    check_equal(cflow_publisher_resume(&publisher,NULL,&value).kind,CFLOW_STEP_DONE);
    cflow_publisher_destroy(&publisher); publisher=(cflow_publisher){0};
    raw_query("SELECT id,score FROM items WHERE id=1 UNION ALL SELECT id,score FROM items WHERE id=2"); open_rows();
    disconnect(); check_equal(orm_runtime_close(runtime,&error),ORM_STATUS_BUSY); row(1,10); row(2,20); end_rows();
    check_equal(orm_runtime_close(runtime,&error),ORM_STATUS_OK);
  }
  it("returns compound EXPLAIN identifiers and plan stages through the real relational plugin") {
    seed(); raw_query("EXPLAIN FORMAT=TRADITIONAL (SELECT a.id+? AS n FROM items a RIGHT JOIN items b ON a.id+9223372036854775807=b.id LIMIT ?) "
      "UNION ALL SELECT COUNT(*) AS n FROM items UNION SELECT id FROM items ORDER BY n LIMIT ?");
    parameter(orm_i64(INT64_MAX)); parameter(orm_i64(0)); parameter(orm_i64(2));
    check_equal(execute(),ORM_STATUS_OK); count_is(7);
    uint64_t columns=0; check_equal(orm_result_column_count(result,&columns,&error),ORM_STATUS_OK); check_equal(columns,12u);
    const char *types[]={"PRIMARY","PRIMARY","QUERY GROUP","UNION","UNION RESULT","UNION","UNION RESULT"};
    const int64_t ids[]={1,1,0,2,0,3,0};
    for(size_t i=0;i<sizeof(ids)/sizeof(ids[0]);++i) {
      text_at(i,1,types[i]); uint8_t is_null=0;
      check_equal(orm_result_is_null(result,i,0,&is_null,&error),ORM_STATUS_OK); check_equal(is_null,ids[i]?0:1);
      if(ids[i]) { int64_t value=0; check_equal(orm_result_get_int64(result,i,0,&value,&error),ORM_STATUS_OK); check_equal(value,ids[i]); }
    }
    text_at(0,2,"b"); text_at(1,2,"a"); text_at(2,2,"query_group");
    text_at(3,11,"Global aggregate"); text_at(4,11,"UNION ALL");
    text_at(6,11,"Using temporary; Using filesort; Limit; UNION DISTINCT; Using temporary; Using filesort");
    raw_query("SELECT id,score FROM items ORDER BY id"); open_rows(); row(1,10); row(2,20); row(3,30); end_rows();
  }
  it("releases compound EXPLAIN admission after cancellation and binding failures") {
    seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    raw_query("EXPLAIN SELECT id FROM items UNION SELECT id FROM items"); open_rows();
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_BUSY);
    cflow_publisher_cancel(&publisher); orm_tides_public_row value={0};
    check_equal(cflow_publisher_resume(&publisher,NULL,&value).kind,CFLOW_STEP_DONE);
    cflow_publisher_destroy(&publisher); publisher=(cflow_publisher){0};
    raw_query("EXPLAIN SELECT id FROM items UNION SELECT missing FROM items LIMIT 0");
    check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
    raw_query("EXPLAIN SELECT id FROM items UNION ALL SELECT id FROM items LIMIT ?"); parameter(orm_i64(-1));
    check_equal(execute(),ORM_STATUS_TYPE_ERROR); check_null(result);
    raw_query("EXPLAIN SELECT 'a' AS n FROM items UNION SELECT 'b' AS n FROM items");
    check_equal(execute(),ORM_STATUS_UNSUPPORTED); check_null(result);
    raw_query("SELECT id,score FROM items WHERE id=2"); open_rows(); row(2,20); end_rows();
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
  }
  it("streams nested scalar queries with shared transaction data through the real plugin") {
    seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    raw_query("UPDATE items SET score=score+5 WHERE id=3"); command(1);
    raw_query("SELECT id,(SELECT MAX(score)+(SELECT MIN(id) FROM items) FROM items)+? AS score FROM items ORDER BY id");
    parameter(orm_i64(4)); open_rows();
    orm_query_destroy(query); query=NULL;
    row(1,40); row(2,40); row(3,40); end_rows();
    raw_query("SELECT id,(SELECT MAX(score) FROM items UNION ALL SELECT MIN(score) FROM items LIMIT 1) AS score FROM items LIMIT 1");
    open_rows(); row(1,35); end_rows();
    check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
    orm_transaction_destroy(transaction); transaction=NULL;
    raw_query("SELECT id,score FROM items WHERE id=3"); open_rows(); row(3,30); end_rows();
  }
  it("propagates scalar cardinality errors and reuses the connection after result cleanup") {
    seed(); raw_query("SELECT id,CASE WHEN id=1 THEN 7 ELSE (SELECT id FROM items) END AS score FROM items");
    open_rows(); row(1,7);
    orm_tides_public_row value={0}; const cflow_step step=cflow_publisher_resume(&publisher,NULL,&value);
    check_equal(step.kind,CFLOW_STEP_ERROR); check_contains(step.error,"scalar");
    cflow_publisher_destroy(&publisher); publisher=(cflow_publisher){0};
    raw_query("SELECT (SELECT id FROM items) AS n FROM items"); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
    raw_query("SELECT (SELECT id FROM items WHERE FALSE) AS n FROM items LIMIT 1");
    check_equal(execute(),ORM_STATUS_OK); count_is(1);
    uint8_t is_null=0; check_equal(orm_result_is_null(result,0,0,&is_null,&error),ORM_STATUS_OK); check_equal(is_null,1);
    raw_query("UPDATE items SET score=31 WHERE id=3"); command(1);
  }
  it("explains scalar UNION dependencies without running their failing expressions") {
    seed(); raw_query("EXPLAIN SELECT (SELECT id+9223372036854775807 FROM items UNION ALL SELECT id FROM items) AS n FROM items");
    check_equal(execute(),ORM_STATUS_OK); count_is(4);
    const char *kinds[]={"PRIMARY","SUBQUERY","UNION","UNION RESULT"};
    for(uint64_t i=0;i<4;++i) text_at(i,1,kinds[i]);
    int64_t id=0; check_equal(orm_result_get_int64(result,1,0,&id,&error),ORM_STATUS_OK); check_equal(id,2);
    uint8_t is_null=0; check_equal(orm_result_is_null(result,3,0,&is_null,&error),ORM_STATUS_OK); check_equal(is_null,1);
    raw_query("EXPLAIN SELECT (SELECT missing FROM items) AS n FROM items");
    check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
    raw_query("SELECT id,score FROM items LIMIT 1"); open_rows(); row(1,10); end_rows();
  }
  it("filters IN and NOT IN on the transaction snapshot through the real plugin") {
    seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    raw_query("UPDATE items SET score=35 WHERE id=3"); command(1);
    raw_query("SELECT id,score FROM items WHERE score IN (SELECT MAX(score) FROM items UNION SELECT MIN(score) FROM items) ORDER BY id");
    open_rows(); row(1,10); row(3,35); end_rows();
    raw_query("SELECT id,score FROM items WHERE id NOT IN (SELECT id FROM items WHERE score>?) ORDER BY id");
    parameter(orm_i64(15)); open_rows(); row(1,10); end_rows();
    check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
    orm_transaction_destroy(transaction); transaction=NULL;
    raw_query("SELECT id,score FROM items WHERE id IN (SELECT id FROM items WHERE score=30)");
    open_rows(); row(3,30); end_rows();
  }
  it("materializes IN boolean and NULL results and recovers from type rejection") {
    seed(); raw_query("SELECT ? IN (SELECT id FROM items) AS member,99 NOT IN (SELECT NULL FROM items) AS absent,NULL IN (SELECT id FROM items WHERE FALSE) AS empty_set FROM items LIMIT 1");
    parameter(orm_u64(3)); check_equal(execute(),ORM_STATUS_OK); count_is(1);
    orm_value_kind_t kind; check_equal(orm_result_value_kind(result,0,0,&kind,&error),ORM_STATUS_OK); check_equal(kind,ORM_VALUE_BOOLEAN);
    uint8_t is_null=0; check_equal(orm_result_is_null(result,0,1,&is_null,&error),ORM_STATUS_OK); check_equal(is_null,1);
    raw_query("SELECT TRUE OR id IN (SELECT 'bad' FROM items) AS member FROM items");
    check_equal(execute(),ORM_STATUS_UNSUPPORTED); check_null(result);
    raw_query("EXPLAIN SELECT id FROM items WHERE id IN (SELECT id+9223372036854775807 FROM items)");
    check_equal(execute(),ORM_STATUS_OK); count_is(2); text_at(0,1,"PRIMARY"); text_at(1,1,"SUBQUERY");
    raw_query("SELECT id,score FROM items WHERE id IN (SELECT MAX(id) FROM items)"); open_rows(); row(3,30); end_rows();
  }
  it("filters EXISTS and NOT EXISTS on the transaction snapshot through the real plugin") {
    seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    raw_query("UPDATE items SET score=35 WHERE id=3"); command(1);
    raw_query("SELECT id,score FROM items WHERE EXISTS(SELECT *,id+9223372036854775807 FROM items WHERE score=?) ORDER BY id");
    parameter(orm_i64(35)); open_rows(); row(1,10); row(2,20); row(3,35); end_rows();
    raw_query("SELECT id,score FROM items WHERE NOT EXISTS(SELECT NULL FROM items WHERE score=30) AND id=3");
    open_rows(); row(3,35); end_rows();
    check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
    orm_transaction_destroy(transaction); transaction=NULL;
    raw_query("SELECT id,score FROM items WHERE EXISTS(SELECT NULL,NULL FROM items WHERE score=35)");
    open_rows(); end_rows();
    raw_query("SELECT id,score FROM items WHERE EXISTS(SELECT NULL FROM items WHERE score=30) AND id=3");
    open_rows(); row(3,30); end_rows();
  }
  it("materializes EXISTS boolean values and preserves compound cardinality") {
    seed();
    raw_query("SELECT EXISTS(SELECT MAX(id+9223372036854775807) FROM items WHERE FALSE) AS present,NOT EXISTS(SELECT 1 FROM items UNION SELECT 1 FROM items LIMIT 1 OFFSET 1) AS absent FROM items LIMIT 1");
    check_equal(execute(),ORM_STATUS_OK); count_is(1);
    for(uint64_t i=0;i<2;++i) {
      orm_value_kind_t kind; uint8_t is_null=1;
      check_equal(orm_result_value_kind(result,0,i,&kind,&error),ORM_STATUS_OK); check_equal(kind,ORM_VALUE_BOOLEAN);
      check_equal(orm_result_is_null(result,0,i,&is_null,&error),ORM_STATUS_OK); check_equal(is_null,0);
    }
    raw_query("SELECT id,score FROM items WHERE EXISTS(SELECT 1 FROM items UNION ALL SELECT 1 FROM items LIMIT 1 OFFSET 5) AND id=2");
    open_rows(); row(2,20); end_rows();
    raw_query("EXPLAIN SELECT id FROM items WHERE EXISTS(SELECT id+9223372036854775807 FROM items UNION SELECT id FROM items)");
    check_equal(execute(),ORM_STATUS_OK); count_is(4);
    text_at(0,1,"PRIMARY"); text_at(1,1,"SUBQUERY"); text_at(2,1,"UNION"); text_at(3,1,"UNION RESULT");
  }
  it("recovers from EXISTS predicate errors and releases cancelled query leases") {
    seed(); raw_query("SELECT id,score FROM items WHERE EXISTS(SELECT id FROM items WHERE id+9223372036854775807>0)");
    check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
    raw_query("SELECT id,score FROM items WHERE EXISTS(SELECT missing FROM items)");
    check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM items WHERE EXISTS(SELECT NULL FROM items)"); open_rows();
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_BUSY);
    cflow_publisher_destroy(&publisher); publisher=(cflow_publisher){0};
    raw_query("UPDATE items SET score=31 WHERE id=3"); command(1);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
    orm_transaction_destroy(transaction); transaction=NULL;
    raw_query("SELECT id,score FROM items WHERE EXISTS(SELECT NULL FROM items WHERE score=31) AND id=3");
    open_rows(); row(3,31); end_rows();
  }
  it("queries nested derived tables through the plugin on the current transaction snapshot") {
    seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    raw_query("UPDATE items SET score=35 WHERE id=3"); command(1);
    raw_query("SELECT d.id,d.score FROM (SELECT q.id,q.score FROM (SELECT id,score FROM items WHERE score>?) q) d ORDER BY d.id");
    parameter(orm_i64(15)); open_rows(); row(2,20); row(3,35); end_rows();
    check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
    orm_transaction_destroy(transaction); transaction=NULL;
    raw_query("SELECT d.id,d.score FROM (SELECT id,score FROM items WHERE score>20 UNION SELECT id,score FROM items WHERE id=1) d ORDER BY d.id");
    open_rows(); row(1,10); row(3,30); end_rows();
    raw_query("SELECT a.id,d.score FROM items a JOIN (SELECT id,score FROM items WHERE id=2) d ON a.id=d.id");
    open_rows(); row(2,20); end_rows();
  }
  it("explains derived plans and releases failed or cancelled plugin query sources") {
    seed(); raw_query("EXPLAIN SELECT d.n FROM (SELECT id+9223372036854775807 AS n FROM items) d");
    check_equal(execute(),ORM_STATUS_OK); count_is(2); text_at(0,1,"PRIMARY"); text_at(1,1,"DERIVED");
    raw_query("SELECT d.n FROM (SELECT id+9223372036854775807 AS n FROM items) d");
    check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
    raw_query("SELECT d.id FROM (SELECT id,id FROM items) d");
    check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    raw_query("SELECT d.id,d.score FROM (SELECT id,score FROM items) d"); open_rows(); row(1,10);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_BUSY);
    cflow_publisher_destroy(&publisher); publisher=(cflow_publisher){0};
    raw_query("UPDATE items SET score=31 WHERE id=3"); command(1);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
    orm_transaction_destroy(transaction); transaction=NULL;
    raw_query("SELECT d.id,d.score FROM (SELECT id,score FROM items WHERE id=3) d"); open_rows(); row(3,31); end_rows();
  }
  it("requires an explicit iteration bound to admit recursive SQL while retaining ordinary WITH") {
    raw_query("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<3) SELECT n FROM c");
    check_equal(execute(),ORM_STATUS_UNSUPPORTED); check_null(result);
    raw_query("EXPLAIN WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c) SELECT n FROM c");
    check_equal(execute(),ORM_STATUS_UNSUPPORTED); check_null(result);
    raw_query("WITH RECURSIVE c(n) AS (SELECT 1) SELECT n FROM c");
    check_equal(execute(),ORM_STATUS_OK); count_is(1);
    recursive_connection("3");
    raw_query("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<3) SELECT n FROM c");
    check_equal(execute(),ORM_STATUS_OK); count_is(3);
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<3) SELECT n FROM c");
    check_equal(execute(),ORM_STATUS_UNSUPPORTED); check_null(result);
  }
  it("executes recursive JOIN and streaming dependencies with bound parameters through the plugin") {
    seed(); recursive_connection("4");
    raw_query("WITH RECURSIVE c(n) AS (SELECT ? UNION ALL SELECT c.n+d.step FROM c CROSS JOIN (SELECT id AS step FROM items WHERE id=1) d WHERE c.n<?) SELECT c.n AS id,i.score FROM c JOIN items i ON i.id=c.n ORDER BY c.n");
    parameter(orm_i64(1)); parameter(orm_i64(3)); open_rows();
    row(1,10); row(2,20); row(3,30); end_rows();
  }
  it("applies recursive LIMIT OFFSET before exposing typed flow rows") {
    recursive_connection("3");
    raw_query("WITH RECURSIVE c(n) AS (SELECT ? UNION ALL SELECT n+? FROM c LIMIT ? OFFSET ?) SELECT n AS id,n*10 AS score FROM c");
    parameter(orm_i64(1)); parameter(orm_i64(1)); parameter(orm_i64(2)); parameter(orm_i64(2));
    open_rows(); row(3,30); row(4,40); end_rows();
    raw_query("WITH RECURSIVE c(n) AS (SELECT 9223372036854775807+1 UNION ALL SELECT n+1 FROM c LIMIT 0) SELECT n FROM c");
    check_equal(execute(),ORM_STATUS_OK); count_is(0);
  }
  it("retains recursive plans parameters and native owners after query and connection handles are released") {
    seed(); recursive_connection("3");
    raw_query("WITH RECURSIVE c(n) AS (SELECT ? UNION ALL SELECT n+1 FROM c WHERE n<3) SELECT c.n AS id,i.score FROM c JOIN items i ON i.id=c.n ORDER BY c.n");
    parameter(orm_i64(1)); open_rows(); disconnect();
    check_equal(orm_runtime_close(runtime,&error),ORM_STATUS_BUSY);
    row(1,10); row(2,20); row(3,30); end_rows();
    check_equal(orm_runtime_close(runtime,&error),ORM_STATUS_OK);
  }
  it("limits each recursive definition independently and retains repeated cache references") {
    recursive_connection("2");
    raw_query("WITH RECURSIVE a(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM a LIMIT 3),b(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM b LIMIT 3) SELECT a.n AS id,b.n*10 AS score FROM a JOIN b ON a.n=b.n ORDER BY a.n");
    open_rows(); row(1,10); row(2,20); row(3,30); end_rows();
    raw_query("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c LIMIT 3) SELECT n FROM c UNION ALL SELECT n FROM c");
    check_equal(execute(),ORM_STATUS_OK); count_is(6);
  }
  it("reports iteration exhaustion without a partial result and preserves transaction writes") {
    seed(); recursive_connection("1");
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    raw_query("UPDATE items SET score=11 WHERE id=1"); command(1);
    raw_query("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c) SELECT n FROM c");
    check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result); check_contains(error.message,"iteration limit");
    raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,11); end_rows();
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,11); end_rows();
  }
  it("releases recursive flow failure and pre-demand cancellation leases") {
    recursive_connection("1");
    const char sql[]="WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c) SELECT n AS id,n AS score FROM c";
    raw_query(sql); open_rows(); orm_tides_public_row value={0};
    cflow_step step=cflow_publisher_resume(&publisher,NULL,&value);
    check_equal(step.kind,CFLOW_STEP_ERROR); check_contains(step.error,"iteration limit");
    check_equal(cflow_publisher_resume(&publisher,NULL,&value).kind,CFLOW_STEP_ERROR);
    cflow_publisher_destroy(&publisher); publisher=(cflow_publisher){0};
    raw_query(sql); open_rows(); cflow_publisher_cancel(&publisher); cflow_publisher_cancel(&publisher);
    check_equal(cflow_publisher_resume(&publisher,NULL,&value).kind,CFLOW_STEP_DONE);
    cflow_publisher_destroy(&publisher); publisher=(cflow_publisher){0};
    raw_query("SELECT 7 AS id,70 AS score"); open_rows(); row(7,70); end_rows();
  }
  it("captures outer rows in recursive CTEs through the relational driver") {
    seed(); recursive_connection("9");
    raw_query("SELECT o.id,(WITH RECURSIVE c(n) AS (SELECT o.id UNION ALL SELECT n+1 FROM c "
        "WHERE n<o.id+?) SELECT (SELECT MAX(n) FROM c)) AS score FROM items o GROUP BY o.id ORDER BY o.id");
    parameter(orm_i64(2)); open_rows(); row(1,3); row(2,4); row(3,5); end_rows();
  }
  it("captures recursive member rows through nested CTEs in the relational driver") {
    seed(); recursive_connection("9");
    raw_query("SELECT o.id,(WITH RECURSIVE c(n) AS (SELECT (SELECT o.id) UNION ALL "
        "SELECT (WITH d(x) AS (SELECT (SELECT c.n+1)) SELECT (SELECT x FROM d)) FROM c "
        "WHERE (SELECT c.n<o.id+?)) SELECT MAX(n) FROM c) AS score FROM items o GROUP BY o.id ORDER BY o.id");
    parameter(orm_i64(2)); open_rows(); row(1,3); row(2,4); row(3,5); end_rows();
    raw_query("UPDATE items o SET score=(WITH RECURSIVE c(n) AS (SELECT (SELECT o.id) UNION ALL "
        "SELECT (SELECT c.n+1) FROM c WHERE (SELECT c.n<o.id+1)) SELECT MAX(n) FROM c)"); command(3);
    raw_query("SELECT id,score FROM items ORDER BY id"); open_rows(); row(1,2); row(2,3); row(3,4); end_rows();
  }
  it("explains recursive definitions with Recursive and pagination metadata without evaluating rows") {
    recursive_connection("1");
    raw_query("EXPLAIN FORMAT=TRADITIONAL WITH RECURSIVE c(n) AS (SELECT 9223372036854775807+1 UNION ALL SELECT n+1 FROM c LIMIT 9 OFFSET 1) SELECT n FROM c");
    check_equal(execute(),ORM_STATUS_OK);
    uint64_t count=0; check_equal(orm_result_row_count(result,&count,&error),ORM_STATUS_OK);
    size_t recursive=0,pages=0;
    for(uint64_t i=0;i<count;++i) {
      uint8_t is_null=0; check_equal(orm_result_is_null(result,i,11,&is_null,&error),ORM_STATUS_OK); if(is_null) continue;
      vstr extra={0}; check_equal(orm_result_get_text(result,i,11,&extra,&error),ORM_STATUS_OK);
      /* Result text is a length-delimited view; match bounded substrings. */
      const char *needles[]={"Recursive","Offset"};
      for(size_t n=0;n<sizeof(needles)/sizeof(needles[0]);++n) {
        const size_t length=strlen(needles[n]);
        for(size_t j=0;length<=extra.len && j<=extra.len-length;++j) if(!memcmp(extra.data+j,needles[n],length)) {
          if(n) ++pages; else { ++recursive; text_at(i,1,"UNION"); text_at(i,2,"c"); } break;
        }
      }
    }
    check_equal(recursive,1u); check_equal(pages,1u);
  }
  it("retains native transaction snapshots and cursor leases during recursive reads") {
    seed(); recursive_connection("3");
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    raw_query("UPDATE items SET score=35 WHERE id=3"); command(1);
    raw_query("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<3) SELECT c.n AS id,i.score FROM c JOIN items i ON i.id=c.n ORDER BY c.n");
    open_rows(); row(1,10); check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_BUSY);
    row(2,20); row(3,35); end_rows();
    check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
    raw_query("SELECT id,score FROM items WHERE id=3"); open_rows(); row(3,30); end_rows();
  }
  it("rejects invalid duplicate and overflowing recursive iteration configuration") {
    disconnect(); const char *bad[]={"0","-1","+1"," 1","1 ","1.5","","18446744073709551616"};
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
      const orm_option_t option={orm_view(recursive_option),orm_view(bad[i])};
      check_equal(connect_profile("relational","false",&option,1),ORM_STATUS_INVALID_ARGUMENT); check_null(connection);
    }
    const orm_option_t duplicate[]={{orm_view(recursive_option),orm_view("1")},{orm_view(recursive_option),orm_view("2")}};
    check_equal(connect_profile("relational","false",duplicate,2),ORM_STATUS_INVALID_ARGUMENT); check_null(connection);
    const orm_option_t largest={orm_view(recursive_option),orm_view("18446744073709551615")};
    check_equal(connect_profile("relational","false",&largest,1),ORM_STATUS_OK);
    raw_query("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c LIMIT 2) SELECT n FROM c");
    check_equal(execute(),ORM_STATUS_OK); count_is(2);
  }
  it("copies recursive configuration and continues enforcing materialization quotas") {
    disconnect(); char bound[]="1"; const orm_option_t option={orm_view(recursive_option),orm_view(bound)};
    check_equal(connect_profile("relational","false",&option,1),ORM_STATUS_OK); bound[0]='9';
    raw_query("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c LIMIT 3) SELECT n FROM c");
    check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
    disconnect(); const orm_option_t limits[]={{orm_view(recursive_option),orm_view("9")},{orm_view("sql_max_materialized_rows"),orm_view("2")}};
    check_equal(connect_profile("relational","false",limits,2),ORM_STATUS_OK);
    raw_query("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c LIMIT 5) SELECT n FROM c");
    check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
    raw_query("SELECT 7 AS id,70 AS score"); open_rows(); row(7,70); end_rows();
  }
  it("rejects invalid recursive shape and types even when the outer result is unused") {
    recursive_connection("3");
    raw_query("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT SUM(n) FROM c) SELECT n FROM c LIMIT 0");
    check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
    raw_query("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT 'bad' FROM c) SELECT n FROM c LIMIT 0");
    check_equal(execute(),ORM_STATUS_TYPE_ERROR); check_null(result);
    raw_query("EXPLAIN FORMAT=JSON WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c) SELECT n FROM c");
    check_equal(execute(),ORM_STATUS_UNSUPPORTED); check_null(result);
    raw_query("SELECT 7 AS id,70 AS score"); open_rows(); row(7,70); end_rows();
  }
  it("runs named CTE references through the plugin on the current transaction snapshot") {
    seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    raw_query("UPDATE items SET score=35 WHERE id=3"); command(1);
    raw_query("WITH c AS (SELECT id,score FROM items WHERE score>?) SELECT a.id,b.score FROM c a JOIN c b ON a.id=b.id ORDER BY a.id");
    parameter(orm_i64(15)); open_rows(); row(2,20); row(3,35); end_rows();
    check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
    raw_query("WITH c(k,v) AS (SELECT id,score FROM items), d AS (SELECT k,v FROM c WHERE k=3) SELECT k AS id,v AS score FROM d");
    open_rows(); row(3,30); end_rows();
  }
  it("explains CTE definitions once and recovers from failed or cancelled CTE queries") {
    seed(); raw_query("EXPLAIN WITH c AS (SELECT id+9223372036854775807 AS n FROM items) SELECT a.n FROM c a JOIN c b ON a.n=b.n");
    check_equal(execute(),ORM_STATUS_OK); count_is(3);
    text_at(0,1,"PRIMARY"); text_at(1,1,"PRIMARY"); text_at(2,1,"DERIVED");
    raw_query("WITH c AS (SELECT id+9223372036854775807 AS n FROM items) SELECT n FROM c");
    check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
    raw_query("WITH c(x) AS (SELECT id,score FROM items) SELECT x FROM c"); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    raw_query("WITH c AS (SELECT id,score FROM items) SELECT id,score FROM c ORDER BY id"); open_rows(); row(1,10);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_BUSY);
    cflow_publisher_destroy(&publisher); publisher=(cflow_publisher){0};
    raw_query("UPDATE items SET score=31 WHERE id=3"); command(1);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
    raw_query("WITH c AS (SELECT id,score FROM items WHERE id=3) SELECT id,score FROM c"); open_rows(); row(3,31); end_rows();
  }
  it("returns TRADITIONAL EXPLAIN columns for the real plan without evaluating the SELECT") {
    seed(); raw_query("EXPLAIN SELECT id+9223372036854775807 AS n FROM items t WHERE id>0 ORDER BY n LIMIT 2");
    check_equal(execute(),ORM_STATUS_OK); count_is(1);
    uint64_t columns=0; check_equal(orm_result_column_count(result,&columns,&error),ORM_STATUS_OK); check_equal(columns,12u);
    int64_t id=0; check_equal(orm_result_get_int64(result,0,0,&id,&error),ORM_STATUS_OK); check_equal(id,1);
    text_at(0,1,"SIMPLE"); text_at(0,2,"t"); text_at(0,4,"ALL");
    text_at(0,11,"Using where; Using temporary; Using filesort; Limit");
    uint8_t is_null=0; check_equal(orm_result_is_null(result,0,9,&is_null,&error),ORM_STATUS_OK); check_equal(is_null,1);
    check_equal(orm_result_is_null(result,0,10,&is_null,&error),ORM_STATUS_OK); check_equal(is_null,1);
    raw_query("EXPLAIN FORMAT=TRADITIONAL SELECT COUNT(*) AS n FROM items LIMIT ?"); parameter(orm_i64(0));
    check_equal(execute(),ORM_STATUS_OK); text_at(0,11,"Zero limit; Global aggregate");
    check_equal(orm_result_get_int64(result,0,9,&id,&error),ORM_STATUS_OK); check_equal(id,0);
    raw_query("EXPLAIN SELECT score AS k,COUNT(*) AS n FROM items GROUP BY k HAVING n>0");
    check_equal(execute(),ORM_STATUS_OK); text_at(0,11,"Using temporary; Using filesort; Group aggregate; Using having");
    raw_query("SELECT id,score FROM items ORDER BY id"); open_rows(); row(1,10); row(2,20); row(3,30); end_rows();
  }
  it("retains EXPLAIN query admission in transactions and rejects formats and DML without side effects") {
    seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    raw_query("EXPLAIN SELECT id FROM items"); open_rows();
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_BUSY);
    cflow_publisher_destroy(&publisher); publisher=(cflow_publisher){0};
    const char *sql[]={"EXPLAIN DELETE FROM items","EXPLAIN UPDATE items SET score=0","EXPLAIN FORMAT=TREE SELECT id FROM items","EXPLAIN FORMAT=JSON SELECT id FROM items"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      raw_query(sql[i]); check_equal(execute(),ORM_STATUS_UNSUPPORTED); check_null(result);
    }
    raw_query("EXPLAIN SELECT missing FROM items"); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
    raw_query("EXPLAIN SELECT id FROM items LIMIT ?"); parameter(orm_i64(-1));
    check_equal(execute(),ORM_STATUS_TYPE_ERROR); check_null(result);
    raw_query("SELECT id,score FROM items ORDER BY id"); open_rows(); row(1,10); row(2,20); row(3,30); end_rows();
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
  }

  it("groups stored expressions by aliases or positions and exposes typed flow results") {
    seed(); raw_query("INSERT INTO items(id,score) VALUES(4,10),(5,NULL)"); command(2);
    raw_query("SELECT COALESCE(score,10) AS id,COUNT(*) AS score FROM items GROUP BY 1 HAVING id>=10 ORDER BY id DESC LIMIT ? OFFSET ?");
    parameter(orm_i64(2)); parameter(orm_i64(1)); open_rows(); row(20,1); row(10,3); end_rows();
    raw_query("SELECT CASE WHEN score>=20 THEN 1 ELSE 0 END AS id,COUNT(*) AS score FROM items GROUP BY 1 ORDER BY id");
    open_rows(); row(0,3); row(1,2); end_rows();
    raw_query("SELECT COALESCE(score,10) AS bucket,COUNT(*) AS n FROM items GROUP BY bucket HAVING bucket=10");
    check_equal(execute(),ORM_STATUS_OK); count_is(1);
    int64_t value=0; check_equal(orm_result_get_int64(result,0,1,&value,&error),ORM_STATUS_OK); check_equal(value,3);
    raw_query("SELECT score+1 AS k FROM items GROUP BY items.score+01 ORDER BY score+1");
    check_equal(execute(),ORM_STATUS_OK); count_is(4);
  }
  it("keeps expression grouping on the savepoint snapshot and rejects ambiguous or aggregate keys") {
    seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("before"),&error),ORM_STATUS_OK);
    raw_query("UPDATE items SET score=NULL WHERE id=3"); command(1);
    raw_query("SELECT COALESCE(score,10) AS id,COUNT(*) AS score FROM items GROUP BY 1 ORDER BY id");
    open_rows(); row(10,2);
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("before"),&error),ORM_STATUS_BUSY);
    row(20,1); end_rows();
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("before"),&error),ORM_STATUS_OK);
    raw_query("SELECT COALESCE(score,10) AS id,COUNT(*) AS score FROM items GROUP BY 1 ORDER BY id");
    open_rows(); row(10,1); row(20,1); row(30,1); end_rows();
    raw_query("SELECT id>2 AS score,COUNT(*) AS n FROM items GROUP BY score");
    check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
    raw_query("SELECT COUNT(*) AS n FROM items GROUP BY n"); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
    raw_query("SELECT score FROM items GROUP BY 2"); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
    raw_query("SELECT score AS id,COUNT(*) AS score FROM items GROUP BY 1 ORDER BY id");
    open_rows(); row(10,1); row(20,1); row(30,1); end_rows();
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
  }

  it("groups nullable stored values and exposes aggregate results through materialized results and flows") {
    seed(); raw_query("INSERT INTO items(id,score) VALUES(4,10),(5,NULL),(6,NULL)"); command(3);
    raw_query("SELECT score,COUNT(*) AS n,COUNT(score) AS present,MAX(id) AS hi FROM items GROUP BY score HAVING n>1 ORDER BY hi DESC");
    check_equal(execute(),ORM_STATUS_OK); count_is(2);
    uint8_t is_null=0; check_equal(orm_result_is_null(result,0,0,&is_null,&error),ORM_STATUS_OK); check_equal(is_null,1);
    const int64_t expected[][3]={{2,0,6},{2,2,4}};
    for(size_t i=0;i<2;++i) for(size_t j=0;j<3;++j) {
      int64_t value=0; check_equal(orm_result_get_int64(result,i,j+1,&value,&error),ORM_STATUS_OK);
      check_equal(value,expected[i][j]);
    }
    raw_query("SELECT score AS id,COUNT(*) AS score FROM items WHERE score IS NOT NULL GROUP BY score ORDER BY id DESC LIMIT ? OFFSET ?");
    parameter(orm_i64(2)); parameter(orm_i64(1)); open_rows(); row(20,1); row(10,2); end_rows();
    raw_query("SELECT COUNT(*) AS id,MAX(score) AS score FROM items"); open_rows(); row(6,30); end_rows();
    raw_query("SELECT COUNT(*) AS n,MIN(score) AS lo FROM items WHERE FALSE"); check_equal(execute(),ORM_STATUS_OK); count_is(1);
    int64_t value=-1; check_equal(orm_result_get_int64(result,0,0,&value,&error),ORM_STATUS_OK); check_equal(value,0);
    check_equal(orm_result_is_null(result,0,1,&is_null,&error),ORM_STATUS_OK); check_equal(is_null,1);
  }
  it("aggregates the transaction snapshot and retains the query lease across savepoint controls") {
    seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("before"),&error),ORM_STATUS_OK);
    raw_query("UPDATE items SET score=10 WHERE id=3"); command(1);
    raw_query("SELECT score AS id,COUNT(*) AS score FROM items GROUP BY score ORDER BY id"); open_rows(); row(10,2);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_BUSY);
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("before"),&error),ORM_STATUS_BUSY);
    row(20,1); end_rows();
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("before"),&error),ORM_STATUS_OK);
    raw_query("SELECT score AS id,COUNT(*) AS score FROM items GROUP BY score ORDER BY id");
    open_rows(); row(10,1); row(20,1); row(30,1); end_rows();
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SELECT COUNT(*) AS id,MAX(score) AS score FROM items"); open_rows(); row(3,30); end_rows();
  }
  it("aggregates persisted doubles through savepoint rollback commit and reopen") {
    seed_doubles();
    raw_query("SELECT SUM(score) AS total,AVG(score) AS mean FROM metrics");
    check_equal(execute(),ORM_STATUS_OK); count_is(1); double_at(0,0,12.0); double_at(0,1,4.0);
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("before"),&error),ORM_STATUS_OK);
    raw_query("UPDATE metrics SET score=? WHERE id=3"); parameter(orm_f64(20.0)); command(1);
    raw_query("SELECT SUM(score) AS total,AVG(score) AS mean FROM metrics");
    check_equal(execute(),ORM_STATUS_OK); double_at(0,0,24.0); double_at(0,1,8.0);
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("before"),&error),ORM_STATUS_OK);
    raw_query("SELECT SUM(score) AS total,AVG(score) AS mean FROM metrics");
    check_equal(execute(),ORM_STATUS_OK); double_at(0,0,12.0); double_at(0,1,4.0);
    raw_query("UPDATE metrics SET score=? WHERE id=4"); parameter(orm_f64(4.0)); command(1);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SELECT SUM(score) AS total,AVG(score) AS mean FROM metrics");
    check_equal(execute(),ORM_STATUS_OK); double_at(0,0,16.0); double_at(0,1,4.0);
  }
  it("groups DOUBLE sums and averages with HAVING ordering and empty result semantics") {
    seed_doubles();
    raw_query("SELECT id>2 AS k,SUM(score) AS total,AVG(score) AS mean FROM metrics "
      "GROUP BY k HAVING mean>? ORDER BY total DESC"); parameter(orm_f64(1.0));
    check_equal(execute(),ORM_STATUS_OK); count_is(2);
    double_at(0,1,8.0); double_at(0,2,8.0); double_at(1,1,4.0); double_at(1,2,2.0);
    const char *sql[]={"SELECT SUM(score) AS s,AVG(score) AS a FROM metrics WHERE FALSE",
      "SELECT SUM(score) AS s,AVG(score) AS a FROM metrics WHERE score IS NULL"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      raw_query(sql[i]); check_equal(execute(),ORM_STATUS_OK); count_is(1);
      for(uint64_t column=0;column<2;++column) {
        uint8_t is_null=0; check_equal(orm_result_is_null(result,0,column,&is_null,&error),ORM_STATUS_OK);
        check_equal(is_null,1);
      }
    }
    raw_query("SELECT SUM(id) AS s FROM metrics LIMIT 0");
    check_equal(execute(),ORM_STATUS_UNSUPPORTED); check_contains(error.message,"DECIMAL"); check_null(result);
    raw_query("SELECT AVG(id) AS a FROM metrics WHERE FALSE");
    check_equal(execute(),ORM_STATUS_UNSUPPORTED); check_contains(error.message,"DECIMAL"); check_null(result);
  }
  it("explains overflowing DOUBLE aggregates without execution and recovers after query failure") {
    seed_doubles();
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("before"),&error),ORM_STATUS_OK);
    raw_query("UPDATE metrics SET score=? WHERE id<3"); parameter(orm_f64(DBL_MAX)); command(2);
    raw_query("EXPLAIN SELECT SUM(score) AS s,AVG(score) AS a FROM metrics");
    check_equal(execute(),ORM_STATUS_OK); count_is(1); text_at(0,11,"Global aggregate");
    raw_query("SELECT SUM(score) AS s,AVG(score) AS a FROM metrics");
    check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("before"),&error),ORM_STATUS_OK);
    raw_query("SELECT SUM(score) AS s,AVG(score) AS a FROM metrics");
    check_equal(execute(),ORM_STATUS_OK); double_at(0,0,12.0); double_at(0,1,4.0);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
  }
  it("fails grouped result capacity explicitly and releases the failed query for reuse") {
    seed(); disconnect(); const orm_option_t limit={orm_view("sql_max_groups"),orm_view("2")};
    check_equal(connect_profile("relational","false",&limit,1),ORM_STATUS_OK);
    raw_query("SELECT score,COUNT(*) AS n FROM items GROUP BY score ORDER BY score");
    check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
    raw_query("SELECT score AS id,COUNT(*) AS score FROM items GROUP BY score ORDER BY id"); open_rows();
    orm_tides_public_row value={0}; check_equal(cflow_publisher_resume(&publisher,NULL,&value).kind,CFLOW_STEP_ERROR);
    cflow_publisher_destroy(&publisher); publisher=(cflow_publisher){0};
    raw_query("SELECT COUNT(*) AS id,MAX(score) AS score FROM items"); open_rows(); row(3,30); end_rows();
    raw_query("SELECT id,COUNT(*) AS n FROM items LIMIT 0"); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
  }

  it("deduplicates nullable outputs before ordering and pagination through materialized results and flows") {
    seed(); raw_query("INSERT INTO items(id,score) VALUES(4,10),(5,NULL),(6,NULL)"); command(3);
    raw_query("SELECT DISTINCT score FROM items ORDER BY score DESC"); check_equal(execute(),ORM_STATUS_OK); count_is(4);
    const int64_t expected[]={30,20,10};
    for(size_t i=0;i<3;++i) {
      int64_t score=0; check_equal(orm_result_get_int64(result,i,0,&score,&error),ORM_STATUS_OK); check_equal(score,expected[i]);
    }
    uint8_t is_null=0; check_equal(orm_result_is_null(result,3,0,&is_null,&error),ORM_STATUS_OK); check_equal(is_null,1);
    raw_query("SELECT DISTINCT score AS id,score FROM items WHERE score IS NOT NULL ORDER BY 2 DESC LIMIT ? OFFSET ?");
    parameter(orm_i64(2)); parameter(orm_i64(1)); open_rows(); row(20,20); row(10,10); end_rows();
    raw_query("SELECT DISTINCT id,score FROM items ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(6);
    raw_query("SELECT DISTINCT score FROM items ORDER BY id LIMIT 0"); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
    raw_query("SELECT DISTINCT score+1 AS k FROM items ORDER BY items.score+1 DESC");
    check_equal(execute(),ORM_STATUS_OK); count_is(4);
  }
  it("keeps DISTINCT on the transaction snapshot and holds its cursor lease until close") {
    seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("before"),&error),ORM_STATUS_OK);
    raw_query("UPDATE items SET score=10 WHERE id=3"); command(1);
    raw_query("SELECT DISTINCT score AS id,score FROM items ORDER BY score DESC"); open_rows(); row(20,20);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_BUSY);
    row(10,10); end_rows();
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("before"),&error),ORM_STATUS_OK);
    raw_query("SELECT DISTINCT score FROM items"); check_equal(execute(),ORM_STATUS_OK); count_is(3);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SELECT DISTINCT score FROM items"); check_equal(execute(),ORM_STATUS_OK); count_is(3);
  }
  it("rejects DISTINCT candidate quota exhaustion and late projection errors without partial output") {
    seed(); raw_query("UPDATE items SET score=10"); command(2); disconnect();
    const orm_option_t limit={orm_view("sql_max_materialized_rows"),orm_view("2")};
    check_equal(connect_profile("relational","false",&limit,1),ORM_STATUS_OK);
    raw_query("SELECT DISTINCT score FROM items LIMIT 1"); check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
    raw_query("SELECT DISTINCT score AS id,score FROM items"); open_rows(); orm_tides_public_row value={0};
    check_equal(cflow_publisher_resume(&publisher,NULL,&value).kind,CFLOW_STEP_ERROR);
    cflow_publisher_destroy(&publisher); publisher=(cflow_publisher){0}; disconnect();
    check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SELECT DISTINCT CASE WHEN id=3 THEN id+9223372036854775807 ELSE 0 END AS k FROM items LIMIT 1");
    check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
    raw_query("SELECT DISTINCT score AS id,score FROM items"); open_rows(); row(10,10); end_rows();
  }
  it("fails result row and byte limits rather than returning a successful prefix") {
    seed(); disconnect(); config.max_result_rows = 1;
    check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM items"); open_rows(); row(1,10);
    orm_tides_public_row value = {0}; cflow_step step = cflow_publisher_resume(&publisher,NULL,&value);
    check_equal(step.kind,CFLOW_STEP_ERROR); check_contains(step.error,"row limit");
    cflow_publisher_destroy(&publisher); publisher = (cflow_publisher){0}; disconnect();
    config.max_result_rows = 10; config.max_result_bytes = 22;
    check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM items LIMIT 1"); check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
  }
  it("resets statement budgets while preserving cumulative transaction write limits") {
    seed(); disconnect();
    orm_option_t limits[] = {
      {orm_view("sql_max_write_bytes"),orm_view("128")},
      {orm_view("sql_max_transaction_write_bytes"),orm_view("128")}
    };
    check_equal(connect_profile("relational","false",limits,2),ORM_STATUS_OK);
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("before_update"),&error),ORM_STATUS_OK);
    raw_query("UPDATE items SET score=99 WHERE id=1"); command(1);
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("before_update"),&error),ORM_STATUS_OK);
    raw_query("UPDATE items SET score=98 WHERE id=2"); check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
    check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
    raw_query("UPDATE items SET score=97 WHERE id=3"); command(1);
    raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,10); end_rows();
  }
  it("rejects duplicate invalid and legacy-only options and refuses reinitialization") {
    disconnect(); check_equal(connect_profile("relational","true",NULL,0),ORM_STATUS_INVALID_STATE); check_null(connection);
    const orm_option_t invalid[] = {
      {orm_view("sql_profile"),orm_view("relational")}, {orm_view("sql_max_work_bytes"),orm_view("0")},
      {orm_view("sql_max_work_bytes"),orm_view("18446744073709551616")},
      {orm_view("sql_max_stack_entries"),orm_view("1")}, {orm_view("sql_max_transaction_write_bytes"),orm_view("1")},
      {orm_view("sql_max_savepoints"),orm_view("0")}, {orm_view("sql_max_savepoints"),orm_view("2147483647")},
      {orm_view("sql_max_warnings"),orm_view("0")}, {orm_view("sql_max_warnings"),orm_view("65536")},
      {orm_view(found_rows_option),orm_view("yes")},
      {orm_view("key_prefix"),orm_view("legacy:")}, {orm_view("typo"),orm_view("1")}
    };
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
      check_equal(connect_profile("relational","false",&invalid[i],1),ORM_STATUS_INVALID_ARGUMENT); check_null(connection);
    }
    check_equal(connect_profile("mysql","false",NULL,0),ORM_STATUS_INVALID_ARGUMENT);
    check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK); seed();
  }
  it("enforces a small work budget and recovers after a failed query open") {
    seed(); disconnect(); orm_option_t limit = {orm_view("sql_max_work_bytes"),orm_view("2048")};
    check_equal(connect_profile("relational","false",&limit,1),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM items"); check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,10); end_rows();
  }
  it("requires an explicit initialized CF and keeps explicit legacy routing usable") {
    disconnect();
    orm_option_t options[] = {
      {orm_view("path"),orm_view(directory)}, {orm_view("sql_profile"),orm_view("relational")},
      {orm_view("column_family"),orm_view("absent")}
    };
    config.driver = orm_view("tidesdb"); config.options = options; config.option_count = 2;
    check_equal(orm_runtime_connect(runtime,&config,&connection,&error),ORM_STATUS_INVALID_ARGUMENT); check_null(connection);
    config.option_count = 3;
    check_equal(orm_runtime_connect(runtime,&config,&connection,&error),ORM_STATUS_INVALID_STATE); check_null(connection);
    options[1].value = orm_view("legacy"); options[2].value = orm_view("rel");
    check_equal(orm_runtime_connect(runtime,&config,&connection,&error),ORM_STATUS_INVALID_STATE); check_null(connection);
    options[1].value = orm_view("legacy"); options[2].value = orm_view("old");
    check_equal(orm_runtime_connect(runtime,&config,&connection,&error),ORM_STATUS_OK);
    raw_query("INSERT INTO people(id,score) VALUES(1,7)"); command(1); disconnect();
    options[1].value = orm_view("relational");
    check_equal(orm_runtime_connect(runtime,&config,&connection,&error),ORM_STATUS_INVALID_STATE); check_null(connection);
    options[1].value = orm_view("legacy");
    check_equal(orm_runtime_connect(runtime,&config,&connection,&error),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM people"); open_rows(); row(1,7); end_rows();
    config.options = NULL; config.option_count = 0;
  }
  group("DOUBLE secondary indexes through the real plugin") {
    it("maintains unique DOUBLE keys across zero signs structured updates REPLACE and reopen") {
      raw_query("CREATE TABLE reals(id BIGINT PRIMARY KEY,score DOUBLE UNIQUE)"); command(0);
      raw_query("INSERT INTO reals VALUES(1,-0.0),(2,1.5),(3,NULL),(4,NULL)"); command(4);
      raw_query("INSERT INTO reals VALUES(5,0.0)"); check_equal(execute(),ORM_STATUS_CONSTRAINT); check_null(result);
      raw_query("INSERT IGNORE INTO reals VALUES(5,0.0),(6,2.5)"); command(1);
      raw_query("REPLACE INTO reals VALUES(5,0.0)"); command(2);
      structured(STRUCT_UPDATE,"reals"); assign("score",orm_f64(3.5)); where("id",ORM_COMPARE_EQUAL,orm_i64(2)); command(1);
      raw_query("SELECT id,score FROM reals WHERE score=3.5"); check_equal(execute(),ORM_STATUS_OK);
      count_is(1); integer_at(0,0,2); double_at(0,1,3.5);
      raw_query("SELECT id FROM reals WHERE score=0 ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(1); integer_at(0,0,5);
      raw_query("SELECT id FROM reals WHERE score IS NULL ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(2); integer_at(0,0,3); integer_at(1,0,4);
      disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("EXPLAIN SELECT id FROM reals WHERE score=3.5"); check_equal(execute(),ORM_STATUS_OK); text_at(0,4,"const"); text_at(0,6,"score");
      raw_query("SELECT id,score FROM reals WHERE score>=2.0 ORDER BY score"); check_equal(execute(),ORM_STATUS_OK);
      count_is(2); integer_at(0,0,6); double_at(0,1,2.5); integer_at(1,0,2); double_at(1,1,3.5);
    }
    it("uses DOUBLE range and multirange access with typed parameters and integer literals") {
      raw_query("CREATE TABLE reals(id BIGINT PRIMARY KEY,score DOUBLE)"); command(0);
      raw_query("INSERT INTO reals VALUES(1,?),(2,-0.0),(3,0.0),(4,?),(5,?)");
      parameter(orm_f64(-DBL_MAX)); parameter(orm_f64(DBL_TRUE_MIN)); parameter(orm_f64(DBL_MAX)); command(5);
      raw_query("CREATE INDEX by_real ON reals(score DESC)"); command(0);
      raw_query("EXPLAIN SELECT id FROM reals WHERE score>?"); parameter(orm_f64(-DBL_TRUE_MIN));
      check_equal(execute(),ORM_STATUS_OK); text_at(0,4,"range"); text_at(0,6,"by_real");
      raw_query("SELECT id FROM reals WHERE score>? AND score<? ORDER BY id");
      parameter(orm_f64(-DBL_TRUE_MIN)); parameter(orm_f64(DBL_TRUE_MIN));
      check_equal(execute(),ORM_STATUS_OK); count_is(2); integer_at(0,0,2); integer_at(1,0,3);
      raw_query("SELECT id FROM reals WHERE score IN(0,?) OR score BETWEEN ? AND ? ORDER BY id");
      parameter(orm_f64(DBL_TRUE_MIN)); parameter(orm_f64(DBL_MAX)); parameter(orm_f64(DBL_MAX));
      check_equal(execute(),ORM_STATUS_OK); count_is(4); integer_at(0,0,2); integer_at(1,0,3); integer_at(2,0,4); integer_at(3,0,5);
      raw_query("SELECT id FROM reals WHERE score>? ORDER BY id"); parameter(orm_f64(DBL_MAX)); check_equal(execute(),ORM_STATUS_OK); count_is(0);
      raw_query("SELECT id FROM reals WHERE score<? ORDER BY id"); parameter(orm_f64(-DBL_MAX)); check_equal(execute(),ORM_STATUS_OK); count_is(0);
    }
    it("rolls back DOUBLE index DDL and writes through SQL savepoints and transactions") {
      raw_query("CREATE TABLE reals(id BIGINT PRIMARY KEY,score DOUBLE)"); command(0);
      raw_query("INSERT INTO reals VALUES(1,1.5),(2,2.5)"); command(2);
      raw_query("BEGIN"); command(0); raw_query("SAVEPOINT before_index"); command(0);
      raw_query("CREATE UNIQUE INDEX ux ON reals(score)"); command(0);
      raw_query("UPDATE reals SET score=4.5 WHERE id=1"); command(1);
      raw_query("ROLLBACK TO before_index"); command(0);
      raw_query("EXPLAIN SELECT id FROM reals WHERE score=1.5"); check_equal(execute(),ORM_STATUS_OK); text_at(0,4,"ALL");
      raw_query("CREATE UNIQUE INDEX ux ON reals(score DESC)"); command(0);
      raw_query("COMMIT"); command(0);
      raw_query("BEGIN"); command(0); raw_query("SAVEPOINT keep_index"); command(0);
      raw_query("DROP INDEX ux ON reals"); command(0); raw_query("TRUNCATE reals"); command(0);
      raw_query("ROLLBACK TO keep_index"); command(0); raw_query("COMMIT"); command(0);
      disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("EXPLAIN SELECT id FROM reals WHERE score=1.5"); check_equal(execute(),ORM_STATUS_OK); text_at(0,4,"const"); text_at(0,6,"ux");
      raw_query("SELECT id,score FROM reals ORDER BY id"); check_equal(execute(),ORM_STATUS_OK); count_is(2); double_at(0,1,1.5); double_at(1,1,2.5);
    }
    it("retains scan semantics for integer columns compared with rounded DOUBLE probes") {
      raw_query("CREATE TABLE numbers(id BIGINT PRIMARY KEY,score BIGINT,real_value DOUBLE)"); command(0);
      raw_query("INSERT INTO numbers VALUES(1,9007199254740992,1.0),(2,9007199254740993,2.0)"); command(2);
      raw_query("CREATE INDEX by_integer ON numbers(score)"); command(0);
      raw_query("CREATE INDEX by_real ON numbers(real_value)"); command(0);
      raw_query("EXPLAIN SELECT id FROM numbers WHERE score IN(?)"); parameter(orm_f64(9007199254740992.0));
      check_equal(execute(),ORM_STATUS_OK); text_at(0,4,"ALL");
      raw_query("SELECT id FROM numbers WHERE score IN(?) ORDER BY id"); parameter(orm_f64(9007199254740992.0));
      check_equal(execute(),ORM_STATUS_OK); count_is(2); integer_at(0,0,1); integer_at(1,0,2);
      raw_query("SELECT id FROM numbers WHERE score=9007199254740992.0 ORDER BY id");
      check_equal(execute(),ORM_STATUS_OK); count_is(2); integer_at(0,0,1); integer_at(1,0,2);
    }
  }
  group("Connection-owned SQL transaction lifecycle") {
    it("shares raw and structured statements until SQL commit or rollback") {
      seed(); raw_query("BEGIN WORK"); command(0);
      raw_query("SAVEPOINT sql_start"); command(0);
      structured(STRUCT_INSERT,"items"); assign("id",orm_i64(4)); assign("score",orm_i64(40)); command(1);
      raw_query("UPDATE items SET score=99 WHERE id=1"); command(1);
      raw_query("ROLLBACK TO sql_start"); command(0);
      structured(STRUCT_UPDATE,"items"); assign("score",orm_i64(11)); where("id",ORM_COMPARE_EQUAL,orm_i64(1)); command(1);
      raw_query("COMMIT WORK AND NO CHAIN NO RELEASE"); command(0);
      raw_query("UPDATE items SET score=22 WHERE id=2"); command(1);
      raw_query("ROLLBACK WORK"); command(0);
      raw_query("START TRANSACTION READ WRITE"); command(0);
      raw_query("INSERT INTO items VALUES(4,40)"); command(1);
      raw_query("ROLLBACK AND NO CHAIN NO RELEASE"); command(0);
      disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("SELECT id,score FROM items ORDER BY id"); open_rows(); row(1,11); row(2,22); row(3,30); end_rows();
    }
    it("uses the same SQL snapshot through grouping windows CTEs and savepoint rollback") {
      seed(); raw_query("START TRANSACTION"); command(0);
      raw_query("SAVEPOINT before_change"); command(0);
      raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      raw_query("WITH q AS (SELECT score FROM items) SELECT SUM(CAST(score AS DOUBLE)) AS total FROM q");
      check_equal(execute(),ORM_STATUS_OK); double_at(0,0,90.0);
      raw_query("SELECT id,SUM(CAST(score AS DOUBLE)) OVER() AS total FROM items ORDER BY id");
      check_equal(execute(),ORM_STATUS_OK); count_is(3);
      for(uint64_t i=0;i<3;++i) { integer_at(i,0,(int64_t)i+1); double_at(i,1,90.0); }
      raw_query("EXPLAIN SELECT id FROM items WHERE id=1"); check_equal(execute(),ORM_STATUS_OK); count_is(1);
      raw_query("ROLLBACK TO before_change"); command(0);
      raw_query("SELECT SUM(CAST(score AS DOUBLE)) AS total FROM items"); check_equal(execute(),ORM_STATUS_OK); double_at(0,0,60.0);
      raw_query("COMMIT"); command(0);
    }
    it("enforces READ ONLY for every table and index write and preserves access mode through CHAIN") {
      seed(); raw_query("CREATE INDEX readonly_index ON items(score)"); command(0);
      raw_query("START TRANSACTION READ ONLY"); command(0);
      raw_query("SAVEPOINT readonly_point"); command(0);
      const char *const writes[]={"UPDATE items SET score=40 WHERE id=1", "INSERT INTO items VALUES(4,40)",
        "REPLACE INTO items VALUES(1,40)", "DELETE FROM items", "CREATE TABLE denied(id BIGINT PRIMARY KEY)",
        "DROP TABLE items", "TRUNCATE TABLE items", "ALTER TABLE items ADD n BIGINT",
        "CREATE INDEX denied ON items(score)", "DROP INDEX readonly_index ON items",
        "INSERT INTO items WITH q AS (SELECT 4 AS id,40 AS score) SELECT id,score FROM q",
        "WITH q AS (SELECT 1 AS id) UPDATE items SET score=40 WHERE id IN(SELECT id FROM q)",
        "WITH q AS (SELECT 1 AS id) DELETE FROM items WHERE id IN(SELECT id FROM q)"};
      for(size_t i=0;i<sizeof(writes)/sizeof(writes[0]);++i) {
        raw_query(writes[i]); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
        check_not_null(strstr(error.message,"READ ONLY"));
      }
      structured(STRUCT_UPDATE,"items"); assign("score",orm_i64(40)); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("ROLLBACK TO readonly_point"); command(0);
      raw_query("COMMIT AND CHAIN NO RELEASE"); command(0);
      raw_query("UPDATE items SET score=40 WHERE id=1"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("ROLLBACK AND CHAIN"); command(0);
      raw_query("INSERT INTO items VALUES(4,40)"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("BEGIN"); command(0);
      raw_query("UPDATE items SET score=40 WHERE id=1"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("START TRANSACTION READ WRITE"); command(0);
      raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      raw_query("ROLLBACK"); command(0);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,10); end_rows();
    }
    it("implicitly commits on repeated BEGIN and does not copy savepoints into the new transaction") {
      seed(); raw_query("BEGIN"); command(0);
      raw_query("SAVEPOINT old_point"); command(0);
      raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      raw_query("START TRANSACTION READ WRITE"); command(0);
      raw_query("ROLLBACK TO old_point"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("UPDATE items SET score=50 WHERE id=1"); command(1);
      raw_query("ROLLBACK"); command(0);
      raw_query("COMMIT AND CHAIN"); command(0);
      raw_query("SAVEPOINT chained"); command(0);
      raw_query("INSERT INTO items VALUES(4,40)"); command(1);
      raw_query("ROLLBACK AND CHAIN"); command(0);
      raw_query("RELEASE SAVEPOINT chained"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("COMMIT"); command(0);
      disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("SELECT id,score FROM items ORDER BY id"); open_rows(); row(1,40); row(2,20); row(3,30); end_rows();
    }
    it("rejects unsupported characteristics and parameters before any implicit commit") {
      seed(); raw_query("BEGIN"); command(0);
      raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      const char *const unsupported[]={"START TRANSACTION WITH CONSISTENT SNAPSHOT",
        "START TRANSACTION READ ONLY,WITH CONSISTENT SNAPSHOT", "COMMIT RELEASE", "ROLLBACK RELEASE",
        "SET GLOBAL autocommit=0", "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ"};
      for(size_t i=0;i<sizeof(unsupported)/sizeof(unsupported[0]);++i) {
        raw_query(unsupported[i]); check_equal(execute(),ORM_STATUS_UNSUPPORTED); check_null(result);
      }
      const char *const bound[]={"BEGIN", "START TRANSACTION READ ONLY", "COMMIT", "ROLLBACK", "COMMIT AND CHAIN"};
      for(size_t i=0;i<sizeof(bound)/sizeof(bound[0]);++i) {
        raw_query(bound[i]); parameter(orm_i64(1)); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
      }
      raw_query("COMMIT AND CHAIN RELEASE"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("COMMIT; BEGIN"); check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,40); end_rows();
      raw_query("ROLLBACK"); command(0);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,10); end_rows();
    }
    it("keeps live cursor admission and ORM transactions isolated from SQL lifecycle") {
      seed(); raw_query("BEGIN"); command(0);
      check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_BUSY); check_null(transaction);
      raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows();
      const char *const busy[]={"BEGIN", "START TRANSACTION", "COMMIT", "ROLLBACK", "DELETE FROM items"};
      for(size_t i=0;i<sizeof(busy)/sizeof(busy[0]);++i) { raw_query(busy[i]); check_equal(execute(),ORM_STATUS_BUSY); check_null(result); }
      row(1,40); end_rows(); raw_query("COMMIT"); command(0);
      check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      raw_query("BEGIN"); check_equal(execute(),ORM_STATUS_UNSUPPORTED);
      check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,40); end_rows();
    }
    it("does not apply cancelled lifecycle commands and retains query ownership through resume") {
      seed(); raw_query("BEGIN"); check_equal(open_command(),ORM_STATUS_OK);
      cflow_publisher_cancel(&publisher); cflow_publisher_destroy(&publisher); publisher=(cflow_publisher){0};
      raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      raw_query("ROLLBACK"); command(0);
      raw_query("BEGIN"); check_equal(open_command(),ORM_STATUS_OK); orm_query_destroy(query); query=NULL;
      orm_command_result_t output=ORM_COMMAND_RESULT_INIT;
      check_equal(cflow_publisher_resume(&publisher,NULL,&output).kind,CFLOW_STEP_VALUE_AND_DONE); check_equal(output.affected_rows,0u);
      cflow_publisher_destroy(&publisher); publisher=(cflow_publisher){0};
      raw_query("UPDATE items SET score=50 WHERE id=1"); command(1);
      const char *const cancelled[]={"COMMIT", "ROLLBACK", "BEGIN"};
      for(size_t i=0;i<sizeof(cancelled)/sizeof(cancelled[0]);++i) {
        raw_query(cancelled[i]); check_equal(open_command(),ORM_STATUS_OK);
        cflow_publisher_cancel(&publisher); cflow_publisher_cancel(&publisher);
        check_equal(cflow_publisher_resume(&publisher,NULL,&output).kind,CFLOW_STEP_DONE);
        cflow_publisher_destroy(&publisher); publisher=(cflow_publisher){0};
      }
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,50); end_rows();
      raw_query("ROLLBACK"); command(0);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,40); end_rows();
    }
    it("retains an SQL transaction for an admitted Publisher and rolls back on final connection release") {
      seed(); raw_query("BEGIN"); command(0);
      raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows();
      orm_query_destroy(query); query=NULL; orm_disconnect(connection); connection=NULL;
      row(1,40); end_rows();
      check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,10); end_rows();
    }
  }
  group("SQL autocommit session mode") {
    it("shares implicit transactions across raw structured statements and savepoints") {
      seed(); raw_query("SET autocommit=0"); command(0);
      raw_query("SAVEPOINT before_write"); command(0);
      raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      structured(STRUCT_INSERT,"items"); assign("id",orm_i64(4)); assign("score",orm_i64(40)); command(1);
      raw_query("ROLLBACK TO before_write"); command(0);
      raw_query("SELECT id,score FROM items ORDER BY id"); open_rows(); row(1,10); row(2,20); row(3,30); end_rows();
      structured(STRUCT_UPDATE,"items"); assign("score",orm_i64(41)); where("id",ORM_COMPARE_EQUAL,orm_i64(1)); command(1);
      raw_query("COMMIT"); command(0);
      raw_query("UPDATE items SET score=42 WHERE id=1"); command(1); raw_query("ROLLBACK"); command(0);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,41); end_rows();
      raw_query("SET autocommit=1"); command(0);
      raw_query("UPDATE items SET score=43 WHERE id=1"); command(1); raw_query("ROLLBACK"); command(0);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,43); end_rows();
    }
    it("commits only a false to true transition and never commits equal-value settings") {
      seed(); raw_query("BEGIN"); command(0); raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      raw_query("SET autocommit=1"); command(0); raw_query("ROLLBACK"); command(0);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,10); end_rows();
      raw_query("BEGIN"); command(0); raw_query("UPDATE items SET score=41 WHERE id=1"); command(1);
      raw_query("SET autocommit=0"); command(0); raw_query("SET autocommit=0"); command(0);
      raw_query("ROLLBACK"); command(0);
      raw_query("UPDATE items SET score=42 WHERE id=1"); command(1); raw_query("SET autocommit=1"); command(0);
      raw_query("ROLLBACK"); command(0);
      disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,42); end_rows();
    }
    it("accepts session spellings symbolic values default and scalar typed expressions") {
      seed(); const char *const off[]={"SET SeSsIoN `AutoCommit`=OFF","SET LOCAL autocommit=FALSE",
        "SET @@autocommit=0","SET @@SESSION.autocommit='oFf'","SET @@LOCAL.autocommit=1-1"};
      const char *const on[]={"SET autocommit=ON","SET @@autocommit=TRUE","SET autocommit=DEFAULT",
        "SET @@SESSION.autocommit='oN'","SET @@LOCAL.autocommit=ON"};
      for(size_t i=0;i<sizeof(off)/sizeof(off[0]);++i) {
        raw_query(off[i]); command(0); raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
        raw_query("ROLLBACK"); command(0); raw_query(on[i]); command(0);
      }
      raw_query("SET autocommit=CASE WHEN ? THEN ? ELSE ? END"); parameter(orm_bool(true)); parameter(orm_u64(0)); parameter(orm_u64(1)); command(0);
      raw_query("UPDATE items SET score=41 WHERE id=1"); command(1);
      raw_query("SET autocommit=COALESCE(?,?)"); parameter(orm_null()); parameter(orm_i64(1)); command(0);
      raw_query("ROLLBACK"); command(0);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,41); end_rows();
    }
    it("rejects invalid values markers and unsupported SET lists before committing") {
      seed(); raw_query("SET autocommit=0"); command(0); raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      const char *const invalid[]={"SET autocommit=2","SET autocommit=-1","SET autocommit=1.0",
        "SET autocommit=NULL","SET autocommit='1'","SET autocommit='true'","SET autocommit='oops'"};
      for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) { raw_query(invalid[i]); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result); }
      const char *const unsupported[]={"SET GLOBAL autocommit=1","SET @@GLOBAL.autocommit=1","SET @autocommit=1",
        "SET autocommit=1,sql_mode='x'","SET autocommit=1,autocommit=0","SET sql_mode='x'","SET NAMES utf8"};
      for(size_t i=0;i<sizeof(unsupported)/sizeof(unsupported[0]);++i) { raw_query(unsupported[i]); check_equal(execute(),ORM_STATUS_UNSUPPORTED); check_null(result); }
      raw_query("SET autocommit=?"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("SET autocommit=1"); parameter(orm_i64(1)); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("SET autocommit=?"); parameter(orm_f64(1)); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("ROLLBACK"); command(0); raw_query("SET autocommit=1"); command(0);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,10); end_rows();
    }
    it("keeps autocommit disabled through BEGIN CHAIN and unchained completion") {
      seed(); raw_query("SET autocommit=0"); command(0);
      raw_query("UPDATE items SET score=40 WHERE id=1"); command(1); raw_query("BEGIN"); command(0);
      raw_query("UPDATE items SET score=41 WHERE id=1"); command(1); raw_query("ROLLBACK AND CHAIN"); command(0);
      raw_query("UPDATE items SET score=42 WHERE id=1"); command(1); raw_query("COMMIT AND CHAIN"); command(0);
      raw_query("UPDATE items SET score=43 WHERE id=1"); command(1); raw_query("ROLLBACK"); command(0);
      raw_query("UPDATE items SET score=44 WHERE id=1"); command(1); raw_query("ROLLBACK"); command(0);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,42); end_rows();
      raw_query("SET autocommit=1"); command(0);
    }
    it("inherits read-only characteristics without blocking a valid autocommit transition") {
      seed(); raw_query("SET TRANSACTION READ ONLY"); command(0); raw_query("SET autocommit=0"); command(0);
      raw_query("DELETE FROM items"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("SET SESSION TRANSACTION READ WRITE"); command(0);
      raw_query("UPDATE items SET score=40 WHERE id=1"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("SET autocommit=1"); command(0);
      raw_query("UPDATE items SET score=41 WHERE id=1"); command(1);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,41); end_rows();
    }
    it("permits ORM handles between implicit owners and rejects SET inside an ORM handle") {
      seed(); raw_query("SET autocommit=0"); command(0);
      check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      raw_query("SET autocommit=1"); check_equal(execute(),ORM_STATUS_UNSUPPORTED);
      raw_query("SET autocommit=0"); check_equal(execute(),ORM_STATUS_UNSUPPORTED);
      check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
      raw_query("UPDATE items SET score=41 WHERE id=1"); command(1);
      check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_BUSY); check_null(transaction);
      raw_query("ROLLBACK"); command(0); raw_query("SET autocommit=1"); command(0);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,40); end_rows();
    }
    it("rejects a live-cursor switch and applies no cancelled command") {
      seed(); raw_query("SET autocommit=0"); check_equal(open_command(),ORM_STATUS_OK);
      cflow_publisher_cancel(&publisher); cflow_publisher_destroy(&publisher); publisher=(cflow_publisher){0};
      raw_query("UPDATE items SET score=40 WHERE id=1"); command(1); raw_query("ROLLBACK"); command(0);
      raw_query("SET autocommit=0"); command(0); raw_query("UPDATE items SET score=41 WHERE id=1"); command(1);
      raw_query("SET autocommit=1"); check_equal(open_command(),ORM_STATUS_OK);
      cflow_publisher_cancel(&publisher); cflow_publisher_destroy(&publisher); publisher=(cflow_publisher){0};
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows();
      raw_query("SET autocommit=1"); check_equal(execute(),ORM_STATUS_BUSY);
      row(1,41); end_rows(); raw_query("ROLLBACK"); command(0); raw_query("SET autocommit=1"); command(0);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,40); end_rows();
    }
    it("rolls back an implicit transaction on final query release and restores autocommit on reopen") {
      seed(); raw_query("SET autocommit=0"); command(0); raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); orm_query_destroy(query); query=NULL;
      orm_disconnect(connection); connection=NULL; row(1,40); end_rows();
      check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("UPDATE items SET score=41 WHERE id=1"); command(1); raw_query("ROLLBACK"); command(0);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,41); end_rows();
    }
  }
  group("Session and next SQL transaction characteristics") {
    it("uses next access for exactly one explicit or automatic Catalog transaction") {
      seed(); raw_query("SET TRANSACTION READ ONLY, ISOLATION LEVEL SERIALIZABLE"); command(0);
      raw_query("BEGIN"); command(0);
      raw_query("UPDATE items SET score=40 WHERE id=1"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("COMMIT"); command(0); raw_query("BEGIN"); command(0);
      raw_query("UPDATE items SET score=40 WHERE id=1"); command(1); raw_query("ROLLBACK"); command(0);
      raw_query("SET TRANSACTION READ ONLY"); command(0);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,10); end_rows();
      raw_query("UPDATE items SET score=41 WHERE id=1"); command(1);
      raw_query("SET TRANSACTION READ ONLY"); command(0);
      raw_query("UPDATE items SET score=42 WHERE id=1"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("UPDATE items SET score=43 WHERE id=1"); command(1);
      raw_query("SET TRANSACTION READ ONLY"); command(0);
      raw_query("START TRANSACTION READ WRITE"); command(0);
      raw_query("UPDATE items SET score=44 WHERE id=1"); command(1); raw_query("COMMIT"); command(0);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,44); end_rows();
    }
    it("freezes active access while session changes survive savepoints rollback chaining and repeated BEGIN") {
      seed(); raw_query("BEGIN"); command(0); raw_query("SAVEPOINT before_setting"); command(0);
      raw_query("SET SESSION TRANSACTION READ ONLY"); command(0);
      raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      raw_query("ROLLBACK TO before_setting"); command(0); raw_query("ROLLBACK"); command(0);
      raw_query("BEGIN"); command(0);
      raw_query("UPDATE items SET score=41 WHERE id=1"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("SET SESSION TRANSACTION READ WRITE, ISOLATION LEVEL SERIALIZABLE"); command(0);
      raw_query("COMMIT AND CHAIN"); command(0);
      raw_query("UPDATE items SET score=42 WHERE id=1"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("ROLLBACK AND CHAIN"); command(0); raw_query("BEGIN"); command(0);
      structured(STRUCT_DELETE,"items"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("START TRANSACTION READ WRITE"); command(0);
      raw_query("UPDATE items SET score=43 WHERE id=1"); command(1); raw_query("ROLLBACK"); command(0);
      raw_query("BEGIN"); command(0); raw_query("UPDATE items SET score=44 WHERE id=1"); command(1);
      raw_query("COMMIT"); command(0);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,44); end_rows();
    }
    it("overwrites only the named characteristic and accepts the LOCAL session alias") {
      seed(); raw_query("SET TRANSACTION READ ONLY"); command(0);
      raw_query("SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE"); command(0);
      raw_query("SET TRANSACTION ISOLATION LEVEL SERIALIZABLE"); command(0);
      raw_query("BEGIN"); command(0);
      raw_query("UPDATE items SET score=40 WHERE id=1"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("ROLLBACK"); command(0);
      raw_query("SET TRANSACTION READ ONLY"); command(0);
      raw_query("SET SESSION TRANSACTION READ WRITE"); command(0);
      raw_query("BEGIN"); command(0); raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      raw_query("ROLLBACK"); command(0);
      raw_query("SET LOCAL TRANSACTION ISOLATION LEVEL SERIALIZABLE, READ ONLY"); command(0);
      raw_query("SET TRANSACTION READ WRITE"); command(0); raw_query("BEGIN"); command(0);
      raw_query("UPDATE items SET score=41 WHERE id=1"); command(1); raw_query("COMMIT"); command(0);
      raw_query("UPDATE items SET score=42 WHERE id=1"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("SET SESSION TRANSACTION READ WRITE"); command(0);
      raw_query("UPDATE items SET score=43 WHERE id=1"); command(1);
    }
    it("shares characteristics with ORM transaction handles without altering their frozen access") {
      seed(); raw_query("SET SESSION TRANSACTION READ ONLY"); command(0);
      raw_query("SET TRANSACTION READ WRITE"); command(0);
      check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      raw_query("SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE, READ ONLY"); command(0);
      structured(STRUCT_UPDATE,"items"); assign("score",orm_i64(40)); where("id",ORM_COMPARE_EQUAL,orm_i64(1)); command(1);
      raw_query("SET TRANSACTION READ WRITE"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("SET TRANSACTION ISOLATION LEVEL SERIALIZABLE"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
      check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      structured(STRUCT_UPDATE,"items"); assign("score",orm_i64(41)); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("SET LOCAL TRANSACTION READ WRITE"); command(0);
      raw_query("DELETE FROM items"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
      raw_query("UPDATE items SET score=42 WHERE id=1"); command(1);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,42); end_rows();
    }
    it("rejects active next settings unknown isolation global scope and parameters before effects") {
      seed(); raw_query("BEGIN"); command(0); raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      const char *const next[]={"SET TRANSACTION READ ONLY","SET TRANSACTION ISOLATION LEVEL SERIALIZABLE"};
      for(size_t i=0;i<sizeof(next)/sizeof(next[0]);++i) { raw_query(next[i]); check_equal(execute(),ORM_STATUS_SQL_ERROR); }
      const char *const unsupported[]={"SET GLOBAL TRANSACTION READ ONLY",
        "SET SESSION TRANSACTION READ ONLY, ISOLATION LEVEL READ COMMITTED",
        "SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED", "SET LOCAL TRANSACTION ISOLATION LEVEL REPEATABLE READ"};
      for(size_t i=0;i<sizeof(unsupported)/sizeof(unsupported[0]);++i) { raw_query(unsupported[i]); check_equal(execute(),ORM_STATUS_UNSUPPORTED); }
      raw_query("SET SESSION TRANSACTION READ ONLY"); parameter(orm_i64(1)); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows();
      raw_query("SET SESSION TRANSACTION READ ONLY"); check_equal(execute(),ORM_STATUS_BUSY);
      row(1,40); end_rows(); raw_query("ROLLBACK"); command(0);
      raw_query("BEGIN"); command(0); raw_query("UPDATE items SET score=41 WHERE id=1"); command(1);
      raw_query("ROLLBACK"); command(0);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,10); end_rows();
    }
    it("applies settings only on Publisher resume and restores connection defaults after reopen") {
      seed(); const char *const cancelled[]={"SET TRANSACTION READ ONLY","SET SESSION TRANSACTION READ ONLY"};
      for(size_t i=0;i<sizeof(cancelled)/sizeof(cancelled[0]);++i) {
        raw_query(cancelled[i]); check_equal(open_command(),ORM_STATUS_OK);
        cflow_publisher_cancel(&publisher); cflow_publisher_destroy(&publisher); publisher=(cflow_publisher){0};
      }
      raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      raw_query("SET SESSION TRANSACTION READ ONLY"); check_equal(open_command(),ORM_STATUS_OK);
      orm_query_destroy(query); query=NULL; orm_command_result_t output=ORM_COMMAND_RESULT_INIT;
      check_equal(cflow_publisher_resume(&publisher,NULL,&output).kind,CFLOW_STEP_VALUE_AND_DONE); check_equal(output.affected_rows,0u);
      cflow_publisher_destroy(&publisher); publisher=(cflow_publisher){0};
      raw_query("UPDATE items SET score=41 WHERE id=1"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("UPDATE items SET score=42 WHERE id=1"); command(1);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,42); end_rows();
    }
    it("resets an unused next access override on successful unchained completion") {
      seed(); const char *const completion[]={"COMMIT","ROLLBACK"};
      for(size_t i=0;i<sizeof(completion)/sizeof(completion[0]);++i) {
        raw_query("SET TRANSACTION READ ONLY"); command(0); raw_query(completion[i]); command(0);
        raw_query("BEGIN"); command(0); raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
        raw_query("ROLLBACK"); command(0);
      }
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,10); end_rows();
    }
  }
  group("SQL text savepoints on the existing transaction owner") {
    it("shares names with ORM controls and replaces points in temporal order") {
      seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      raw_query("/* command */ SAVEPOINT `First`"); command(0);
      raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      check_equal(orm_transaction_savepoint(transaction,orm_view("middle"),&error),ORM_STATUS_OK);
      raw_query("UPDATE items SET score=50 WHERE id=1"); command(1);
      raw_query("SAVEPOINT FIRST"); check_equal(execute(),ORM_STATUS_OK);
      uint64_t affected=1; check_equal(orm_result_affected_rows(result,&affected,&error),ORM_STATUS_OK); check_equal(affected,0u);
      raw_query("UPDATE items SET score=60 WHERE id=1"); command(1);
      raw_query("ROLLBACK WORK TO SAVEPOINT `MIDDLE`"); command(0);
      check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("first"),&error),ORM_STATUS_SQL_ERROR);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,40); end_rows();
      raw_query("UPDATE items SET score=70 WHERE id=1"); command(1);
      raw_query("ROLLBACK TO middle"); command(0);
      raw_query("RELEASE SAVEPOINT middle"); command(0);
      check_equal(orm_transaction_release_savepoint(transaction,orm_view("MIDDLE"),&error),ORM_STATUS_SQL_ERROR);
      check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
      disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,40); end_rows();
    }
    it("releases only the named point without committing or losing newer points") {
      seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      raw_query("SAVEPOINT first"); command(0);
      raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      raw_query("SAVEPOINT second"); command(0);
      raw_query("UPDATE items SET score=50 WHERE id=1"); command(1);
      raw_query("RELEASE SAVEPOINT FIRST"); command(0);
      raw_query("ROLLBACK TO SAVEPOINT first"); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
      raw_query("ROLLBACK TO second"); command(0);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,40); end_rows();
      check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,10); end_rows();
    }
    it("restores rows indexes and Catalog changes through one SQL savepoint") {
      seed(); raw_query("CREATE UNIQUE INDEX by_score ON items(score)"); command(0);
      check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      raw_query("SAVEPOINT catalog_start"); command(0);
      raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      raw_query("ALTER TABLE items ADD extra BIGINT DEFAULT(ROUND(25,-1))"); command(0);
      raw_query("CREATE INDEX by_extra ON items(extra)"); command(0);
      raw_query("CREATE TABLE undone(id BIGINT PRIMARY KEY,score BIGINT)"); command(0);
      raw_query("INSERT INTO undone VALUES(1,9)"); command(1);
      raw_query("ROLLBACK WORK TO catalog_start"); command(0);
      raw_query("SELECT score FROM items WHERE score=10"); check_equal(execute(),ORM_STATUS_OK); count_is(1); integer_at(0,0,10);
      raw_query("SELECT extra FROM items"); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
      raw_query("INSERT INTO items VALUES(4,10)"); check_equal(execute(),ORM_STATUS_CONSTRAINT); check_null(result);
      raw_query("CREATE TABLE undone(id BIGINT PRIMARY KEY,score BIGINT)"); command(0);
      raw_query("CREATE INDEX by_extra ON items(score)"); command(0);
      raw_query("INSERT INTO items VALUES(4,40)"); command(1);
      raw_query("RELEASE SAVEPOINT catalog_start"); command(0);
      check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
      disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
      raw_query("SELECT id,score FROM items ORDER BY id"); open_rows(); row(1,10); row(2,20); row(3,30); row(4,40); end_rows();
      raw_query("SELECT id,score FROM undone"); check_equal(execute(),ORM_STATUS_OK); count_is(0);
    }
    it("shares the configured live-point bound with ORM names and permits replacement at capacity") {
      seed(); disconnect(); const orm_option_t option={orm_view("sql_max_savepoints"),orm_view("2")};
      check_equal(connect_profile("relational","false",&option,1),ORM_STATUS_OK);
      check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      raw_query("SAVEPOINT one"); command(0);
      check_equal(orm_transaction_savepoint(transaction,orm_view("two"),&error),ORM_STATUS_OK);
      raw_query("SAVEPOINT three"); check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
      raw_query("SAVEPOINT `ONE`"); command(0);
      raw_query("RELEASE SAVEPOINT two"); command(0);
      check_equal(orm_transaction_savepoint(transaction,orm_view("three"),&error),ORM_STATUS_OK);
      raw_query("ROLLBACK TO SAVEPOINT one"); command(0);
      raw_query("RELEASE SAVEPOINT three"); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
      raw_query("SAVEPOINT four"); command(0);
      check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
    }
    it("rejects bad names parameters and lifecycle commands without changing existing points") {
      seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      raw_query("SAVEPOINT `select`"); command(0);
      raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      const char *const bad[]={"SAVEPOINT `bad-name`","SAVEPOINT `a``b`","SAVEPOINT `9name`",
        "SAVEPOINT ``","BEGIN","START TRANSACTION","COMMIT","ROLLBACK",
        "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ"};
      for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        raw_query(bad[i]); check_equal(execute(),ORM_STATUS_UNSUPPORTED); check_null(result);
      }
      const char *const parameterized[]={"SAVEPOINT next","ROLLBACK TO `select`","RELEASE SAVEPOINT `select`"};
      for(size_t i=0;i<sizeof(parameterized)/sizeof(parameterized[0]);++i) {
        raw_query(parameterized[i]); parameter(orm_i64(1)); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
      }
      raw_query("SAVEPOINT next; RELEASE SAVEPOINT next"); check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
      enum { NAME_LIMIT=63 }; char name[NAME_LIMIT+2],sql[NAME_LIMIT+sizeof("SAVEPOINT ``")+1];
      memset(name,'a',sizeof(name)-1); name[sizeof(name)-1]=0;
      (void)snprintf(sql,sizeof(sql),"SAVEPOINT `%s`",name);
      raw_query(sql); check_equal(execute(),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
      name[NAME_LIMIT]=0; (void)snprintf(sql,sizeof(sql),"SAVEPOINT `%s`",name); raw_query(sql); command(0);
      check_equal(orm_transaction_release_savepoint(transaction,orm_view(name),&error),ORM_STATUS_OK);
      raw_query("ROLLBACK TO `SELECT`"); command(0);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,10); end_rows();
      check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
    }
    it("keeps query admission and cancelled command flows free of savepoint effects") {
      seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      raw_query("SAVEPOINT safe"); command(0);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows();
      const char *const controls[]={"SAVEPOINT next","ROLLBACK TO safe","RELEASE SAVEPOINT safe"};
      for(size_t i=0;i<sizeof(controls)/sizeof(controls[0]);++i) {
        raw_query(controls[i]); check_equal(execute(),ORM_STATUS_BUSY); check_null(result);
      }
      row(1,10); end_rows();
      raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      for(size_t i=0;i<sizeof(controls)/sizeof(controls[0]);++i) {
        raw_query(controls[i]); check_equal(open_command(),ORM_STATUS_OK);
        orm_query_destroy(query); query=NULL;
        cflow_publisher_cancel(&publisher); cflow_publisher_cancel(&publisher);
        orm_command_result_t output=ORM_COMMAND_RESULT_INIT;
        check_equal(cflow_publisher_resume(&publisher,NULL,&output).kind,CFLOW_STEP_DONE);
        cflow_publisher_destroy(&publisher); publisher=(cflow_publisher){0};
      }
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,40); end_rows();
      check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("next"),&error),ORM_STATUS_SQL_ERROR);
      raw_query("ROLLBACK TO safe"); check_equal(open_command(),ORM_STATUS_OK); orm_query_destroy(query); query=NULL;
      orm_command_result_t output=ORM_COMMAND_RESULT_INIT;
      check_equal(cflow_publisher_resume(&publisher,NULL,&output).kind,CFLOW_STEP_VALUE_AND_DONE); check_equal(output.affected_rows,0u);
      cflow_publisher_destroy(&publisher); publisher=(cflow_publisher){0};
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,10); end_rows();
      check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
    }
    it("clears command diagnostics and preserves points through failed statement rollback") {
      seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      raw_query("SELECT MOD(1,0)"); check_equal(execute(),ORM_STATUS_OK);
      raw_query("SHOW WARNINGS"); check_equal(execute(),ORM_STATUS_OK); count_is(1);
      raw_query("SAVEPOINT clean"); command(0);
      raw_query("SHOW WARNINGS"); check_equal(execute(),ORM_STATUS_OK); count_is(0);
      raw_query("INSERT INTO items VALUES(4,40),(1,99)"); check_equal(execute(),ORM_STATUS_CONSTRAINT); check_null(result);
      raw_query("SELECT id FROM items WHERE id=4"); check_equal(execute(),ORM_STATUS_OK); count_is(0);
      raw_query("ROLLBACK TO clean"); command(0);
      raw_query("INSERT INTO items VALUES(4,40)"); command(1);
      raw_query("ROLLBACK TO clean"); command(0);
      raw_query("SELECT id FROM items"); check_equal(execute(),ORM_STATUS_OK); count_is(3);
      check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
    }
    it("does not retain savepoints in autocommit or carry points into the next transaction") {
      seed(); raw_query("SAVEPOINT absent"); command(0);
      raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
      const char *const missing[]={"ROLLBACK TO absent","RELEASE SAVEPOINT absent"};
      for(size_t i=0;i<sizeof(missing)/sizeof(missing[0]);++i) {
        raw_query(missing[i]); check_equal(execute(),ORM_STATUS_SQL_ERROR); check_null(result);
      }
      check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      raw_query("ROLLBACK TO absent"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("SAVEPOINT absent"); command(0);
      check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
      check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
      raw_query("RELEASE SAVEPOINT absent"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
      raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,40); end_rows();
      check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
    }
  }
  it("replaces same-name savepoints in temporal order and keeps rollback targets reusable") {
    seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    char borrowed[] = "First";
    check_equal(orm_transaction_savepoint(transaction,orm_view(borrowed),&error),ORM_STATUS_OK);
    borrowed[0] = 'x';
    raw_query("UPDATE items SET score=20 WHERE id=1"); command(1);
    check_equal(orm_transaction_savepoint(transaction,orm_view("middle"),&error),ORM_STATUS_OK);
    raw_query("UPDATE items SET score=30 WHERE id=1"); command(1);
    check_equal(orm_transaction_savepoint(transaction,orm_view("FIRST"),&error),ORM_STATUS_OK);
    raw_query("UPDATE items SET score=40 WHERE id=1"); command(1);
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("middle"),&error),ORM_STATUS_OK);
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("first"),&error),ORM_STATUS_SQL_ERROR);
    raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,20); end_rows();
    raw_query("UPDATE items SET score=25 WHERE id=1"); command(1);
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("MIDDLE"),&error),ORM_STATUS_OK);
    check_equal(orm_transaction_release_savepoint(transaction,orm_view("middle"),&error),ORM_STATUS_OK);
    check_equal(orm_transaction_release_savepoint(transaction,orm_view("middle"),&error),ORM_STATUS_SQL_ERROR);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
    raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,20); end_rows();
  }
  it("releases only the named savepoint without losing newer points or committing data") {
    seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("first"),&error),ORM_STATUS_OK);
    raw_query("UPDATE items SET score=20 WHERE id=1"); command(1);
    check_equal(orm_transaction_savepoint(transaction,orm_view("second"),&error),ORM_STATUS_OK);
    raw_query("UPDATE items SET score=30 WHERE id=1"); command(1);
    check_equal(orm_transaction_release_savepoint(transaction,orm_view("first"),&error),ORM_STATUS_OK);
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("second"),&error),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,20); end_rows();
    check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
    raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,10); end_rows();
  }
  it("rolls back Catalog and primary-key changes through the same savepoint owner") {
    seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("schema_start"),&error),ORM_STATUS_OK);
    raw_query("CREATE TABLE undone(id BIGINT PRIMARY KEY,score BIGINT)"); command(0);
    raw_query("INSERT INTO undone(id,score) VALUES(1,9)"); command(1);
    raw_query("UPDATE items SET id=id+1 ORDER BY id DESC"); command(3);
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("schema_start"),&error),ORM_STATUS_OK);
    raw_query("SHOW TABLES"); check_equal(execute(),ORM_STATUS_OK); text_at(0,0,"items");
    uint64_t count=0; check_equal(orm_result_row_count(result,&count,&error),ORM_STATUS_OK); check_equal(count,1u);
    raw_query("CREATE TABLE undone(id BIGINT PRIMARY KEY,score BIGINT)"); command(0);
    raw_query("INSERT INTO undone(id,score) VALUES(2,7)"); command(1);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,10); end_rows();
    raw_query("SELECT id,score FROM undone"); open_rows(); row(2,7); end_rows();
  }
  it("renames indexed tables and columns through raw SQL with savepoint rollback and reconnect") {
    seed(); raw_query("CREATE UNIQUE INDEX score_key ON items(score)"); command(0);
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("before_rename"),&error),ORM_STATUS_OK);
    raw_query("ALTER TABLE items RENAME TO renamed"); command(0);
    raw_query("ALTER TABLE renamed RENAME COLUMN score TO amount"); command(0);
    raw_query("SELECT id,amount AS score FROM renamed WHERE amount=20"); open_rows(); row(2,20); end_rows();
    raw_query("INSERT INTO renamed(id,amount) VALUES(4,20)"); check_equal(execute(),ORM_STATUS_CONSTRAINT);
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("before_rename"),&error),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM items WHERE score=20"); open_rows(); row(2,20); end_rows();
    raw_query("ALTER TABLE items RENAME COLUMN id TO identifier"); command(0);
    raw_query("ALTER TABLE items RENAME TO renamed"); command(0);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SELECT identifier AS id,score FROM renamed WHERE score=20"); open_rows(); row(2,20); end_rows();
    raw_query("SHOW TABLES"); check_equal(execute(),ORM_STATUS_OK); text_at(0,0,"renamed");
    raw_query("UPDATE renamed SET identifier=4,score=40 WHERE identifier=2"); command(1);
    raw_query("SELECT identifier AS id,score FROM renamed WHERE score=40"); open_rows(); row(4,40); end_rows();
    raw_query("DROP INDEX score_key ON renamed"); command(0);
    raw_query("TRUNCATE renamed"); command(0);
  }
  it("keeps valid tables usable after rejected ALTER commands") {
    seed(); raw_query("CREATE TABLE occupied(id BIGINT PRIMARY KEY)"); command(0);
    raw_query("ALTER TABLE items RENAME TO occupied"); check_equal(execute(),ORM_STATUS_CONSTRAINT);
    raw_query("ALTER TABLE items RENAME COLUMN score TO id"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
    raw_query("ALTER TABLE items ENGINE=InnoDB"); check_equal(execute(),ORM_STATUS_UNSUPPORTED);
    raw_query("ALTER TABLE items ADD CONSTRAINT fk FOREIGN KEY(score) REFERENCES occupied(id) ON DELETE CASCADE"); check_equal(execute(),ORM_STATUS_UNSUPPORTED);
    raw_query("ALTER TABLE missing RENAME TO renamed"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
    raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,10); end_rows();
  }
  it("alters empty table columns with indexes and rolls back schema changes through public savepoints") {
    raw_query(ddl); command(0); raw_query("CREATE UNIQUE INDEX score_key ON items(score)"); command(0);
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("before_columns"),&error),ORM_STATUS_OK);
    raw_query("ALTER TABLE items ADD COLUMN extra DOUBLE"); command(0);
    raw_query("INSERT INTO items(id,score,extra) VALUES(1,10,?)"); parameter(orm_f64(2.5)); command(1);
    raw_query("SELECT id,score FROM items WHERE score=10 AND extra=?"); parameter(orm_f64(2.5)); open_rows(); row(1,10); end_rows();
    raw_query("ALTER TABLE items DROP extra"); command(0);
    raw_query("SELECT id,score FROM items WHERE score=10"); open_rows(); row(1,10); end_rows();
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("before_columns"),&error),ORM_STATUS_OK);
    raw_query("ALTER TABLE items ADD COLUMN extra BIGINT NOT NULL"); command(0);
    raw_query("ALTER TABLE items DROP COLUMN extra"); command(0);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("INSERT INTO items(id,score) VALUES(1,10)"); command(1);
    raw_query("SELECT id,score FROM items WHERE score=10"); open_rows(); row(1,10); end_rows();
    raw_query("INSERT INTO items(id,score) VALUES(2,10)"); check_equal(execute(),ORM_STATUS_CONSTRAINT);
  }
  it("adds nullable columns to populated indexed rows and restores removed values through savepoints") {
    seed(); raw_query("CREATE UNIQUE INDEX score_key ON items(score)"); command(0);
    raw_query("ALTER TABLE items ADD extra DOUBLE"); command(0);
    raw_query("SELECT id,score FROM items WHERE id=1 AND extra IS NULL"); open_rows(); row(1,10); end_rows();
    raw_query("UPDATE items SET extra=? WHERE id=1"); parameter(orm_f64(2.5)); command(1);
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("before_drop"),&error),ORM_STATUS_OK);
    raw_query("ALTER TABLE items DROP extra"); command(0);
    raw_query("SELECT id,score FROM items WHERE score=10"); open_rows(); row(1,10); end_rows();
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("before_drop"),&error),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM items WHERE score=10 AND extra=?"); parameter(orm_f64(2.5)); open_rows(); row(1,10); end_rows();
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM items WHERE score=10 AND extra=?"); parameter(orm_f64(2.5)); open_rows(); row(1,10); end_rows();
    raw_query("ALTER TABLE items DROP extra"); command(0);
    raw_query("INSERT INTO items(id,score) VALUES(9,10)"); check_equal(execute(),ORM_STATUS_CONSTRAINT);
  }
  it("keeps surviving indexes usable after rewriting a populated preceding column") {
    raw_query("CREATE TABLE items(extra DOUBLE,id BIGINT PRIMARY KEY,score BIGINT)"); command(0);
    raw_query("CREATE UNIQUE INDEX score_key ON items(score)"); command(0);
    raw_query("INSERT INTO items(extra,id,score) VALUES(?,1,10)"); parameter(orm_f64(2.5)); command(1);
    raw_query("ALTER TABLE items DROP extra"); command(0);
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM items WHERE score=10"); open_rows(); row(1,10); end_rows();
    raw_query("UPDATE items SET score=20 WHERE id=1"); command(1);
    raw_query("SELECT id,score FROM items WHERE score=20"); open_rows(); row(1,20); end_rows();
    raw_query("DELETE FROM items WHERE score=20"); command(1);
    raw_query("ALTER TABLE items DROP score"); check_equal(execute(),ORM_STATUS_UNSUPPORTED);
    raw_query("DROP INDEX score_key ON items"); command(0);
    raw_query("ALTER TABLE items DROP score"); command(0);
    raw_query("ALTER TABLE items ADD score BIGINT"); command(0);
    raw_query("INSERT INTO items(id,score) VALUES(2,30)"); command(1);
  }
  it("persists positioned columns and exposes their order through SHOW and SELECT star") {
    seed(); raw_query("CREATE UNIQUE INDEX score_key ON items(score)"); command(0);
    raw_query("ALTER TABLE items ADD extra DOUBLE FIRST"); command(0);
    raw_query("ALTER TABLE items ADD marker BIGINT UNSIGNED AFTER id"); command(0);
    raw_query("UPDATE items SET extra=?,marker=? WHERE id=1"); parameter(orm_f64(2.5)); parameter(orm_u64(UINT64_MAX)); command(1);
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SHOW COLUMNS FROM items"); check_equal(execute(),ORM_STATUS_OK);
    text_at(0,0,"extra"); text_at(1,0,"id"); text_at(1,3,"PRI"); text_at(2,0,"marker"); text_at(3,0,"score");
    raw_query("SELECT * FROM items WHERE score=10"); check_equal(execute(),ORM_STATUS_OK);
    double floating=0; uint64_t marker=0; int64_t id=0,score=0;
    check_equal(orm_result_get_double(result,0,0,&floating,&error),ORM_STATUS_OK); check_true(floating==2.5);
    check_equal(orm_result_get_int64(result,0,1,&id,&error),ORM_STATUS_OK); check_equal(id,1);
    check_equal(orm_result_get_uint64(result,0,2,&marker,&error),ORM_STATUS_OK); check_equal(marker,UINT64_MAX);
    check_equal(orm_result_get_int64(result,0,3,&score,&error),ORM_STATUS_OK); check_equal(score,10);
    raw_query("INSERT INTO items(extra,id,marker,score) VALUES(NULL,9,NULL,10)"); check_equal(execute(),ORM_STATUS_CONSTRAINT);
    raw_query("ALTER TABLE items DROP extra"); command(0); raw_query("ALTER TABLE items DROP marker"); command(0);
    raw_query("SELECT id,score FROM items WHERE score=10"); open_rows(); row(1,10); end_rows();
  }
  it("positions NOT NULL columns on empty tables and rolls back their schema") {
    raw_query(ddl); command(0);
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("before_position"),&error),ORM_STATUS_OK);
    raw_query("ALTER TABLE items ADD extra DOUBLE NOT NULL FIRST"); command(0);
    raw_query("INSERT INTO items(extra,id,score) VALUES(?,1,10)"); parameter(orm_f64(2.5)); command(1);
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("before_position"),&error),ORM_STATUS_OK);
    raw_query("INSERT INTO items(id,score) VALUES(1,10)"); command(1);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
    raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,10); end_rows();
  }
  it("reports primary and secondary column keys with MySQL precedence") {
    raw_query("CREATE TABLE items(id BIGINT PRIMARY KEY,a BIGINT,b BIGINT UNSIGNED,c DOUBLE)"); command(0);
    raw_query("CREATE UNIQUE INDEX ab ON items(a,b)"); command(0);
    raw_query("CREATE INDEX ordinary ON items(a DESC)"); command(0);
    raw_query("CREATE UNIQUE INDEX redundant ON items(id)"); command(0);
    raw_query("SHOW COLUMNS FROM items"); check_equal(execute(),ORM_STATUS_OK); count_is(4);
    text_at(0,3,"PRI"); text_at(1,3,"MUL"); text_at(2,3,""); text_at(3,3,"");
    raw_query("CREATE UNIQUE INDEX only_a ON items(a)"); command(0);
    raw_query("SHOW FIELDS IN items"); check_equal(execute(),ORM_STATUS_OK); text_at(1,3,"UNI");
    raw_query("DROP INDEX only_a ON items"); command(0);
    raw_query("SHOW COLUMNS FROM items"); check_equal(execute(),ORM_STATUS_OK); text_at(1,3,"MUL");
    raw_query("DROP INDEX ab ON items"); command(0); raw_query("DROP INDEX ordinary ON items"); command(0);
    raw_query("SHOW COLUMNS FROM items"); check_equal(execute(),ORM_STATUS_OK); text_at(1,3,"");
  }
  it("exposes all SHOW INDEX fields for primary and nullable descending composite keys") {
    seed();
    raw_query("SHOW KEYS IN items"); check_equal(execute(),ORM_STATUS_OK); count_is(1);
    text_at(0,2,"PRIMARY"); text_at(0,4,"id"); text_at(0,5,"A"); text_at(0,9,"");
    integer_at(0,1,0); integer_at(0,3,1);
    raw_query("CREATE UNIQUE INDEX pair ON items(score DESC,id ASC)"); command(0);
    const char *sql[]={"SHOW INDEX FROM items","SHOW INDEXES IN items","SHOW KEYS FROM items"};
    for(size_t pass=0;pass<sizeof(sql)/sizeof(sql[0]);++pass) {
      raw_query(sql[pass]); check_equal(execute(),ORM_STATUS_OK); count_is(3);
      uint64_t columns=0; check_equal(orm_result_column_count(result,&columns,&error),ORM_STATUS_OK); check_equal(columns,15u);
      text_at(1,2,"pair"); text_at(1,4,"score"); text_at(1,5,"D"); text_at(1,9,"YES");
      text_at(2,2,"pair"); text_at(2,4,"id"); text_at(2,5,"A"); text_at(2,9,"");
      integer_at(1,1,0); integer_at(2,1,0); integer_at(1,3,1); integer_at(2,3,2);
      for(uint64_t r=0;r<3;++r) {
        text_at(r,0,"items"); text_at(r,10,"LSM"); text_at(r,11,""); text_at(r,12,""); text_at(r,13,"YES");
        const uint64_t nullable[]={6,7,8,14};
        for(size_t i=0;i<sizeof(nullable)/sizeof(nullable[0]);++i) {
          uint8_t null=0; check_equal(orm_result_is_null(result,r,nullable[i],&null,&error),ORM_STATUS_OK); check_true(null);
        }
      }
    }
    raw_query("DROP INDEX pair ON items"); command(0);
    raw_query("CREATE INDEX by_score ON items(score)"); command(0);
    raw_query("SHOW INDEX FROM items"); check_equal(execute(),ORM_STATUS_OK); count_is(2); integer_at(1,1,1);
  }
  it("reads index metadata after positioned ALTER rename rollback and reopen") {
    seed(); raw_query("CREATE UNIQUE INDEX score_key ON items(score)"); command(0);
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("metadata"),&error),ORM_STATUS_OK);
    raw_query("ALTER TABLE items ADD extra DOUBLE FIRST"); command(0);
    raw_query("ALTER TABLE items RENAME COLUMN score TO points"); command(0);
    raw_query("ALTER TABLE items RENAME TO renamed"); command(0);
    raw_query("SHOW INDEX FROM renamed"); check_equal(execute(),ORM_STATUS_OK); text_at(1,0,"renamed"); text_at(1,4,"points");
    raw_query("SHOW COLUMNS FROM renamed"); check_equal(execute(),ORM_STATUS_OK); text_at(1,3,"PRI"); text_at(2,3,"UNI");
    raw_query("DROP INDEX score_key ON renamed"); command(0);
    raw_query("SHOW INDEX FROM renamed"); check_equal(execute(),ORM_STATUS_OK); count_is(1);
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("metadata"),&error),ORM_STATUS_OK);
    raw_query("SHOW INDEX FROM items"); check_equal(execute(),ORM_STATUS_OK); count_is(2); text_at(1,4,"score");
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SHOW COLUMNS FROM items"); check_equal(execute(),ORM_STATUS_OK); text_at(1,3,"UNI");
    raw_query("SHOW INDEX FROM items"); check_equal(execute(),ORM_STATUS_OK); count_is(2); text_at(1,2,"score_key");
  }
  it("rejects unsupported SHOW INDEX filters and qualifiers without damaging data") {
    seed();
    const char *sql[]={"SHOW EXTENDED INDEX FROM items","SHOW INDEX FROM items FROM app",
      "SHOW INDEX FROM app.items","SHOW INDEX FROM items WHERE @@unknown=0"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      raw_query(sql[i]); check_equal(execute(),ORM_STATUS_UNSUPPORTED);
    }
    raw_query("SHOW INDEX FROM absent"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
    raw_query("SELECT id,score FROM items WHERE id=1"); open_rows(); row(1,10); end_rows();
  }
  it("round trips SHOW CREATE numeric types nullability column order and quoted names") {
    raw_query("CREATE TABLE `select` (`from` DOUBLE NOT NULL,`key` BIGINT UNSIGNED PRIMARY KEY,`order` BIGINT,optional DOUBLE)"); command(0);
    raw_query("SHOW CREATE TABLE `select`"); check_equal(execute(),ORM_STATUS_OK); count_is(1);
    text_at(0,0,"select");
    const char expected[]="CREATE TABLE `select` (\n  `from` double NOT NULL,\n  `key` bigint unsigned NOT NULL,\n  `order` bigint NULL,\n  `optional` double NULL,\n  PRIMARY KEY (`key`)\n)";
    text_at(0,1,expected);
    vstr ddl_view={0}; check_equal(orm_result_get_text(result,0,1,&ddl_view,&error),ORM_STATUS_OK);
    tstr ddl_copy=tstr_from_v(ddl_view); check_not_null(ddl_copy);
    raw_query("DROP TABLE `select`"); command(0); raw_query(ddl_copy); command(0); tstr_free(ddl_copy);
    raw_query("INSERT INTO `select` (`from`,`key`,`order`,optional) VALUES(?,?,NULL,NULL)");
    parameter(orm_f64(2.5)); parameter(orm_u64(UINT64_MAX)); command(1);
    raw_query("SHOW CREATE TABLE `select`"); check_equal(execute(),ORM_STATUS_OK); text_at(0,1,expected);
    raw_query("SHOW CREATE TABLE missing"); check_equal(execute(),ORM_STATUS_SQL_ERROR);
    raw_query("SHOW CREATE TABLE app.items"); check_equal(execute(),ORM_STATUS_UNSUPPORTED);
  }
  it("shows complete ordinary and unique composite index definitions") {
    seed(); raw_query("CREATE UNIQUE INDEX pair ON items(score DESC,id ASC)"); command(0);
    raw_query("CREATE INDEX by_id ON items(id DESC)"); command(0);
    raw_query("SHOW CREATE TABLE items"); check_equal(execute(),ORM_STATUS_OK); count_is(1);
    vstr ddl_view={0}; check_equal(orm_result_get_text(result,0,1,&ddl_view,&error),ORM_STATUS_OK);
    tstr ddl_copy=tstr_from_v(ddl_view); check_not_null(ddl_copy);
    check_not_null(strstr(ddl_copy,"UNIQUE KEY `pair` (`score` DESC, `id` ASC)"));
    check_not_null(strstr(ddl_copy,"KEY `by_id` (`id` DESC)"));
    raw_query("DROP TABLE items"); command(0); raw_query(ddl_copy); command(0); tstr_free(ddl_copy);
    raw_query("INSERT INTO items(id,score) VALUES(1,10),(2,20)"); command(2);
    raw_query("SELECT id,score FROM items WHERE score=20 AND id=2"); open_rows(); row(2,20); end_rows();
    raw_query("DROP INDEX pair ON items"); command(0); raw_query("DROP INDEX by_id ON items"); command(0);
    raw_query("SHOW CREATE TABLE items"); check_equal(execute(),ORM_STATUS_OK);
    text_at(0,1,"CREATE TABLE `items` (\n  `id` bigint NOT NULL,\n  `score` bigint NULL,\n  PRIMARY KEY (`id`)\n)");
  }
  it("updates SHOW CREATE after ALTER and restores definitions on rollback and reopen") {
    seed();
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("before_create_text"),&error),ORM_STATUS_OK);
    raw_query("ALTER TABLE items ADD extra DOUBLE FIRST"); command(0);
    raw_query("ALTER TABLE items RENAME COLUMN score TO points"); command(0);
    raw_query("ALTER TABLE items RENAME TO renamed"); command(0);
    raw_query("CREATE UNIQUE INDEX points_key ON renamed(points DESC)"); command(0);
    raw_query("SHOW CREATE TABLE renamed"); check_equal(execute(),ORM_STATUS_OK);
    text_at(0,0,"renamed"); text_at(0,1,"CREATE TABLE `renamed` (\n  `extra` double NULL,\n  `id` bigint NOT NULL,\n  `points` bigint NULL,\n  PRIMARY KEY (`id`),\n  UNIQUE KEY `points_key` (`points` DESC)\n)");
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("before_create_text"),&error),ORM_STATUS_OK);
    raw_query("SHOW CREATE TABLE items"); check_equal(execute(),ORM_STATUS_OK);
    text_at(0,1,"CREATE TABLE `items` (\n  `id` bigint NOT NULL,\n  `score` bigint NULL,\n  PRIMARY KEY (`id`)\n)");
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SHOW CREATE TABLE items"); check_equal(execute(),ORM_STATUS_OK);
    text_at(0,1,"CREATE TABLE `items` (\n  `id` bigint NOT NULL,\n  `score` bigint NULL,\n  PRIMARY KEY (`id`)\n)");
  }
  it("enforces inline unique indexes after SHOW CREATE replay and database reopen") {
    raw_query("CREATE TABLE items(id BIGINT PRIMARY KEY,score BIGINT,UNIQUE KEY uq(score DESC),INDEX by_id(id ASC))"); command(0);
    raw_query("SHOW CREATE TABLE items"); check_equal(execute(),ORM_STATUS_OK);
    vstr text={0}; check_equal(orm_result_get_text(result,0,1,&text,&error),ORM_STATUS_OK);
    tstr copy=tstr_from_v(text); check_not_null(copy);
    raw_query("DROP TABLE items"); command(0); raw_query(copy); command(0); tstr_free(copy);
    raw_query("INSERT INTO items(id,score) VALUES(1,10),(2,NULL),(3,NULL)"); command(3);
    raw_query("INSERT INTO items(id,score) VALUES(4,10)"); check_equal(execute(),ORM_STATUS_CONSTRAINT);
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM items WHERE score=10"); open_rows(); row(1,10); end_rows();
    raw_query("UPDATE items SET score=10 WHERE id=2"); check_equal(execute(),ORM_STATUS_CONSTRAINT);
    raw_query("CREATE TABLE IF NOT EXISTS items(id BIGINT PRIMARY KEY,score BIGINT,KEY ignored(score))"); command(0);
    raw_query("SHOW INDEX FROM items"); check_equal(execute(),ORM_STATUS_OK); count_is(3);
    raw_query("INSERT INTO items(id,score) VALUES(4,10)"); check_equal(execute(),ORM_STATUS_CONSTRAINT);
  }
  it("rolls back complete inline CREATE through public savepoints") {
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("before_create"),&error),ORM_STATUS_OK);
    raw_query("CREATE TABLE items(id BIGINT PRIMARY KEY,score BIGINT,UNIQUE KEY uq(score))"); command(0);
    raw_query("INSERT INTO items(id,score) VALUES(1,10)"); command(1);
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("before_create"),&error),ORM_STATUS_OK);
    raw_query("SHOW TABLES"); check_equal(execute(),ORM_STATUS_OK); count_is(0);
    raw_query("CREATE TABLE items(id BIGINT PRIMARY KEY,score BIGINT,KEY ordinary(score DESC))"); command(0);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK); orm_transaction_destroy(transaction); transaction=NULL;
    disconnect(); check_equal(connect_profile("relational","false",NULL,0),ORM_STATUS_OK);
    raw_query("INSERT INTO items(id,score) VALUES(1,10),(2,10)"); command(2);
    raw_query("SHOW COLUMNS FROM items"); check_equal(execute(),ORM_STATUS_OK); text_at(1,3,"MUL");
  }
  it("generates stable names for omitted and column unique indexes") {
    raw_query("CREATE TABLE items(id BIGINT PRIMARY KEY,a BIGINT UNIQUE,b BIGINT,UNIQUE(a DESC),KEY b(b),INDEX a(b))"); command(0);
    raw_query("SHOW INDEX FROM items"); check_equal(execute(),ORM_STATUS_OK); count_is(5);
    const char *names[]={"a_2","a_3","b","a"}; const int64_t non_unique[]={0,0,1,1}; bool found[4]={0};
    for(uint64_t row=1;row<5;++row) {
      vstr name={0}; int64_t flag=-1;
      check_equal(orm_result_get_text(result,row,2,&name,&error),ORM_STATUS_OK);
      check_equal(orm_result_get_int64(result,row,1,&flag,&error),ORM_STATUS_OK);
      size_t match=4;
      for(size_t i=0;i<4;++i) if(name.len==strlen(names[i]) && !memcmp(name.data,names[i],name.len)) { match=i; break; }
      check_true(match<4); if(match<4) { check_false(found[match]); found[match]=true; check_equal(flag,non_unique[match]); }
    }
    for(size_t i=0;i<4;++i) check_true(found[i]);
    raw_query("INSERT INTO items(id,a,b) VALUES(1,10,7),(2,NULL,7),(3,NULL,8)"); command(3);
    raw_query("INSERT INTO items(id,a,b) VALUES(4,10,9)"); check_equal(execute(),ORM_STATUS_CONSTRAINT);
    raw_query("SHOW CREATE TABLE items"); check_equal(execute(),ORM_STATUS_OK);
    vstr create_sql={0}; check_equal(orm_result_get_text(result,0,1,&create_sql,&error),ORM_STATUS_OK);
    tstr ddl_copy=tstr_from_v(create_sql); check_not_null(ddl_copy);
    check_not_null(strstr(ddl_copy,"UNIQUE KEY `a_2` (`a` ASC)"));
    check_not_null(strstr(ddl_copy,"UNIQUE KEY `a_3` (`a` DESC)"));
    check_not_null(strstr(ddl_copy,"KEY `b` (`b` ASC)"));
    check_not_null(strstr(ddl_copy,"KEY `a` (`b` ASC)")); tstr_free(ddl_copy);
  }
  it("uses MySQL constraint symbols only when an index name is absent") {
    raw_query("CREATE TABLE items(id BIGINT PRIMARY KEY,a BIGINT,b BIGINT,c BIGINT,"
      "CONSTRAINT uq UNIQUE INDEX ix(a),CONSTRAINT by_b UNIQUE(b),CONSTRAINT UNIQUE(c))"); command(0);
    raw_query("SHOW INDEX FROM items"); check_equal(execute(),ORM_STATUS_OK); count_is(4);
    const char *names[]={"ix","by_b","c"}; bool found[3]={0};
    for(uint64_t row=1;row<4;++row) {
      vstr name={0}; int64_t non_unique=-1;
      check_equal(orm_result_get_text(result,row,2,&name,&error),ORM_STATUS_OK);
      check_equal(orm_result_get_int64(result,row,1,&non_unique,&error),ORM_STATUS_OK);
      size_t match=3;
      for(size_t i=0;i<3;++i) if(name.len==strlen(names[i]) && !memcmp(name.data,names[i],name.len)) { match=i; break; }
      check_true(match<3); if(match<3) { check_false(found[match]); found[match]=true; check_equal(non_unique,0); }
    }
    for(size_t i=0;i<3;++i) check_true(found[i]);
    raw_query("INSERT INTO items(id,a,b,c) VALUES(1,10,20,30)"); command(1);
    const char *duplicates[]={
      "INSERT INTO items(id,a,b,c) VALUES(2,10,21,31)",
      "INSERT INTO items(id,a,b,c) VALUES(3,11,20,32)",
      "INSERT INTO items(id,a,b,c) VALUES(4,12,22,30)"};
    for(size_t i=0;i<3;++i) { raw_query(duplicates[i]); check_equal(execute(),ORM_STATUS_CONSTRAINT); }
    raw_query("SHOW CREATE TABLE items"); check_equal(execute(),ORM_STATUS_OK);
    vstr create_sql={0}; check_equal(orm_result_get_text(result,0,1,&create_sql,&error),ORM_STATUS_OK);
    tstr replay=tstr_from_v(create_sql); check_not_null(replay);
    check_not_null(strstr(replay,"UNIQUE KEY `ix` (`a` ASC)"));
    check_not_null(strstr(replay,"UNIQUE KEY `by_b` (`b` ASC)"));
    check_not_null(strstr(replay,"UNIQUE KEY `c` (`c` ASC)"));
    raw_query("DROP TABLE items"); command(0); raw_query(replay); command(0); tstr_free(replay);
  }
  it("normalizes primary constraint symbols and materializes redundant column unique keys") {
    raw_query("CREATE TABLE items(id BIGINT PRIMARY KEY UNIQUE,a BIGINT)"); command(0);
    raw_query("SHOW INDEX FROM items"); check_equal(execute(),ORM_STATUS_OK); count_is(2);
    text_at(0,2,"PRIMARY"); text_at(1,2,"id");
    raw_query("SHOW CREATE TABLE items"); check_equal(execute(),ORM_STATUS_OK);
    vstr create_sql={0}; check_equal(orm_result_get_text(result,0,1,&create_sql,&error),ORM_STATUS_OK);
    tstr replay=tstr_from_v(create_sql); check_not_null(replay);
    check_not_null(strstr(replay,"PRIMARY KEY (`id`)"));
    check_not_null(strstr(replay,"UNIQUE KEY `id` (`id` ASC)"));
    raw_query("DROP TABLE items"); command(0); raw_query(replay); command(0); tstr_free(replay);
    raw_query("DROP TABLE items"); command(0);
    raw_query("CREATE TABLE items(id BIGINT,CONSTRAINT pk PRIMARY KEY(id))"); command(0);
    raw_query("SHOW INDEX FROM items"); check_equal(execute(),ORM_STATUS_OK); count_is(1); text_at(0,2,"PRIMARY");
    raw_query("SHOW CREATE TABLE items"); check_equal(execute(),ORM_STATUS_OK);
    text_at(0,1,"CREATE TABLE `items` (\n  `id` bigint NOT NULL,\n  PRIMARY KEY (`id`)\n)");
    raw_query("DROP TABLE items"); command(0);
    raw_query("CREATE TABLE items(id BIGINT KEY,a BIGINT)"); command(0);
    raw_query("INSERT INTO items(id,a) VALUES(1,10),(2,20)"); command(2);
    raw_query("INSERT INTO items(id,a) VALUES(1,30)"); check_equal(execute(),ORM_STATUS_CONSTRAINT);
    raw_query("SHOW CREATE TABLE items"); check_equal(execute(),ORM_STATUS_OK);
    text_at(0,1,"CREATE TABLE `items` (\n  `id` bigint NOT NULL,\n  `a` bigint NULL,\n  PRIMARY KEY (`id`)\n)");
  }
  it("validates every inline index before creating the table") {
    const char *sql[]={
      "CREATE TABLE items(id BIGINT PRIMARY KEY,a BIGINT,KEY good(a),KEY bad(absent))",
      "CREATE TABLE items(id BIGINT PRIMARY KEY,a BIGINT,KEY repeated(a),KEY repeated(id))",
      "CREATE TABLE items(id BIGINT PRIMARY KEY,a BOOLEAN,KEY bad(a))",
      "CREATE TABLE items(id BIGINT PRIMARY KEY,a BIGINT,KEY bad(a,a))",
      "CREATE TABLE items(id BIGINT PRIMARY KEY,KEY `PRIMARY`(id))"};
    const orm_status_t codes[]={ORM_STATUS_SQL_ERROR,ORM_STATUS_CONSTRAINT,ORM_STATUS_UNSUPPORTED,
      ORM_STATUS_SQL_ERROR,ORM_STATUS_SQL_ERROR};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      raw_query(sql[i]); const orm_status_t actual=execute();
      if(actual!=codes[i]) info("inline index rejection %zu: %s",i,sql[i]);
      check_equal(actual,codes[i]);
      raw_query("SHOW TABLES"); check_equal(execute(),ORM_STATUS_OK); count_is(0);
    }
  }
  it("bounds live savepoints while allowing replacement and slot reuse at capacity") {
    seed(); disconnect(); orm_option_t option={orm_view("sql_max_savepoints"),orm_view("2")};
    check_equal(connect_profile("relational","false",&option,1),ORM_STATUS_OK);
    check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("one"),&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("two"),&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("three"),&error),ORM_STATUS_LIMIT_EXCEEDED);
    check_equal(orm_transaction_savepoint(transaction,orm_view("ONE"),&error),ORM_STATUS_OK);
    check_equal(orm_transaction_release_savepoint(transaction,orm_view("two"),&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("three"),&error),ORM_STATUS_OK);
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("one"),&error),ORM_STATUS_OK);
    check_equal(orm_transaction_release_savepoint(transaction,orm_view("three"),&error),ORM_STATUS_SQL_ERROR);
    check_equal(orm_transaction_savepoint(transaction,orm_view("four"),&error),ORM_STATUS_OK);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
  }
  it("rejects savepoint control during a live query and invalid names without damaging the transaction") {
    seed(); check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("safe"),&error),ORM_STATUS_OK);
    raw_query("SELECT id,score FROM items LIMIT 1"); open_rows();
    check_equal(orm_transaction_savepoint(transaction,orm_view("next"),&error),ORM_STATUS_BUSY);
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("safe"),&error),ORM_STATUS_BUSY);
    check_equal(orm_transaction_release_savepoint(transaction,orm_view("safe"),&error),ORM_STATUS_BUSY);
    row(1,10); end_rows();
    check_equal(orm_transaction_savepoint(transaction,orm_view("catalog-private-create"),&error),ORM_STATUS_INVALID_ARGUMENT);
    check_equal(orm_transaction_savepoint(transaction,orm_view("`quoted`"),&error),ORM_STATUS_INVALID_ARGUMENT);
    enum { NAME_CAPACITY=64 }; char name[NAME_CAPACITY+1]; memset(name,'a',NAME_CAPACITY); name[NAME_CAPACITY]=0;
    check_equal(orm_transaction_savepoint(transaction,orm_view(name),&error),ORM_STATUS_LIMIT_EXCEEDED);
    name[NAME_CAPACITY-1]=0;
    check_equal(orm_transaction_savepoint(transaction,orm_view(name),&error),ORM_STATUS_OK);
    raw_query("INSERT INTO items(id,score) VALUES(1,99)"); check_equal(execute(),ORM_STATUS_CONSTRAINT);
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("safe"),&error),ORM_STATUS_OK);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
  }
}

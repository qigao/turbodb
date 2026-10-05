#include <tidessql/tidessql.h>
#include "work.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

static size_t reserves, resizes, fail_reserve, fail_resize;
static stl_status probe_reserve(vec_t *v, size_t n) { return ++reserves == fail_reserve ? STL_OUT_OF_MEMORY : vec_reserve(v,n); }
static stl_status probe_resize(vec_t *v, size_t n) { return ++resizes == fail_resize ? STL_OUT_OF_MEMORY : vec_resize(v,n); }
#define vec_reserve probe_reserve
#define vec_resize probe_resize
#include "../../src/work.c"
#undef vec_reserve
#undef vec_resize

enum { ORIGINAL_SCORE=10, CHANGED_SCORE=25, RESPONSE_SENTINEL=47, VALUE_COUNT=7 };
static tdsql_database *database;
static tdsql_connection *connection, *other;
static tdsql_statement *statement, *second;
static tdsql_response response;
static turbodb_error_t error;
static char *directory;
static const char family_name[]="prepared";

static turbodb_status_t run_on(tdsql_connection *c,const char *sql) {
  const tdsql_request request=tdsql_request_default(turbodb_view(sql));
  return tdsql_connection_run(c,&request,&response,&error);
}
static turbodb_status_t run(const char *sql) { return run_on(connection,sql); }
static turbodb_status_t prepare(const char *sql,tdsql_statement **out) {
  const tdsql_request request=tdsql_request_default(turbodb_view(sql));
  return tdsql_connection_statement_prepare(connection,&request,out,&error);
}
static turbodb_status_t execute(const turbodb_value_t *values,size_t count) {
  const tdsql_bindings bindings=tdsql_bindings_default(values,count);
  return tdsql_statement_execute(statement,&bindings,&response,&error);
}
static void close_result(void) {
  check_equal(tdsql_result_destroy_checked(response.result,&error),TURBODB_STATUS_OK);
  response.result=NULL;
}
static void close_statement(tdsql_statement **s) {
  check_equal(tdsql_statement_close(*s,&error),TURBODB_STATUS_OK); *s=NULL;
}
static tdsql_session_state state(void) {
  tdsql_session_state out=tdsql_session_state_default();
  check_equal(tdsql_connection_state(connection,&out,&error),TURBODB_STATUS_OK); return out;
}
static tdsql_row next(void) {
  tdsql_row row={0}; check_equal(tdsql_result_next(response.result,&row,&error),TURBODB_STATUS_OK); return row;
}
static void score_is(int64_t expected) {
  check_equal(run("SELECT score FROM items WHERE id=1"),TURBODB_STATUS_OK);
  const tdsql_row row=next(); check_equal(row.state,TDSQL_ROW);
  check_equal(row.values[0].kind,TURBODB_VALUE_INT64); check_equal(row.values[0].data.int64_value,expected);
  close_result();
}
static void ddl_command(const char *sql) {
  check_equal(prepare(sql,&statement),TURBODB_STATUS_OK);
  check_equal(tdsql_statement_parameters(statement),0u); check_equal(tdsql_statement_columns(statement),0u);
  check_equal(execute(NULL,0),TURBODB_STATUS_OK); check_null(response.result); check_equal(response.affected_rows,0u);
  check_equal(tdsql_statement_reset(statement,&error),TURBODB_STATUS_OK); close_statement(&statement);
}
static void open_database(const char *count,const char *bytes) {
  const turbodb_option_t options[]={
    {turbodb_view("path"),turbodb_view(directory)},
    {turbodb_view("column_family"),turbodb_view(family_name)},
    {turbodb_view("sql_initialize"),turbodb_view("true")},
    {turbodb_view("sql_max_prepared_statements"),turbodb_view(count)},
    {turbodb_view("sql_max_prepared_bytes"),turbodb_view(bytes)}
  };
  tdsql_config config=tdsql_config_default(); config.options=options; config.option_count=sizeof(options)/sizeof(options[0]);
  check_equal(tdsql_database_open(&config,&database,&error),TURBODB_STATUS_OK);
  check_equal(tdsql_database_connect(database,&connection,&error),TURBODB_STATUS_OK);
  check_equal(tdsql_database_connect(database,&other,&error),TURBODB_STATUS_OK);
  check_equal(run("CREATE TABLE items(id BIGINT PRIMARY KEY,score BIGINT)"),TURBODB_STATUS_OK);
  check_equal(run("INSERT INTO items VALUES(1,10)"),TURBODB_STATUS_OK);
}

spec("TidesSQL public prepared statement lifecycle") {
  before_each() {
    database=NULL; connection=other=NULL; statement=second=NULL;
    response=tdsql_response_default(); turbodb_error_init(&error);
    reserves=resizes=fail_reserve=fail_resize=0;
    directory=tt_make_temp_dir("tidessql-prepared"); check_not_null(directory);
    open_database("2","16777216");
  }
  after_each() {
    fail_reserve=fail_resize=0;
    close_result(); close_statement(&second); close_statement(&statement);
    check_equal(tdsql_connection_close(other,&error),TURBODB_STATUS_OK); other=NULL;
    check_equal(tdsql_connection_close(connection,&error),TURBODB_STATUS_OK); connection=NULL;
    check_equal(tdsql_database_close(database,&error),TURBODB_STATUS_OK); database=NULL;
    check_equal(tt_remove_tree(directory),0); free(directory); directory=NULL;
  }
  it("owns SQL and column metadata independently of the prepare input") {
    char sql[]="SELECT score AS value FROM items WHERE id=? LIMIT ?";
    check_equal(prepare(sql,&statement),TURBODB_STATUS_OK); memset(sql,'x',strlen(sql));
    check_equal(tdsql_statement_parameters(statement),2u); check_equal(tdsql_statement_columns(statement),1u);
    tdsql_column column={0};
    check_equal(tdsql_statement_parameter(statement,0,&column,&error),TURBODB_STATUS_OK);
    check_equal(column.kind,TURBODB_VALUE_INT64); check_true(column.nullable);
    check_equal(tdsql_statement_parameter(statement,1,&column,&error),TURBODB_STATUS_OK);
    check_equal(column.kind,TURBODB_VALUE_UINT64);
    check_equal(tdsql_statement_column(statement,0,&column,&error),TURBODB_STATUS_OK);
    check_equal(column.name.data,"value",column.name.len); check_equal(column.kind,TURBODB_VALUE_INT64);
    check_false(state().in_transaction); check_false(state().busy);
    const turbodb_value_t values[]={turbodb_i64(1),turbodb_u64(1)};
    check_equal(execute(values,2),TURBODB_STATUS_OK); check_equal(next().values[0].data.int64_value,ORIGINAL_SCORE);
    close_result(); check_equal(execute(values,2),TURBODB_STATUS_OK); close_result();
  }
  it("prepares a command without writing and reuses it after DML version changes") {
    check_equal(prepare("UPDATE items SET score=? WHERE id=?",&statement),TURBODB_STATUS_OK);
    check_equal(tdsql_statement_columns(statement),0u); score_is(ORIGINAL_SCORE);
    turbodb_value_t values[]={turbodb_i64(CHANGED_SCORE),turbodb_i64(1)};
    check_equal(execute(values,2),TURBODB_STATUS_OK); check_equal(response.kind,TDSQL_COMMAND);
    check_equal(response.affected_rows,1u); score_is(CHANGED_SCORE);
    values[0]=turbodb_i64(ORIGINAL_SCORE); check_equal(execute(values,2),TURBODB_STATUS_OK); score_is(ORIGINAL_SCORE);
  }
  it("defers dangerous expression evaluation until result consumption") {
    check_equal(prepare("SELECT 1 DIV 0 AS value",&statement),TURBODB_STATUS_OK);
    check_equal(state().warning_count,0u); check_equal(execute(NULL,0),TURBODB_STATUS_OK);
    check_equal(next().state,TDSQL_ROW); close_result();
    check_equal(state().warning_count,1u);
    close_statement(&statement);
    check_equal(prepare("SELECT score FROM items WHERE id=?",&statement),TURBODB_STATUS_OK);
    check_equal(state().warning_count,1u); check_equal(tdsql_statement_reset(statement,&error),TURBODB_STATUS_OK);
    check_equal(state().warning_count,1u);
  }
  it("preserves next read-only transaction mode during prepare") {
    check_equal(run("SET TRANSACTION READ ONLY"),TURBODB_STATUS_OK);
    check_equal(prepare("UPDATE items SET score=? WHERE id=?",&statement),TURBODB_STATUS_OK);
    check_false(state().in_transaction);
    const turbodb_value_t values[]={turbodb_i64(CHANGED_SCORE),turbodb_i64(1)};
    check_equal(execute(values,2),TURBODB_STATUS_SQL_ERROR); score_is(ORIGINAL_SCORE);
    check_equal(execute(values,2),TURBODB_STATUS_OK); score_is(CHANGED_SCORE);
  }
  it("executes seven actual typed values with authoritative per-execution metadata") {
    check_equal(prepare("SELECT ? AS n,? AS i,? AS u,? AS d,? AS b,? AS t,? AS bytes",&statement),TURBODB_STATUS_OK);
    unsigned char blob[]={0,1,255}; char text[]="text";
    turbodb_value_t values[VALUE_COUNT]={turbodb_null(),turbodb_i64(INT64_MIN),turbodb_u64(UINT64_MAX),
      turbodb_f64(1.5),turbodb_bool(true),turbodb_text(text),turbodb_blob(blob,sizeof(blob))};
    check_equal(execute(values,VALUE_COUNT),TURBODB_STATUS_OK);
    for(size_t i=0;i<VALUE_COUNT;++i) {
      tdsql_column column={0}; check_equal(tdsql_result_column(response.result,i,&column,&error),TURBODB_STATUS_OK);
      check_equal(column.kind,values[i].kind);
    }
    text[0]='x'; blob[1]=0;
    const tdsql_row row=next(); check_equal(row.state,TDSQL_ROW); check_equal(row.count,(size_t)VALUE_COUNT);
    check_equal(row.values[1].data.int64_value,INT64_MIN); check_equal(row.values[2].data.uint64_value,UINT64_MAX);
    check_equal(row.values[3].data.double_value,1.5); check_equal(row.values[4].data.boolean_value,1u);
    check_equal(row.values[5].data.text_value.data,"text",strlen("text"));
    check_equal(row.values[6].data.blob_value.size,sizeof(blob));
    const unsigned char expected[]={0,1,255}; check_equal(row.values[6].data.blob_value.data,expected,sizeof(expected));
    close_result(); values[0]=turbodb_i64(ORIGINAL_SCORE); check_equal(execute(values,VALUE_COUNT),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,ORIGINAL_SCORE); close_result();
  }
  it("retains statement and connection while a result exists including cancellation") {
    check_equal(prepare("SELECT ? AS value",&statement),TURBODB_STATUS_OK);
    check_equal(prepare("SELECT 1 AS value",&second),TURBODB_STATUS_OK);
    const turbodb_value_t value=turbodb_i64(ORIGINAL_SCORE);
    check_equal(execute(&value,1),TURBODB_STATUS_OK);
    check_equal(tdsql_statement_reset(statement,&error),TURBODB_STATUS_BUSY);
    check_equal(tdsql_statement_close(statement,&error),TURBODB_STATUS_BUSY);
    check_equal(execute(&value,1),TURBODB_STATUS_BUSY);
    check_equal(tdsql_connection_close(connection,&error),TURBODB_STATUS_BUSY);
    close_statement(&second);
    check_equal(tdsql_result_cancel(response.result,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_statement_close(statement,&error),TURBODB_STATUS_BUSY);
    close_result(); check_equal(tdsql_statement_reset(statement,&error),TURBODB_STATUS_OK);
    check_equal(execute(&value,1),TURBODB_STATUS_OK); check_equal(next().state,TDSQL_ROW);
    check_equal(next().state,TDSQL_DONE); check_equal(tdsql_statement_reset(statement,&error),TURBODB_STATUS_BUSY);
    close_result(); close_statement(&statement);
  }
  it("latches schema changes before executing a prepared write") {
    check_equal(prepare("UPDATE items SET score=? WHERE id=?",&statement),TURBODB_STATUS_OK);
    check_equal(run_on(other,"ALTER TABLE items ADD COLUMN extra BIGINT"),TURBODB_STATUS_OK);
    const turbodb_value_t values[]={turbodb_i64(CHANGED_SCORE),turbodb_i64(1)};
    response.affected_rows=RESPONSE_SENTINEL;
    check_equal(execute(values,2),TURBODB_STATUS_INVALID_STATE); check_equal(response.affected_rows,RESPONSE_SENTINEL);
    check_equal(tdsql_statement_reset(statement,&error),TURBODB_STATUS_INVALID_STATE);
    check_equal(run_on(other,"ALTER TABLE items DROP COLUMN extra"),TURBODB_STATUS_OK);
    check_equal(execute(values,2),TURBODB_STATUS_INVALID_STATE); score_is(ORIGINAL_SCORE);
    close_statement(&statement); check_equal(prepare("UPDATE items SET score=? WHERE id=?",&statement),TURBODB_STATUS_OK);
    check_equal(execute(values,2),TURBODB_STATUS_OK); score_is(CHANGED_SCORE);
  }
  it("invalidates an identical schema after drop and recreate changes table identity") {
    check_equal(prepare("SELECT score FROM items WHERE id=?",&statement),TURBODB_STATUS_OK);
    check_equal(run_on(other,"DROP TABLE items"),TURBODB_STATUS_OK);
    check_equal(run_on(other,"CREATE TABLE items(id BIGINT PRIMARY KEY,score BIGINT)"),TURBODB_STATUS_OK);
    check_equal(run_on(other,"INSERT INTO items VALUES(1,25)"),TURBODB_STATUS_OK);
    const turbodb_value_t value=turbodb_i64(1);
    tdsql_bindings bindings=tdsql_bindings_default(&value,1); bindings.result_context_bytes=SIZE_MAX;
    check_equal(tdsql_statement_execute(statement,&bindings,&response,&error),TURBODB_STATUS_INVALID_STATE); check_null(response.result);
    tdsql_column column={.kind=TURBODB_VALUE_BLOB};
    check_equal(tdsql_statement_column(statement,0,&column,&error),TURBODB_STATUS_INVALID_STATE);
    check_equal(column.kind,TURBODB_VALUE_BLOB);
  }
  it("invalidates renamed columns and changed defaults even when projected types remain the same") {
    check_equal(prepare("SELECT score FROM items WHERE id=?",&statement),TURBODB_STATUS_OK);
    check_equal(run_on(other,"ALTER TABLE items ALTER COLUMN score SET DEFAULT 25"),TURBODB_STATUS_OK);
    const turbodb_value_t value=turbodb_i64(1);
    check_equal(execute(&value,1),TURBODB_STATUS_INVALID_STATE); close_statement(&statement);
    check_equal(prepare("SELECT score FROM items WHERE id=?",&statement),TURBODB_STATUS_OK);
    check_equal(run_on(other,"ALTER TABLE items RENAME COLUMN score TO amount"),TURBODB_STATUS_OK);
    check_equal(execute(&value,1),TURBODB_STATUS_INVALID_STATE); check_null(response.result);
  }
  it("uses the current SQL snapshot and detects changes only after that snapshot ends") {
    check_equal(run("BEGIN"),TURBODB_STATUS_OK);
    check_equal(prepare("SELECT score FROM items WHERE id=?",&statement),TURBODB_STATUS_OK);
    check_true(state().in_transaction);
    check_equal(run_on(other,"ALTER TABLE items ADD COLUMN extra BIGINT"),TURBODB_STATUS_OK);
    const turbodb_value_t value=turbodb_i64(1);
    check_equal(execute(&value,1),TURBODB_STATUS_OK); check_equal(next().values[0].data.int64_value,ORIGINAL_SCORE); close_result();
    check_equal(run("ROLLBACK"),TURBODB_STATUS_OK);
    check_equal(execute(&value,1),TURBODB_STATUS_INVALID_STATE);
  }
  it("honors explicit transactions and autocommit-off without committing at prepare or reset") {
    check_equal(run("SET autocommit=0"),TURBODB_STATUS_OK);
    check_equal(prepare("UPDATE items SET score=? WHERE id=?",&statement),TURBODB_STATUS_OK);
    check_false(state().in_transaction);
    const turbodb_value_t values[]={turbodb_i64(CHANGED_SCORE),turbodb_i64(1)};
    check_equal(execute(values,2),TURBODB_STATUS_OK); check_true(state().in_transaction);
    check_equal(tdsql_statement_reset(statement,&error),TURBODB_STATUS_OK); check_true(state().in_transaction);
    check_equal(run("ROLLBACK"),TURBODB_STATUS_OK); score_is(ORIGINAL_SCORE);
    check_equal(run("COMMIT"),TURBODB_STATUS_OK);
    check_equal(execute(values,2),TURBODB_STATUS_OK); check_equal(run("COMMIT"),TURBODB_STATUS_OK);
    check_equal(run("SET autocommit=1"),TURBODB_STATUS_OK); score_is(CHANGED_SCORE);
  }
  it("keeps statements isolated across shared database sessions") {
    check_equal(prepare("SELECT ? AS value",&statement),TURBODB_STATUS_OK);
    const tdsql_request request=tdsql_request_default(turbodb_view("SELECT ? AS value"));
    check_equal(tdsql_connection_statement_prepare(other,&request,&second,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_connection_close(other,&error),TURBODB_STATUS_BUSY);
    const turbodb_value_t value=turbodb_i64(CHANGED_SCORE); const tdsql_bindings bindings=tdsql_bindings_default(&value,1);
    check_equal(tdsql_statement_execute(second,&bindings,&response,&error),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,CHANGED_SCORE); close_result(); close_statement(&second);
    check_equal(execute(&value,1),TURBODB_STATUS_OK); close_result();
  }
  it("refunds count capacity on close and gives exact admission errors") {
    check_equal(prepare("SELECT ? AS value",&statement),TURBODB_STATUS_OK);
    check_equal(prepare("SELECT 1 AS value",&second),TURBODB_STATUS_OK);
    tdsql_statement *extra=statement;
    check_equal(prepare("SELECT 2 AS value",&extra),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(extra);
    close_statement(&second); check_equal(prepare("SELECT 2 AS value",&second),TURBODB_STATUS_OK);
    close_statement(&second); close_statement(&statement);
    tdsql_request request=tdsql_request_default(turbodb_view("SELECT ? AS a,? AS b")); request.limits.max_parameters=1;
    check_equal(tdsql_connection_statement_prepare(connection,&request,&statement,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(statement);
    request=tdsql_request_default(turbodb_view("SELECT 1 AS a,2 AS b")); request.limits.max_columns=1;
    check_equal(tdsql_connection_statement_prepare(connection,&request,&statement,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(statement);
    request=tdsql_request_default(turbodb_view("SELECT 1 AS value")); request.limits.max_query_bytes=1;
    check_equal(tdsql_connection_statement_prepare(connection,&request,&statement,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(statement);
  }
  it("rejects invalid ABI counts and payloads with unchanged response and usable descriptor") {
    check_equal(prepare("SELECT ? AS value",&statement),TURBODB_STATUS_OK);
    const turbodb_value_t value=turbodb_i64(ORIGINAL_SCORE); tdsql_bindings bindings=tdsql_bindings_default(&value,1);
    response.affected_rows=RESPONSE_SENTINEL; bindings.abi_version++;
    check_equal(tdsql_statement_execute(statement,&bindings,&response,&error),TURBODB_STATUS_ABI_MISMATCH);
    check_equal(response.affected_rows,RESPONSE_SENTINEL); bindings=tdsql_bindings_default(&value,1);
    bindings.struct_size=sizeof(bindings.struct_size);
    check_equal(tdsql_statement_execute(statement,&bindings,&response,&error),TURBODB_STATUS_ABI_MISMATCH);
    bindings=tdsql_bindings_default(NULL,1);
    check_equal(tdsql_statement_execute(statement,&bindings,&response,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(execute(&value,0),TURBODB_STATUS_INVALID_ARGUMENT); check_equal(execute(&value,2),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(response.affected_rows,RESPONSE_SENTINEL);
    turbodb_value_t bad={.kind=TURBODB_VALUE_TEXT,.data.text_value={NULL,1}};
    check_equal(execute(&bad,1),TURBODB_STATUS_TYPE_ERROR); check_equal(response.affected_rows,RESPONSE_SENTINEL);
    check_equal(execute(&value,1),TURBODB_STATUS_OK); close_result();
    tdsql_column column={.kind=TURBODB_VALUE_BLOB};
    check_equal(tdsql_statement_parameter(statement,1,&column,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(tdsql_statement_column(statement,1,&column,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(column.kind,TURBODB_VALUE_BLOB);
  }
  it("rejects unsupported prepare shapes without reserving a descriptor") {
    const char *const unsupported[]={"CREATE TABLE x(id BIGINT)","WITH q AS(SELECT 1 AS id) SELECT id FROM q",
      "SELECT a.id FROM items a JOIN items b ON a.id=b.id","SELECT ABS(?) AS value",
      "SELECT score FROM items WHERE id=? AND score=?","INSERT INTO items SELECT id,score FROM items"};
    for(size_t i=0;i<sizeof(unsupported)/sizeof(unsupported[0]);++i) {
      info("unsupported prepared SQL: %s",unsupported[i]);
      check_equal(prepare(unsupported[i],&statement),TURBODB_STATUS_UNSUPPORTED); check_null(statement);
      check_false(state().in_transaction); check_false(state().rollback_required);
    }
    check_equal(prepare("SELECT FROM",&statement),TURBODB_STATUS_SQL_ERROR); check_null(statement);
    check_equal(prepare("SELECT missing FROM items",&statement),TURBODB_STATUS_SQL_ERROR); check_null(statement);
    check_equal(prepare("SELECT ? AS value",&statement),TURBODB_STATUS_OK);
  }
  it("validates prepare admission without accepting execution values or touching session state") {
    tdsql_request request=tdsql_request_default(turbodb_view("SELECT ? AS value"));
    request.abi_version++;
    check_equal(tdsql_connection_statement_prepare(connection,&request,&statement,&error),TURBODB_STATUS_ABI_MISMATCH); check_null(statement);
    request=tdsql_request_default(turbodb_view("SELECT ? AS value")); request.parameter_count=1;
    check_equal(tdsql_connection_statement_prepare(connection,&request,&statement,&error),TURBODB_STATUS_INVALID_ARGUMENT); check_null(statement);
    request.parameter_count=0; request.parameter_context=&request;
    check_equal(tdsql_connection_statement_prepare(connection,&request,&statement,&error),TURBODB_STATUS_INVALID_ARGUMENT); check_null(statement);
    request.parameter_context=NULL;
    check_equal(tdsql_connection_statement_prepare(NULL,&request,&statement,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(tdsql_connection_statement_prepare(connection,NULL,&statement,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(tdsql_connection_statement_prepare(connection,&request,NULL,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(tdsql_statement_reset(NULL,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(tdsql_statement_close(NULL,&error),TURBODB_STATUS_OK);
    check_false(state().in_transaction); check_false(state().busy);
    check_equal(prepare("SELECT ? AS value",&statement),TURBODB_STATUS_OK);
    const turbodb_value_t value=turbodb_i64(1); const tdsql_bindings bindings=tdsql_bindings_default(&value,1);
    response.abi_version++;
    check_equal(tdsql_statement_execute(statement,&bindings,&response,&error),TURBODB_STATUS_ABI_MISMATCH);
    response=tdsql_response_default(); check_equal(execute(&value,1),TURBODB_STATUS_OK);
    check_equal(prepare("SELECT 1 AS value",&second),TURBODB_STATUS_BUSY); check_null(second); close_result();
  }
  it("rejects zero overflowing and duplicate prepared connection limits before native open") {
    const char *const names[]={"sql_max_prepared_statements","sql_max_prepared_bytes"};
    const char *const invalid[]={"0","-1","18446744073709551616"};
    turbodb_option_t options[]={
      {turbodb_view("path"),turbodb_view(directory)},
      {turbodb_view("column_family"),turbodb_view(family_name)},
      {turbodb_view(names[0]),turbodb_view("1")},
      {turbodb_view(names[0]),turbodb_view("1")}
    };
    tdsql_config config=tdsql_config_default(); config.options=options; config.option_count=3;
    tdsql_connection *rejected=NULL;
    for(size_t i=0;i<sizeof(names)/sizeof(names[0]);++i) {
      options[2].keyword=turbodb_view(names[i]);
      for(size_t j=0;j<sizeof(invalid)/sizeof(invalid[0]);++j) {
        options[2].value=turbodb_view(invalid[j]);
        check_equal(tdsql_connection_open(&config,&rejected,&error),TURBODB_STATUS_INVALID_ARGUMENT); check_null(rejected);
      }
    }
    options[2].keyword=options[3].keyword; options[2].value=turbodb_view("1"); config.option_count=4;
    check_equal(tdsql_connection_open(&config,&rejected,&error),TURBODB_STATUS_INVALID_ARGUMENT); check_null(rejected);
  }
  it("prepares INSERT REPLACE and DELETE with source-ordered target metadata") {
    check_equal(prepare("INSERT INTO items(score,id) VALUES(?,?)",&statement),TURBODB_STATUS_OK);
    turbodb_value_t values[]={turbodb_i64(CHANGED_SCORE),turbodb_i64(2)};
    check_equal(execute(values,2),TURBODB_STATUS_OK); check_equal(response.affected_rows,1u); close_statement(&statement);
    check_equal(prepare("REPLACE INTO items SET id=?,score=?",&statement),TURBODB_STATUS_OK);
    values[0]=turbodb_i64(2); values[1]=turbodb_i64(ORIGINAL_SCORE);
    check_equal(execute(values,2),TURBODB_STATUS_OK); check_equal(response.affected_rows,2u); close_statement(&statement);
    check_equal(prepare("DELETE FROM items WHERE id=?",&statement),TURBODB_STATUS_OK);
    check_equal(execute(values,1),TURBODB_STATUS_OK); check_equal(response.affected_rows,1u); score_is(ORIGINAL_SCORE);
  }
  it("refunds every temporary and retained WORK reservation on prepare allocation failures") {
    const char *sql="SELECT score AS value FROM items WHERE id=? LIMIT ?";
    reserves=resizes=0; check_equal(prepare(sql,&statement),TURBODB_STATUS_OK);
    const size_t reserve_count=reserves,resize_count=resizes; check_greater(reserve_count,0u); check_greater(resize_count,0u);
    close_statement(&statement);
    for(size_t pass=0;pass<2;++pass) {
      const size_t count=pass?resize_count:reserve_count;
      for(size_t point=1;point<=count;++point) {
        reserves=resizes=0; fail_reserve=pass?0:point; fail_resize=pass?point:0;
        check_equal(prepare(sql,&statement),TURBODB_STATUS_OUT_OF_MEMORY); check_null(statement);
        fail_reserve=fail_resize=0; check_false(state().busy); check_false(state().in_transaction);
        check_false(state().rollback_required); check_equal(state().failure,TURBODB_STATUS_OK);
        check_equal(prepare(sql,&statement),TURBODB_STATUS_OK); close_statement(&statement);
      }
    }
    score_is(ORIGINAL_SCORE);
  }
  group("DDL PREPARE") {
    it("creates a table with defaults and indexes only at execute and retains command intent") {
      check_equal(prepare("CREATE TABLE created(id BIGINT UNSIGNED PRIMARY KEY,score DOUBLE DEFAULT(7/2.0),UNIQUE KEY score_key(score))",&statement),TURBODB_STATUS_OK);
      check_equal(tdsql_statement_parameters(statement),0u); check_equal(tdsql_statement_columns(statement),0u);
      check_equal(run("SELECT id FROM created"),TURBODB_STATUS_SQL_ERROR); check_null(response.result);
      check_equal(execute(NULL,0),TURBODB_STATUS_OK); check_equal(response.affected_rows,0u); check_null(response.result);
      check_equal(execute(NULL,0),TURBODB_STATUS_CONSTRAINT);
      check_equal(tdsql_statement_reset(statement,&error),TURBODB_STATUS_OK); close_statement(&statement);
      check_equal(run("INSERT INTO created(id) VALUES(1)"),TURBODB_STATUS_OK);
      check_equal(run("SELECT score FROM created WHERE id=1"),TURBODB_STATUS_OK);
      const tdsql_row row=next(); check_equal(row.state,TDSQL_ROW); check_equal(row.values[0].data.double_value,3.5); close_result();
      check_equal(run("INSERT INTO created(id) VALUES(2)"),TURBODB_STATUS_CONSTRAINT);
      check_equal(prepare("CREATE TABLE IF NOT EXISTS created(id BIGINT PRIMARY KEY)",&statement),TURBODB_STATUS_OK);
      check_equal(execute(NULL,0),TURBODB_STATUS_OK); check_equal(execute(NULL,0),TURBODB_STATUS_OK);
    }
    it("defers default value evaluation and assignment errors to execute") {
      const struct { const char *sql; turbodb_status_t status; } cases[]={
        {"CREATE TABLE invalid_default(id BIGINT PRIMARY KEY,v BIGINT DEFAULT(7 DIV 0))",TURBODB_STATUS_SQL_ERROR},
        {"CREATE TABLE invalid_default(id BIGINT PRIMARY KEY,v BIGINT DEFAULT 'bad')",TURBODB_STATUS_TYPE_ERROR},
        {"CREATE TABLE invalid_default(id BIGINT PRIMARY KEY,v BIGINT NOT NULL DEFAULT NULL)",TURBODB_STATUS_SQL_ERROR},
        {"CREATE TABLE invalid_default(id BIGINT PRIMARY KEY,v BIGINT UNSIGNED DEFAULT -1)",TURBODB_STATUS_OUT_OF_RANGE}
      };
      for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        info("default execution phase: %s",cases[i].sql);
        check_equal(prepare(cases[i].sql,&statement),TURBODB_STATUS_OK);
        check_equal(state().warning_count,0u); check_false(state().in_transaction);
        check_equal(execute(NULL,0),cases[i].status); check_false(state().rollback_required);
        check_equal(tdsql_statement_reset(statement,&error),TURBODB_STATUS_OK);
        check_equal(execute(NULL,0),cases[i].status); close_statement(&statement);
        check_equal(run("SELECT id FROM invalid_default"),TURBODB_STATUS_SQL_ERROR);
      }
      score_is(ORIGINAL_SCORE);
    }
    it("supports all existing ALTER actions without changing rows or metadata at prepare") {
      check_equal(prepare("ALTER TABLE items ADD extra DOUBLE DEFAULT(7/2.0) FIRST",&statement),TURBODB_STATUS_OK);
      check_equal(run("SELECT extra FROM items"),TURBODB_STATUS_SQL_ERROR); score_is(ORIGINAL_SCORE);
      check_equal(execute(NULL,0),TURBODB_STATUS_OK); close_statement(&statement);
      check_equal(run("SELECT extra FROM items WHERE id=1"),TURBODB_STATUS_OK);
      const tdsql_row added=next(); check_equal(added.state,TDSQL_ROW); check_equal(added.values[0].data.double_value,3.5); close_result();
      ddl_command("ALTER TABLE items ALTER score SET DEFAULT(7 DIV 2)");
      check_equal(run("INSERT INTO items(id) VALUES(2)"),TURBODB_STATUS_OK);
      check_equal(run("SELECT score FROM items WHERE id=2"),TURBODB_STATUS_OK);
      const tdsql_row defaulted=next(); check_equal(defaulted.state,TDSQL_ROW); check_equal(defaulted.values[0].data.int64_value,3); close_result();
      ddl_command("ALTER TABLE items ALTER score DROP DEFAULT");
      ddl_command("ALTER TABLE items DROP COLUMN extra");
      ddl_command("ALTER TABLE items RENAME COLUMN score TO value");
      check_equal(prepare("ALTER TABLE items RENAME TO renamed",&statement),TURBODB_STATUS_OK);
      check_equal(execute(NULL,0),TURBODB_STATUS_OK);
      check_equal(execute(NULL,0),TURBODB_STATUS_SQL_ERROR); close_statement(&statement);
      check_equal(run("SELECT value FROM renamed WHERE id=1"),TURBODB_STATUS_OK);
      const tdsql_row renamed=next(); check_equal(renamed.state,TDSQL_ROW); check_equal(renamed.values[0].data.int64_value,ORIGINAL_SCORE); close_result();
    }
    it("checks nonempty column rewrite requirements only during execute") {
      check_equal(prepare("ALTER TABLE items ADD extra BIGINT NOT NULL",&statement),TURBODB_STATUS_OK);
      check_equal(execute(NULL,0),TURBODB_STATUS_UNSUPPORTED); close_statement(&statement);
      check_equal(run("SELECT extra FROM items"),TURBODB_STATUS_SQL_ERROR); score_is(ORIGINAL_SCORE);
      check_equal(prepare("ALTER TABLE items ALTER score SET DEFAULT(7 DIV 0)",&statement),TURBODB_STATUS_OK);
      check_equal(execute(NULL,0),TURBODB_STATUS_SQL_ERROR); close_statement(&statement);
      score_is(ORIGINAL_SCORE);
    }
    it("prepares and executes secondary index create and drop against current metadata") {
      ddl_command("CREATE UNIQUE INDEX score_key ON items(score DESC)");
      check_equal(prepare("DROP INDEX score_key ON items",&statement),TURBODB_STATUS_OK);
      check_equal(run("INSERT INTO items VALUES(2,10)"),TURBODB_STATUS_CONSTRAINT);
      check_equal(execute(NULL,0),TURBODB_STATUS_OK);
      check_equal(execute(NULL,0),TURBODB_STATUS_SQL_ERROR); close_statement(&statement);
      check_equal(run("INSERT INTO items VALUES(2,10)"),TURBODB_STATUS_OK);
      check_equal(run("ALTER TABLE items ADD real_value DOUBLE DEFAULT 0.5"),TURBODB_STATUS_OK);
      ddl_command("CREATE INDEX real_key ON items(real_value)");
      check_equal(prepare("ALTER TABLE items DROP COLUMN real_value",&statement),TURBODB_STATUS_UNSUPPORTED); check_null(statement);
      ddl_command("DROP INDEX real_key ON items"); ddl_command("ALTER TABLE items DROP COLUMN real_value");
      score_is(ORIGINAL_SCORE);
    }
    it("prepares truncate and multi-table drop without clearing data before execute") {
      check_equal(prepare("TRUNCATE TABLE items",&statement),TURBODB_STATUS_OK); score_is(ORIGINAL_SCORE);
      check_equal(execute(NULL,0),TURBODB_STATUS_OK); check_equal(execute(NULL,0),TURBODB_STATUS_OK); close_statement(&statement);
      check_equal(run("SELECT id FROM items"),TURBODB_STATUS_OK); check_equal(next().state,TDSQL_DONE); close_result();
      check_equal(run("CREATE TABLE companion(id BIGINT PRIMARY KEY)"),TURBODB_STATUS_OK);
      check_equal(prepare("DROP TABLE items,companion",&statement),TURBODB_STATUS_OK);
      check_equal(execute(NULL,0),TURBODB_STATUS_OK); check_equal(execute(NULL,0),TURBODB_STATUS_SQL_ERROR); close_statement(&statement);
      check_equal(run("SELECT id FROM items"),TURBODB_STATUS_SQL_ERROR);
      check_equal(run("SELECT id FROM companion"),TURBODB_STATUS_SQL_ERROR);
      check_equal(prepare("DROP TABLE IF EXISTS items,companion",&statement),TURBODB_STATUS_OK);
      check_equal(execute(NULL,0),TURBODB_STATUS_OK); check_equal(execute(NULL,0),TURBODB_STATUS_OK);
    }
    it("preserves transactional DDL rollback and next read-only admission") {
      check_equal(run("SET TRANSACTION READ ONLY"),TURBODB_STATUS_OK);
      check_equal(prepare("CREATE TABLE created(id BIGINT PRIMARY KEY)",&statement),TURBODB_STATUS_OK);
      check_equal(execute(NULL,0),TURBODB_STATUS_SQL_ERROR); check_equal(execute(NULL,0),TURBODB_STATUS_OK); close_statement(&statement);
      check_equal(run("BEGIN"),TURBODB_STATUS_OK);
      ddl_command("ALTER TABLE items ADD extra BIGINT DEFAULT 7 AFTER id");
      check_true(state().in_transaction); check_equal(run("ROLLBACK"),TURBODB_STATUS_OK);
      check_equal(run("SELECT extra FROM items"),TURBODB_STATUS_SQL_ERROR); score_is(ORIGINAL_SCORE);
    }
    it("rejects unsupported DDL structure and markers before publishing metadata") {
      const char *const rejected[]={
        "CREATE TABLE unsupported(id BIGINT PRIMARY KEY,v TEXT)",
        "CREATE TABLE unsupported(id BIGINT PRIMARY KEY,v BIGINT DEFAULT ?)",
        "CREATE TABLE unsupported(id BIGINT PRIMARY KEY,v BIGINT DEFAULT(id+1))",
        "CREATE TABLE unsupported(id BIGINT PRIMARY KEY,v BIGINT DEFAULT RAND())",
        "CREATE TABLE unsupported(id BIGINT PRIMARY KEY,v BIGINT DEFAULT @@autocommit)",
        "CREATE TABLE unsupported(id BIGINT PRIMARY KEY,CHECK(id>0))",
        "ALTER TABLE items ADD id DOUBLE", "ALTER TABLE items DROP id",
        "ALTER TABLE items ADD extra TEXT", "ALTER TABLE items ALTER score SET DEFAULT ?",
        "CREATE INDEX unsupported ON items((score+1))", "DROP INDEX `PRIMARY` ON items"
      };
      for(size_t i=0;i<sizeof(rejected)/sizeof(rejected[0]);++i) {
        info("DDL structural rejection: %s",rejected[i]);
        const turbodb_status_t status=prepare(rejected[i],&statement);
        check_not_equal(status,TURBODB_STATUS_OK); check_null(statement); check_false(state().rollback_required);
      }
      check_equal(prepare("ALTER TABLE items ADD extra BIGINT AFTER missing",&statement),TURBODB_STATUS_SQL_ERROR); check_null(statement);
      check_equal(prepare("TRUNCATE TABLE missing",&statement),TURBODB_STATUS_SQL_ERROR); check_null(statement);
      check_equal(prepare("CREATE INDEX missing_key ON items(missing)",&statement),TURBODB_STATUS_SQL_ERROR); check_null(statement);
      check_equal(prepare("DROP INDEX missing_key ON items",&statement),TURBODB_STATUS_SQL_ERROR); check_null(statement);
      score_is(ORIGINAL_SCORE);
    }
    it("refunds every DDL binding allocation failure without consuming descriptor quota") {
      const char *const statements[]={
        "CREATE TABLE created(id BIGINT PRIMARY KEY,v DOUBLE DEFAULT(7/2.0),KEY v_key(v))",
        "ALTER TABLE items ADD extra BIGINT DEFAULT(7 DIV 2)",
        "CREATE INDEX score_key ON items(score)"
      };
      for(size_t s=0;s<sizeof(statements)/sizeof(statements[0]);++s) {
        reserves=resizes=0; check_equal(prepare(statements[s],&statement),TURBODB_STATUS_OK);
        const size_t counts[]={reserves,resizes}; close_statement(&statement);
        for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=counts[pass];++point) {
          info("DDL allocation failure: statement %zu, pass %zu, point %zu",s,pass,point);
          reserves=resizes=0; fail_reserve=pass?0:point; fail_resize=pass?point:0;
          check_equal(prepare(statements[s],&statement),TURBODB_STATUS_OUT_OF_MEMORY); check_null(statement);
          fail_reserve=fail_resize=0; check_false(state().rollback_required); check_false(state().in_transaction);
          check_equal(prepare(statements[s],&statement),TURBODB_STATUS_OK); close_statement(&statement);
        }
      }
      score_is(ORIGINAL_SCORE);
    }
  }
}

#include "registry.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

static size_t vector_steps,fail_vector_step,close_calls;
static bool fail_close;
static stl_status probe_init(vec_t *v,size_t size,size_t align,size_t limit) {
  return ++vector_steps==fail_vector_step?STL_OUT_OF_MEMORY:vec_init_bytes(v,size,align,limit);
}
static stl_status probe_reserve(vec_t *v,size_t count) {
  return ++vector_steps==fail_vector_step?STL_OUT_OF_MEMORY:vec_reserve(v,count);
}
static stl_status probe_resize(vec_t *v,size_t count) {
  return ++vector_steps==fail_vector_step?STL_OUT_OF_MEMORY:vec_resize(v,count);
}
static turbodb_status_t probe_close(tdsql_statement *s,turbodb_error_t *e) {
  ++close_calls; const turbodb_status_t status=tdsql_statement_close(s,e);
  /* Simulate the documented consumed-handle cleanup error at the SDK boundary. */
  if(status==TURBODB_STATUS_OK && fail_close) { fail_close=false; return TURBODB_STATUS_CLEANUP_FAILED; }
  return status;
}
#define vec_init_bytes probe_init
#define vec_reserve probe_reserve
#define vec_resize probe_resize
#define tdsql_statement_close probe_close
#include "../../server/mysql/registry.c"
#undef tdsql_statement_close
#undef vec_resize
#undef vec_reserve
#undef vec_init_bytes

enum { TEST_STATEMENTS=3,TEST_BUFFER=1024,TEST_SENTINEL=55,TEST_CHANGED=25,TEST_ORIGINAL=10 };
static tdsql_database *database;
static tdsql_connection *first,*second;
static tdsql_mysql_registry registry,other;
static tdsql_mysql_registry_config configuration;
static tdsql_response response;
static turbodb_error_t error;
static char *directory;
static uint8_t command_bytes[TEST_BUFFER];
static size_t command_size;
static tdsql_mysql_command command;
static mysql_stmt_prepare_ok_t prepared;

static turbodb_status_t sql(tdsql_connection *connection,const char *text) {
  const tdsql_request request=tdsql_request_default(turbodb_view(text));
  return tdsql_connection_run(connection,&request,&response,&error);
}
static void clear_result(void) {
  check_equal(tdsql_result_destroy_checked(response.result,&error),TURBODB_STATUS_OK); response.result=NULL;
}
static tdsql_row row(void) {
  tdsql_row out={0}; check_equal(tdsql_result_next(response.result,&out,&error),TURBODB_STATUS_OK); return out;
}
static void decode(void) {
  check_equal(tdsql_mysql_command_decode(command_bytes,command_size,sizeof(command_bytes),&command),TDSQL_MYSQL_OK);
}
static turbodb_status_t prepare_on(tdsql_mysql_registry *r,const char *text,mysql_stmt_prepare_ok_t *out) {
  check_equal(mysql_wire_build_stmt_prepare((const uint8_t *)text,strlen(text),command_bytes,sizeof(command_bytes),&command_size),MYSQL_WIRE_STATUS_OK);
  decode(); return tdsql_mysql_registry_prepare(r,&command,out,&error);
}
static turbodb_status_t prepare(const char *text) { return prepare_on(&registry,text,&prepared); }
static void execute_bytes(uint32_t id,const mysql_stmt_value_t *values,size_t count) {
  check_equal(mysql_wire_build_stmt_execute(id,values,count,command_bytes,sizeof(command_bytes),&command_size),MYSQL_WIRE_STATUS_OK); decode();
}
static turbodb_status_t execute(uint32_t id,const mysql_stmt_value_t *values,size_t count) {
  execute_bytes(id,values,count); return tdsql_mysql_registry_execute(&registry,&command,&response,&error);
}
static void reuse_type(void) {
    enum { TEST_TYPE_FLAG=11,TEST_TYPE_OFFSET=12,TEST_VALUE_OFFSET=14 };
  command_bytes[TEST_TYPE_FLAG]=0; memmove(command_bytes+TEST_TYPE_OFFSET,command_bytes+TEST_VALUE_OFFSET,command_size-TEST_VALUE_OFFSET);
  command_size-=TDSQL_MYSQL_TYPE_BYTES; decode();
}
static tdsql_session_state state(tdsql_connection *c) {
  tdsql_session_state out=tdsql_session_state_default(); check_equal(tdsql_connection_state(c,&out,&error),TURBODB_STATUS_OK); return out;
}
static void unchanged_response(const tdsql_response *expected) { check_equal(&response,expected,sizeof(response)); }
static void score_is(int64_t value) {
  check_equal(sql(second,"SELECT score FROM items WHERE id=1"),TURBODB_STATUS_OK);
  check_equal(row().values[0].data.int64_value,value); clear_result();
}

spec("MySQL connection statement registry with real TidesSQL") {
  before_each() {
    database=NULL; first=second=NULL; registry=(tdsql_mysql_registry){0}; other=(tdsql_mysql_registry){0};
    configuration=tdsql_mysql_registry_config_default(); configuration.max_statements=TEST_STATEMENTS;
    response=tdsql_response_default(); prepared=(mysql_stmt_prepare_ok_t){.statement_id=TEST_SENTINEL}; turbodb_error_init(&error);
    vector_steps=fail_vector_step=close_calls=0; fail_close=false;
    directory=tt_make_temp_dir("tidessql-mysql-registry"); check_not_null(directory);
    const turbodb_option_t options[]={
      {turbodb_view("path"),turbodb_view(directory)},
      {turbodb_view("column_family"),turbodb_view("registry")},
      {turbodb_view("sql_initialize"),turbodb_view("true")}
    };
    tdsql_config config=tdsql_config_default(); config.options=options; config.option_count=sizeof(options)/sizeof(options[0]);
    check_equal(tdsql_database_open(&config,&database,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_database_connect(database,&first,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_database_connect(database,&second,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_mysql_registry_init(&registry,first,&configuration,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_mysql_registry_init(&other,second,&configuration,&error),TURBODB_STATUS_OK);
    check_equal(sql(first,"CREATE TABLE items(id BIGINT PRIMARY KEY,score BIGINT)"),TURBODB_STATUS_OK);
    check_equal(sql(first,"INSERT INTO items VALUES(1,10)"),TURBODB_STATUS_OK);
    vector_steps=close_calls=0;
  }
  after_each() {
    fail_vector_step=0; fail_close=false; clear_result();
    check_equal(tdsql_mysql_registry_dispose(&registry,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_mysql_registry_dispose(&other,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_connection_close(first,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_connection_close(second,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_database_close(database,&error),TURBODB_STATUS_OK);
    check_equal(tt_remove_tree(directory),0); free(directory); directory=NULL;
  }
  it("prepares real stable metadata and executes a native marker through the client codec") {
    char text[]="SELECT score AS value FROM items WHERE id=?";
    check_equal(prepare(text),TURBODB_STATUS_OK); const uint32_t id=prepared.statement_id;
    check_equal(id,1u); check_equal(prepared.parameter_count,1u); check_equal(prepared.column_count,1u); check_equal(prepared.warning_count,0u);
    memset(text,'x',strlen(text)); memset(command_bytes,'x',sizeof(command_bytes));
    tdsql_column column={0}; check_equal(tdsql_mysql_registry_parameter(&registry,id,0,&column,&error),TURBODB_STATUS_OK);
    check_equal(column.kind,TURBODB_VALUE_INT64); check_equal(column.name.data,"?",column.name.len);
    check_equal(tdsql_mysql_registry_column(&registry,id,0,&column,&error),TURBODB_STATUS_OK); check_equal(column.name.data,"value",column.name.len);
    const mysql_stmt_value_t value={.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=1};
    check_equal(execute(id,&value,1),TURBODB_STATUS_OK); check_equal(response.kind,TDSQL_ROWS);
    check_equal(row().values[0].data.int64_value,TEST_ORIGINAL); check_equal(row().state,TDSQL_DONE); clear_result();
    check_equal(tdsql_connection_close(first,&error),TURBODB_STATUS_BUSY);
    check_equal(tdsql_mysql_registry_close(&registry,id,&error),TURBODB_STATUS_OK); check_equal(tdsql_mysql_registry_count(&registry),0u);
  }
  it("keeps ID namespaces and caches independent across two SQL sessions") {
    check_equal(prepare("SELECT ? AS value"),TURBODB_STATUS_OK); const uint32_t id=prepared.statement_id;
    mysql_stmt_prepare_ok_t other_id={0}; check_equal(prepare_on(&other,"SELECT 9 AS value",&other_id),TURBODB_STATUS_OK);
    check_equal(other_id.statement_id,id); check_equal(other_id.parameter_count,0u);
    const mysql_stmt_value_t value={.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=TEST_CHANGED};
    check_equal(execute(id,&value,1),TURBODB_STATUS_OK); check_equal(row().values[0].data.int64_value,TEST_CHANGED); clear_result();
    execute_bytes(id,NULL,0); check_equal(tdsql_mysql_registry_execute(&other,&command,&response,&error),TURBODB_STATUS_OK);
    check_equal(row().values[0].data.int64_value,9); clear_result();
    check_equal(tdsql_mysql_registry_close(&registry,id,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_mysql_registry_reset(&registry,id,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(tdsql_mysql_registry_reset(&other,id,&error),TURBODB_STATUS_OK);
  }
  it("preserves the validated cache through RESET and a malformed replacement") {
    check_equal(prepare("SELECT ? AS value"),TURBODB_STATUS_OK); const uint32_t id=prepared.statement_id;
    const mysql_stmt_value_t value={.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=TEST_CHANGED};
    execute_bytes(id,&value,1); reuse_type(); const tdsql_response untouched=response;
    check_equal(tdsql_mysql_registry_execute(&registry,&command,&response,&error),TURBODB_STATUS_INVALID_ARGUMENT); unchanged_response(&untouched);
    check_equal(execute(id,&value,1),TURBODB_STATUS_OK); clear_result();
    registry_entry *entry=registry_find(&registry,id); check_true(entry->types_valid);
    check_equal(tdsql_mysql_registry_reset(&registry,id,&error),TURBODB_STATUS_OK);
    execute_bytes(id,&value,1); command_bytes[12]=MYSQL_TYPE_DOUBLE; --command_size; decode();
    check_equal(tdsql_mysql_registry_execute(&registry,&command,&response,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(*(const uint16_t *)vec_data_const(&entry->types),MYSQL_TYPE_LONGLONG); check_true(entry->types_valid);
    execute_bytes(id,&value,1); reuse_type(); check_equal(tdsql_mysql_registry_execute(&registry,&command,&response,&error),TURBODB_STATUS_OK);
    check_equal(row().values[0].data.int64_value,TEST_CHANGED); clear_result();
  }
  it("erases borrowed byte scratch on return and result owns its parameter copy") {
    check_equal(prepare("SELECT ? AS text,? AS bytes"),TURBODB_STATUS_OK); const uint32_t id=prepared.statement_id;
    const uint8_t blob[]={0,255,1}; const mysql_stmt_value_t values[]={
      {.kind=MYSQL_STMT_VALUE_TEXT,.data.bytes={(const uint8_t *)"'quoted'",8}},
      {.kind=MYSQL_STMT_VALUE_BLOB,.data.bytes={blob,sizeof(blob)}}
    };
    check_equal(execute(id,values,2),TURBODB_STATUS_OK); registry_entry *entry=registry_find(&registry,id);
    const turbodb_value_t empty[2]={0}; check_equal(vec_data_const(&entry->values),empty,sizeof(empty));
    memset(command_bytes,'x',sizeof(command_bytes)); const tdsql_row actual=row();
    check_equal(actual.values[0].data.text_value.data,"'quoted'",8); check_equal(actual.values[1].data.blob_value.data,blob,sizeof(blob)); clear_result();
  }
  it("rejects busy execute/reset/close including EOF and cancellation without changing mappings") {
    check_equal(prepare("SELECT ? AS value"),TURBODB_STATUS_OK); const uint32_t id=prepared.statement_id;
    const mysql_stmt_value_t value={.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=1};
    check_equal(execute(id,&value,1),TURBODB_STATUS_OK); const tdsql_response unchanged=response;
    check_equal(tdsql_mysql_registry_execute(&registry,&command,&response,&error),TURBODB_STATUS_BUSY); unchanged_response(&unchanged);
    tdsql_response vacant=tdsql_response_default();
    check_equal(tdsql_mysql_registry_execute(&registry,&command,&vacant,&error),TURBODB_STATUS_BUSY);
    check_equal(tdsql_mysql_registry_reset(&registry,id,&error),TURBODB_STATUS_BUSY);
    check_equal(tdsql_mysql_registry_close(&registry,id,&error),TURBODB_STATUS_BUSY); check_equal(tdsql_mysql_registry_count(&registry),1u);
    check_equal(row().state,TDSQL_ROW); check_equal(row().state,TDSQL_DONE);
    check_equal(tdsql_mysql_registry_reset(&registry,id,&error),TURBODB_STATUS_BUSY);
    check_equal(tdsql_result_cancel(response.result,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_mysql_registry_close(&registry,id,&error),TURBODB_STATUS_BUSY);
    clear_result(); check_equal(tdsql_mysql_registry_reset(&registry,id,&error),TURBODB_STATUS_OK);
  }
  it("does not reuse closed IDs or wrap the exhausted uint32 namespace") {
    check_equal(prepare("SELECT 1 AS value"),TURBODB_STATUS_OK); const uint32_t stale=prepared.statement_id;
    check_equal(tdsql_mysql_registry_close(&registry,stale,&error),TURBODB_STATUS_OK);
    check_equal(prepare("SELECT 2 AS value"),TURBODB_STATUS_OK); check_equal(prepared.statement_id,stale+1);
    check_equal(execute(stale,NULL,0),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(tdsql_mysql_registry_close(&registry,stale,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_mysql_registry_count(&registry),1u); registry.last_id=UINT32_MAX-1;
    check_equal(prepare("SELECT 3 AS value"),TURBODB_STATUS_OK); check_equal(prepared.statement_id,UINT32_MAX);
    check_equal(tdsql_mysql_registry_close(&registry,UINT32_MAX,&error),TURBODB_STATUS_OK);
    check_equal(prepare("SELECT 4 AS value"),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(registry.last_id,UINT32_MAX);
  }
  it("bounds slots before SDK preparation and refunds exact wire byte quota on close") {
    for(size_t i=0;i<TEST_STATEMENTS;++i) check_equal(prepare("SELECT 1 AS value"),TURBODB_STATUS_OK);
    const size_t steps=vector_steps; const mysql_stmt_prepare_ok_t sentinel=prepared;
    check_equal(prepare("SELECT invalid"),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(vector_steps,steps); check_equal(&prepared,&sentinel,sizeof(prepared));
    check_equal(tdsql_mysql_registry_close(&registry,1,&error),TURBODB_STATUS_OK);
    check_equal(prepare("SELECT ? AS value"),TURBODB_STATUS_OK); size_t bytes=0; check_true(registry_bytes(&registry,&bytes));
    registry.config.max_wire_bytes=bytes; check_equal(tdsql_mysql_registry_close(&registry,2,&error),TURBODB_STATUS_OK);
    check_equal(prepare("SELECT ? AS value"),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(tdsql_mysql_registry_count(&registry),2u);
    check_equal(tdsql_mysql_registry_close(&registry,prepared.statement_id,&error),TURBODB_STATUS_OK);
    check_equal(prepare("SELECT ? AS value"),TURBODB_STATUS_OK); check_true(registry_bytes(&registry,&bytes)); check_equal(bytes,registry.config.max_wire_bytes);
  }
  it("rolls back every fixed vector construction failure and consumes no prepare ID") {
    for(size_t step=1;step<=6;++step) {
      const uint32_t before=registry.last_id; fail_vector_step=vector_steps+step;
      check_equal(prepare("SELECT ? AS value"),TURBODB_STATUS_OUT_OF_MEMORY);
      check_equal(registry.last_id,before); check_equal(tdsql_mysql_registry_count(&registry),0u);
      fail_vector_step=0; check_equal(prepare("SELECT ? AS value"),TURBODB_STATUS_OK); check_equal(prepared.statement_id,before+1);
      check_equal(tdsql_mysql_registry_close(&registry,prepared.statement_id,&error),TURBODB_STATUS_OK);
    }
  }
  it("preserves zero initialization and wire budget on all slot construction failures") {
    for(size_t step=1;step<=3;++step) {
      tdsql_mysql_registry candidate={0}; fail_vector_step=vector_steps+step;
      check_equal(tdsql_mysql_registry_init(&candidate,first,&configuration,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(candidate.connection); check_false(candidate.slots.initialized); fail_vector_step=0;
      check_equal(tdsql_mysql_registry_init(&candidate,first,&configuration,&error),TURBODB_STATUS_OK);
      check_equal(tdsql_mysql_registry_dispose(&candidate,&error),TURBODB_STATUS_OK);
    }
    tdsql_mysql_registry candidate={0}; tdsql_mysql_registry_config bad=configuration; bad.max_wire_bytes=1;
    check_equal(tdsql_mysql_registry_init(&candidate,first,&bad,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(candidate.connection);
    bad=configuration; bad.max_statements=SIZE_MAX;
    check_equal(tdsql_mysql_registry_init(&candidate,first,&bad,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    bad=configuration; bad.limits.max_parameters=(size_t)UINT16_MAX+1;
    check_equal(tdsql_mysql_registry_init(&candidate,first,&bad,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    bad=configuration; bad.max_command_bytes=0;
    check_equal(tdsql_mysql_registry_init(&candidate,first,&bad,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(tdsql_mysql_registry_init(&registry,first,&configuration,&error),TURBODB_STATUS_INVALID_ARGUMENT);
  }
  it("executes DDL intent and native writes in the existing transaction and schema lifecycle") {
    check_equal(prepare("CREATE TABLE made(id BIGINT PRIMARY KEY)"),TURBODB_STATUS_OK); const uint32_t ddl=prepared.statement_id;
    check_equal(prepared.parameter_count,0u); check_equal(prepared.column_count,0u);
    check_equal(execute(ddl,NULL,0),TURBODB_STATUS_OK); check_equal(response.kind,TDSQL_COMMAND);
    check_equal(prepare("UPDATE items SET score=? WHERE id=1"),TURBODB_STATUS_OK); const uint32_t update=prepared.statement_id;
    check_equal(sql(first,"BEGIN"),TURBODB_STATUS_OK);
    const mysql_stmt_value_t value={.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=TEST_CHANGED};
    check_equal(execute(update,&value,1),TURBODB_STATUS_OK); check_equal(response.affected_rows,1u); check_true(state(first).in_transaction);
    score_is(TEST_ORIGINAL); check_equal(sql(first,"ROLLBACK"),TURBODB_STATUS_OK); score_is(TEST_ORIGINAL);
    check_equal(sql(first,"ALTER TABLE items ADD COLUMN extra BIGINT"),TURBODB_STATUS_OK);
    check_equal(execute(update,&value,1),TURBODB_STATUS_INVALID_STATE);
    check_equal(tdsql_mysql_registry_reset(&registry,update,&error),TURBODB_STATUS_INVALID_STATE);
    check_equal(tdsql_mysql_registry_close(&registry,update,&error),TURBODB_STATUS_OK);
  }
  it("retains valid types after a SQL error and preserves transaction access state across RESET") {
    check_equal(prepare("UPDATE items SET score=? WHERE id=1"),TURBODB_STATUS_OK); const uint32_t id=prepared.statement_id;
    check_equal(sql(first,"SET TRANSACTION READ ONLY"),TURBODB_STATUS_OK);
    check_equal(tdsql_mysql_registry_reset(&registry,id,&error),TURBODB_STATUS_OK);
    const mysql_stmt_value_t value={.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=TEST_CHANGED};
    const tdsql_response unchanged=response;
    check_equal(execute(id,&value,1),TURBODB_STATUS_SQL_ERROR); unchanged_response(&unchanged);
    registry_entry *entry=registry_find(&registry,id); check_true(entry->types_valid); check_false(state(first).in_transaction);
    const turbodb_value_t empty={0}; check_equal(vec_data_const(&entry->values),&empty,sizeof(empty)); score_is(TEST_ORIGINAL);
    check_equal(tdsql_mysql_registry_reset(&registry,id,&error),TURBODB_STATUS_OK);
    execute_bytes(id,&value,1); reuse_type();
    check_equal(tdsql_mysql_registry_execute(&registry,&command,&response,&error),TURBODB_STATUS_OK);
    check_equal(response.affected_rows,1u); score_is(TEST_CHANGED);
  }
  it("drains statements before disconnect rollback and a fresh session restarts the ID namespace") {
    check_equal(prepare("UPDATE items SET score=? WHERE id=1"),TURBODB_STATUS_OK); const uint32_t id=prepared.statement_id;
    check_equal(sql(first,"BEGIN"),TURBODB_STATUS_OK);
    const mysql_stmt_value_t value={.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=TEST_CHANGED};
    check_equal(execute(id,&value,1),TURBODB_STATUS_OK); score_is(TEST_ORIGINAL);
    check_equal(tdsql_mysql_registry_dispose(&registry,&error),TURBODB_STATUS_OK);
    check_true(state(first).in_transaction); check_equal(tdsql_connection_close(first,&error),TURBODB_STATUS_OK); first=NULL;
    score_is(TEST_ORIGINAL); check_equal(tdsql_database_connect(database,&first,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_mysql_registry_init(&registry,first,&configuration,&error),TURBODB_STATUS_OK);
    check_equal(prepare("SELECT 1 AS value"),TURBODB_STATUS_OK); check_equal(prepared.statement_id,1u);
  }
  it("retains only pending result ownership when disposal enters closing") {
    check_equal(prepare("SELECT 1 AS value"),TURBODB_STATUS_OK); const uint32_t first_id=prepared.statement_id;
    check_equal(prepare("SELECT 2 AS value"),TURBODB_STATUS_OK);
    check_equal(execute(first_id,NULL,0),TURBODB_STATUS_OK);
    check_equal(tdsql_mysql_registry_dispose(&registry,&error),TURBODB_STATUS_BUSY);
    check_true(registry.closing); check_equal(tdsql_mysql_registry_count(&registry),1u);
    check_equal(prepare("SELECT 3 AS value"),TURBODB_STATUS_INVALID_STATE);
    clear_result(); check_equal(tdsql_mysql_registry_dispose(&registry,&error),TURBODB_STATUS_OK);
    check_null(registry.connection); check_equal(tdsql_mysql_registry_dispose(&registry,&error),TURBODB_STATUS_OK);
  }
  it("consumes cleanup-failed handles once and quarantines further registry dispatch") {
    check_equal(prepare("SELECT 1 AS value"),TURBODB_STATUS_OK); const uint32_t id=prepared.statement_id;
    check_equal(prepare("SELECT 2 AS value"),TURBODB_STATUS_OK); close_calls=0; fail_close=true;
    check_equal(tdsql_mysql_registry_close(&registry,id,&error),TURBODB_STATUS_CLEANUP_FAILED);
    check_equal(tdsql_mysql_registry_count(&registry),1u); check_equal(close_calls,1u);
    check_equal(tdsql_mysql_registry_close(&registry,id,&error),TURBODB_STATUS_OK); check_equal(close_calls,1u);
    check_equal(prepare("SELECT 3 AS value"),TURBODB_STATUS_CLEANUP_FAILED);
    check_equal(tdsql_mysql_registry_dispose(&registry,&error),TURBODB_STATUS_CLEANUP_FAILED);
    check_equal(close_calls,2u); check_null(registry.connection);
  }
  it("retains a prior cleanup failure while BUSY disposal drains other handles") {
    check_equal(prepare("SELECT 1 AS value"),TURBODB_STATUS_OK); const uint32_t id=prepared.statement_id;
    check_equal(prepare("SELECT 2 AS value"),TURBODB_STATUS_OK); check_equal(prepare("SELECT 3 AS value"),TURBODB_STATUS_OK);
    check_equal(execute(id,NULL,0),TURBODB_STATUS_OK); close_calls=0; fail_close=true;
    check_equal(tdsql_mysql_registry_dispose(&registry,&error),TURBODB_STATUS_BUSY);
    check_equal(tdsql_mysql_registry_count(&registry),1u); check_equal(registry.failure,TURBODB_STATUS_CLEANUP_FAILED); check_equal(close_calls,3u);
    clear_result(); check_equal(tdsql_mysql_registry_dispose(&registry,&error),TURBODB_STATUS_CLEANUP_FAILED);
    check_null(registry.connection); check_equal(close_calls,4u);
  }
  it("rejects protocol/request limits and multiple statements before any native mutation") {
    registry.config.limits.max_query_bytes=1;
    check_equal(prepare("CREATE TABLE forbidden(id BIGINT PRIMARY KEY)"),TURBODB_STATUS_LIMIT_EXCEEDED);
    registry.config.limits=configuration.limits;
    check_equal(prepare("UPDATE items SET score=99; DELETE FROM items"),TURBODB_STATUS_LIMIT_EXCEEDED);
    score_is(TEST_ORIGINAL); check_equal(tdsql_mysql_registry_count(&registry),0u); check_equal(registry.last_id,0u);
    check_equal(prepare("SELECT ? AS value"),TURBODB_STATUS_OK); const uint32_t id=prepared.statement_id;
    const mysql_stmt_value_t value={.kind=MYSQL_STMT_VALUE_TEXT,.data.bytes={(const uint8_t *)"abc",3}};
    registry.config.limits.max_parameter_bytes=2; const tdsql_response unchanged=response;
    check_equal(execute(id,&value,1),TURBODB_STATUS_LIMIT_EXCEEDED); unchanged_response(&unchanged);
    registry_entry *entry=registry_find(&registry,id); check_false(entry->types_valid);
    registry.config.limits=configuration.limits; execute_bytes(id,&value,1);
    registry.config.max_command_bytes=command_size-1;
    check_equal(tdsql_mysql_registry_execute(&registry,&command,&response,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_false(entry->types_valid);
    registry.config.max_command_bytes=configuration.max_command_bytes; response.abi_version++;
    check_equal(tdsql_mysql_registry_execute(&registry,&command,&response,&error),TURBODB_STATUS_ABI_MISMATCH); check_false(entry->types_valid); response.abi_version--;
  }
}

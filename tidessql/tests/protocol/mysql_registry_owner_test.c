#include "dispatch.h"
#include "catalog_store.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

static size_t commits,rollbacks;
static bool commit_unknown,rollback_failure;
static int probe_commit(orm_tidesdb_transaction_t *transaction) {
  ++commits; const int status=orm_tidesdb_txn_commit(transaction);
  if(status==ORM_TDB_SUCCESS && commit_unknown) { commit_unknown=false; return ORM_TDB_ERR_IO; }
  return status;
}
static int probe_rollback(orm_tidesdb_transaction_t *transaction) {
  ++rollbacks; const int status=orm_tidesdb_txn_rollback(transaction);
  if(status==ORM_TDB_SUCCESS && rollback_failure) { rollback_failure=false; return ORM_TDB_ERR_IO; }
  return status;
}
#define orm_tidesdb_txn_commit probe_commit
#define orm_tidesdb_txn_rollback probe_rollback
#include "../../src/catalog_store.c"
#undef orm_tidesdb_txn_rollback
#undef orm_tidesdb_txn_commit

enum { TEST_BUFFER=1024,TEST_ORIGINAL=10,TEST_CHANGED=25,TEST_SENTINEL=77,TEST_ERR_GENERAL=1105 };
static tdsql_database *database;
static tdsql_connection *first,*second;
static tdsql_mysql_registry registry;
static tdsql_mysql_dispatch dispatch;
static tdsql_response response;
static turbodb_error_t error;
static mysql_stmt_prepare_ok_t prepared;
static tdsql_mysql_command command;
static uint8_t payload[TEST_BUFFER];
static size_t payload_size;
static uint8_t scratch[TEST_BUFFER],output[TEST_BUFFER];
static size_t reply_size;
static char *directory;
static void decode(void) { check_equal(tdsql_mysql_command_decode(payload,payload_size,sizeof(payload),&command),TDSQL_MYSQL_OK); }
static turbodb_status_t prepare(const char *sql) {
  check_equal(mysql_wire_build_stmt_prepare((const uint8_t *)sql,strlen(sql),payload,sizeof(payload),&payload_size),MYSQL_WIRE_STATUS_OK);
  decode(); return tdsql_mysql_registry_prepare(&registry,&command,&prepared,&error);
}
static turbodb_status_t execute(uint32_t id,const mysql_stmt_value_t *value) {
  check_equal(mysql_wire_build_stmt_execute(id,value,1,payload,sizeof(payload),&payload_size),MYSQL_WIRE_STATUS_OK);
  decode(); return tdsql_mysql_registry_execute(&registry,&command,&response,&error);
}
static tdsql_session_state state(void) {
  tdsql_session_state out=tdsql_session_state_default(); check_equal(tdsql_connection_state(first,&out,&error),TURBODB_STATUS_OK); return out;
}
static void score_is(int64_t expected) {
  const tdsql_request request=tdsql_request_default(turbodb_view("SELECT score FROM items WHERE id=1"));
  tdsql_result *result=NULL; check_equal(tdsql_connection_query(second,&request,&result,&error),TURBODB_STATUS_OK);
  tdsql_row row={0}; check_equal(tdsql_result_next(result,&row,&error),TURBODB_STATUS_OK);
  check_equal(row.values[0].data.int64_value,expected); check_equal(tdsql_result_destroy_checked(result,&error),TURBODB_STATUS_OK);
}
static void admit_query(const char *sql) {
  payload[0]=TDSQL_MYSQL_QUERY; payload_size=strlen(sql)+1; memcpy(payload+1,sql,payload_size-1);
  check_equal(tdsql_mysql_dispatch_accept(&dispatch,payload,payload_size,1,&error),TURBODB_STATUS_OK);
}
static const uint8_t *reply(void) {
  tdsql_mysql_output out={output,sizeof(output),0}; int kind=0;
  check_equal(tdsql_mysql_dispatch_emit(&dispatch,&out,&kind,&error),TURBODB_STATUS_OK);
  check_equal(kind,TDSQL_MYSQL_REPLY_PACKET); check_true(out.size>MYSQL_WIRE_PACKET_HEADER_SIZE);
  reply_size=out.size-MYSQL_WIRE_PACKET_HEADER_SIZE;
  return output+MYSQL_WIRE_PACKET_HEADER_SIZE;
}
static void acknowledge(void) { check_equal(tdsql_mysql_dispatch_acknowledge(&dispatch,&error),TURBODB_STATUS_OK); }
static void error_then_disconnect(void) {
  const uint8_t *data=reply(); mysql_wire_err_packet_t out={0};
  check_equal(mysql_wire_decode_err_packet(data,reply_size,MYSQL_WIRE_CLIENT_PROTOCOL_41,&out),MYSQL_WIRE_STATUS_OK);
  check_equal(out.error_code,TEST_ERR_GENERAL); check_equal(out.sql_state,"HY000",6); acknowledge();
  tdsql_mysql_output frame={output,sizeof(output),0}; int kind=0;
  check_equal(tdsql_mysql_dispatch_emit(&dispatch,&frame,&kind,&error),TURBODB_STATUS_OK);
  check_equal(kind,TDSQL_MYSQL_REPLY_DISCONNECT);
  payload[0]=TDSQL_MYSQL_PING;
  check_equal(tdsql_mysql_dispatch_accept(&dispatch,payload,1,1,&error),TURBODB_STATUS_INVALID_STATE);
}
spec("MySQL registry native commit and cleanup boundaries") {
  before_each() {
    database=NULL; first=second=NULL; registry=(tdsql_mysql_registry){0}; dispatch=(tdsql_mysql_dispatch){0}; response=tdsql_response_default(); turbodb_error_init(&error);
    prepared=(mysql_stmt_prepare_ok_t){.statement_id=TEST_SENTINEL}; commits=rollbacks=0; commit_unknown=rollback_failure=false;
    directory=tt_make_temp_dir("tidessql-mysql-registry-owner"); check_not_null(directory);
    const turbodb_option_t options[]={
      {turbodb_view("path"),turbodb_view(directory)},
      {turbodb_view("column_family"),turbodb_view("registry_owner")},
      {turbodb_view("sql_initialize"),turbodb_view("true")}
    };
    tdsql_config config=tdsql_config_default(); config.options=options; config.option_count=sizeof(options)/sizeof(options[0]);
    check_equal(tdsql_database_open(&config,&database,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_database_connect(database,&first,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_database_connect(database,&second,&error),TURBODB_STATUS_OK);
    tdsql_request request=tdsql_request_default(turbodb_view("CREATE TABLE items(id BIGINT PRIMARY KEY,score BIGINT)")); uint64_t affected=0;
    check_equal(tdsql_connection_execute(first,&request,&affected,&error),TURBODB_STATUS_OK);
    request=tdsql_request_default(turbodb_view("INSERT INTO items VALUES(1,10)"));
    check_equal(tdsql_connection_execute(first,&request,&affected,&error),TURBODB_STATUS_OK);
    const tdsql_mysql_registry_config bounds=tdsql_mysql_registry_config_default();
    check_equal(tdsql_mysql_registry_init(&registry,first,&bounds,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_mysql_dispatch_init(&dispatch,first,&bounds,true,scratch,sizeof(scratch),&error),TURBODB_STATUS_OK); commits=rollbacks=0;
  }
  after_each() {
    commit_unknown=rollback_failure=false;
    check_equal(tdsql_result_destroy_checked(response.result,&error),TURBODB_STATUS_OK); response.result=NULL;
    check_equal(tdsql_mysql_dispatch_dispose(&dispatch,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_mysql_registry_dispose(&registry,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_connection_close(first,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_connection_close(second,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_database_close(database,&error),TURBODB_STATUS_OK);
    check_equal(tt_remove_tree(directory),0); free(directory); directory=NULL;
  }
  it("does not replay a native commit whose acknowledgement was lost") {
    check_equal(prepare("UPDATE items SET score=? WHERE id=1"),TURBODB_STATUS_OK); const uint32_t id=prepared.statement_id;
    const mysql_stmt_value_t value={.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=TEST_CHANGED};
    const tdsql_response unchanged=response; commits=0; commit_unknown=true;
    check_equal(execute(id,&value),TURBODB_STATUS_COMMIT_UNKNOWN); check_equal(&response,&unchanged,sizeof(response));
    check_equal(commits,1u); check_equal(state().failure,TURBODB_STATUS_COMMIT_UNKNOWN);
    check_equal(tdsql_mysql_registry_execute(&registry,&command,&response,&error),TURBODB_STATUS_COMMIT_UNKNOWN);
    check_equal(commits,1u); score_is(TEST_CHANGED);
    check_equal(tdsql_mysql_registry_reset(&registry,id,&error),TURBODB_STATUS_COMMIT_UNKNOWN);
    check_equal(tdsql_mysql_registry_close(&registry,id,&error),TURBODB_STATUS_OK); check_equal(tdsql_mysql_registry_count(&registry),0u);
  }
  it("publishes no protocol ID when metadata snapshot rollback fails") {
    rollback_failure=true;
    check_equal(prepare("SELECT score FROM items WHERE id=?"),TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(prepared.statement_id,TEST_SENTINEL); check_equal(registry.last_id,0u); check_equal(tdsql_mysql_registry_count(&registry),0u);
    check_equal(rollbacks,1u); check_equal(commits,0u); check_equal(state().failure,TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(prepare("SELECT 1 AS value"),TURBODB_STATUS_DATASTORE_ERROR); check_equal(rollbacks,1u); score_is(TEST_ORIGINAL);
  }
  it("releases registry handles after a consumed result cleanup quarantines its session") {
    check_equal(prepare("SELECT score FROM items WHERE id=?"),TURBODB_STATUS_OK); const uint32_t id=prepared.statement_id;
    const mysql_stmt_value_t value={.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=1};
    check_equal(execute(id,&value),TURBODB_STATUS_OK); rollback_failure=true;
    check_equal(tdsql_result_destroy_checked(response.result,&error),TURBODB_STATUS_DATASTORE_ERROR); response.result=NULL;
    check_equal(state().failure,TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(tdsql_mysql_registry_reset(&registry,id,&error),TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(tdsql_mysql_registry_close(&registry,id,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_mysql_registry_count(&registry),0u); score_is(TEST_ORIGINAL);
  }
  it("replaces result success with ERR and disconnect when checked EOF cleanup fails") {
    admit_query("SELECT score FROM items");
    (void)reply(); acknowledge(); /* count */
    (void)reply(); acknowledge(); /* metadata */
    (void)reply(); acknowledge(); /* row */
    rollback_failure=true; error_then_disconnect();
    check_null(dispatch.response.result); check_equal(state().failure,TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(rollbacks,1u); check_equal(commits,0u); score_is(TEST_ORIGINAL);
  }
  it("sends terminal ERR for a commit with lost acknowledgement and never replays it") {
    commit_unknown=true; admit_query("UPDATE items SET score=score+1 WHERE id=1");
    error_then_disconnect(); check_equal(commits,1u); check_equal(state().failure,TURBODB_STATUS_COMMIT_UNKNOWN);
    score_is(TEST_ORIGINAL+1); check_equal(commits,1u);
  }
  it("returns terminal ERR without PREPARE metadata when its native snapshot cleanup fails") {
    const char sql[]="SELECT score FROM items WHERE id=?";
    check_equal(mysql_wire_build_stmt_prepare((const uint8_t *)sql,sizeof(sql)-1,payload,sizeof(payload),&payload_size),MYSQL_WIRE_STATUS_OK);
    rollback_failure=true;
    check_equal(tdsql_mysql_dispatch_accept(&dispatch,payload,payload_size,1,&error),TURBODB_STATUS_OK);
    error_then_disconnect(); check_equal(tdsql_mysql_registry_count(&dispatch.registry),0u);
    check_equal(rollbacks,1u); check_equal(commits,0u); score_is(TEST_ORIGINAL);
  }
}

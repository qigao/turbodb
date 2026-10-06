#include "connection.h"
#include "catalog_store.h"
#include "index_directory.h"
#include <tinytest.h>
#include <stdlib.h>

static size_t allocations, fail_allocation, commits, rollbacks, get_calls, put_calls;
static bool rollback_failure, get_failure, commit_unknown;
enum { TEST_DATA_NAMESPACE=3 };
static size_t business_gets,business_seeks;
static bool reject_business_reads;
static bool business_namespace(const uint8_t *key,size_t size) {
  return size && (key[0]==TEST_DATA_NAMESPACE || key[0]==INDEX_UNIQUE_NS || key[0]==INDEX_DATA_NS);
}
static int probe_seek(orm_tidesdb_iterator_t *iterator,const uint8_t *key,size_t size) {
  if(business_namespace(key,size)) {
    ++business_seeks;
    if(reject_business_reads) return ORM_TDB_ERR_IO;
  }
  return orm_tidesdb_iter_seek(iterator,key,size);
}
static void *probe_calloc(size_t count,size_t size) {
  return ++allocations==fail_allocation?NULL:calloc(count,size);
}
static int probe_commit(orm_tidesdb_transaction_t *transaction) {
  ++commits; const int code=orm_tidesdb_txn_commit(transaction);
  if(commit_unknown&&code==ORM_TDB_SUCCESS) { commit_unknown=false; return ORM_TDB_ERR_IO; }
  return code;
}
static int probe_rollback(orm_tidesdb_transaction_t *transaction) {
  ++rollbacks; const int code=orm_tidesdb_txn_rollback(transaction);
  if(rollback_failure&&code==ORM_TDB_SUCCESS) { rollback_failure=false; return ORM_TDB_ERR_IO; }
  return code;
}
static int probe_get(orm_tidesdb_transaction_t *transaction,orm_tidesdb_column_family_t *family,
    const uint8_t *key,size_t key_size,uint8_t **value,size_t *value_size) {
  ++get_calls;
  if(business_namespace(key,key_size)) {
    ++business_gets;
    if(reject_business_reads) return ORM_TDB_ERR_IO;
  }
  if(get_failure) { get_failure=false; return ORM_TDB_ERR_IO; }
  return orm_tidesdb_txn_get(transaction,family,key,key_size,value,value_size);
}
static int probe_put(orm_tidesdb_transaction_t *transaction,orm_tidesdb_column_family_t *family,
    const uint8_t *key,size_t key_size,const uint8_t *value,size_t value_size,time_t ttl) {
  ++put_calls; return orm_tidesdb_txn_put(transaction,family,key,key_size,value,value_size,ttl);
}
#define orm_tidesdb_txn_commit probe_commit
#define orm_tidesdb_txn_rollback probe_rollback
#define orm_tidesdb_txn_get probe_get
#define orm_tidesdb_txn_put probe_put
#define orm_tidesdb_iter_seek probe_seek
#include "../../src/catalog_store.c"
#include "../../src/relation.c"
#include "../../src/index_directory.c"
#undef orm_tidesdb_iter_seek
#undef orm_tidesdb_txn_put
#undef orm_tidesdb_txn_get
#undef orm_tidesdb_txn_rollback
#undef orm_tidesdb_txn_commit
#define calloc probe_calloc
#include "../../src/connection.c"
#undef calloc

enum { ORIGINAL_SCORE=10, CHANGED_SCORE=99 };
static tdsql_database *database;
static tdsql_connection *first,*second;
static tdsql_statement *statement;
static tdsql_result *result;
static tdsql_response response;
static turbodb_error_t error;
static char *directory;
static const char select_sql[]="SELECT score FROM items WHERE id=?";
static const char update_sql[]="UPDATE items SET score=? WHERE id=?";
static turbodb_status_t run(const char *sql) {
  const tdsql_request request=tdsql_request_default(turbodb_view(sql));
  return tdsql_connection_run(first,&request,&response,&error);
}
static turbodb_status_t prepare(const char *sql) {
  const tdsql_request request=tdsql_request_default(turbodb_view(sql));
  return tdsql_connection_statement_prepare(first,&request,&statement,&error);
}
static void score_is(int64_t expected) {
  const tdsql_request request=tdsql_request_default(turbodb_view("SELECT score FROM items WHERE id=1"));
  check_equal(tdsql_connection_query(second,&request,&result,&error),TURBODB_STATUS_OK);
  tdsql_row row={0}; check_equal(tdsql_result_next(result,&row,&error),TURBODB_STATUS_OK);
  check_equal(row.state,TDSQL_ROW); check_equal(row.values[0].data.int64_value,expected);
  check_equal(tdsql_result_destroy_checked(result,&error),TURBODB_STATUS_OK); result=NULL;
}
static void close_statement(void) {
  check_equal(tdsql_statement_close(statement,&error),TURBODB_STATUS_OK); statement=NULL;
}

spec("TidesSQL prepared owner allocation and native failures") {
  before_each() {
    database=NULL; first=second=NULL; statement=NULL; result=NULL;
    response=tdsql_response_default(); turbodb_error_init(&error);
    allocations=fail_allocation=commits=rollbacks=get_calls=put_calls=0;
    rollback_failure=get_failure=commit_unknown=false;
    business_gets=business_seeks=0; reject_business_reads=false;
    directory=tt_make_temp_dir("tidessql-prepared-owner"); check_not_null(directory);
    const turbodb_option_t options[]={
      {turbodb_view("path"),turbodb_view(directory)},
      {turbodb_view("column_family"),turbodb_view("prepared_owner")},
      {turbodb_view("sql_initialize"),turbodb_view("true")}
    };
    tdsql_config config=tdsql_config_default(); config.options=options; config.option_count=sizeof(options)/sizeof(options[0]);
    check_equal(tdsql_database_open(&config,&database,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_database_connect(database,&first,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_database_connect(database,&second,&error),TURBODB_STATUS_OK);
    check_equal(run("CREATE TABLE items(id BIGINT PRIMARY KEY,score BIGINT)"),TURBODB_STATUS_OK);
    check_equal(run("INSERT INTO items VALUES(1,10)"),TURBODB_STATUS_OK);
    allocations=commits=rollbacks=get_calls=put_calls=0;
  }
  after_each() {
    fail_allocation=0; rollback_failure=get_failure=commit_unknown=false;
    reject_business_reads=false;
    check_equal(tdsql_result_destroy_checked(response.result,&error),TURBODB_STATUS_OK); response.result=NULL;
    check_equal(tdsql_result_destroy_checked(result,&error),TURBODB_STATUS_OK); result=NULL;
    close_statement();
    check_equal(tdsql_connection_close(first,&error),TURBODB_STATUS_OK); first=NULL;
    check_equal(tdsql_connection_close(second,&error),TURBODB_STATUS_OK); second=NULL;
    check_equal(tdsql_database_close(database,&error),TURBODB_STATUS_OK); database=NULL;
    check_equal(tt_remove_tree(directory),0); free(directory); directory=NULL;
  }
  it("refunds prepared heap admission and both statement byte quotas") {
    const size_t dependents=first->dependents;
    fail_allocation=allocations+1;
    check_equal(prepare(select_sql),TURBODB_STATUS_OUT_OF_MEMORY);
    check_null(statement); check_equal(first->prepared_count,0u); check_equal(first->prepared_bytes,0u);
    check_equal(first->dependents,dependents); fail_allocation=0;
    check_equal(prepare(select_sql),TURBODB_STATUS_OK);
    const size_t charged=statement->charged; check_greater(charged,sizeof(tdsql_statement));
    check_equal(first->prepared_bytes,charged); first->config.max_prepared_bytes=charged*2-1;
    tdsql_statement *additional=NULL; const tdsql_request request=tdsql_request_default(turbodb_view(select_sql));
    check_equal(tdsql_connection_statement_prepare(first,&request,&additional,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(additional);
    check_equal(first->prepared_bytes,charged); check_equal(first->prepared_count,1u);
    close_statement(); check_equal(first->prepared_bytes,0u); check_equal(first->dependents,dependents);
    check_equal(prepare(select_sql),TURBODB_STATUS_OK); close_statement();
    first->config.max_prepared_bytes=sizeof(tdsql_statement)-1;
    const size_t allocated=allocations;
    check_equal(prepare(select_sql),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(statement); check_equal(allocations,allocated);
  }
  it("prepares metadata with one rollback no writes commits or connection transaction") {
    check_equal(prepare(update_sql),TURBODB_STATUS_OK);
    check_equal(put_calls,0u); check_equal(commits,0u); check_equal(rollbacks,1u);
    check_null(first->sql_transaction); check_false(first->active);
    const size_t read=get_calls,rolled=rollbacks;
    check_equal(tdsql_statement_reset(statement,&error),TURBODB_STATUS_OK); close_statement();
    check_equal(get_calls,read); check_equal(rollbacks,rolled); check_equal(put_calls,0u); score_is(ORIGINAL_SCORE);
  }
  it("uses existing SQL ownership without native lifecycle or leaked WORK") {
    check_equal(run("BEGIN"),TURBODB_STATUS_OK);
    tdsql_transaction *transaction=first->sql_transaction;
    const size_t retained=(size_t)transaction->budget.retained_work_bytes;
    allocations=commits=rollbacks=put_calls=0;
    check_equal(prepare(select_sql),TURBODB_STATUS_OK);
    check_true(first->sql_transaction==transaction); check_false(transaction->budget.statement_active);
    check_equal(transaction->budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],(uint64_t)retained);
    check_equal(commits,0u); check_equal(rollbacks,0u); check_equal(put_calls,0u); check_equal(transaction->owner.active_sources,0u);
    close_statement(); check_equal(run("ROLLBACK"),TURBODB_STATUS_OK);
  }
  it("quarantines metadata rollback failure without publishing a prepared owner") {
    rollback_failure=true;
    check_equal(prepare(select_sql),TURBODB_STATUS_DATASTORE_ERROR); check_null(statement);
    check_equal(first->failure,TURBODB_STATUS_DATASTORE_ERROR); check_equal(first->prepared_bytes,0u); check_equal(first->prepared_count,0u);
    check_equal(commits,0u); check_equal(rollbacks,1u); check_equal(put_calls,0u); score_is(ORIGINAL_SCORE);
  }
  it("rejects catalog read failure without publishing ownership or mutating business data") {
    get_failure=true;
    check_equal(prepare(select_sql),TURBODB_STATUS_DATASTORE_ERROR); check_null(statement);
    check_equal(first->prepared_bytes,0u); check_equal(first->prepared_count,0u); check_equal(first->dependents,0u);
    check_equal(put_calls,0u); check_equal(commits,0u); check_equal(rollbacks,1u);
    check_equal(prepare(select_sql),TURBODB_STATUS_OK); score_is(ORIGINAL_SCORE);
  }
  it("quarantines begin abort cleanup even when its primary read error has the same status") {
    get_failure=true; rollback_failure=true;
    check_equal(prepare(select_sql),TURBODB_STATUS_DATASTORE_ERROR); check_null(statement);
    check_equal(first->failure,TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(first->prepared_count,0u); check_equal(first->prepared_bytes,0u); check_equal(first->dependents,0u);
    const size_t read=get_calls,rolled=rollbacks;
    check_equal(prepare(select_sql),TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(get_calls,read); check_equal(rollbacks,rolled); check_equal(commits,0u); check_equal(put_calls,0u);
    score_is(ORIGINAL_SCORE);
  }
  it("refunds failed transaction and result allocation before reexecuting the descriptor") {
    check_equal(prepare(select_sql),TURBODB_STATUS_OK);
    const turbodb_value_t value=turbodb_i64(1); const tdsql_bindings bindings=tdsql_bindings_default(&value,1);
    const size_t charged=first->prepared_bytes;
    for(size_t point=1;point<=2;++point) {
      fail_allocation=allocations+point;
      check_equal(tdsql_statement_execute(statement,&bindings,&response,&error),TURBODB_STATUS_OUT_OF_MEMORY); check_null(response.result);
      fail_allocation=0; check_false(first->active); check_equal(first->failure,TURBODB_STATUS_OK);
      check_equal(first->prepared_bytes,charged); check_equal(first->prepared_count,1u); check_equal(first->dependents,1u);
      check_equal(tdsql_statement_execute(statement,&bindings,&response,&error),TURBODB_STATUS_OK);
      check_equal(tdsql_result_destroy_checked(response.result,&error),TURBODB_STATUS_OK); response.result=NULL;
    }
  }
  it("never replays a prepared command after unknown commit and permits final close") {
    check_equal(prepare(update_sql),TURBODB_STATUS_OK);
    const turbodb_value_t values[]={turbodb_i64(CHANGED_SCORE),turbodb_i64(1)};
    const tdsql_bindings bindings=tdsql_bindings_default(values,sizeof(values)/sizeof(values[0]));
    response.affected_rows=UINT64_MAX; commit_unknown=true;
    check_equal(tdsql_statement_execute(statement,&bindings,&response,&error),TURBODB_STATUS_COMMIT_UNKNOWN);
    check_equal(response.affected_rows,UINT64_MAX); check_null(response.result);
    const size_t committed=commits,rolled=rollbacks,allocated=allocations;
    check_equal(tdsql_statement_execute(statement,&bindings,&response,&error),TURBODB_STATUS_COMMIT_UNKNOWN);
    check_equal(tdsql_statement_reset(statement,&error),TURBODB_STATUS_COMMIT_UNKNOWN);
    check_equal(commits,committed); check_equal(rollbacks,rolled); check_equal(allocations,allocated);
    close_statement(); score_is(CHANGED_SCORE);
  }
  it("consumes a failed result cleanup and detaches it before closing the statement") {
    check_equal(prepare(select_sql),TURBODB_STATUS_OK);
    const turbodb_value_t value=turbodb_i64(1); const tdsql_bindings bindings=tdsql_bindings_default(&value,1);
    check_equal(tdsql_statement_execute(statement,&bindings,&response,&error),TURBODB_STATUS_OK);
    rollback_failure=true;
    check_equal(tdsql_result_destroy_checked(response.result,&error),TURBODB_STATUS_DATASTORE_ERROR); response.result=NULL;
    check_null(statement->result); check_equal(first->failure,TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(tdsql_statement_reset(statement,&error),TURBODB_STATUS_DATASTORE_ERROR);
    close_statement(); check_equal(first->prepared_count,0u); check_equal(first->prepared_bytes,0u); score_is(ORIGINAL_SCORE);
  }
  it("prepares all DDL using metadata without physical row or index reads writes or IDs") {
    check_equal(run("CREATE UNIQUE INDEX score_key ON items(score)"),TURBODB_STATUS_OK);
    const char *const commands[]={
      "CREATE TABLE created(id BIGINT PRIMARY KEY,v BIGINT DEFAULT(7 DIV 0),KEY v_key(v))",
      "ALTER TABLE items ADD extra BIGINT DEFAULT(7 DIV 0)",
      "ALTER TABLE items ALTER score SET DEFAULT(7 DIV 0)",
      "ALTER TABLE items RENAME COLUMN score TO value",
      "ALTER TABLE items RENAME TO renamed",
      "DROP TABLE items", "TRUNCATE TABLE items",
      "CREATE INDEX another_key ON items(score)", "DROP INDEX score_key ON items"
    };
    commits=rollbacks=put_calls=business_gets=business_seeks=0; reject_business_reads=true;
    for(size_t i=0;i<sizeof(commands)/sizeof(commands[0]);++i) {
      info("DDL native boundary: %s",commands[i]);
      check_equal(prepare(commands[i]),TURBODB_STATUS_OK);
      check_equal(tdsql_statement_parameters(statement),0u); check_equal(tdsql_statement_columns(statement),0u);
      check_equal(business_gets,0u); check_equal(business_seeks,0u);
      check_equal(put_calls,0u); check_equal(commits,0u); check_null(first->sql_transaction);
      close_statement(); check_equal(first->prepared_bytes,0u); check_equal(first->dependents,0u);
    }
    check_equal(rollbacks,sizeof(commands)/sizeof(commands[0])); reject_business_reads=false; score_is(ORIGINAL_SCORE);
    check_equal(run("INSERT INTO items VALUES(2,10)"),TURBODB_STATUS_CONSTRAINT);
  }
  it("refunds DDL temporary ownership in an existing SQL transaction") {
    check_equal(run("BEGIN"),TURBODB_STATUS_OK); tdsql_transaction *transaction=first->sql_transaction;
    const uint64_t retained=transaction->budget.retained_work_bytes;
    commits=rollbacks=put_calls=business_gets=business_seeks=0; reject_business_reads=true;
    check_equal(prepare("ALTER TABLE items ADD extra DOUBLE DEFAULT(7/0.0)"),TURBODB_STATUS_OK);
    check_true(first->sql_transaction==transaction); check_false(transaction->budget.statement_active);
    check_equal(transaction->budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained);
    check_equal(transaction->owner.active_sources,0u); check_equal(business_gets,0u); check_equal(business_seeks,0u);
    check_equal(put_calls,0u); check_equal(commits,0u); check_equal(rollbacks,0u);
    close_statement(); reject_business_reads=false; check_equal(run("ROLLBACK"),TURBODB_STATUS_OK); score_is(ORIGINAL_SCORE);
  }
  it("quarantines DDL metadata cleanup failure without publishing the descriptor or table") {
    rollback_failure=true;
    check_equal(prepare("CREATE TABLE created(id BIGINT PRIMARY KEY)"),TURBODB_STATUS_DATASTORE_ERROR);
    check_null(statement); check_equal(first->failure,TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(first->prepared_count,0u); check_equal(first->prepared_bytes,0u); check_equal(first->dependents,0u);
    check_equal(put_calls,0u); check_equal(commits,0u); check_equal(rollbacks,1u); score_is(ORIGINAL_SCORE);
  }
}

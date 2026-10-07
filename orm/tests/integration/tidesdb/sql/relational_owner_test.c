#define CSTL_NO_LEGACY_STACK_T
#define TINYTEST_NO_MAIN
#include "relational_backend.h"
#include "catalog_store.h"
#include <tinytest.h>
#include <stdlib.h>
#include <cmeta_process.h>
#include <cmeta_error.h>
#include <signal.h>

enum { SQL_CLOSE_CHILD_TIMEOUT_MS=10000, SQL_CLOSE_CHILD_OUTPUT_BYTES=64*1024, SQL_CLOSE_CHILD_FAILED=70 };
static const char *program_path;
static cmeta_process_t *child_process;

typedef enum commit_probe { COMMIT_NORMAL, COMMIT_CONFLICT, COMMIT_AFTER_SUCCESS } commit_probe;
static commit_probe commit_mode;
static bool rollback_failure, rollback_failure_before, get_failure;
static size_t commits, rollbacks, allocations, fail_allocation;
typedef enum point_probe { POINT_CREATE, POINT_ROLLBACK, POINT_RELEASE, POINT_NONE } point_probe;
static size_t point_calls[POINT_NONE], fail_point_call;
static point_probe fail_point;
static bool fail_point_after, reserve_failure;
static size_t put_calls, fail_put_call;
static int probe_point(orm_tidesdb_transaction_t *transaction, const char *name, point_probe operation) {
  const bool fail = ++point_calls[operation] == fail_point_call && fail_point == operation;
  if (fail && !fail_point_after) return ORM_TDB_ERR_IO;
  int code = operation == POINT_CREATE ? orm_tidesdb_txn_savepoint(transaction,name) :
      operation == POINT_ROLLBACK ? orm_tidesdb_txn_rollback_to_savepoint(transaction,name) :
      orm_tidesdb_txn_release_savepoint(transaction,name);
  return fail && code == ORM_TDB_SUCCESS ? ORM_TDB_ERR_IO : code;
}
static int probe_savepoint(orm_tidesdb_transaction_t *t,const char *n) { return probe_point(t,n,POINT_CREATE); }
static int probe_rollback_to(orm_tidesdb_transaction_t *t,const char *n) { return probe_point(t,n,POINT_ROLLBACK); }
static int probe_release_point(orm_tidesdb_transaction_t *t,const char *n) { return probe_point(t,n,POINT_RELEASE); }
static stl_status probe_reserve(vec_t *vector,size_t capacity) {
  if (reserve_failure) { reserve_failure=false; return STL_OUT_OF_MEMORY; }
  return vec_reserve(vector,capacity);
}
static int probe_put(orm_tidesdb_transaction_t *t,orm_tidesdb_column_family_t *f,
    const uint8_t *key,size_t key_size,const uint8_t *value,size_t value_size,time_t ttl) {
  if (++put_calls == fail_put_call) return ORM_TDB_ERR_IO;
  return orm_tidesdb_txn_put(t,f,key,key_size,value,value_size,ttl);
}
static int probe_commit(orm_tidesdb_transaction_t *transaction) {
  ++commits;
  if (commit_mode == COMMIT_CONFLICT) { commit_mode = COMMIT_NORMAL; return ORM_TDB_ERR_CONFLICT; }
  const int code = orm_tidesdb_txn_commit(transaction);
  if (commit_mode == COMMIT_AFTER_SUCCESS && code == ORM_TDB_SUCCESS) {
    commit_mode = COMMIT_NORMAL; return ORM_TDB_ERR_IO;
  }
  return code;
}
static int probe_rollback(orm_tidesdb_transaction_t *transaction) {
  ++rollbacks;
  if (rollback_failure_before) { rollback_failure_before=false; return ORM_TDB_ERR_IO; }
  const int code = orm_tidesdb_txn_rollback(transaction);
  if (rollback_failure && code == ORM_TDB_SUCCESS) { rollback_failure = false; return ORM_TDB_ERR_IO; }
  return code;
}
static int probe_get(orm_tidesdb_transaction_t *transaction, orm_tidesdb_column_family_t *family,
    const uint8_t *key, size_t key_size, uint8_t **value, size_t *value_size) {
  if (get_failure) { get_failure = false; return ORM_TDB_ERR_IO; }
  return orm_tidesdb_txn_get(transaction,family,key,key_size,value,value_size);
}
#define orm_tidesdb_txn_commit probe_commit
#define orm_tidesdb_txn_rollback probe_rollback
#define orm_tidesdb_txn_get probe_get
#define orm_tidesdb_txn_savepoint probe_savepoint
#define orm_tidesdb_txn_rollback_to_savepoint probe_rollback_to
#define orm_tidesdb_txn_release_savepoint probe_release_point
#define orm_tidesdb_txn_put probe_put
#define vec_reserve probe_reserve
#include "../../../../../tidessql/src/catalog_store.c"
#undef vec_reserve
#undef orm_tidesdb_txn_put
#undef orm_tidesdb_txn_release_savepoint
#undef orm_tidesdb_txn_rollback_to_savepoint
#undef orm_tidesdb_txn_savepoint
#undef orm_tidesdb_txn_get
#undef orm_tidesdb_txn_commit
#undef orm_tidesdb_txn_rollback
static size_t render_appends, fail_render_append;
static bool render_allocation_failure;
static void *render_probe_calloc(size_t count, size_t size) {
  if (render_allocation_failure) { render_allocation_failure=false; return NULL; }
  return calloc(count,size);
}
static tstr render_probe_append(tstr text, const void *data, size_t size) {
  return ++render_appends == fail_render_append ? NULL : tstr_cat_len(text,data,size);
}
#define calloc render_probe_calloc
#define tstr_cat_len render_probe_append
#include "../../../../src/sql/orm_mysql_render.c"
#undef tstr_cat_len
#undef calloc
static void *probe_calloc(size_t count, size_t size) {
  return ++allocations == fail_allocation ? NULL : calloc(count,size);
}
#define calloc probe_calloc
#include "../../../../../tidessql/src/connection.c"
#include "../../../../../drivers/tidesdb/relational_backend.c"
#undef calloc

static orm_connection_t *connection;
static orm_transaction_t *transaction;
static orm_query_t *query;
static orm_result_t *result;
static orm_error_t error;
static char *directory;
static tdsql_database *shared_database;
static tdsql_connection *shared_first, *shared_second;
static tdsql_transaction *shared_transaction;
static tdsql_result *shared_result;

static void connect_database(bool initialize) {
  orm_config_t config; orm_config(&config);
  orm_option_t options[] = {{orm_view("path"),orm_view(directory)},
    {orm_view("column_family"),orm_view("rel")},
    {orm_view("sql_initialize"),orm_view(initialize ? "true" : "false")}};
  config.driver = orm_view("tidesdb"); config.options = options; config.option_count = sizeof(options)/sizeof(options[0]);
  check_equal(orm_connect_with_factory_v1(&config,orm_tidesdb_relational_create,&connection,&error),ORM_STATUS_OK);
}
static orm_status_t execute_sql(const char *sql) {
  orm_query_destroy(query); query = NULL; orm_result_destroy(result); result = NULL;
  check_equal(orm_raw(connection,orm_view(sql),&query,&error),ORM_STATUS_OK);
  return transaction ? orm_query_execute_in_transaction(query,transaction,&result,&error)
                     : orm_query_execute(query,&result,&error);
}
static void disconnect_database(void) {
  orm_result_destroy(result); result = NULL; orm_query_destroy(query); query = NULL;
  orm_transaction_destroy(transaction); transaction = NULL; orm_disconnect(connection); connection = NULL;
}
static void open_shared_database(void) {
  disconnect_database();
  const turbodb_option_t options[] = {
    {turbodb_view("path"),turbodb_view(directory)},
    {turbodb_view("column_family"),turbodb_view("rel")},
    {turbodb_view("sql_max_connections"),turbodb_view("2")}
  };
  tdsql_config config=tdsql_config_default();
  config.options=options; config.option_count=sizeof(options)/sizeof(options[0]);
  check_equal(tdsql_database_open(&config,&shared_database,&error),ORM_STATUS_OK);
  check_equal(tdsql_database_connect(shared_database,&shared_first,&error),ORM_STATUS_OK);
}
static void shared_score_is(int64_t expected) {
  const tdsql_request input=tdsql_request_default(turbodb_view("SELECT score FROM items WHERE id=1"));
  check_equal(tdsql_connection_query(shared_second,&input,&shared_result,&error),ORM_STATUS_OK);
  tdsql_row row={0};
  check_equal(tdsql_result_next(shared_result,&row,&error),ORM_STATUS_OK);
  check_equal(row.state,TDSQL_ROW); check_equal(row.count,1u);
  check_equal(row.values[0].data.int64_value,expected);
  check_equal(tdsql_result_destroy_checked(shared_result,&error),ORM_STATUS_OK); shared_result=NULL;
}
static void begin_transaction(void) {
  check_equal(orm_transaction_begin(connection,ORM_ISOLATION_SERIALIZABLE,&transaction,&error),ORM_STATUS_OK);
}
static void score_is(int64_t expected) {
  check_equal(execute_sql("SELECT score FROM items WHERE id=1"),ORM_STATUS_OK);
  int64_t score = 0; check_equal(orm_result_get_int64(result,0,0,&score,&error),ORM_STATUS_OK); check_equal(score,expected);
}
static void fail_next_point(point_probe operation, bool after) {
  fail_point = operation; fail_point_call = point_calls[operation]+1; fail_point_after = after;
}
static void require_full_rollback(void) {
  tdsql_transaction *owner=transaction->backend.context;
  check_true(owner->owner.failed); check_false(owner->budget.statement_active);
  check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_INVALID_STATE);
  check_equal(orm_transaction_savepoint(transaction,orm_view("blocked"),&error),ORM_STATUS_INVALID_STATE);
  check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("target"),&error),ORM_STATUS_INVALID_STATE);
  check_equal(execute_sql("SAVEPOINT blocked"),ORM_STATUS_INVALID_STATE);
  check_equal(execute_sql("ROLLBACK TO target"),ORM_STATUS_INVALID_STATE);
  check_equal(execute_sql("RELEASE SAVEPOINT target"),ORM_STATUS_INVALID_STATE);
  check_equal(execute_sql("SELECT score FROM items"),ORM_STATUS_INVALID_STATE);
  fail_point=POINT_NONE;
  check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
  check_equal(owner->owner.savepoint_bytes,0u); check_false(owner->owner.savepoints.initialized);
  check_equal(owner->budget.retained_work_bytes,owner->reserved);
  orm_transaction_destroy(transaction); transaction=NULL; score_is(10);
}
spec("TidesDB relational ORM owner failures") {
  before_each() {
    connection = NULL; transaction = NULL; query = NULL; result = NULL;
    shared_database=NULL; shared_first=shared_second=NULL;
    shared_transaction=NULL; shared_result=NULL;
    child_process=NULL;
    commit_mode = COMMIT_NORMAL; rollback_failure = rollback_failure_before = get_failure = false; commits = rollbacks = allocations = fail_allocation = 0;
    fail_point=POINT_NONE; fail_point_call=0; fail_point_after=reserve_failure=false;
    memset(point_calls,0,sizeof(point_calls)); put_calls=fail_put_call=0;
    render_appends=fail_render_append=0; render_allocation_failure=false;
    orm_error_init(&error); directory = tt_make_temp_dir("orm-relational-owner"); check_not_null(directory);
    connect_database(true);
    check_equal(execute_sql("CREATE TABLE items(id BIGINT PRIMARY KEY,score BIGINT)"),ORM_STATUS_OK);
    check_equal(execute_sql("INSERT INTO items(id,score) VALUES(1,10)"),ORM_STATUS_OK);
    commits = rollbacks = allocations = 0;
  }
  after_each() {
    cmeta_process_destroy(child_process); child_process=NULL;
    commit_mode = COMMIT_NORMAL; rollback_failure = rollback_failure_before = get_failure = false; fail_allocation = 0;
    fail_point=POINT_NONE; reserve_failure=false; fail_put_call=0;
    fail_render_append=0; render_allocation_failure=false;
    check_equal(tdsql_result_destroy_checked(shared_result,&error),ORM_STATUS_OK); shared_result=NULL;
    check_equal(tdsql_transaction_release_checked(shared_transaction,&error),ORM_STATUS_OK); shared_transaction=NULL;
    check_equal(tdsql_connection_close(shared_first,&error),ORM_STATUS_OK); shared_first=NULL;
    check_equal(tdsql_connection_close(shared_second,&error),ORM_STATUS_OK); shared_second=NULL;
    check_equal(tdsql_database_close(shared_database,&error),ORM_STATUS_OK); shared_database=NULL;
    disconnect_database(); check_equal(tt_remove_tree(directory),0); free(directory);
  }
  group("shared SQL session failures") {
    it("preserves the unified response and exposes terminal commit state without retry or allocation") {
      open_shared_database();
      check_equal(tdsql_database_connect(shared_database,&shared_second,&error),ORM_STATUS_OK);
      const tdsql_request input=tdsql_request_default(turbodb_view("UPDATE items SET score=99 WHERE id=1"));
      tdsql_response response=tdsql_response_default(); response.affected_rows=UINT64_MAX;
      commit_mode=COMMIT_AFTER_SUCCESS;
      check_equal(tdsql_connection_run(shared_first,&input,&response,&error),ORM_STATUS_COMMIT_UNKNOWN);
      check_equal(response.affected_rows,UINT64_MAX); check_null(response.result);
      const size_t committed=commits,rolled=rollbacks,allocated=allocations;
      fail_allocation=allocations+1;
      tdsql_session_state state=tdsql_session_state_default();
      check_equal(tdsql_connection_state(shared_first,&state,&error),ORM_STATUS_OK);
      check_equal(state.failure,ORM_STATUS_COMMIT_UNKNOWN); check_false(state.in_transaction); check_false(state.busy);
      check_equal(tdsql_connection_run(shared_first,&input,&response,&error),ORM_STATUS_COMMIT_UNKNOWN);
      check_equal(response.affected_rows,UINT64_MAX);
      check_equal(commits,committed); check_equal(rollbacks,rolled); check_equal(allocations,allocated);
      fail_allocation=0; commit_mode=COMMIT_NORMAL; shared_score_is(99);
    }
    it("observes rollback-required external ownership without advancing cleanup") {
      open_shared_database();
      check_equal(tdsql_connection_begin(shared_first,&shared_transaction,&error),ORM_STATUS_OK);
      rollback_failure_before=true;
      check_equal(tdsql_transaction_finish(shared_transaction,false,&error),ORM_STATUS_DATASTORE_ERROR);
      const size_t rolled=rollbacks,allocated=allocations;
      tdsql_session_state state=tdsql_session_state_default();
      fail_allocation=allocations+1;
      check_equal(tdsql_connection_state(shared_first,&state,&error),ORM_STATUS_OK);
      check_true(state.in_transaction); check_true(state.rollback_required); check_true(state.busy);
      check_equal(rollbacks,rolled); check_equal(allocations,allocated);
      fail_allocation=0; rollback_failure_before=false;
      check_equal(tdsql_transaction_release_checked(shared_transaction,&error),ORM_STATUS_OK); shared_transaction=NULL;
      check_equal(tdsql_connection_state(shared_first,&state,&error),ORM_STATUS_OK);
      check_false(state.in_transaction); check_false(state.rollback_required); check_false(state.busy);
    }
    it("does not consume admission capacity when session allocation fails") {
      open_shared_database();
      fail_allocation=allocations+1;
      check_equal(tdsql_database_connect(shared_database,&shared_second,&error),ORM_STATUS_OUT_OF_MEMORY);
      check_null(shared_second); check_equal(shared_database->connections,1u);
      fail_allocation=0;
      check_equal(tdsql_database_connect(shared_database,&shared_second,&error),ORM_STATUS_OK);
      check_equal(shared_database->connections,2u); shared_score_is(10);
    }
    it("quarantines only the SQL session whose checked rollback cleanup fails") {
      open_shared_database();
      check_equal(tdsql_database_connect(shared_database,&shared_second,&error),ORM_STATUS_OK);
      check_equal(tdsql_connection_begin(shared_first,&shared_transaction,&error),ORM_STATUS_OK);
      rollback_failure=true;
      check_equal(tdsql_transaction_release_checked(shared_transaction,&error),ORM_STATUS_DATASTORE_ERROR);
      shared_transaction=NULL;
      check_equal(tdsql_connection_prepare(shared_first,false,&error),ORM_STATUS_DATASTORE_ERROR);
      check_equal(tdsql_connection_prepare(shared_second,false,&error),ORM_STATUS_OK);
      shared_score_is(10);
    }
    it("preserves a committed write after unknown commit without poisoning a sibling") {
      open_shared_database();
      check_equal(tdsql_database_connect(shared_database,&shared_second,&error),ORM_STATUS_OK);
      const tdsql_request input=tdsql_request_default(turbodb_view("UPDATE items SET score=99 WHERE id=1"));
      uint64_t affected=UINT64_MAX;
      commit_mode=COMMIT_AFTER_SUCCESS;
      check_equal(tdsql_connection_execute(shared_first,&input,&affected,&error),ORM_STATUS_COMMIT_UNKNOWN);
      check_equal(affected,UINT64_MAX);
      const size_t committed=commits;
      check_equal(tdsql_connection_execute(shared_first,&input,&affected,&error),ORM_STATUS_COMMIT_UNKNOWN);
      check_equal(commits,committed);
      commit_mode=COMMIT_NORMAL; shared_score_is(99);
    }
  }
  it("quarantines a direct engine transaction after unknown commit without ORM mediation") {
    tdsql_connection *engine=connection->backend.context;
    tdsql_transaction *owner=NULL;
    tdsql_result *rows=NULL;
    tdsql_request input={.struct_size=sizeof(tdsql_request),.abi_version=TDSQL_ABI_VERSION,
        .sql=orm_view("UPDATE items SET score=99 WHERE id=1"),
        .limits=tdsql_limits_default()};
    uint64_t affected=UINT64_MAX;
    check_equal(tdsql_connection_begin(engine,&owner,&error),ORM_STATUS_OK);
    check_equal(tdsql_transaction_execute(owner,&input,&affected,&error),ORM_STATUS_OK);
    check_equal(affected,1u);
    commit_mode=COMMIT_AFTER_SUCCESS;
    check_equal(tdsql_transaction_finish(owner,true,&error),ORM_STATUS_COMMIT_UNKNOWN);
    check_equal(engine->failure,ORM_STATUS_COMMIT_UNKNOWN);
    check_equal(connection->failure,ORM_STATUS_OK);
    const size_t committed=commits,rolled=rollbacks,allocated=allocations;
    affected=UINT64_MAX;
    check_equal(tdsql_connection_execute(engine,&input,&affected,&error),ORM_STATUS_COMMIT_UNKNOWN);
    check_equal(affected,UINT64_MAX);
    input.sql=orm_view("SELECT score FROM items");
    check_equal(tdsql_connection_query(engine,&input,&rows,&error),ORM_STATUS_COMMIT_UNKNOWN);
    check_null(rows);
    tdsql_transaction *blocked=NULL;
    check_equal(tdsql_connection_begin(engine,&blocked,&error),ORM_STATUS_COMMIT_UNKNOWN);
    check_null(blocked);
    check_equal(tdsql_transaction_finish(owner,false,&error),ORM_STATUS_INVALID_STATE);
    check_equal(tdsql_connection_close(engine,&error),ORM_STATUS_BUSY);
    tdsql_transaction_release(owner);
    check_equal(commits,committed); check_equal(rollbacks,rolled); check_equal(allocations,allocated);
    disconnect_database(); connect_database(false); score_is(99);
  }
  it("reports checked handle cleanup failures and quarantines the engine after consuming each handle") {
    enum { CHECKED_TRANSACTION, CHECKED_RESULT, CHECKED_HANDLE_COUNT };
    for (size_t kind=0;kind<CHECKED_HANDLE_COUNT;++kind) {
      tdsql_connection *engine=connection->backend.context;
      if (kind==CHECKED_TRANSACTION) {
        tdsql_transaction *owner=NULL;
        check_equal(tdsql_connection_begin(engine,&owner,&error),ORM_STATUS_OK);
        rollback_failure=true;
        check_equal(tdsql_transaction_release_checked(owner,&error),ORM_STATUS_DATASTORE_ERROR);
      } else {
        const tdsql_request input=tdsql_request_default(orm_view("SELECT score FROM items"));
        tdsql_result *rows=NULL;
        check_equal(tdsql_connection_query(engine,&input,&rows,&error),ORM_STATUS_OK);
        rollback_failure=true;
        check_equal(tdsql_result_destroy_checked(rows,&error),ORM_STATUS_DATASTORE_ERROR);
      }
      check_equal(error.status,ORM_STATUS_DATASTORE_ERROR);
      check_not_null(strstr(error.message,"rollback"));
      check_equal(engine->dependents,0u); check_false(engine->active);
      tdsql_session_state snapshot=tdsql_session_state_default();
      check_equal(tdsql_connection_state(engine,&snapshot,&error),ORM_STATUS_OK);
      check_equal(snapshot.failure,ORM_STATUS_DATASTORE_ERROR);
      check_false(snapshot.in_transaction); check_false(snapshot.busy);
      check_equal(tdsql_connection_prepare(engine,false,&error),ORM_STATUS_DATASTORE_ERROR);
      disconnect_database(); connect_database(false); score_is(10);
    }
  }
  group("SET transaction variable ownership") {
    it("keeps variable assignments pure and preserves only the pending characteristic they name") {
      tdsql_connection *backend=connection->backend.context;
      check_equal(execute_sql("SET @@transaction_read_only=ON"),ORM_STATUS_OK);
      check_equal(backend->next_access,SQLPARSER_READ_ONLY);check_equal(backend->session_access,SQLPARSER_READ_WRITE);
      check_equal(execute_sql("SET transaction_isolation=@@transaction_isolation"),ORM_STATUS_OK);
      check_equal(backend->next_access,SQLPARSER_READ_ONLY);
      check_equal(execute_sql("SET @@transaction_isolation=3"),ORM_STATUS_OK);
      check_equal(execute_sql("SET transaction_read_only=ON"),ORM_STATUS_OK);
      check_equal(backend->session_access,SQLPARSER_READ_ONLY);check_equal(backend->next_access,SQLPARSER_ACCESS_DEFAULT);
      check_equal(execute_sql("SET transaction_read_only=DEFAULT"),ORM_STATUS_OK);
      check_equal(backend->session_access,SQLPARSER_READ_WRITE);
      check_null(backend->sql_transaction);check_false(backend->active);check_true(backend->autocommit);
      check_equal(allocations,0u);check_equal(commits,0u);check_equal(rollbacks,0u);
    }
    it("refunds workspace and retains settings and active mode after failed preparations") {
      tdsql_connection *backend=connection->backend.context;
      check_equal(execute_sql("BEGIN"),ORM_STATUS_OK);
      tdsql_transaction *owner=backend->sql_transaction;
      const uint64_t work_limit=owner->budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES];
      const uint64_t step_limit=owner->budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      const orm_sql_transaction_budget_amount used=owner->budget.transaction_used;
      const size_t committed=commits,rolled=rollbacks;
      const char *const sql[]={"SET transaction_read_only=@@transaction_read_only=0",
        "SET transaction_isolation=COALESCE(NULL,@@transaction_isolation)"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        owner->budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=owner->budget.retained_work_bytes;
        check_equal(execute_sql(sql[i]),ORM_STATUS_LIMIT_EXCEEDED);
        check_equal(backend->session_access,SQLPARSER_READ_WRITE);check_true(backend->sql_transaction==owner);
        check_false(owner->read_only);check_false(owner->owner.failed);check_false(owner->budget.statement_active);
        check_equal(owner->budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],owner->budget.retained_work_bytes);
        owner->budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=work_limit;
      }
      owner->budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=1;
      check_equal(execute_sql("SET transaction_read_only=ON"),ORM_STATUS_LIMIT_EXCEEDED);
      uint64_t affected=UINT64_MAX;
      check_equal(rel_backend_execute(backend,&query->plan,&connection->limits,&affected,&error),ORM_STATUS_LIMIT_EXCEEDED);
      check_equal(affected,UINT64_MAX);check_equal(backend->session_access,SQLPARSER_READ_WRITE);
      owner->budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=step_limit;
      check_equal(owner->budget.transaction_used.read_rows,used.read_rows);
      check_equal(owner->budget.transaction_used.read_bytes,used.read_bytes);
      check_equal(owner->budget.transaction_used.write_bytes,used.write_bytes);
      check_equal(commits,committed);check_equal(rollbacks,rolled);
      check_equal(execute_sql("SET transaction_read_only=ON"),ORM_STATUS_OK);
      check_equal(backend->session_access,SQLPARSER_READ_ONLY);check_false(owner->read_only);
      check_equal(execute_sql("ROLLBACK"),ORM_STATUS_OK);check_equal(backend->session_access,SQLPARSER_READ_ONLY);
    }
    it("prepares ORM handle settings under its budget and publishes after statement cleanup") {
      tdsql_connection *backend=connection->backend.context;begin_transaction();
      tdsql_transaction *owner=transaction->backend.context;
      const uint64_t work_limit=owner->budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES];
      owner->budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=owner->budget.retained_work_bytes;
      check_equal(execute_sql("SET transaction_read_only=@@transaction_read_only=0"),ORM_STATUS_LIMIT_EXCEEDED);
      check_equal(backend->session_access,SQLPARSER_READ_WRITE);check_false(owner->owner.failed);
      check_false(owner->budget.statement_active);
      check_equal(owner->budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],owner->budget.retained_work_bytes);
      owner->budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=work_limit;
      check_equal(execute_sql("SET transaction_read_only=@@transaction_read_only=0"),ORM_STATUS_OK);
      check_equal(backend->session_access,SQLPARSER_READ_ONLY);check_false(owner->read_only);
      check_false(owner->budget.statement_active);
      check_equal(owner->budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],owner->budget.retained_work_bytes);
      check_equal(execute_sql("SET @@transaction_read_only=OFF"),ORM_STATUS_SQL_ERROR);
      check_equal(execute_sql("SET @@transaction_isolation=3"),ORM_STATUS_SQL_ERROR);
      check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);orm_transaction_destroy(transaction);transaction=NULL;
      check_equal(commits,0u);check_equal(rollbacks,1u);
    }
    it("refuses settings on failed owners before any defaults or pending writes can change") {
      tdsql_connection *backend=connection->backend.context;
      check_equal(execute_sql("SET autocommit=0"),ORM_STATUS_OK);
      check_equal(execute_sql("UPDATE items SET score=99 WHERE id=1"),ORM_STATUS_OK);
      commit_mode=COMMIT_CONFLICT;check_equal(execute_sql("COMMIT"),ORM_STATUS_BUSY);
      const size_t committed=commits,rolled=rollbacks;
      check_equal(execute_sql("SET transaction_read_only=ON"),ORM_STATUS_INVALID_STATE);
      check_equal(execute_sql("SET @@transaction_read_only=ON"),ORM_STATUS_INVALID_STATE);
      check_equal(execute_sql("SET transaction_isolation=3"),ORM_STATUS_INVALID_STATE);
      check_equal(backend->session_access,SQLPARSER_READ_WRITE);check_equal(backend->next_access,SQLPARSER_ACCESS_DEFAULT);
      check_equal(commits,committed);check_equal(rollbacks,rolled);
      check_equal(execute_sql("ROLLBACK"),ORM_STATUS_OK);check_equal(execute_sql("SET autocommit=1"),ORM_STATUS_OK);
      check_equal(execute_sql("SET transaction_read_only=ON"),ORM_STATUS_OK);
      check_equal(backend->session_access,SQLPARSER_READ_ONLY);
    }
  }
  group("Connection-owned SQL transaction failures") {
    it("keeps SET autocommit pure until a Catalog statement starts the shared owner") {
      tdsql_connection *backend=connection->backend.context; check_true(backend->autocommit);
      check_equal(execute_sql("SET TRANSACTION READ ONLY"),ORM_STATUS_OK);
      check_equal(execute_sql("SET autocommit=FALSE"),ORM_STATUS_OK);
      check_false(backend->autocommit); check_null(backend->sql_transaction); check_false(backend->active);
      check_equal(backend->next_access,SQLPARSER_READ_ONLY);
      check_equal(allocations,0u); check_equal(commits,0u); check_equal(rollbacks,0u);
      check_equal(execute_sql("SELECT score FROM items WHERE id=1"),ORM_STATUS_OK);
      tdsql_transaction *owner=backend->sql_transaction; check_not_null(owner); check_true(owner->read_only);
      const size_t allocated=allocations,committed=commits,rolled=rollbacks;
      const orm_sql_transaction_budget_amount used=owner->budget.transaction_used;
      check_equal(execute_sql("SET autocommit=0+0"),ORM_STATUS_OK);
      check_true(backend->sql_transaction==owner); check_equal(allocations,allocated);
      check_equal(commits,committed); check_equal(rollbacks,rolled);
      check_equal(owner->budget.transaction_used.read_rows,used.read_rows);
      check_equal(owner->budget.transaction_used.read_bytes,used.read_bytes);
      check_equal(owner->budget.transaction_used.write_bytes,used.write_bytes);
      check_equal(owner->budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],owner->budget.retained_work_bytes);
      check_false(owner->budget.statement_active); check_false(owner->owner.failed);
      check_equal(execute_sql("SET autocommit=ON"),ORM_STATUS_OK);
      check_true(backend->autocommit); check_null(backend->sql_transaction); check_false(backend->active);
      check_equal(commits,committed+1);
    }
    it("keeps autocommit false and requires rollback acknowledgement after transition commit conflicts") {
      tdsql_connection *backend=connection->backend.context;
      check_equal(execute_sql("SET autocommit=0"),ORM_STATUS_OK);
      check_equal(execute_sql("UPDATE items SET score=99 WHERE id=1"),ORM_STATUS_OK);
      commit_mode=COMMIT_CONFLICT;
      check_equal(execute_sql("SET autocommit=1"),ORM_STATUS_BUSY); check_null(result);
      check_false(backend->autocommit); check_not_null(backend->sql_transaction);
      check_equal(backend->sql_transaction->state,REL_ROLLBACK_REQUIRED); check_null(backend->sql_transaction->owner.transaction);
      const size_t committed=commits,rolled=rollbacks;
      check_equal(execute_sql("SET autocommit=1"),ORM_STATUS_INVALID_STATE);
      check_equal(execute_sql("SET autocommit=0"),ORM_STATUS_INVALID_STATE);
      check_equal(commits,committed); check_equal(rollbacks,rolled);
      check_equal(execute_sql("ROLLBACK"),ORM_STATUS_OK); check_false(backend->autocommit); check_null(backend->sql_transaction);
      check_equal(execute_sql("SET autocommit=1"),ORM_STATUS_OK);
      check_true(backend->autocommit); check_equal(commits,committed); score_is(10);
    }
    it("quarantines an unknown autocommit transition without publishing its new mode or retrying") {
      tdsql_connection *backend=connection->backend.context;
      check_equal(execute_sql("SET autocommit=0"),ORM_STATUS_OK);
      check_equal(execute_sql("UPDATE items SET score=99 WHERE id=1"),ORM_STATUS_OK);
      commit_mode=COMMIT_AFTER_SUCCESS;
      check_equal(execute_sql("SET autocommit=DEFAULT"),ORM_STATUS_COMMIT_UNKNOWN); check_null(result);
      check_false(backend->autocommit); check_null(backend->sql_transaction); check_false(backend->active);
      check_equal(backend->failure,ORM_STATUS_COMMIT_UNKNOWN); check_equal(connection->failure,ORM_STATUS_COMMIT_UNKNOWN);
      check_equal(commits,1u); const size_t rolled=rollbacks;
      orm_query_destroy(query); query=NULL;
      check_equal(orm_raw(connection,orm_view("SET autocommit=1"),&query,&error),ORM_STATUS_INVALID_STATE);
      disconnect_database(); check_equal(rollbacks,rolled); connect_database(false); score_is(99);
    }
    it("preserves disabled mode and pending access when implicit command or query owner creation fails") {
      tdsql_connection *backend=connection->backend.context;
      check_equal(execute_sql("SET autocommit=0"),ORM_STATUS_OK);
      check_equal(execute_sql("SET TRANSACTION READ WRITE"),ORM_STATUS_OK);
      const char *const statements[]={"UPDATE items SET score=99 WHERE id=1","SELECT score FROM items WHERE id=1"};
      for(size_t i=0;i<sizeof(statements)/sizeof(statements[0]);++i) {
        fail_allocation=allocations+1; check_equal(execute_sql(statements[i]),ORM_STATUS_OUT_OF_MEMORY); fail_allocation=0;
        check_false(backend->autocommit); check_null(backend->sql_transaction); check_false(backend->active);
        check_equal(backend->next_access,SQLPARSER_READ_WRITE); check_equal(backend->failure,ORM_STATUS_OK);
        get_failure=true; check_equal(execute_sql(statements[i]),ORM_STATUS_DATASTORE_ERROR);
        check_false(backend->autocommit); check_null(backend->sql_transaction); check_false(backend->active);
        check_equal(backend->next_access,SQLPARSER_READ_WRITE);
      }
      check_equal(commits,0u);
      check_equal(execute_sql(statements[0]),ORM_STATUS_OK); check_not_null(backend->sql_transaction);
      check_equal(backend->next_access,SQLPARSER_ACCESS_DEFAULT);
      check_equal(execute_sql("ROLLBACK"),ORM_STATUS_OK); check_equal(execute_sql("SET autocommit=1"),ORM_STATUS_OK); score_is(10);
    }
    it("retains the implicit owner after cursor allocation failure and rolls it back on final release") {
      tdsql_connection *backend=connection->backend.context;
      check_equal(execute_sql("SET autocommit=0"),ORM_STATUS_OK);
      fail_allocation=allocations+2;
      check_equal(execute_sql("SELECT score FROM items WHERE id=1"),ORM_STATUS_OUT_OF_MEMORY); fail_allocation=0;
      tdsql_transaction *owner=backend->sql_transaction; check_not_null(owner); check_equal(owner->references,1u);
      check_false(owner->budget.statement_active); check_false(owner->owner.failed);
      check_equal(owner->budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],owner->budget.retained_work_bytes);
      check_equal(execute_sql("UPDATE items SET score=99 WHERE id=1"),ORM_STATUS_OK); check_true(backend->sql_transaction==owner);
      const size_t rolled=rollbacks; disconnect_database(); check_equal(rollbacks,rolled+1);
      connect_database(false); score_is(10);
    }
    it("refunds expression workspace and checks SET limits before mode updates or implicit commit") {
      tdsql_connection *backend=connection->backend.context;
      check_equal(execute_sql("SET autocommit=0"),ORM_STATUS_OK);
      check_equal(execute_sql("UPDATE items SET score=99 WHERE id=1"),ORM_STATUS_OK);
      tdsql_transaction *owner=backend->sql_transaction; const size_t committed=commits,rolled=rollbacks;
      const orm_sql_transaction_budget_amount used=owner->budget.transaction_used;
      const uint64_t work_limit=owner->budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES];
      const uint64_t step_limit=owner->budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      const uint64_t limits[]={owner->budget.retained_work_bytes,owner->budget.retained_work_bytes+1};
      for(size_t i=0;i<sizeof(limits)/sizeof(limits[0]);++i) {
        owner->budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=limits[i];
        check_equal(execute_sql("SET autocommit=CASE WHEN TRUE THEN 1 ELSE 0 END"),ORM_STATUS_LIMIT_EXCEEDED);
        check_false(backend->autocommit); check_true(backend->sql_transaction==owner); check_false(owner->owner.failed);
        check_false(owner->budget.statement_active);
        check_equal(owner->budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],owner->budget.retained_work_bytes);
      }
      owner->budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=work_limit;
      owner->budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=1;
      check_equal(execute_sql("SET autocommit=1"),ORM_STATUS_LIMIT_EXCEEDED);
      uint64_t affected=UINT64_MAX;
      check_equal(rel_backend_execute(backend,&query->plan,&connection->limits,&affected,&error),ORM_STATUS_LIMIT_EXCEEDED);
      check_equal(affected,UINT64_MAX); check_false(backend->autocommit); check_false(owner->owner.failed);
      owner->budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=step_limit;
      check_equal(execute_sql("SET autocommit=ABS(-2)"),ORM_STATUS_SQL_ERROR);
      check_equal(owner->budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],owner->budget.retained_work_bytes);
      check_false(owner->budget.statement_active); check_false(owner->owner.failed);
      check_equal(owner->budget.transaction_used.read_rows,used.read_rows);
      check_equal(owner->budget.transaction_used.read_bytes,used.read_bytes);
      check_equal(owner->budget.transaction_used.write_bytes,used.write_bytes);
      check_equal(commits,committed); check_equal(rollbacks,rolled);
      check_equal(execute_sql("SET autocommit=1"),ORM_STATUS_OK); check_true(backend->autocommit); score_is(99);
    }
    it("updates next and session characteristics without opening committing or rolling back a native owner") {
      tdsql_connection *backend=connection->backend.context;
      check_equal(backend->session_access,SQLPARSER_READ_WRITE);
      check_equal(execute_sql("SET TRANSACTION READ ONLY"),ORM_STATUS_OK);
      check_equal(backend->next_access,SQLPARSER_READ_ONLY);
      check_equal(execute_sql("SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE"),ORM_STATUS_OK);
      check_equal(backend->next_access,SQLPARSER_READ_ONLY);
      check_equal(commits,0u); check_equal(rollbacks,0u); check_equal(allocations,0u);
      check_false(backend->active); check_null(backend->sql_transaction);
      check_equal(execute_sql("BEGIN"),ORM_STATUS_OK);
      tdsql_transaction *owner=backend->sql_transaction; check_true(owner->read_only);
      const size_t allocated=allocations,committed=commits,rolled=rollbacks;
      check_equal(execute_sql("SET LOCAL TRANSACTION READ WRITE"),ORM_STATUS_OK);
      check_true(owner==backend->sql_transaction); check_true(owner->read_only);
      check_equal(execute_sql("SET TRANSACTION READ WRITE"),ORM_STATUS_SQL_ERROR);
      check_equal(backend->next_access,SQLPARSER_ACCESS_DEFAULT);
      check_equal(backend->session_access,SQLPARSER_READ_WRITE);
      check_equal(commits,committed); check_equal(rollbacks,rolled); check_equal(allocations,allocated);
      check_false(owner->budget.statement_active); check_false(owner->owner.failed);
      check_equal(execute_sql("ROLLBACK"),ORM_STATUS_OK);
    }
    it("preserves pending access when BEGIN allocation initialization parsing or parameter admission fails") {
      tdsql_connection *backend=connection->backend.context;
      check_equal(execute_sql("SET TRANSACTION READ ONLY"),ORM_STATUS_OK);
      fail_allocation=allocations+1; check_equal(execute_sql("BEGIN"),ORM_STATUS_OUT_OF_MEMORY); fail_allocation=0;
      check_equal(backend->next_access,SQLPARSER_READ_ONLY); check_false(backend->active);
      get_failure=true; check_equal(execute_sql("BEGIN"),ORM_STATUS_DATASTORE_ERROR);
      check_equal(backend->next_access,SQLPARSER_READ_ONLY); check_false(backend->active);
      const size_t allocated=allocations,committed=commits,rolled=rollbacks;
      check_equal(execute_sql("SELECT FROM"),ORM_STATUS_SQL_ERROR);
      check_equal(backend->next_access,SQLPARSER_READ_ONLY);
      check_equal(execute_sql("BEGIN; COMMIT"),ORM_STATUS_LIMIT_EXCEEDED);
      check_equal(backend->next_access,SQLPARSER_READ_ONLY);
      orm_query_destroy(query); query=NULL;
      check_equal(orm_raw(connection,orm_view("SET SESSION TRANSACTION READ WRITE"),&query,&error),ORM_STATUS_OK);
      check_equal(orm_query_bind(query,orm_i64(1),&error),ORM_STATUS_OK);
      check_equal(orm_query_execute(query,&result,&error),ORM_STATUS_SQL_ERROR);
      check_equal(backend->next_access,SQLPARSER_READ_ONLY);
      check_equal(commits,committed); check_equal(rollbacks,rolled); check_equal(allocations,allocated);
      check_equal(execute_sql("BEGIN"),ORM_STATUS_OK); check_true(backend->sql_transaction->read_only);
      check_equal(backend->next_access,SQLPARSER_ACCESS_DEFAULT); check_equal(execute_sql("ROLLBACK"),ORM_STATUS_OK);
      score_is(10);
    }
    it("rejects settings on poisoned or consumed owners without changing session state") {
      tdsql_connection *backend=connection->backend.context;
      check_equal(execute_sql("BEGIN"),ORM_STATUS_OK); check_equal(execute_sql("SAVEPOINT target"),ORM_STATUS_OK);
      fail_next_point(POINT_RELEASE,false);
      check_equal(execute_sql("RELEASE SAVEPOINT target"),ORM_STATUS_DATASTORE_ERROR);
      check_equal(execute_sql("SET SESSION TRANSACTION READ ONLY"),ORM_STATUS_INVALID_STATE);
      check_equal(backend->session_access,SQLPARSER_READ_WRITE); check_equal(backend->next_access,SQLPARSER_ACCESS_DEFAULT);
      check_equal(execute_sql("ROLLBACK"),ORM_STATUS_OK);
      check_equal(execute_sql("BEGIN"),ORM_STATUS_OK); commit_mode=COMMIT_CONFLICT;
      check_equal(execute_sql("COMMIT"),ORM_STATUS_BUSY);
      const size_t committed=commits,rolled=rollbacks;
      check_equal(execute_sql("SET LOCAL TRANSACTION READ ONLY"),ORM_STATUS_INVALID_STATE);
      check_equal(execute_sql("SET TRANSACTION READ ONLY"),ORM_STATUS_INVALID_STATE);
      check_equal(backend->session_access,SQLPARSER_READ_WRITE); check_equal(backend->next_access,SQLPARSER_ACCESS_DEFAULT);
      check_equal(commits,committed); check_equal(rollbacks,rolled);
      check_equal(execute_sql("ROLLBACK"),ORM_STATUS_OK); score_is(10);
    }
    it("checks setting budgets before publishing access or affected output in both entry paths") {
      tdsql_connection *backend=connection->backend.context;
      check_equal(execute_sql("SET TRANSACTION READ ONLY"),ORM_STATUS_OK);
      uint64_t limit=backend->config.budget_limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      backend->config.budget_limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=1;
      check_equal(execute_sql("SET SESSION TRANSACTION READ WRITE"),ORM_STATUS_LIMIT_EXCEEDED);
      uint64_t affected=UINT64_MAX;
      check_equal(rel_backend_execute(backend,&query->plan,&connection->limits,&affected,&error),ORM_STATUS_LIMIT_EXCEEDED);
      check_equal(affected,UINT64_MAX); check_equal(backend->next_access,SQLPARSER_READ_ONLY);
      check_equal(backend->session_access,SQLPARSER_READ_WRITE); check_false(backend->active);
      check_equal(commits,0u); check_equal(rollbacks,0u); check_equal(allocations,0u);
      backend->config.budget_limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=limit;
      begin_transaction(); tdsql_transaction *owner=transaction->backend.context; check_true(owner->read_only);
      limit=owner->budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      owner->budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=1;
      check_equal(execute_sql("SET SESSION TRANSACTION READ ONLY"),ORM_STATUS_LIMIT_EXCEEDED);
      check_equal(backend->session_access,SQLPARSER_READ_WRITE); check_false(owner->owner.failed);
      check_false(owner->budget.statement_active);
      owner->budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=limit;
      check_equal(execute_sql("SET SESSION TRANSACTION READ ONLY"),ORM_STATUS_OK);
      check_equal(backend->session_access,SQLPARSER_READ_ONLY);
      check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
      orm_transaction_destroy(transaction); transaction=NULL;
      check_equal(backend->session_access,SQLPARSER_READ_ONLY);
    }
    it("requires SQL rollback acknowledgement after commit conflict and does not retry COMMIT") {
      check_equal(execute_sql("BEGIN"),ORM_STATUS_OK);
      check_equal(execute_sql("UPDATE items SET score=99 WHERE id=1"),ORM_STATUS_OK);
      tdsql_connection *backend=connection->backend.context;
      commit_mode=COMMIT_CONFLICT;
      check_equal(execute_sql("COMMIT AND CHAIN"),ORM_STATUS_BUSY); check_null(result);
      check_not_null(backend->sql_transaction); check_null(backend->sql_transaction->owner.transaction);
      check_equal(backend->sql_transaction->state,REL_ROLLBACK_REQUIRED); check_equal(commits,1u);
      const size_t rolled=rollbacks;
      check_equal(execute_sql("COMMIT"),ORM_STATUS_INVALID_STATE);
      check_equal(execute_sql("BEGIN"),ORM_STATUS_INVALID_STATE);
      check_equal(execute_sql("SAVEPOINT blocked"),ORM_STATUS_INVALID_STATE);
      check_equal(execute_sql("SELECT score FROM items"),ORM_STATUS_INVALID_STATE);
      check_equal(commits,1u); check_equal(execute_sql("ROLLBACK"),ORM_STATUS_OK);
      check_equal(rollbacks,rolled); check_null(backend->sql_transaction); check_false(backend->active); score_is(10);
      check_equal(execute_sql("BEGIN"),ORM_STATUS_OK);
      check_equal(execute_sql("UPDATE items SET score=77 WHERE id=1"),ORM_STATUS_OK);
      check_equal(execute_sql("COMMIT"),ORM_STATUS_OK); score_is(77);
    }
    it("quarantines SQL COMMIT_UNKNOWN without starting a chained transaction or retrying") {
      check_equal(execute_sql("BEGIN"),ORM_STATUS_OK);
      check_equal(execute_sql("UPDATE items SET score=99 WHERE id=1"),ORM_STATUS_OK);
      tdsql_connection *backend=connection->backend.context; const size_t allocated=allocations;
      commit_mode=COMMIT_AFTER_SUCCESS;
      check_equal(execute_sql("COMMIT AND CHAIN"),ORM_STATUS_COMMIT_UNKNOWN); check_null(result);
      check_null(backend->sql_transaction); check_false(backend->active);
      check_equal(backend->failure,ORM_STATUS_COMMIT_UNKNOWN); check_equal(connection->failure,ORM_STATUS_COMMIT_UNKNOWN);
      check_equal(commits,1u); check_equal(allocations,allocated);
      const size_t rolled=rollbacks;
      orm_query_destroy(query); query=NULL;
      check_equal(orm_raw(connection,orm_view("ROLLBACK"),&query,&error),ORM_STATUS_INVALID_STATE); check_null(query);
      disconnect_database(); check_equal(rollbacks,rolled); connect_database(false); score_is(99);
    }
    it("acknowledges a failed SQL rollback without touching its already consumed native owner") {
      check_equal(execute_sql("BEGIN"),ORM_STATUS_OK);
      check_equal(execute_sql("UPDATE items SET score=99 WHERE id=1"),ORM_STATUS_OK);
      tdsql_connection *backend=connection->backend.context;
      rollback_failure=true; check_equal(execute_sql("ROLLBACK"),ORM_STATUS_DATASTORE_ERROR);
      check_not_null(backend->sql_transaction); check_null(backend->sql_transaction->owner.transaction);
      check_equal(backend->sql_transaction->state,REL_ROLLBACK_REQUIRED);
      check_equal(execute_sql("COMMIT"),ORM_STATUS_INVALID_STATE);
      const size_t rolled=rollbacks;
      check_equal(execute_sql("ROLLBACK"),ORM_STATUS_OK); check_equal(rollbacks,rolled);
      check_null(backend->sql_transaction); score_is(10);
    }
    it("permits full SQL rollback after native savepoint failure while rejecting every other control") {
      const point_probe phases[]={POINT_CREATE,POINT_ROLLBACK,POINT_RELEASE};
      const char *const commands[]={"SAVEPOINT fresh","ROLLBACK TO target","RELEASE SAVEPOINT target"};
      for(size_t i=0;i<sizeof(phases)/sizeof(phases[0]);++i) for(int after=0;after<2;++after) {
        check_equal(execute_sql("BEGIN"),ORM_STATUS_OK); check_equal(execute_sql("SAVEPOINT target"),ORM_STATUS_OK);
        check_equal(execute_sql("UPDATE items SET score=99 WHERE id=1"),ORM_STATUS_OK);
        fail_next_point(phases[i],after!=0);
        check_equal(execute_sql(commands[i]),ORM_STATUS_DATASTORE_ERROR); check_null(result);
        tdsql_connection *backend=connection->backend.context;
        check_true(backend->sql_transaction->owner.failed); check_false(backend->sql_transaction->budget.statement_active);
        check_equal(execute_sql("COMMIT"),ORM_STATUS_INVALID_STATE); check_equal(execute_sql("BEGIN"),ORM_STATUS_INVALID_STATE);
        check_equal(execute_sql("UPDATE items SET score=77"),ORM_STATUS_INVALID_STATE);
        fail_point=POINT_NONE;
        check_equal(execute_sql("ROLLBACK"),ORM_STATUS_OK); check_null(backend->sql_transaction); score_is(10);
      }
    }
    it("reports failed new-owner allocation after a completed BEGIN replacement or CHAIN without retrying the old transaction") {
      tdsql_connection *backend=connection->backend.context;
      fail_allocation=allocations+1;
      check_equal(execute_sql("BEGIN"),ORM_STATUS_OUT_OF_MEMORY); check_null(result);
      check_null(backend->sql_transaction); check_false(backend->active); fail_allocation=0;
      const char *const controls[]={"BEGIN","COMMIT AND CHAIN","ROLLBACK AND CHAIN"};
      for(size_t i=0;i<sizeof(controls)/sizeof(controls[0]);++i) {
        check_equal(execute_sql("BEGIN"),ORM_STATUS_OK);
        check_equal(execute_sql("UPDATE items SET score=99 WHERE id=1"),ORM_STATUS_OK);
        const size_t committed=commits;
        fail_allocation=allocations+1;
        check_equal(execute_sql(controls[i]),ORM_STATUS_OUT_OF_MEMORY); check_null(result);
        check_not_null(strstr(error.message,"previous SQL transaction ended"));
        check_null(backend->sql_transaction); check_false(backend->active); check_equal(backend->failure,ORM_STATUS_OK);
        check_equal(commits,committed+(i==2 ? 0 : 1)); fail_allocation=0;
        score_is(i==2 ? 10 : 99);
        check_equal(execute_sql("UPDATE items SET score=10 WHERE id=1"),ORM_STATUS_OK);
      }
    }
    it("checks control budgets before commit and leaves affected unchanged on failure") {
      check_equal(execute_sql("BEGIN"),ORM_STATUS_OK);
      check_equal(execute_sql("SAVEPOINT target"),ORM_STATUS_OK);
      check_equal(execute_sql("UPDATE items SET score=99 WHERE id=1"),ORM_STATUS_OK);
      tdsql_connection *backend=connection->backend.context; tdsql_transaction *owner=backend->sql_transaction;
      const uint64_t limit=owner->budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      const size_t committed=commits,rolled=rollbacks;
      owner->budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=1;
      const char *const controls[]={"COMMIT","BEGIN","ROLLBACK"};
      for(size_t i=0;i<sizeof(controls)/sizeof(controls[0]);++i) {
        check_equal(execute_sql(controls[i]),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
        uint64_t affected=UINT64_MAX;
        check_equal(rel_backend_execute(backend,&query->plan,&connection->limits,&affected,&error),ORM_STATUS_LIMIT_EXCEEDED);
        check_equal(affected,UINT64_MAX); check_true(backend->sql_transaction==owner);
        check_false(owner->budget.statement_active); check_false(owner->owner.failed); check_equal(vec_size(&owner->owner.savepoints),1u);
      }
      check_equal(commits,committed); check_equal(rollbacks,rolled);
      owner->budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=limit;
      score_is(99); check_equal(execute_sql("ROLLBACK"),ORM_STATUS_OK); score_is(10);
    }
    it("retains cumulative write quotas across SQL savepoint rollback and resets them only on a new transaction") {
      check_equal(execute_sql("BEGIN"),ORM_STATUS_OK); check_equal(execute_sql("SAVEPOINT target"),ORM_STATUS_OK);
      check_equal(execute_sql("UPDATE items SET score=99 WHERE id=1"),ORM_STATUS_OK);
      tdsql_connection *backend=connection->backend.context; tdsql_transaction *owner=backend->sql_transaction;
      const uint64_t used=owner->budget.transaction_used.write_bytes;
      check_greater(used,0u);
      check_equal(execute_sql("ROLLBACK TO target"),ORM_STATUS_OK);
      check_equal(owner->budget.transaction_used.write_bytes,used);
      const uint64_t limit=owner->budget.limits.transaction.write_bytes;
      owner->budget.limits.transaction.write_bytes=used;
      check_equal(execute_sql("UPDATE items SET score=77 WHERE id=1"),ORM_STATUS_LIMIT_EXCEEDED); check_null(result); score_is(10);
      owner->budget.limits.transaction.write_bytes=limit;
      check_equal(execute_sql("ROLLBACK AND CHAIN"),ORM_STATUS_OK);
      check_equal(backend->sql_transaction->budget.transaction_used.write_bytes,0u);
      check_equal(execute_sql("UPDATE items SET score=77 WHERE id=1"),ORM_STATUS_OK);
      check_equal(execute_sql("ROLLBACK"),ORM_STATUS_OK); score_is(10);
    }
    it("delays SQL transaction rollback until the connection's final query reference is released") {
      check_equal(execute_sql("BEGIN"),ORM_STATUS_OK);
      check_equal(execute_sql("UPDATE items SET score=99 WHERE id=1"),ORM_STATUS_OK);
      orm_result_destroy(result); result=NULL;
      tdsql_connection *backend=connection->backend.context; const size_t rolled=rollbacks;
      orm_disconnect(connection); connection=NULL;
      check_not_null(backend->sql_transaction); check_equal(rollbacks,rolled);
      orm_query_destroy(query); query=NULL; check_equal(rollbacks,rolled+1);
      connect_database(false); score_is(10);
    }
    it("fails fast in an owned child when final SQL transaction rollback fails before or after the native call") {
      disconnect_database();
      const char *const modes[]={"before","after"};
      for(size_t i=0;i<sizeof(modes)/sizeof(modes[0]);++i) {
        const char *args[]={"--sql-close-child",modes[i],directory,NULL};
        cmeta_process_options_t options; cmeta_process_options_init(&options);
        options.program=program_path; options.args=args; options.cwd=directory;
        options.flags=SALTS_PROCESS_CAPTURE_STDERR | SALTS_PROCESS_CAPTURE_STDOUT; options.timeout_ms=SQL_CLOSE_CHILD_TIMEOUT_MS;
        options.max_output_bytes=SQL_CLOSE_CHILD_OUTPUT_BYTES;
        check_equal(cmeta_process_spawn(&options,&child_process),SALTS_OK);
        cmeta_process_result_t stopped={0}; check_equal(cmeta_process_wait(child_process,&stopped),SALTS_OK);
        char output[SQL_CLOSE_CHILD_OUTPUT_BYTES]={0}; size_t total=0;
        for(size_t channel=0;channel<2;++channel)
          while(total<sizeof(output)-1) {
            size_t count=0;
            const int status=channel ? cmeta_process_read_stdout(child_process,output+total,sizeof(output)-1-total,&count) :
                cmeta_process_read_stderr(child_process,output+total,sizeof(output)-1-total,&count);
            total+=count; if(status==SALTS_EOF || !count) break; check_equal(status,SALTS_OK);
          }
#ifdef _WIN32
        check_equal(stopped.state,SALTS_PROCESS_EXITED); check_not_equal(stopped.exit_code,EXIT_SUCCESS);
        check_not_equal(stopped.exit_code,SQL_CLOSE_CHILD_FAILED);
#else
        check_equal(stopped.state,SALTS_PROCESS_SIGNALED); check_equal(stopped.term_signal,SIGABRT);
#endif
        check_not_null(strstr(output,"SQL session rollback during connection destroy failed"));
        cmeta_process_destroy(child_process); child_process=NULL;
        connect_database(false); score_is(10); disconnect_database();
      }
      connect_database(false);
    }
  }
  it("requires rollback acknowledgement after a commit conflict without reusing the freed native handle") {
    begin_transaction(); check_equal(execute_sql("UPDATE items SET score=99 WHERE id=1"),ORM_STATUS_OK);
    tdsql_transaction *owner = transaction->backend.context;
    commit_mode = COMMIT_CONFLICT;
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_BUSY);
    check_null(owner->owner.transaction); check_equal(owner->state,REL_ROLLBACK_REQUIRED);
    check_equal(commits,1u); check_equal(rollbacks,1u);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_INVALID_STATE);
    check_equal(execute_sql("SELECT score FROM items"),ORM_STATUS_INVALID_STATE); check_null(result);
    check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
    check_equal(commits,1u); check_equal(rollbacks,1u);
    orm_transaction_destroy(transaction); transaction = NULL; score_is(10);
  }
  it("keeps autocommit affected output unchanged when commit conflicts") {
    orm_query_destroy(query); query = NULL;
    check_equal(orm_raw(connection,orm_view("UPDATE items SET score=99 WHERE id=1"),&query,&error),ORM_STATUS_OK);
    uint64_t affected = UINT64_MAX; commit_mode = COMMIT_CONFLICT;
    check_equal(connection->backend.ops->execute_command(connection->backend.context,&query->plan,
      &connection->limits,&affected,&error),ORM_STATUS_BUSY);
    check_equal(affected,UINT64_MAX); check_equal(commits,1u); check_equal(rollbacks,1u); score_is(10);
  }
  it("quarantines an explicit unknown commit and never retries it") {
    begin_transaction(); check_equal(execute_sql("UPDATE items SET score=99 WHERE id=1"),ORM_STATUS_OK);
    commit_mode = COMMIT_AFTER_SUCCESS;
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_COMMIT_UNKNOWN);
    check_equal(transaction->state,ORM_TRANSACTION_COMMIT_UNKNOWN);
    check_equal(connection->failure,ORM_STATUS_COMMIT_UNKNOWN); check_equal(commits,1u);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_INVALID_STATE);
    check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_INVALID_STATE);
    const size_t rolled = rollbacks; disconnect_database(); check_equal(rollbacks,rolled);
    connect_database(false); score_is(99);
  }
  it("quarantines an autocommit unknown result instead of publishing a successful command") {
    commit_mode = COMMIT_AFTER_SUCCESS;
    check_equal(execute_sql("UPDATE items SET score=99 WHERE id=1"),ORM_STATUS_COMMIT_UNKNOWN);
    check_null(result); check_equal(connection->failure,ORM_STATUS_COMMIT_UNKNOWN); check_equal(commits,1u);
    disconnect_database(); connect_database(false); score_is(99);
  }
  it("allows rollback acknowledgement after native rollback cleanup has already consumed the owner") {
    begin_transaction(); check_equal(execute_sql("UPDATE items SET score=99 WHERE id=1"),ORM_STATUS_OK);
    rollback_failure = true; check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_DATASTORE_ERROR);
    tdsql_transaction *owner = transaction->backend.context;
    check_null(owner->owner.transaction); check_equal(owner->state,REL_ROLLBACK_REQUIRED);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_INVALID_STATE);
    const size_t rolled = rollbacks;
    check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK); check_equal(rollbacks,rolled);
    orm_transaction_destroy(transaction); transaction = NULL; score_is(10);
  }
  it("refunds failed transaction and cursor metadata allocations before retry") {
    fail_allocation = allocations + 1;
    check_equal(execute_sql("UPDATE items SET score=99 WHERE id=1"),ORM_STATUS_OUT_OF_MEMORY);
    fail_allocation = 0; score_is(10);
    begin_transaction(); fail_allocation = allocations + 1;
    check_equal(execute_sql("SELECT score FROM items"),ORM_STATUS_OUT_OF_MEMORY); check_null(result);
    tdsql_transaction *owner = transaction->backend.context;
    check_false(owner->budget.statement_active); check_false(owner->owner.failed);
    check_equal(owner->budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],owner->budget.retained_work_bytes);
    fail_allocation = 0; score_is(10);
    check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
  }
  it("cleans every structured rendering failure before native writes and allows retry") {
    begin_transaction();
    orm_query_destroy(query); query=NULL; orm_result_destroy(result); result=NULL;
    check_equal(orm_update(connection,orm_view("items"),&query,&error),ORM_STATUS_OK);
    check_equal(orm_query_set(query,orm_view("score"),orm_i64(99),&error),ORM_STATUS_OK);
    check_equal(orm_query_where(query,orm_view("id"),ORM_COMPARE_EQUAL,orm_i64(1),&error),ORM_STATUS_OK);
    orm_mysql_rendered_query rendered={0}; render_appends=0;
    check_equal(orm_mysql_render_plan(&query->plan,&connection->limits,&rendered,&error),ORM_STATUS_OK);
    const size_t append_count=render_appends, writes=put_calls;
    orm_mysql_rendered_query_destroy(&rendered);
    tdsql_transaction *owner=transaction->backend.context;
    for(size_t i=0;i<=append_count;++i) {
      render_appends=0; fail_render_append=i; render_allocation_failure=i==0;
      check_equal(orm_query_execute_in_transaction(query,transaction,&result,&error),ORM_STATUS_OUT_OF_MEMORY);
      check_null(result); check_equal(put_calls,writes);
      check_false(owner->owner.failed); check_false(owner->budget.statement_active);
      check_equal(owner->budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],owner->budget.retained_work_bytes);
    }
    fail_render_append=0;
    const uint64_t limit=owner->budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES];
    owner->budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=owner->budget.retained_work_bytes;
    check_equal(orm_query_execute_in_transaction(query,transaction,&result,&error),ORM_STATUS_LIMIT_EXCEEDED);
    check_null(result); check_equal(put_calls,writes); check_false(owner->owner.failed);
    owner->budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=limit;
    check_equal(orm_query_execute_in_transaction(query,transaction,&result,&error),ORM_STATUS_OK);
    score_is(99); check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
    orm_transaction_destroy(transaction); transaction=NULL; score_is(10);
  }
  it("rejects ignored structured command modifiers before changing rows") {
    begin_transaction();
    orm_query_destroy(query); query=NULL; orm_result_destroy(result); result=NULL;
    check_equal(orm_delete(connection,orm_view("items"),&query,&error),ORM_STATUS_OK);
    /* The public builder forbids these fields. Probe the backend boundary so
     * an alternate plan provider cannot silently widen a restricted command. */
    query->plan.has_limit=true; query->plan.limit=0;
    check_equal(orm_query_execute_in_transaction(query,transaction,&result,&error),ORM_STATUS_UNSUPPORTED);
    check_null(result); score_is(10);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
  }
  it("poisons savepoint creation before or after a reported native failure") {
    for (int after=0;after<2;++after) {
      begin_transaction(); check_equal(execute_sql("UPDATE items SET score=99 WHERE id=1"),ORM_STATUS_OK);
      fail_next_point(POINT_CREATE,after!=0);
      check_equal(orm_transaction_savepoint(transaction,orm_view("target"),&error),ORM_STATUS_DATASTORE_ERROR);
      require_full_rollback();
    }
  }
  it("poisons each native phase of a same-name replacement") {
    const point_probe stages[]={POINT_RELEASE,POINT_CREATE};
    for(size_t i=0;i<sizeof(stages)/sizeof(stages[0]);++i) for(int after=0;after<2;++after) {
      begin_transaction(); check_equal(orm_transaction_savepoint(transaction,orm_view("target"),&error),ORM_STATUS_OK);
      check_equal(execute_sql("UPDATE items SET score=99 WHERE id=1"),ORM_STATUS_OK);
      check_equal(orm_transaction_savepoint(transaction,orm_view("later"),&error),ORM_STATUS_OK);
      fail_next_point(stages[i],after!=0);
      check_equal(orm_transaction_savepoint(transaction,orm_view("target"),&error),ORM_STATUS_DATASTORE_ERROR);
      require_full_rollback();
    }
  }
  it("poisons rollback or target recreation failure without exposing a half-restored transaction") {
    const point_probe stages[]={POINT_ROLLBACK,POINT_CREATE};
    for(size_t i=0;i<sizeof(stages)/sizeof(stages[0]);++i) for(int after=0;after<2;++after) {
      begin_transaction(); check_equal(orm_transaction_savepoint(transaction,orm_view("target"),&error),ORM_STATUS_OK);
      check_equal(execute_sql("UPDATE items SET score=99 WHERE id=1"),ORM_STATUS_OK);
      fail_next_point(stages[i],after!=0);
      check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("target"),&error),ORM_STATUS_DATASTORE_ERROR);
      require_full_rollback();
    }
  }
  it("requires full rollback after release failure even when the native point was removed") {
    for(int after=0;after<2;++after) {
      begin_transaction(); check_equal(orm_transaction_savepoint(transaction,orm_view("target"),&error),ORM_STATUS_OK);
      check_equal(execute_sql("UPDATE items SET score=99 WHERE id=1"),ORM_STATUS_OK);
      fail_next_point(POINT_RELEASE,after!=0);
      check_equal(orm_transaction_release_savepoint(transaction,orm_view("target"),&error),ORM_STATUS_DATASTORE_ERROR);
      require_full_rollback();
    }
  }
  it("requires full rollback for each failed native phase reached through SQL savepoint commands") {
    const struct { const char *sql; point_probe stage; } cases[]={
      {"SAVEPOINT fresh",POINT_CREATE}, {"SAVEPOINT target",POINT_RELEASE},
      {"SAVEPOINT target",POINT_CREATE}, {"ROLLBACK TO target",POINT_ROLLBACK},
      {"ROLLBACK WORK TO SAVEPOINT target",POINT_CREATE}, {"RELEASE SAVEPOINT target",POINT_RELEASE}
    };
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) for(int after=0;after<2;++after) {
      begin_transaction(); check_equal(execute_sql("SAVEPOINT target"),ORM_STATUS_OK);
      check_equal(execute_sql("UPDATE items SET score=99 WHERE id=1"),ORM_STATUS_OK);
      fail_next_point(cases[i].stage,after!=0);
      check_equal(execute_sql(cases[i].sql),ORM_STATUS_DATASTORE_ERROR); check_null(result);
      require_full_rollback();
    }
  }
  it("refunds SQL savepoint allocation failure without changing native state or prior writes") {
    begin_transaction(); check_equal(execute_sql("UPDATE items SET score=99 WHERE id=1"),ORM_STATUS_OK);
    tdsql_transaction *owner=transaction->backend.context;
    const uint64_t retained=owner->budget.retained_work_bytes; const size_t calls=point_calls[POINT_CREATE];
    reserve_failure=true;
    check_equal(execute_sql("SAVEPOINT target"),ORM_STATUS_OUT_OF_MEMORY); check_null(result);
    check_false(reserve_failure); check_false(owner->owner.failed); check_false(owner->budget.statement_active);
    check_equal(owner->budget.retained_work_bytes,retained); check_equal(point_calls[POINT_CREATE],calls);
    check_equal(execute_sql("SAVEPOINT target"),ORM_STATUS_OK);
    const uint64_t allocated=owner->budget.retained_work_bytes; check_greater(allocated,retained);
    check_equal(execute_sql("UPDATE items SET score=77 WHERE id=1"),ORM_STATUS_OK);
    check_equal(execute_sql("ROLLBACK TO target"),ORM_STATUS_OK); score_is(99);
    check_equal(execute_sql("RELEASE SAVEPOINT target"),ORM_STATUS_OK);
    check_equal(owner->budget.retained_work_bytes,allocated); check_equal(vec_size(&owner->owner.savepoints),0u);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
    check_equal(owner->budget.retained_work_bytes,owner->reserved); check_null(owner->owner.savepoints.data);
  }
  it("rejects SQL savepoint step and workspace exhaustion before native effects and leaves affected unchanged") {
    begin_transaction(); check_equal(execute_sql("SAVEPOINT target"),ORM_STATUS_OK);
    tdsql_transaction *owner=transaction->backend.context;
    const uint64_t step_limit=owner->budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    size_t calls[POINT_NONE]; memcpy(calls,point_calls,sizeof(calls));
    owner->budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=1;
    const char *const commands[]={"SAVEPOINT target","ROLLBACK TO target","RELEASE SAVEPOINT target"};
    for(size_t i=0;i<sizeof(commands)/sizeof(commands[0]);++i) {
      check_equal(execute_sql(commands[i]),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
      uint64_t affected=99;
      check_equal(rel_execute(owner,&query->plan,&connection->limits,&affected,&error),ORM_STATUS_LIMIT_EXCEEDED);
      check_equal(affected,99u); check_false(owner->budget.statement_active); check_false(owner->owner.failed);
    }
    owner->budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=step_limit;
    const uint64_t work_limit=owner->budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES];
    owner->budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=owner->budget.retained_work_bytes;
    /* The registry already fits; empty parameters must not require extra WORK. */
    check_equal(execute_sql("ROLLBACK TO target"),ORM_STATUS_OK);
    owner->budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=work_limit;
    check_equal(vec_size(&owner->owner.savepoints),1u);
    for(size_t i=0;i<POINT_NONE;++i) {
      const size_t expected=calls[i]+(i==POINT_CREATE || i==POINT_ROLLBACK ? 1 : 0);
      check_equal(point_calls[i],expected);
    }
    check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
    orm_transaction_destroy(transaction); transaction=NULL; begin_transaction();
    owner=transaction->backend.context;
    const uint64_t fresh_work_limit=owner->budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES];
    const size_t create_calls=point_calls[POINT_CREATE];
    owner->budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=owner->budget.retained_work_bytes;
    check_equal(execute_sql("SAVEPOINT fresh"),ORM_STATUS_LIMIT_EXCEEDED); check_null(result);
    check_equal(point_calls[POINT_CREATE],create_calls); check_false(owner->owner.failed);
    check_false(owner->budget.statement_active); check_false(owner->owner.savepoints.initialized);
    owner->budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=fresh_work_limit;
    check_equal(execute_sql("SAVEPOINT fresh"),ORM_STATUS_OK);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
  }
  it("does not call native savepoint creation for SQL SAVEPOINT in autocommit") {
    const size_t calls=point_calls[POINT_CREATE];
    fail_next_point(POINT_CREATE,false);
    check_equal(execute_sql("SAVEPOINT target"),ORM_STATUS_OK); check_equal(point_calls[POINT_CREATE],calls);
    check_equal(execute_sql("ROLLBACK TO target"),ORM_STATUS_SQL_ERROR);
    check_equal(execute_sql("RELEASE SAVEPOINT target"),ORM_STATUS_SQL_ERROR);
    begin_transaction(); check_equal(execute_sql("SAVEPOINT target"),ORM_STATUS_DATASTORE_ERROR);
    require_full_rollback();
  }
  it("refunds a failed registry allocation and retains capacity only until transaction finish") {
    begin_transaction(); tdsql_transaction *owner=transaction->backend.context;
    const uint64_t retained=owner->budget.retained_work_bytes; const size_t calls=point_calls[POINT_CREATE];
    reserve_failure=true;
    check_equal(orm_transaction_savepoint(transaction,orm_view("target"),&error),ORM_STATUS_OUT_OF_MEMORY);
    check_false(reserve_failure); check_false(owner->owner.failed); check_false(owner->budget.statement_active);
    check_equal(owner->budget.retained_work_bytes,retained); check_equal(point_calls[POINT_CREATE],calls);
    check_equal(orm_transaction_savepoint(transaction,orm_view("target"),&error),ORM_STATUS_OK);
    const uint64_t allocated=owner->budget.retained_work_bytes; check_greater(allocated,retained);
    check_equal(orm_transaction_release_savepoint(transaction,orm_view("target"),&error),ORM_STATUS_OK);
    check_equal(owner->budget.retained_work_bytes,allocated); check_equal(vec_size(&owner->owner.savepoints),0u);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
    check_equal(owner->budget.retained_work_bytes,owner->reserved); check_null(owner->owner.savepoints.data);
  }
  it("rejects insufficient retained work capacity before touching native savepoints") {
    begin_transaction(); tdsql_transaction *owner=transaction->backend.context;
    size_t reserved=0; const size_t calls=point_calls[POINT_CREATE];
    check_equal(orm_tidesdb_sql_budget_begin(&owner->budget,&error),ORM_STATUS_OK);
    const size_t available=(size_t)(owner->budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]-owner->budget.retained_work_bytes);
    check_equal(orm_tidesdb_sql_budget_reserve_retained_capacity(&owner->budget,available,1,0,&reserved,&error),ORM_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_end(&owner->budget,&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("target"),&error),ORM_STATUS_LIMIT_EXCEEDED);
    check_equal(point_calls[POINT_CREATE],calls); check_false(owner->owner.failed);
    check_equal(orm_tidesdb_sql_budget_release_retained(&owner->budget,reserved,&error),ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction,orm_view("target"),&error),ORM_STATUS_OK);
    check_equal(orm_transaction_rollback(transaction,&error),ORM_STATUS_OK);
  }
  it("rejects exhausted execution steps before changing any native savepoint") {
    begin_transaction(); check_equal(orm_transaction_savepoint(transaction,orm_view("target"),&error),ORM_STATUS_OK);
    tdsql_transaction *owner=transaction->backend.context;
    const uint64_t limit=owner->budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    size_t calls[POINT_NONE]; memcpy(calls,point_calls,sizeof(calls));
    owner->budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=1;
    check_equal(orm_transaction_savepoint(transaction,orm_view("target"),&error),ORM_STATUS_LIMIT_EXCEEDED);
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("target"),&error),ORM_STATUS_LIMIT_EXCEEDED);
    check_equal(orm_transaction_release_savepoint(transaction,orm_view("target"),&error),ORM_STATUS_LIMIT_EXCEEDED);
    for(size_t i=0;i<POINT_NONE;++i) check_equal(point_calls[i],calls[i]);
    check_false(owner->owner.failed); check_false(owner->budget.statement_active);
    check_equal(vec_size(&owner->owner.savepoints),1u);
    owner->budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=limit;
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("target"),&error),ORM_STATUS_OK);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
  }
  it("isolates private statement rollback from user savepoints after a partial batch write") {
    begin_transaction(); check_equal(orm_transaction_savepoint(transaction,orm_view("target"),&error),ORM_STATUS_OK);
    fail_put_call=put_calls+2;
    check_equal(execute_sql("INSERT INTO items(id,score) VALUES(2,20),(3,30)"),ORM_STATUS_DATASTORE_ERROR);
    fail_put_call=0;
    tdsql_transaction *owner=transaction->backend.context;
    check_false(owner->owner.failed); check_equal(vec_size(&owner->owner.savepoints),1u);
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("target"),&error),ORM_STATUS_OK);
    check_equal(execute_sql("INSERT INTO items(id,score) VALUES(2,20),(3,30)"),ORM_STATUS_OK);
    check_equal(orm_transaction_rollback_to_savepoint(transaction,orm_view("target"),&error),ORM_STATUS_OK);
    check_equal(execute_sql("SELECT id FROM items"),ORM_STATUS_OK);
    uint64_t count=0; check_equal(orm_result_row_count(result,&count,&error),ORM_STATUS_OK); check_equal(count,1u);
    check_equal(orm_transaction_commit(transaction,&error),ORM_STATUS_OK);
  }
}

static void sql_close_child_require(bool condition) {
  if (!condition) _Exit(SQL_CLOSE_CHILD_FAILED);
}
static void sql_close_child_execute(const char *sql) {
  orm_result_destroy(result); result=NULL; orm_query_destroy(query); query=NULL;
  sql_close_child_require(orm_raw(connection,orm_view(sql),&query,&error)==ORM_STATUS_OK);
  sql_close_child_require(orm_query_execute(query,&result,&error)==ORM_STATUS_OK);
}
int main(int argc, char **argv) {
  program_path=argv[0];
  if(argc==4 && !strcmp(argv[1],"--sql-close-child")) {
#ifdef _WIN32
    (void)_set_abort_behavior(0,_WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    directory=argv[3]; orm_error_init(&error);
    orm_config_t child_config; orm_config(&child_config);
    const orm_option_t options[]={{orm_view("path"),orm_view(directory)},
      {orm_view("column_family"),orm_view("rel")}};
    child_config.driver=orm_view("tidesdb"); child_config.options=options;
    child_config.option_count=sizeof(options)/sizeof(options[0]);
    sql_close_child_require(orm_connect_with_factory_v1(&child_config,orm_tidesdb_relational_create,&connection,&error)==ORM_STATUS_OK);
    sql_close_child_execute("BEGIN"); sql_close_child_execute("UPDATE items SET score=99 WHERE id=1");
    rollback_failure_before=!strcmp(argv[2],"before"); rollback_failure=!rollback_failure_before;
    orm_result_destroy(result); result=NULL; orm_query_destroy(query); query=NULL;
    orm_disconnect(connection);
    _Exit(SQL_CLOSE_CHILD_FAILED);
  }
  return ttest_main__(argc,argv,TTEST_INVOKE_SPEC_ADAPTER__,TT_USE_COLOR!=0,
      TT_USE_TAP!=0,TTEST_IS_ATTY__()!=0,TTEST_PRINT_TRACE_DEFAULT__);
}

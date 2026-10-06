#ifndef TIDESSQL_H
#define TIDESSQL_H

#include <turbodb/types.h>
#include <stdbool.h>

#if defined(_WIN32)
#define TDSQL_CALL __cdecl
#else
#define TDSQL_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define TDSQL_ABI_VERSION UINT32_C(1)
#define TDSQL_VERSION_MAJOR UINT32_C(1)
#define TDSQL_VERSION_MINOR UINT32_C(3)
#define TDSQL_VERSION_PATCH UINT32_C(0)

uint32_t TDSQL_CALL tdsql_abi_version(void);

typedef struct tdsql_connection tdsql_connection;
typedef struct tdsql_database tdsql_database;
typedef struct tdsql_transaction tdsql_transaction;
typedef struct tdsql_result tdsql_result;
typedef struct tdsql_statement tdsql_statement;

typedef struct tdsql_limits {
  size_t max_parameters, max_columns, max_query_bytes, max_parameter_bytes;
  uint64_t max_result_rows, max_result_bytes;
} tdsql_limits;

static inline tdsql_limits tdsql_limits_default(void) {
  const tdsql_limits limits = {TURBODB_DEFAULT_MAX_PARAMETERS, TURBODB_DEFAULT_MAX_COLUMNS,
      TURBODB_DEFAULT_MAX_QUERY_BYTES, TURBODB_DEFAULT_MAX_PARAMETER_BYTES,
      TURBODB_DEFAULT_MAX_RESULT_ROWS, TURBODB_DEFAULT_MAX_RESULT_BYTES};
  return limits;
}

typedef struct tdsql_config {
  uint32_t struct_size, abi_version;
  const turbodb_option_t *options;
  size_t option_count;
  tdsql_limits limits;
} tdsql_config;

static inline tdsql_config tdsql_config_default(void) {
  tdsql_config config = {0};
  config.struct_size = sizeof(config); config.abi_version = TDSQL_ABI_VERSION;
  config.limits = tdsql_limits_default(); return config;
}

/* One synchronous borrowed request. read_parameter is called once per admitted
 * parameter; context and all value bytes stay immutable until the call returns.
 * The reader cannot reenter this connection, throw or longjmp across the call.
 * Query execution copies its retained parameters before returning. SQL is MySQL
 * with NO_BACKSLASH_ESCAPES. No ORM plan, renderer or publisher crosses here.
 * Consumer context is a fixed zeroed tail with Salts native scalar alignment;
 * its checked size and the result itself share the statement WORK budget.
 * unique_column_names selects an additional row-map constraint after binding;
 * it does not relax the execution profile's existing alias/name validation. */
typedef struct tdsql_request {
  uint32_t struct_size, abi_version;
  vstr sql;
  size_t parameter_count;
  turbodb_value_t (TDSQL_CALL *read_parameter)(const void *context, size_t index);
  const void *parameter_context;
  tdsql_limits limits;
  size_t result_context_bytes;
  bool unique_column_names;
} tdsql_request;

static inline tdsql_request tdsql_request_default(turbodb_string_view_t sql) {
  tdsql_request request = {0};
  request.struct_size = sizeof(request); request.abi_version = TDSQL_ABI_VERSION;
  request.sql = sql; request.limits = tdsql_limits_default(); return request;
}

typedef int32_t tdsql_row_state;
enum { TDSQL_ROW = 1, TDSQL_DONE = 2, TDSQL_CANCELLED = 3 };
typedef struct tdsql_row {
  tdsql_row_state state;
  const turbodb_value_t *values;
  size_t count;
} tdsql_row;
typedef struct tdsql_column {
  turbodb_string_view_t name;
  turbodb_value_kind_t kind;
  bool nullable;
} tdsql_column;

typedef int32_t tdsql_response_kind;
enum { TDSQL_COMMAND = 1, TDSQL_ROWS = 2 };
typedef struct tdsql_response {
  uint32_t struct_size, abi_version;
  tdsql_response_kind kind;
  uint64_t affected_rows, last_insert_id;
  tdsql_result *result;
} tdsql_response;

static inline tdsql_response tdsql_response_default(void) {
  tdsql_response response = {0};
  response.struct_size = sizeof(response); response.abi_version = TDSQL_ABI_VERSION;
  return response;
}

/* Typed values borrow execute only; no SQL or parameter callback is retained.
 * Query execution copies payload bytes. Initialize with bindings_default(). */
typedef struct tdsql_bindings {
  uint32_t struct_size, abi_version;
  const turbodb_value_t *values;
  size_t count, result_context_bytes;
  bool unique_column_names;
} tdsql_bindings;

static inline tdsql_bindings tdsql_bindings_default(const turbodb_value_t *values, size_t count) {
  tdsql_bindings bindings = {0};
  bindings.struct_size = sizeof(bindings); bindings.abi_version = TDSQL_ABI_VERSION;
  bindings.values = values; bindings.count = count; return bindings;
}

typedef struct tdsql_session_state {
  uint32_t struct_size, abi_version;
  bool autocommit, in_transaction, transaction_read_only, session_read_only;
  bool busy, rollback_required;
  uint64_t warning_count;
  turbodb_status_t failure;
} tdsql_session_state;

static inline tdsql_session_state tdsql_session_state_default(void) {
  tdsql_session_state state = {0};
  state.struct_size = sizeof(state); state.abi_version = TDSQL_ABI_VERSION;
  return state;
}

/* Static C SDK v1. Initialize config/request with the provided defaults. ABI
 * version mismatch or undersized inputs fail before consuming their fields.
 * Single synchronous owner. Existing
 * relational option names/bounds apply; config/options borrow this call only.
 * open success owns a connection; failure empties out. close needs quiescence,
 * returns BUSY while a result/statement/external transaction remains, and retains the
 * connection on failure. SQL session rollback failure also retains it.
 * Callers release results/statement/transaction handles before closing the connection. */
turbodb_status_t TDSQL_CALL tdsql_connection_open(const tdsql_config *config,
    tdsql_connection **out, turbodb_error_t *error);
turbodb_status_t TDSQL_CALL tdsql_connection_close(tdsql_connection *connection, turbodb_error_t *error);

/* Shared database API, added in SDK 1.1 without changing v1 DTO layouts.
 * open borrows config only for the call; success owns one native database/CF.
 * connect creates an independent SQL session with the database's fixed settings;
 * it never reopens storage or repeats sql_initialize. sql_max_connections bounds
 * admission (default 128); full returns LIMIT_EXCEEDED and clears out.
 * All database/session/result/transaction operations sharing this database must
 * be serialized on one owner thread, including callbacks and open/close.
 * close returns BUSY and retains the database while any session exists; otherwise
 * it consumes the database, including on native close error. NULL is a no-op.
 * Close results/statements/transactions, then sessions, then the database. Session close
 * rolls back its SQL transaction and preserves the existing close contract.
 * Existing connection_open owns a private database and remains unchanged.
 * Example: tdsql_database_open(&config,&db,&error), followed by
 * tdsql_database_connect(db,&session,&error); after requests, close session/db.
 */
turbodb_status_t TDSQL_CALL tdsql_database_open(const tdsql_config *config,
    tdsql_database **out, turbodb_error_t *error);
turbodb_status_t TDSQL_CALL tdsql_database_connect(tdsql_database *database,
    tdsql_connection **out, turbodb_error_t *error);
turbodb_status_t TDSQL_CALL tdsql_database_close(tdsql_database *database,
    turbodb_error_t *error);
turbodb_status_t TDSQL_CALL tdsql_connection_execute(tdsql_connection *connection,
    const tdsql_request *request, uint64_t *affected, turbodb_error_t *error);
turbodb_status_t TDSQL_CALL tdsql_connection_query(tdsql_connection *connection,
    const tdsql_request *request, tdsql_result **out, turbodb_error_t *error);

/* Unified SQL execution, added in SDK 1.2. Uses one parsed AST to select command
 * or rows; no trial execution, fallback, interpolation or automatic replay.
 * request follows execute/query's limits and lifetime. Initialize out with
 * response_default and release its previous result before reuse. On success,
 * COMMAND has affected_rows and no result; ROWS owns a result to be destroyed
 * with result_destroy_checked, including after EOF/cancel. last_insert_id is 0
 * for this profile: AUTO_INCREMENT/LAST_INSERT_ID() are unsupported. Failure
 * preserves the complete response; SQL/cleanup failure may still change the
 * connection's transaction state as documented for execute/query.
 * Example: response=response_default(); connection_run(c,&request,&response,&e);
 * after a successful ROWS response, consume and destroy response.result.
 */
turbodb_status_t TDSQL_CALL tdsql_connection_run(tdsql_connection *connection,
    const tdsql_request *request, tdsql_response *out, turbodb_error_t *error);

/* Prepared lifecycle, SDK 1.3; existing ABI v1 DTO layouts remain unchanged.
 * Prepare accepts request SQL/limits only: parameter_count must be zero and
 * parameter reader/context absent. It copies SQL, inferred types/column names
 * and schema metadata without evaluating expressions, reading business rows,
 * writing, consuming next transaction access mode or resetting diagnostics.
 * Supports single-table SELECT, INSERT/REPLACE VALUES/SET, UPDATE and DELETE;
 * marker-bearing scalar expressions currently support arithmetic/comparisons
 * and numeric CAST. Also supports the engine's zero-parameter CREATE/ALTER/DROP/
 * TRUNCATE TABLE and CREATE/DROP INDEX subset. DEFAULT only compiles at prepare;
 * value conversion/range/nullability and physical data checks occur at execute.
 * DDL copies SQL only and binds current metadata on every execute. It does not
 * cache schema or invalidate itself after changing the Catalog. Unsupported
 * types/constraints/dynamic defaults and all DDL markers reject at prepare.
 * JOIN/CTE/subqueries/grouping/windows/SHOW/EXPLAIN explicitly reject.
 * Outside SQL transactions metadata uses a temporary rolled-back Catalog view;
 * inside one it uses that transaction's snapshot. No native lease survives.
 * sql_max_prepared_statements (64) and sql_max_prepared_bytes (16 MiB) bound
 * per-session retained owners/SQL/schema/types/names; full returns LIMIT_EXCEEDED.
 * Prepare failure clears out. Owned statement keeps its connection alive.
 * Execute reparses owned SQL and, for DML, checks schema in the execution snapshot before
 * loading values or business rows/writes. Changed/recreated/missing tables latch
 * INVALID_STATE until close/new prepare; RESET cannot restore them. Ordinary
 * statement errors leave the descriptor usable. No automatic reprepare/replay.
 * Parameter names are "?"; metadata names borrow statement until close. Initial
 * column types are inferred, whereas result_column after each execute is the
 * authoritative actual typed metadata. No implicit MySQL conversion guarantee.
 * Exact bindings count required; response follows connection_run's contract.
 * A live result (including EOF/cancel) makes execute/reset/close BUSY: destroy
 * it first. RESET checks quiescence/validity and preserves SQL/metadata/transaction;
 * this profile retains no bindings, long-data buffers or server cursors.
 * Close consumes even on cleanup error and quarantines the connection; NULL is
 * a no-op. Same serialized owner-thread rule as the connection, no reentrancy.
 * Executable usage with complete checked setup/cleanup is in the public SDK
 * tests: tests/integration/prepared_test.c and tests/api/sdk_cpp_test.cpp.
 * Destroy response.result, then close the statement before its connection.
 */
turbodb_status_t TDSQL_CALL tdsql_connection_statement_prepare(tdsql_connection *connection,
    const tdsql_request *request, tdsql_statement **out, turbodb_error_t *error);
size_t TDSQL_CALL tdsql_statement_parameters(const tdsql_statement *statement);
turbodb_status_t TDSQL_CALL tdsql_statement_parameter(const tdsql_statement *statement,
    size_t index, tdsql_column *out, turbodb_error_t *error);
size_t TDSQL_CALL tdsql_statement_columns(const tdsql_statement *statement);
turbodb_status_t TDSQL_CALL tdsql_statement_column(const tdsql_statement *statement,
    size_t index, tdsql_column *out, turbodb_error_t *error);
turbodb_status_t TDSQL_CALL tdsql_statement_execute(tdsql_statement *statement,
    const tdsql_bindings *bindings, tdsql_response *out, turbodb_error_t *error);
turbodb_status_t TDSQL_CALL tdsql_statement_reset(tdsql_statement *statement, turbodb_error_t *error);
turbodb_status_t TDSQL_CALL tdsql_statement_close(tdsql_statement *statement, turbodb_error_t *error);

/* Read-only by-value state; initialize out with session_state_default(). No
 * SQL, allocation, diagnostic reset, native transaction or state transition.
 * May observe BUSY/quarantined connections on the same owner thread. busy means
 * connection dispatch is blocked by a result or external transaction. in_transaction
 * covers SQL/API multi-statement owners, excluding autocommit query snapshots;
 * transaction_read_only describes that active owner, session_read_only its default.
 * warning_count is the count so far; result consumption can add warnings. EOF
 * does not end BUSY: destroy first, then read final state for protocol completion.
 * Invalid arguments/output ABI return errors with out unchanged. failure reports
 * the latched connection error even when the snapshot itself succeeds.
 */
turbodb_status_t TDSQL_CALL tdsql_connection_state(const tdsql_connection *connection,
    tdsql_session_state *out, turbodb_error_t *error);
/* Fixed native SERIALIZABLE isolation; consumes next/session access mode. */
turbodb_status_t TDSQL_CALL tdsql_connection_begin(tdsql_connection *connection,
    tdsql_transaction **out, turbodb_error_t *error);
/* Adapter admission before producing SQL. No native transaction is opened;
 * commands reset diagnostics as before rendering. Dispatch checks again. */
turbodb_status_t TDSQL_CALL tdsql_connection_prepare(tdsql_connection *connection,
    bool command, turbodb_error_t *error);
turbodb_status_t TDSQL_CALL tdsql_transaction_prepare(tdsql_transaction *transaction,
    bool command, turbodb_error_t *error);

typedef int32_t tdsql_savepoint_operation;
enum {
  TDSQL_SAVEPOINT_CREATE, TDSQL_SAVEPOINT_ROLLBACK, TDSQL_SAVEPOINT_RELEASE
};

/* A transaction owns one native Catalog snapshot. finish never releases the
 * handle; release consumes it. Results retain that handle through destroy.
 * Failed statements preserve affected; cleanup failure requires full rollback.
 * COMMIT_UNKNOWN is terminal and quarantines the connection; never replay it. */
turbodb_status_t TDSQL_CALL tdsql_transaction_execute(tdsql_transaction *transaction,
    const tdsql_request *request, uint64_t *affected, turbodb_error_t *error);
turbodb_status_t TDSQL_CALL tdsql_transaction_query(tdsql_transaction *transaction,
    const tdsql_request *request, tdsql_result **out, turbodb_error_t *error);
turbodb_status_t TDSQL_CALL tdsql_transaction_finish(tdsql_transaction *transaction,
    bool commit, turbodb_error_t *error);
turbodb_status_t TDSQL_CALL tdsql_transaction_savepoint(tdsql_transaction *transaction,
    turbodb_string_view_t name, tdsql_savepoint_operation operation, turbodb_error_t *error);
/* Checked release consumes the handle/reference even on cleanup failure;
 * the connection is quarantined in that case. NULL is a successful no-op. */
turbodb_status_t TDSQL_CALL tdsql_transaction_release_checked(tdsql_transaction *transaction,
    turbodb_error_t *error);
void TDSQL_CALL tdsql_transaction_release(tdsql_transaction *transaction);

/* Query failure empties out; success returns an owned stable result. Row values
 * and column names borrow it, invalidated by next/cancel/destroy. EOF and cancel
 * do not release the active statement: destroy is still required. Result cleanup
 * failures quarantine its connection. No concurrent calls or retained row views.
 * context is valid until destroy, including during CSerde decoding in the ORM
 * adapter; it must not be freed separately. next preserves out on failure. */
turbodb_status_t TDSQL_CALL tdsql_result_next(tdsql_result *result, tdsql_row *out,
    turbodb_error_t *error);
turbodb_status_t TDSQL_CALL tdsql_result_column(tdsql_result *result, size_t index,
    tdsql_column *out, turbodb_error_t *error);
size_t TDSQL_CALL tdsql_result_columns(const tdsql_result *result);
void *TDSQL_CALL tdsql_result_context(tdsql_result *result);
turbodb_status_t TDSQL_CALL tdsql_result_cancel(tdsql_result *result, turbodb_error_t *error);
/* Checked destroy always consumes the result; failure quarantines its connection.
 * Prefer it when the caller can report a close error. NULL is a successful no-op. */
turbodb_status_t TDSQL_CALL tdsql_result_destroy_checked(tdsql_result *result, turbodb_error_t *error);
void TDSQL_CALL tdsql_result_destroy(tdsql_result *result);

#ifdef __cplusplus
}
#endif

#endif

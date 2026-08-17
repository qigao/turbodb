#ifndef ORM_H
#define ORM_H

/*
 * QueryDSL-inspired ORM API for C11.
 *
 * Exported functions form the stable C ABI. orm_chain_t and orm_expression_t
 * are source-only DSL values: keep them inside one consuming module and pass
 * database resources only through the opaque connection/query/result handles.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && !defined(NOMINMAX)
  #define NOMINMAX
#endif

#include <fmt.h>
#include <tlog.h>
#include <turbo_str.h>

#if defined(ORM_C_STATIC)
  #define ORM_C_API
  #define ORM_C_CALL
#elif defined(_WIN32)
  #if defined(ORM_C_BUILD)
    #define ORM_C_API __declspec(dllexport)
  #else
    #define ORM_C_API __declspec(dllimport)
  #endif
  #define ORM_C_CALL __cdecl
#elif defined(ORM_C_BUILD) && (defined(__GNUC__) || defined(__clang__))
  #define ORM_C_API __attribute__((visibility("default")))
  #define ORM_C_CALL
#else
  #define ORM_C_API
  #define ORM_C_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define ORM_C_ABI_VERSION UINT32_C(2)
#define ORM_C_ERROR_MESSAGE_CAPACITY UINT32_C(512)
#define ORM_C_DEFAULT_MAX_PARAMETERS UINT32_C(256)
#define ORM_C_DEFAULT_MAX_COLUMNS UINT32_C(256)
#define ORM_C_DEFAULT_MAX_PREDICATES UINT32_C(256)
#define ORM_C_DEFAULT_MAX_QUERY_BYTES UINT64_C(65536)
#define ORM_C_DEFAULT_MAX_PARAMETER_BYTES UINT64_C(1048576)
#define ORM_C_DEFAULT_MAX_RESULT_ROWS UINT64_C(10000)
#define ORM_C_DEFAULT_MAX_RESULT_BYTES UINT64_C(16777216)
#define ORM_C_DEFAULT_MAX_JOINS UINT32_C(32)
#define ORM_C_DEFAULT_MAX_GROUP_COLUMNS UINT32_C(64)
#define ORM_C_DEFAULT_MAX_ASSIGNMENTS UINT32_C(256)
#define ORM_C_DEFAULT_MAX_CONDITION_DEPTH UINT32_C(16)

typedef int32_t orm_status_t;
enum {
  ORM_STATUS_OK = 0,
  ORM_STATUS_INVALID_ARGUMENT = 1,
  ORM_STATUS_ABI_MISMATCH = 2,
  ORM_STATUS_OUT_OF_MEMORY = 3,
  ORM_STATUS_CONNECTION_ERROR = 4,
  ORM_STATUS_SQL_ERROR = 5,
  ORM_STATUS_TYPE_ERROR = 6,
  ORM_STATUS_OUT_OF_RANGE = 7,
  ORM_STATUS_LIMIT_EXCEEDED = 8,
  ORM_STATUS_INVALID_STATE = 9,
  ORM_STATUS_NULL_VALUE = 10,
  ORM_STATUS_INTERNAL_ERROR = 11,
  ORM_STATUS_BUSY = 12,
  ORM_STATUS_UNSUPPORTED = 13,
  ORM_STATUS_DATASTORE_ERROR = 14
};

typedef int32_t orm_value_kind_t;
enum {
  ORM_VALUE_NULL = 0,
  ORM_VALUE_INT64 = 1,
  ORM_VALUE_UINT64 = 2,
  ORM_VALUE_DOUBLE = 3,
  ORM_VALUE_BOOLEAN = 4,
  ORM_VALUE_TEXT = 5,
  ORM_VALUE_BLOB = 6
};

typedef int32_t orm_compare_t;
enum {
  ORM_COMPARE_EQUAL = 0,
  ORM_COMPARE_NOT_EQUAL = 1,
  ORM_COMPARE_LESS = 2,
  ORM_COMPARE_LESS_EQUAL = 3,
  ORM_COMPARE_GREATER = 4,
  ORM_COMPARE_GREATER_EQUAL = 5,
  ORM_COMPARE_LIKE = 6,
  ORM_COMPARE_NOT_LIKE = 7
};

typedef int32_t orm_order_t;
enum { ORM_ORDER_ASCENDING = 0, ORM_ORDER_DESCENDING = 1 };

typedef int32_t orm_logic_t;
enum { ORM_LOGIC_AND = 0, ORM_LOGIC_OR = 1 };

typedef int32_t orm_join_t;
enum { ORM_JOIN_INNER = 0, ORM_JOIN_LEFT = 1 };

typedef int32_t orm_aggregate_t;
enum {
  ORM_AGGREGATE_COUNT_ALL = 0,
  ORM_AGGREGATE_COUNT = 1,
  ORM_AGGREGATE_SUM = 2,
  ORM_AGGREGATE_AVERAGE = 3,
  ORM_AGGREGATE_MINIMUM = 4,
  ORM_AGGREGATE_MAXIMUM = 5
};

typedef int32_t orm_isolation_t;
enum {
  ORM_ISOLATION_READ_UNCOMMITTED = 0,
  ORM_ISOLATION_READ_COMMITTED = 1,
  ORM_ISOLATION_REPEATABLE_READ = 2,
  ORM_ISOLATION_SNAPSHOT = 3,
  ORM_ISOLATION_SERIALIZABLE = 4
};

typedef struct orm_connection orm_connection_t;
typedef struct orm_query orm_query_t;
typedef struct orm_result orm_result_t;
typedef struct orm_transaction orm_transaction_t;

/* ABI-compatible alias for TurboUtils' borrowed, non-owning string view. */
typedef tstr_v orm_string_view_t;

/* Borrowed, non-owning binary view. */
typedef struct orm_blob {
  const void *data;
  size_t size;
} orm_blob_t;

typedef struct orm_option {
  orm_string_view_t keyword;
  orm_string_view_t value;
} orm_option_t;

typedef struct orm_config {
  uint32_t struct_size;
  uint32_t abi_version;
  /* Runtime driver identifier. */
  orm_string_view_t driver;
  const orm_option_t *options;
  uint32_t option_count;
  uint32_t max_parameters;
  uint32_t max_columns;
  uint32_t max_predicates;
  uint32_t max_joins;
  uint32_t max_group_columns;
  uint32_t max_assignments;
  uint32_t max_condition_depth;
  uint64_t max_query_bytes;
  /* Total copied parameter payload owned by one query. */
  uint64_t max_parameter_bytes;
  uint64_t max_result_rows;
  uint64_t max_result_bytes;
} orm_config_t;

typedef struct orm_error {
  uint32_t struct_size;
  orm_status_t status;
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
} orm_error_t;

typedef union orm_value_data {
  int64_t int64_value;
  uint64_t uint64_value;
  double double_value;
  uint8_t boolean_value;
  orm_string_view_t text_value;
  orm_blob_t blob_value;
} orm_value_data_t;

typedef struct orm_value {
  orm_value_kind_t kind;
  uint32_t reserved;
  orm_value_data_t data;
} orm_value_t;

/*
 * Unless documented otherwise, functions returning orm_status_t report
 * ORM_STATUS_OK on success and leave owned outputs with the caller. On failure,
 * output handles are null, and error receives the status and diagnostic text
 * when it was initialized with orm_error_init(). Builder calls can also return
 * INVALID_STATE for an incompatible query kind, LIMIT_EXCEEDED for a configured
 * bound, UNSUPPORTED when a backend cannot preserve the requested semantics,
 * or INVALID_ARGUMENT for malformed identifiers and enum values.
 */

ORM_C_API uint32_t ORM_C_CALL orm_c_abi_version(void);
ORM_C_API const char *ORM_C_CALL orm_status_message(orm_status_t status);

ORM_C_API void ORM_C_CALL orm_error_init(orm_error_t *error);

ORM_C_API void ORM_C_CALL orm_config(orm_config_t *config);

/*
 * Driver and option strings are borrowed only for this call. The returned
 * connection is owned by the caller. Queries retain the underlying connection,
 * so disconnecting this handle does not invalidate an existing query. A
 * connection, its queries, and its transactions form one single-thread domain.
 *
 * The optional "redis" driver uses Redis hashes plus Query Engine indexes.
 * Its connect/query calls must run inside one active CoroNet coroutine. It
 * fails with ORM_STATUS_UNSUPPORTED for operations whose SQL semantics cannot
 * be preserved; it never falls back to a key scan.
 *
 * The optional "tidesdb" driver stores versioned, length-delimited rows in an
 * embedded TidesDB column family. INSERT and id-based UPDATE/DELETE are
 * transactional. SELECT and aggregate queries use a bounded snapshot prefix
 * scan; joins and raw SQL are unsupported.
 *
 * The optional "mongo" driver maps tables to collections in a MongoDB
 * database and stores the configured id_column under MongoDB's reserved _id
 * field, so duplicate inserts fail and id lookups stay indexed. It requires
 * the "database" option; "uri" (default mongodb://127.0.0.1:27017) and
 * "id_column" (default "id") are optional. UPDATE/DELETE require an id
 * equality predicate and accept additional conjunctive equality predicates.
 * SELECT and aggregate queries are pushed down as find
 * filters and aggregation pipelines. Explicit transactions map to MongoDB
 * sessions and require a replica set or sharded cluster;
 * savepoints, joins, and raw SQL are unsupported.
 */
ORM_C_API orm_status_t ORM_C_CALL orm_connect(const orm_config_t *config,
                                              orm_connection_t **out_connection,
                                              orm_error_t *error);

ORM_C_API void ORM_C_CALL orm_disconnect(orm_connection_t *connection);

/*
 * An explicit transaction retains its connection backend. It and all queries
 * executed through it remain in the connection's single-thread domain.
 * Destroying an active transaction performs a best-effort rollback. Commit
 * failure is terminal: the handle may only be destroyed afterwards.
 *
 * PostgreSQL maps READ_UNCOMMITTED to READ_COMMITTED and SNAPSHOT to its
 * REPEATABLE READ snapshot isolation. SQLite satisfies every requested level
 * with its stronger serializable transaction. Redis supports SERIALIZABLE
 * only; its MULTI/EXEC results are deferred until commit and it does not
 * support savepoints. TidesDB supports all five native v9 isolation levels.
 */
ORM_C_API orm_status_t ORM_C_CALL orm_transaction_begin(
    orm_connection_t *connection, orm_isolation_t isolation,
    orm_transaction_t **out_transaction, orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_transaction_commit(
    orm_transaction_t *transaction, orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_transaction_rollback(
    orm_transaction_t *transaction, orm_error_t *error);

/*
 * Savepoint names are copied by each call. PostgreSQL and SQLite use native
 * SQL savepoints. On TidesDB, rollback-to removes the named savepoint and every
 * savepoint created after it; it must not then be released. Redis returns
 * ORM_STATUS_UNSUPPORTED. Releasing a savepoint preserves its writes.
 */
ORM_C_API orm_status_t ORM_C_CALL orm_transaction_savepoint(
    orm_transaction_t *transaction, orm_string_view_t name, orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_transaction_rollback_to_savepoint(
    orm_transaction_t *transaction, orm_string_view_t name, orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_transaction_release_savepoint(
    orm_transaction_t *transaction, orm_string_view_t name, orm_error_t *error);

ORM_C_API void ORM_C_CALL orm_transaction_destroy(orm_transaction_t *transaction);

/* Creates SELECT. Table and column names are portable ASCII SQL identifiers. */
ORM_C_API orm_status_t ORM_C_CALL orm_query_create(orm_connection_t *connection,
                                                   orm_string_view_t table, orm_query_t **out_query,
                                                   orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_insert(orm_connection_t *connection, orm_string_view_t table,
                                             orm_query_t **out_query, orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_update(orm_connection_t *connection, orm_string_view_t table,
                                             orm_query_t **out_query, orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_delete(orm_connection_t *connection, orm_string_view_t table,
                                             orm_query_t **out_query, orm_error_t *error);

/* Raw SQL is trusted, driver-native, single-statement text copied by this call. */
ORM_C_API orm_status_t ORM_C_CALL orm_raw(orm_connection_t *connection, orm_string_view_t sql,
                                          orm_query_t **out_query, orm_error_t *error);

ORM_C_API void ORM_C_CALL orm_query_destroy(orm_query_t *query);

ORM_C_API orm_status_t ORM_C_CALL orm_query_select_all(orm_query_t *query, orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_query_add_column(orm_query_t *query, orm_string_view_t column,
                                                       orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_query_add_aggregate(orm_query_t *query,
                                                          orm_aggregate_t aggregate,
                                                          orm_string_view_t column,
                                                          orm_string_view_t alias,
                                                          orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_query_set(orm_query_t *query, orm_string_view_t column,
                                                orm_value_t value, orm_error_t *error);

/*
 * A predicate is appended to the current group. Root groups use AND; a nested
 * group's logic controls how its direct children are combined. Text is copied.
 */
ORM_C_API orm_status_t ORM_C_CALL orm_query_where(orm_query_t *query, orm_string_view_t column,
                                                  orm_compare_t comparison, orm_value_t value,
                                                  orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_query_begin_where_group(orm_query_t *query, orm_logic_t logic,
                                                              orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_query_end_where_group(orm_query_t *query, orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_query_join(
    orm_query_t *query, orm_join_t join, orm_string_view_t table, orm_string_view_t left_column,
    orm_compare_t comparison, orm_string_view_t right_column, orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_query_group_by(orm_query_t *query, orm_string_view_t column,
                                                     orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_query_having(orm_query_t *query, orm_string_view_t column,
                                                   orm_compare_t comparison, orm_value_t value,
                                                   orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_query_having_aggregate(orm_query_t *query,
                                                             orm_aggregate_t aggregate,
                                                             orm_string_view_t column,
                                                             orm_compare_t comparison,
                                                             orm_value_t value, orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_query_begin_having_group(orm_query_t *query,
                                                               orm_logic_t logic,
                                                               orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_query_end_having_group(orm_query_t *query,
                                                             orm_error_t *error);

/* Appends one value for the next driver-native placeholder in a raw query. */
ORM_C_API orm_status_t ORM_C_CALL orm_query_bind(orm_query_t *query, orm_value_t value,
                                                 orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_query_order_by(orm_query_t *query, orm_string_view_t column,
                                                     orm_order_t order, orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_query_set_limit(orm_query_t *query, uint64_t limit,
                                                      orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_query_set_offset(orm_query_t *query, uint64_t offset,
                                                       orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_query_execute(orm_query_t *query, orm_result_t **out_result,
                                                    orm_error_t *error);

/*
 * The query and transaction must share a connection. PostgreSQL, SQLite, and
 * TidesDB results are readable immediately. Redis returns an owned deferred
 * result: all result accessors return ORM_STATUS_INVALID_STATE until EXEC
 * succeeds, and remain unavailable after rollback or a failed queued command.
 */
ORM_C_API orm_status_t ORM_C_CALL orm_query_execute_in_transaction(
    orm_query_t *query, orm_transaction_t *transaction,
    orm_result_t **out_result, orm_error_t *error);

ORM_C_API void ORM_C_CALL orm_result_destroy(orm_result_t *result);

ORM_C_API orm_status_t ORM_C_CALL orm_result_row_count(const orm_result_t *result,
                                                       uint64_t *out_count, orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_result_column_count(const orm_result_t *result,
                                                          uint64_t *out_count, orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_result_affected_rows(const orm_result_t *result,
                                                           uint64_t *out_count, orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_result_is_null(const orm_result_t *result, uint64_t row,
                                                     uint64_t column, uint8_t *out_is_null,
                                                     orm_error_t *error);

/* The returned view is borrowed and expires when result is destroyed. */
ORM_C_API orm_status_t ORM_C_CALL orm_result_get_text(const orm_result_t *result, uint64_t row,
                                                      uint64_t column, orm_string_view_t *out_value,
                                                      orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_result_get_blob(const orm_result_t *result, uint64_t row,
                                                      uint64_t column, orm_blob_t *out_value,
                                                      orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_result_get_int64(const orm_result_t *result, uint64_t row,
                                                       uint64_t column, int64_t *out_value,
                                                       orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_result_get_uint64(const orm_result_t *result, uint64_t row,
                                                        uint64_t column, uint64_t *out_value,
                                                        orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_result_get_double(const orm_result_t *result, uint64_t row,
                                                        uint64_t column, double *out_value,
                                                        orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL orm_result_get_boolean(const orm_result_t *result, uint64_t row,
                                                         uint64_t column, uint8_t *out_value,
                                                         orm_error_t *error);

/*
 * QueryDSL-inspired C11 condition expressions. They are bounded values with no heap
 * ownership and must remain local to one consuming module. Column and text
 * views are borrowed until where_expr/having_expr returns; the query copies
 * them before that call completes. Override the capacity before including this
 * header when a larger local expression is required.
 */
#ifndef ORM_C_EXPRESSION_CAPACITY
  #define ORM_C_EXPRESSION_CAPACITY 64
#endif

#if ORM_C_EXPRESSION_CAPACITY < 4
  #error "ORM_C_EXPRESSION_CAPACITY must be at least 4"
#endif

typedef int32_t orm_expression_token_kind_t;
enum {
  ORM_EXPRESSION_PREDICATE = 0,
  ORM_EXPRESSION_AGGREGATE_PREDICATE = 1,
  ORM_EXPRESSION_GROUP_BEGIN = 2,
  ORM_EXPRESSION_GROUP_END = 3
};

typedef struct orm_expression_token {
  orm_expression_token_kind_t kind;
  orm_logic_t logic;
  orm_aggregate_t aggregate;
  orm_string_view_t column;
  orm_compare_t comparison;
  orm_value_t value;
} orm_expression_token_t;

typedef struct orm_expression {
  orm_status_t status;
  uint32_t token_count;
  orm_expression_token_t tokens[ORM_C_EXPRESSION_CAPACITY];
} orm_expression_t;

static inline orm_string_view_t orm_view(const char *text) {
  return tstr_v_from_cstr(text);
}

static inline orm_string_view_t orm_view_tstr(tstr_t text) { return tstr_to_v(text); }

static inline orm_value_t orm_null(void) {
  orm_value_t value;
  value.kind = ORM_VALUE_NULL;
  value.reserved = 0;
  value.data.uint64_value = 0;
  return value;
}

static inline orm_value_t orm_i64(int64_t input) {
  orm_value_t value;
  value.kind = ORM_VALUE_INT64;
  value.reserved = 0;
  value.data.int64_value = input;
  return value;
}

static inline orm_value_t orm_u64(uint64_t input) {
  orm_value_t value;
  value.kind = ORM_VALUE_UINT64;
  value.reserved = 0;
  value.data.uint64_value = input;
  return value;
}

static inline orm_value_t orm_f64(double input) {
  orm_value_t value;
  value.kind = ORM_VALUE_DOUBLE;
  value.reserved = 0;
  value.data.double_value = input;
  return value;
}

static inline orm_value_t orm_bool(int input) {
  orm_value_t value;
  value.kind = ORM_VALUE_BOOLEAN;
  value.reserved = 0;
  value.data.boolean_value = (uint8_t)(input != 0);
  return value;
}

static inline orm_value_t orm_text(const char *input) {
  orm_value_t value;
  value.kind = ORM_VALUE_TEXT;
  value.reserved = 0;
  value.data.text_value = orm_view(input);
  return value;
}

static inline orm_value_t orm_text_v(tstr_v input) {
  orm_value_t value;
  value.kind = ORM_VALUE_TEXT;
  value.reserved = 0;
  value.data.text_value = input;
  return value;
}

static inline orm_value_t orm_text_tstr(tstr_t input) { return orm_text_v(tstr_to_v(input)); }

static inline orm_value_t orm_blob_v(orm_blob_t input) {
  orm_value_t value;
  value.kind = ORM_VALUE_BLOB;
  value.reserved = 0;
  value.data.blob_value = input;
  return value;
}

static inline orm_value_t orm_blob(const void *data, size_t size) {
  orm_blob_t blob;
  blob.data = data;
  blob.size = size;
  return orm_blob_v(blob);
}

static inline orm_expression_token_t orm_expression_detail_token(orm_expression_token_kind_t kind) {
  orm_expression_token_t token;
  token.kind = kind;
  token.logic = ORM_LOGIC_AND;
  token.aggregate = ORM_AGGREGATE_COUNT_ALL;
  token.column = orm_view(NULL);
  token.comparison = ORM_COMPARE_EQUAL;
  token.value = orm_null();
  return token;
}

static inline orm_expression_t orm_expression_detail_error(orm_status_t status) {
  orm_expression_t expression;
  orm_expression_token_t token = orm_expression_detail_token(ORM_EXPRESSION_PREDICATE);
  uint32_t index;
  expression.status = status;
  expression.token_count = 0;
  for (index = 0; index < ORM_C_EXPRESSION_CAPACITY; ++index)
    expression.tokens[index] = token;
  return expression;
}

static inline orm_expression_t orm_expression_compare(orm_string_view_t column,
                                                      orm_compare_t comparison, orm_value_t value) {
  orm_expression_t expression = orm_expression_detail_error(ORM_STATUS_OK);
  orm_expression_token_t token = orm_expression_detail_token(ORM_EXPRESSION_PREDICATE);
  token.column = column;
  token.comparison = comparison;
  token.value = value;
  expression.status = ORM_STATUS_OK;
  expression.token_count = 1;
  expression.tokens[0] = token;
  return expression;
}

static inline orm_expression_t orm_expression_aggregate(orm_aggregate_t aggregate,
                                                        orm_string_view_t column,
                                                        orm_compare_t comparison,
                                                        orm_value_t value) {
  orm_expression_t expression = orm_expression_detail_error(ORM_STATUS_OK);
  orm_expression_token_t token = orm_expression_detail_token(ORM_EXPRESSION_AGGREGATE_PREDICATE);
  token.aggregate = aggregate;
  token.column = column;
  token.comparison = comparison;
  token.value = value;
  expression.status = ORM_STATUS_OK;
  expression.token_count = 1;
  expression.tokens[0] = token;
  return expression;
}

static inline int orm_expression_detail_is_root_group(const orm_expression_t *expression,
                                                      orm_logic_t logic) {
  return expression->token_count >= 4 && expression->tokens[0].kind == ORM_EXPRESSION_GROUP_BEGIN &&
         expression->tokens[0].logic == logic &&
         expression->tokens[expression->token_count - 1].kind == ORM_EXPRESSION_GROUP_END;
}

static inline orm_expression_t orm_expression_combine(orm_logic_t logic, orm_expression_t left,
                                                      orm_expression_t right) {
  orm_expression_t output;
  uint32_t output_index = 0;
  uint32_t left_begin = 0;
  uint32_t left_end = left.token_count;
  uint32_t right_begin = 0;
  uint32_t right_end = right.token_count;
  uint32_t index;

  if (left.status != ORM_STATUS_OK) return left;
  if (right.status != ORM_STATUS_OK) return right;
  if (left.token_count == 0 || right.token_count == 0 ||
      left.token_count > ORM_C_EXPRESSION_CAPACITY || right.token_count > ORM_C_EXPRESSION_CAPACITY)
    return orm_expression_detail_error(ORM_STATUS_INVALID_ARGUMENT);

  if (orm_expression_detail_is_root_group(&left, logic)) {
    left_begin = 1;
    left_end -= 1;
  }
  if (orm_expression_detail_is_root_group(&right, logic)) {
    right_begin = 1;
    right_end -= 1;
  }
  if ((left_end - left_begin) > ORM_C_EXPRESSION_CAPACITY - 2 ||
      (right_end - right_begin) > ORM_C_EXPRESSION_CAPACITY - 2 - (left_end - left_begin))
    return orm_expression_detail_error(ORM_STATUS_LIMIT_EXCEEDED);

  output = orm_expression_detail_error(ORM_STATUS_OK);
  output.tokens[output_index] = orm_expression_detail_token(ORM_EXPRESSION_GROUP_BEGIN);
  output.tokens[output_index].logic = logic;
  ++output_index;
  for (index = left_begin; index < left_end; ++index)
    output.tokens[output_index++] = left.tokens[index];
  for (index = right_begin; index < right_end; ++index)
    output.tokens[output_index++] = right.tokens[index];
  output.tokens[output_index++] = orm_expression_detail_token(ORM_EXPRESSION_GROUP_END);
  output.token_count = output_index;
  return output;
}

static inline orm_expression_t orm_expression_and(orm_expression_t left, orm_expression_t right) {
  return orm_expression_combine(ORM_LOGIC_AND, left, right);
}

static inline orm_expression_t orm_expression_or(orm_expression_t left, orm_expression_t right) {
  return orm_expression_combine(ORM_LOGIC_OR, left, right);
}

#ifndef __cplusplus
  #define ORM_EQ(column, value)                                                                    \
    orm_expression_compare(orm_view((column)), ORM_COMPARE_EQUAL, (value))
  #define ORM_NE(column, value)                                                                    \
    orm_expression_compare(orm_view((column)), ORM_COMPARE_NOT_EQUAL, (value))
  #define ORM_LT(column, value)                                                                    \
    orm_expression_compare(orm_view((column)), ORM_COMPARE_LESS, (value))
  #define ORM_LE(column, value)                                                                    \
    orm_expression_compare(orm_view((column)), ORM_COMPARE_LESS_EQUAL, (value))
  #define ORM_GT(column, value)                                                                    \
    orm_expression_compare(orm_view((column)), ORM_COMPARE_GREATER, (value))
  #define ORM_GE(column, value)                                                                    \
    orm_expression_compare(orm_view((column)), ORM_COMPARE_GREATER_EQUAL, (value))
  #define ORM_LIKE(column, value)                                                                  \
    orm_expression_compare(orm_view((column)), ORM_COMPARE_LIKE, (value))
  #define ORM_NOT_LIKE(column, value)                                                              \
    orm_expression_compare(orm_view((column)), ORM_COMPARE_NOT_LIKE, (value))
  #define ORM_AGG_EQ(aggregate, column, value)                                                     \
    orm_expression_aggregate((aggregate), orm_view((column)), ORM_COMPARE_EQUAL, (value))
  #define ORM_AGG_NE(aggregate, column, value)                                                     \
    orm_expression_aggregate((aggregate), orm_view((column)), ORM_COMPARE_NOT_EQUAL, (value))
  #define ORM_AGG_LT(aggregate, column, value)                                                     \
    orm_expression_aggregate((aggregate), orm_view((column)), ORM_COMPARE_LESS, (value))
  #define ORM_AGG_LE(aggregate, column, value)                                                     \
    orm_expression_aggregate((aggregate), orm_view((column)), ORM_COMPARE_LESS_EQUAL, (value))
  #define ORM_AGG_GT(aggregate, column, value)                                                     \
    orm_expression_aggregate((aggregate), orm_view((column)), ORM_COMPARE_GREATER, (value))
  #define ORM_AGG_GE(aggregate, column, value)                                                     \
    orm_expression_aggregate((aggregate), orm_view((column)), ORM_COMPARE_GREATER_EQUAL, (value))
  #define ORM_AND(left, right) orm_expression_and((left), (right))
  #define ORM_OR(left, right) orm_expression_or((left), (right))
#endif

/*
 * QueryDSL-inspired source-only C11 fluent DSL. It is returned by value, borrows
 * the query and error handles, and must not cross a shared-library boundary.
 * Each operation must receive the address of the same chain as its explicit self.
 * The first failure is sticky; later builder operations do not mutate the
 * query, and execute returns that failure with a null result. Recreate the
 * chain with orm_chain() to begin a new attempt on the same query.
 */
typedef struct orm_chain orm_chain_t;

typedef struct orm_chain_select_ops {
  orm_chain_t *(ORM_C_CALL *all)(orm_chain_t *self);
  orm_chain_t *(ORM_C_CALL *column)(orm_chain_t *self, orm_string_view_t column);
  orm_chain_t *(ORM_C_CALL *aggregate)(orm_chain_t *self, orm_aggregate_t aggregate,
                                       orm_string_view_t column, orm_string_view_t alias);
  orm_chain_t *(ORM_C_CALL *join)(orm_chain_t *self, orm_join_t join, orm_string_view_t table,
                                  orm_string_view_t left_column, orm_compare_t comparison,
                                  orm_string_view_t right_column);
  orm_chain_t *(ORM_C_CALL *group_by)(orm_chain_t *self, orm_string_view_t column);
} orm_chain_select_ops_t;

typedef struct orm_chain_condition_ops {
  orm_chain_t *(ORM_C_CALL *begin_where)(orm_chain_t *self, orm_logic_t logic);
  orm_chain_t *(ORM_C_CALL *end_where)(orm_chain_t *self);
  orm_chain_t *(ORM_C_CALL *having)(orm_chain_t *self, orm_string_view_t column,
                                    orm_compare_t comparison, orm_value_t value);
  orm_chain_t *(ORM_C_CALL *having_aggregate)(orm_chain_t *self, orm_aggregate_t aggregate,
                                              orm_string_view_t column, orm_compare_t comparison,
                                              orm_value_t value);
  orm_chain_t *(ORM_C_CALL *begin_having)(orm_chain_t *self, orm_logic_t logic);
  orm_chain_t *(ORM_C_CALL *end_having)(orm_chain_t *self);
} orm_chain_condition_ops_t;

struct orm_chain {
  orm_query_t *query;
  orm_error_t *error;
  orm_status_t status;
  orm_chain_t *(ORM_C_CALL *set)(orm_chain_t *self, orm_string_view_t column, orm_value_t value);
  orm_chain_t *(ORM_C_CALL *where)(orm_chain_t *self, orm_string_view_t column,
                                   orm_compare_t comparison, orm_value_t value);
  orm_chain_t *(ORM_C_CALL *bind)(orm_chain_t *self, orm_value_t value);
  orm_chain_t *(ORM_C_CALL *order_by)(orm_chain_t *self, orm_string_view_t column,
                                      orm_order_t order);
  orm_chain_t *(ORM_C_CALL *limit)(orm_chain_t *self, uint64_t limit);
  orm_chain_t *(ORM_C_CALL *offset)(orm_chain_t *self, uint64_t offset);
  orm_status_t(ORM_C_CALL *execute)(orm_chain_t *self, orm_result_t **out_result);
  orm_chain_select_ops_t select;
  orm_chain_condition_ops_t condition;
  orm_chain_t *(ORM_C_CALL *where_expr)(orm_chain_t *self, orm_expression_t expression);
  orm_chain_t *(ORM_C_CALL *having_expr)(orm_chain_t *self, orm_expression_t expression);
};

static inline int orm_chain_detail_ready(const orm_chain_t *self) {
  return self != NULL && self->status == ORM_STATUS_OK;
}

static inline orm_chain_t *orm_chain_detail_fail(orm_chain_t *self, orm_status_t status,
                                                 const char *message) {
  size_t index = 0;
  if (self == NULL) return NULL;
  self->status = status;
  if (self->error == NULL || self->error->struct_size != (uint32_t)sizeof(orm_error_t)) return self;
  self->error->status = status;
  while (message[index] != '\0' && index + 1 < ORM_C_ERROR_MESSAGE_CAPACITY) {
    self->error->message[index] = message[index];
    ++index;
  }
  self->error->message[index] = '\0';
  TURBO_LOG_TYPED(tlog_peek_default(), TURBO_LOG_LEVEL_DEBUG, "orm", "chain failure ({}): {}",
                  status, tstr_v_from_cstr(message));
  return self;
}

static inline int orm_chain_detail_valid_logic(orm_logic_t logic) {
  return logic == ORM_LOGIC_AND || logic == ORM_LOGIC_OR;
}

static inline int orm_chain_detail_valid_comparison(orm_compare_t comparison) {
  return comparison >= ORM_COMPARE_EQUAL && comparison <= ORM_COMPARE_NOT_LIKE;
}

static inline int orm_chain_detail_valid_aggregate(orm_aggregate_t aggregate) {
  return aggregate >= ORM_AGGREGATE_COUNT_ALL && aggregate <= ORM_AGGREGATE_MAXIMUM;
}

static inline orm_status_t orm_chain_detail_validate_expression(const orm_expression_t *expression,
                                                                int having) {
  uint32_t group_children[ORM_C_EXPRESSION_CAPACITY];
  uint32_t depth = 0;
  uint32_t root_children = 0;
  uint32_t index;

  if (expression->status != ORM_STATUS_OK) return expression->status;
  if (expression->token_count == 0 || expression->token_count > ORM_C_EXPRESSION_CAPACITY)
    return ORM_STATUS_INVALID_ARGUMENT;

  for (index = 0; index < expression->token_count; ++index) {
    const orm_expression_token_t *token = &expression->tokens[index];
    if (token->kind == ORM_EXPRESSION_GROUP_BEGIN) {
      if (!orm_chain_detail_valid_logic(token->logic) || depth == ORM_C_EXPRESSION_CAPACITY)
        return ORM_STATUS_INVALID_ARGUMENT;
      group_children[depth++] = 0;
      continue;
    }
    if (token->kind == ORM_EXPRESSION_GROUP_END) {
      if (depth == 0 || group_children[depth - 1] < 2) return ORM_STATUS_INVALID_ARGUMENT;
      --depth;
    } else if (token->kind == ORM_EXPRESSION_PREDICATE) {
      if (!orm_chain_detail_valid_comparison(token->comparison)) return ORM_STATUS_INVALID_ARGUMENT;
    } else if (token->kind == ORM_EXPRESSION_AGGREGATE_PREDICATE) {
      if (!having || !orm_chain_detail_valid_aggregate(token->aggregate) ||
          !orm_chain_detail_valid_comparison(token->comparison))
        return ORM_STATUS_INVALID_ARGUMENT;
    } else {
      return ORM_STATUS_INVALID_ARGUMENT;
    }

    if (depth == 0) ++root_children;
    else ++group_children[depth - 1];
  }
  if (depth != 0 || root_children != 1) return ORM_STATUS_INVALID_ARGUMENT;
  return ORM_STATUS_OK;
}

static inline orm_chain_t *
orm_chain_detail_apply_expression(orm_chain_t *self, orm_expression_t expression, int having) {
  orm_status_t validation;
  uint32_t index;
  if (!orm_chain_detail_ready(self)) return self;

  validation = orm_chain_detail_validate_expression(&expression, having);
  if (validation != ORM_STATUS_OK) {
    if (validation == ORM_STATUS_LIMIT_EXCEEDED)
      return orm_chain_detail_fail(self, validation, "ORM expression capacity exceeded");
    return orm_chain_detail_fail(self, validation, "ORM condition expression is invalid");
  }

  for (index = 0; index < expression.token_count && orm_chain_detail_ready(self); ++index) {
    const orm_expression_token_t *token = &expression.tokens[index];
    if (token->kind == ORM_EXPRESSION_GROUP_BEGIN) {
      self->status = having ? orm_query_begin_having_group(self->query, token->logic, self->error)
                            : orm_query_begin_where_group(self->query, token->logic, self->error);
    } else if (token->kind == ORM_EXPRESSION_GROUP_END) {
      self->status = having ? orm_query_end_having_group(self->query, self->error)
                            : orm_query_end_where_group(self->query, self->error);
    } else if (token->kind == ORM_EXPRESSION_AGGREGATE_PREDICATE) {
      self->status = orm_query_having_aggregate(self->query, token->aggregate, token->column,
                                                token->comparison, token->value, self->error);
    } else if (having) {
      self->status = orm_query_having(self->query, token->column, token->comparison, token->value,
                                      self->error);
    } else {
      self->status =
          orm_query_where(self->query, token->column, token->comparison, token->value, self->error);
    }
  }
  return self;
}

static inline orm_chain_t *ORM_C_CALL orm_chain_detail_set(orm_chain_t *self,
                                                           orm_string_view_t column,
                                                           orm_value_t value) {
  if (orm_chain_detail_ready(self))
    self->status = orm_query_set(self->query, column, value, self->error);
  return self;
}

static inline orm_chain_t *ORM_C_CALL orm_chain_detail_where(orm_chain_t *self,
                                                             orm_string_view_t column,
                                                             orm_compare_t comparison,
                                                             orm_value_t value) {
  if (orm_chain_detail_ready(self))
    self->status = orm_query_where(self->query, column, comparison, value, self->error);
  return self;
}

static inline orm_chain_t *ORM_C_CALL orm_chain_detail_where_expr(orm_chain_t *self,
                                                                  orm_expression_t expression) {
  return orm_chain_detail_apply_expression(self, expression, 0);
}

static inline orm_chain_t *ORM_C_CALL orm_chain_detail_having_expr(orm_chain_t *self,
                                                                   orm_expression_t expression) {
  return orm_chain_detail_apply_expression(self, expression, 1);
}

static inline orm_chain_t *ORM_C_CALL orm_chain_detail_bind(orm_chain_t *self, orm_value_t value) {
  if (orm_chain_detail_ready(self)) self->status = orm_query_bind(self->query, value, self->error);
  return self;
}

static inline orm_chain_t *ORM_C_CALL orm_chain_detail_order_by(orm_chain_t *self,
                                                                orm_string_view_t column,
                                                                orm_order_t order) {
  if (orm_chain_detail_ready(self))
    self->status = orm_query_order_by(self->query, column, order, self->error);
  return self;
}

static inline orm_chain_t *ORM_C_CALL orm_chain_detail_limit(orm_chain_t *self, uint64_t limit) {
  if (orm_chain_detail_ready(self))
    self->status = orm_query_set_limit(self->query, limit, self->error);
  return self;
}

static inline orm_chain_t *ORM_C_CALL orm_chain_detail_offset(orm_chain_t *self, uint64_t offset) {
  if (orm_chain_detail_ready(self))
    self->status = orm_query_set_offset(self->query, offset, self->error);
  return self;
}

static inline orm_chain_t *ORM_C_CALL orm_chain_detail_select_all(orm_chain_t *self) {
  if (orm_chain_detail_ready(self)) self->status = orm_query_select_all(self->query, self->error);
  return self;
}

static inline orm_chain_t *ORM_C_CALL orm_chain_detail_select_column(orm_chain_t *self,
                                                                     orm_string_view_t column) {
  if (orm_chain_detail_ready(self))
    self->status = orm_query_add_column(self->query, column, self->error);
  return self;
}

static inline orm_chain_t *ORM_C_CALL orm_chain_detail_select_aggregate(orm_chain_t *self,
                                                                        orm_aggregate_t aggregate,
                                                                        orm_string_view_t column,
                                                                        orm_string_view_t alias) {
  if (orm_chain_detail_ready(self))
    self->status = orm_query_add_aggregate(self->query, aggregate, column, alias, self->error);
  return self;
}

static inline orm_chain_t *ORM_C_CALL orm_chain_detail_select_join(
    orm_chain_t *self, orm_join_t join, orm_string_view_t table, orm_string_view_t left_column,
    orm_compare_t comparison, orm_string_view_t right_column) {
  if (orm_chain_detail_ready(self))
    self->status = orm_query_join(self->query, join, table, left_column, comparison, right_column,
                                  self->error);
  return self;
}

static inline orm_chain_t *ORM_C_CALL orm_chain_detail_select_group_by(orm_chain_t *self,
                                                                       orm_string_view_t column) {
  if (orm_chain_detail_ready(self))
    self->status = orm_query_group_by(self->query, column, self->error);
  return self;
}

static inline orm_chain_t *ORM_C_CALL orm_chain_detail_begin_where(orm_chain_t *self,
                                                                   orm_logic_t logic) {
  if (orm_chain_detail_ready(self))
    self->status = orm_query_begin_where_group(self->query, logic, self->error);
  return self;
}

static inline orm_chain_t *ORM_C_CALL orm_chain_detail_end_where(orm_chain_t *self) {
  if (orm_chain_detail_ready(self))
    self->status = orm_query_end_where_group(self->query, self->error);
  return self;
}

static inline orm_chain_t *ORM_C_CALL orm_chain_detail_having(orm_chain_t *self,
                                                              orm_string_view_t column,
                                                              orm_compare_t comparison,
                                                              orm_value_t value) {
  if (orm_chain_detail_ready(self))
    self->status = orm_query_having(self->query, column, comparison, value, self->error);
  return self;
}

static inline orm_chain_t *ORM_C_CALL orm_chain_detail_having_aggregate(orm_chain_t *self,
                                                                        orm_aggregate_t aggregate,
                                                                        orm_string_view_t column,
                                                                        orm_compare_t comparison,
                                                                        orm_value_t value) {
  if (orm_chain_detail_ready(self))
    self->status =
        orm_query_having_aggregate(self->query, aggregate, column, comparison, value, self->error);
  return self;
}

static inline orm_chain_t *ORM_C_CALL orm_chain_detail_begin_having(orm_chain_t *self,
                                                                    orm_logic_t logic) {
  if (orm_chain_detail_ready(self))
    self->status = orm_query_begin_having_group(self->query, logic, self->error);
  return self;
}

static inline orm_chain_t *ORM_C_CALL orm_chain_detail_end_having(orm_chain_t *self) {
  if (orm_chain_detail_ready(self))
    self->status = orm_query_end_having_group(self->query, self->error);
  return self;
}

static inline orm_status_t ORM_C_CALL orm_chain_detail_execute(orm_chain_t *self,
                                                               orm_result_t **out_result) {
  if (out_result != NULL) *out_result = NULL;
  if (self == NULL) return ORM_STATUS_INVALID_ARGUMENT;
  if (self->status != ORM_STATUS_OK) return self->status;
  self->status = orm_query_execute(self->query, out_result, self->error);
  return self->status;
}

static inline orm_chain_t orm_chain(orm_query_t *query, orm_error_t *error) {
  orm_chain_t chain;
  chain.query = query;
  chain.error = error;
  chain.status = ORM_STATUS_OK;
  chain.set = orm_chain_detail_set;
  chain.where = orm_chain_detail_where;
  chain.bind = orm_chain_detail_bind;
  chain.order_by = orm_chain_detail_order_by;
  chain.limit = orm_chain_detail_limit;
  chain.offset = orm_chain_detail_offset;
  chain.execute = orm_chain_detail_execute;
  chain.select.all = orm_chain_detail_select_all;
  chain.select.column = orm_chain_detail_select_column;
  chain.select.aggregate = orm_chain_detail_select_aggregate;
  chain.select.join = orm_chain_detail_select_join;
  chain.select.group_by = orm_chain_detail_select_group_by;
  chain.condition.begin_where = orm_chain_detail_begin_where;
  chain.condition.end_where = orm_chain_detail_end_where;
  chain.condition.having = orm_chain_detail_having;
  chain.condition.having_aggregate = orm_chain_detail_having_aggregate;
  chain.condition.begin_having = orm_chain_detail_begin_having;
  chain.condition.end_having = orm_chain_detail_end_having;
  chain.where_expr = orm_chain_detail_where_expr;
  chain.having_expr = orm_chain_detail_having_expr;
  return chain;
}

#ifdef __cplusplus
}
#endif

#endif

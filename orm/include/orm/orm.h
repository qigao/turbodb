#ifndef ORM_H
#define ORM_H

/* Typed, demand-driven ORM facade. Query execution yields CFlow Publishers. */

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <cbind/cbind.h>
#include <cflow/cflow.h>
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

#define ORM_C_ABI_VERSION UINT32_C(4)
#define ORM_C_ERROR_MESSAGE_CAPACITY UINT32_C(512)
#define ORM_C_DEFAULT_MAX_PARAMETERS UINT32_C(256)
#define ORM_C_DEFAULT_MAX_COLUMNS UINT32_C(256)
#define ORM_C_DEFAULT_MAX_PREDICATES UINT32_C(256)
#define ORM_C_DEFAULT_MAX_QUERY_BYTES UINT64_C(65536)
#define ORM_C_DEFAULT_MAX_PARAMETER_BYTES UINT64_C(1048576)
#define ORM_C_DEFAULT_MAX_RESULT_ROWS UINT64_C(10000)
#define ORM_C_DEFAULT_MAX_RESULT_BYTES UINT64_C(16777216)
#define ORM_C_DEFAULT_MAX_ASSIGNMENTS UINT32_C(256)
#define ORM_C_DEFAULT_FLOW_SCRATCH_BYTES UINT64_C(4096)
#define ORM_C_DEFAULT_FLOW_MAX_DEPTH UINT64_C(64)
#define ORM_C_DEFAULT_FLOW_MAX_CONTAINER_ITEMS UINT64_C(10000)
#define ORM_C_DEFAULT_FLOW_MAX_BUFFER_BYTES UINT64_C(1048576)

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
  ORM_STATUS_DATASTORE_ERROR = 14,
  ORM_STATUS_CONSTRAINT = 15
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
  ORM_COMPARE_NOT_EQUAL,
  ORM_COMPARE_LESS,
  ORM_COMPARE_LESS_EQUAL,
  ORM_COMPARE_GREATER,
  ORM_COMPARE_GREATER_EQUAL,
  ORM_COMPARE_LIKE,
  ORM_COMPARE_NOT_LIKE
};

typedef int32_t orm_order_t;
enum { ORM_ORDER_ASCENDING = 0, ORM_ORDER_DESCENDING = 1 };
typedef int32_t orm_isolation_t;
enum {
  ORM_ISOLATION_READ_UNCOMMITTED = 0,
  ORM_ISOLATION_READ_COMMITTED,
  ORM_ISOLATION_REPEATABLE_READ,
  ORM_ISOLATION_SNAPSHOT,
  ORM_ISOLATION_SERIALIZABLE
};

typedef struct orm_connection orm_connection_t;
typedef struct orm_query orm_query_t;
typedef struct orm_result orm_result_t;
typedef struct orm_transaction orm_transaction_t;
typedef vstr orm_string_view_t;

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
  orm_string_view_t driver;
  const orm_option_t *options;
  uint32_t option_count;
  uint32_t max_parameters;
  uint32_t max_columns;
  uint32_t max_predicates;
  uint32_t max_assignments;
  uint64_t max_query_bytes;
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

typedef struct orm_key_part {
  orm_string_view_t column;
  orm_value_t value;
} orm_key_part_t;

typedef struct orm_flow_config {
  uint32_t struct_size;
  uint32_t abi_version;
  const cmeta_data_desc *row_shape;
  size_t scratch_bytes;
  size_t max_depth;
  size_t max_container_items;
  size_t max_buffer_bytes;
} orm_flow_config_t;

typedef struct orm_command_result {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t affected_rows;
} orm_command_result_t;

#define ORM_COMMAND_RESULT_INIT \
  { sizeof(orm_command_result_t), ORM_C_ABI_VERSION, 0u }

ORM_C_API uint32_t ORM_C_CALL orm_c_abi_version(void);
ORM_C_API const char *ORM_C_CALL orm_status_message(orm_status_t status);
ORM_C_API void ORM_C_CALL orm_error_init(orm_error_t *error);
ORM_C_API void ORM_C_CALL orm_config(orm_config_t *config);
ORM_C_API void ORM_C_CALL orm_flow_config(orm_flow_config_t *config,
                                         const cmeta_data_desc *row_shape);

ORM_C_API orm_status_t ORM_C_CALL orm_connect(
    const orm_config_t *config, orm_connection_t **out_connection,
    orm_error_t *error);
ORM_C_API void ORM_C_CALL orm_disconnect(orm_connection_t *connection);

ORM_C_API orm_status_t ORM_C_CALL orm_transaction_begin(
    orm_connection_t *connection, orm_isolation_t isolation,
    orm_transaction_t **out_transaction, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_transaction_commit(
    orm_transaction_t *transaction, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_transaction_rollback(
    orm_transaction_t *transaction, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_transaction_savepoint(
    orm_transaction_t *transaction, orm_string_view_t name,
    orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_transaction_rollback_to_savepoint(
    orm_transaction_t *transaction, orm_string_view_t name,
    orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_transaction_release_savepoint(
    orm_transaction_t *transaction, orm_string_view_t name,
    orm_error_t *error);
ORM_C_API void ORM_C_CALL orm_transaction_destroy(
    orm_transaction_t *transaction);

ORM_C_API orm_status_t ORM_C_CALL orm_query_create(
    orm_connection_t *connection, orm_string_view_t table,
    orm_query_t **out_query, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_insert(
    orm_connection_t *connection, orm_string_view_t table,
    orm_query_t **out_query, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_update(
    orm_connection_t *connection, orm_string_view_t table,
    orm_query_t **out_query, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_delete(
    orm_connection_t *connection, orm_string_view_t table,
    orm_query_t **out_query, orm_error_t *error);
/*
 * Raw SQL accepts one-based ?N parameters as a portable spelling. SQL
 * backends normalize that spelling to their native placeholder grammar while
 * preserving quoted strings, identifiers, and comments. Driver-native
 * placeholders remain accepted.
 */
ORM_C_API orm_status_t ORM_C_CALL orm_raw(
    orm_connection_t *connection, orm_string_view_t sql,
    orm_query_t **out_query, orm_error_t *error);
ORM_C_API void ORM_C_CALL orm_query_destroy(orm_query_t *query);

ORM_C_API orm_status_t ORM_C_CALL orm_query_select_all(
    orm_query_t *query, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_query_add_column(
    orm_query_t *query, orm_string_view_t column, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_query_set(
    orm_query_t *query, orm_string_view_t column, orm_value_t value,
    orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_query_where(
    orm_query_t *query, orm_string_view_t column, orm_compare_t comparison,
    orm_value_t value, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_query_where_key(
    orm_query_t *query, const orm_key_part_t *parts, uint32_t part_count,
    orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_query_bind(
    orm_query_t *query, orm_value_t value, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_query_order_by(
    orm_query_t *query, orm_string_view_t column, orm_order_t order,
    orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_query_set_limit(
    orm_query_t *query, uint64_t limit, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_query_set_offset(
    orm_query_t *query, uint64_t offset, orm_error_t *error);

/*
 * Materialized execution copies a bounded result snapshot owned by the
 * caller. Text and blob views returned by accessors remain valid until
 * orm_result_destroy(). Async cursors that yield WAIT are not supported by
 * this synchronous interface; use the CFlow APIs for those backends.
 */
ORM_C_API orm_status_t ORM_C_CALL orm_query_execute(
    orm_query_t *query, orm_result_t **out_result, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_query_execute_in_transaction(
    orm_query_t *query, orm_transaction_t *transaction,
    orm_result_t **out_result, orm_error_t *error);
ORM_C_API void ORM_C_CALL orm_result_destroy(orm_result_t *result);
ORM_C_API orm_status_t ORM_C_CALL orm_result_row_count(
    const orm_result_t *result, uint64_t *out_count, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_result_column_count(
    const orm_result_t *result, uint64_t *out_count, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_result_affected_rows(
    const orm_result_t *result, uint64_t *out_count, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_result_is_null(
    const orm_result_t *result, uint64_t row, uint64_t column,
    uint8_t *out_is_null, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_result_value_kind(
    const orm_result_t *result, uint64_t row, uint64_t column,
    orm_value_kind_t *out_kind, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_result_get_text(
    const orm_result_t *result, uint64_t row, uint64_t column,
    orm_string_view_t *out_value, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_result_get_blob(
    const orm_result_t *result, uint64_t row, uint64_t column,
    orm_blob_t *out_value, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_result_get_int64(
    const orm_result_t *result, uint64_t row, uint64_t column,
    int64_t *out_value, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_result_get_uint64(
    const orm_result_t *result, uint64_t row, uint64_t column,
    uint64_t *out_value, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_result_get_double(
    const orm_result_t *result, uint64_t row, uint64_t column,
    double *out_value, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_result_get_boolean(
    const orm_result_t *result, uint64_t row, uint64_t column,
    uint8_t *out_value, orm_error_t *error);

/*
 * On success the Publisher owns its driver cursor. The query, connection,
 * row_shape, and reachable metadata must outlive the Publisher. A Publisher opened
 * in a transaction also requires that transaction to outlive the Publisher.
 * out_publisher must be zero-initialized and must not already own a Publisher.
 */
ORM_C_API orm_status_t ORM_C_CALL orm_query_open_flow(
    orm_query_t *query, const orm_flow_config_t *config,
    cflow_publisher *out_publisher, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_query_open_flow_in_transaction(
    orm_query_t *query, orm_transaction_t *transaction,
    const orm_flow_config_t *config, cflow_publisher *out_publisher,
    orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_query_open_command_flow(
    orm_query_t *query, cflow_publisher *out_publisher, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_query_open_command_flow_in_transaction(
    orm_query_t *query, orm_transaction_t *transaction,
    cflow_publisher *out_publisher, orm_error_t *error);

static inline orm_string_view_t orm_view(const char *text) {
  return vstr_from_cstr(text);
}
static inline orm_string_view_t orm_view_tstr(tstr text) {
  return tstr_to_v(text);
}
static inline orm_value_t orm_null(void) {
  orm_value_t value = {ORM_VALUE_NULL, 0u, {0}};
  return value;
}
static inline orm_value_t orm_i64(int64_t input) {
  orm_value_t value = {ORM_VALUE_INT64, 0u, {0}};
  value.data.int64_value = input;
  return value;
}
static inline orm_value_t orm_u64(uint64_t input) {
  orm_value_t value = {ORM_VALUE_UINT64, 0u, {0}};
  value.data.uint64_value = input;
  return value;
}
static inline orm_value_t orm_f64(double input) {
  orm_value_t value = {ORM_VALUE_DOUBLE, 0u, {0}};
  value.data.double_value = input;
  return value;
}
static inline orm_value_t orm_bool(int input) {
  orm_value_t value = {ORM_VALUE_BOOLEAN, 0u, {0}};
  value.data.boolean_value = (uint8_t)(input != 0);
  return value;
}
static inline orm_value_t orm_text_v(vstr input) {
  orm_value_t value = {ORM_VALUE_TEXT, 0u, {0}};
  value.data.text_value = input;
  return value;
}
static inline orm_value_t orm_text(const char *input) {
  return orm_text_v(orm_view(input));
}
static inline orm_value_t orm_text_tstr(tstr input) {
  return orm_text_v(tstr_to_v(input));
}
static inline orm_value_t orm_blob_v(orm_blob_t input) {
  orm_value_t value = {ORM_VALUE_BLOB, 0u, {0}};
  value.data.blob_value = input;
  return value;
}
static inline orm_value_t orm_blob(const void *data, size_t size) {
  const orm_blob_t blob = {data, size};
  return orm_blob_v(blob);
}
static inline orm_key_part_t orm_key_part(orm_string_view_t column,
                                          orm_value_t value) {
  const orm_key_part_t part = {column, value};
  return part;
}

#ifdef __cplusplus
}
#endif

#endif

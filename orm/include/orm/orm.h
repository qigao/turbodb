#ifndef ORM_H
#define ORM_H

/* Typed, demand-driven ORM facade. Query execution yields CFlow Publishers. */

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <cmeta/data.h>
#include <cmeta/object.h>
#include <cflow/cflow.h>
#include <tstr.h>

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
  ORM_STATUS_CONSTRAINT = 15,
  /* A dispatched commit may have reached the datastore, but acknowledgement
   * was lost. The transaction/connection is quarantined and must not replay. */
  ORM_STATUS_COMMIT_UNKNOWN = 16,
  /* Final native cleanup failed. The owner is quarantined; this is not a
   * retryable close result and resources may remain pinned until process exit. */
  ORM_STATUS_CLEANUP_FAILED = 17,
  ORM_STATUS_DRIVER_NOT_REGISTERED = 18,
  ORM_STATUS_DRIVER_ALREADY_REGISTERED = 19,
  ORM_STATUS_DRIVER_MODULE_NOT_FOUND = 20,
  /* The explicit module file exists but the OS loader rejected it, including
   * unresolved native dependencies or an invalid module image. */
  ORM_STATUS_DRIVER_LOAD_ERROR = 21,
  ORM_STATUS_DRIVER_ENTRY_MISSING = 22,
  ORM_STATUS_DRIVER_ID_MISMATCH = 23,
  /* A complete logical value violated the canonical SaltsUtils DataBind
   * ValidationPlan. This is distinct from database/native constraint errors. */
  ORM_STATUS_VALIDATION_ERROR = 24
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
typedef struct orm_metadata_snapshot orm_metadata_snapshot_t;
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

typedef struct DataBindMessagePlan DataBindMessagePlan;
typedef struct DataBindMessageObjectStateProvider DataBindMessageObjectStateProvider;

typedef orm_status_t (*orm_object_row_create_fn)(
    void *context, void *out_value, cmeta_object_ref *out_object,
    orm_error_t *error);
typedef void (*orm_object_row_destroy_fn)(
    void *context, void *value);

typedef struct orm_object_row_factory {
  uint32_t struct_size;
  uint32_t abi_version;
  const cmeta_type_desc *output_type;
  void *context;
  orm_object_row_create_fn create;
  orm_object_row_destroy_fn destroy;
} orm_object_row_factory_t;

typedef struct orm_object_flow_config {
  uint32_t struct_size;
  uint32_t abi_version;
  const cmeta_data_desc *row_shape;
  const DataBindMessagePlan *message_plan;
  const DataBindMessageObjectStateProvider *state_provider;
  orm_object_row_factory_t factory;
  size_t scratch_bytes;
  size_t max_depth;
  size_t max_container_items;
  size_t max_buffer_bytes;
} orm_object_flow_config_t;


/* Borrowed single-owner scheduler. Poll and timeout use its clock ticks.
 * A delayed, non-concurrent scheduler is required; no worker is created. */
typedef struct orm_async_config {
  uint32_t struct_size;
  cflow_scheduler *scheduler;
  uint64_t poll_interval_ticks;
  uint64_t timeout_ticks;
} orm_async_config_t;

typedef struct orm_command_result {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t affected_rows;
} orm_command_result_t;

#define ORM_COMMAND_RESULT_INIT \
  { sizeof(orm_command_result_t), ORM_C_ABI_VERSION, 0u }

typedef int32_t orm_metadata_kind_t;
enum {
  ORM_METADATA_CATALOG = 0,
  ORM_METADATA_SCHEMA = 1,
  ORM_METADATA_TABLE = 2,
  ORM_METADATA_VIEW = 3,
  ORM_METADATA_COLUMN = 4,
  ORM_METADATA_INDEX = 5
};

typedef struct orm_metadata_entry {
  uint32_t struct_size;
  orm_metadata_kind_t kind;
  /* Zero-based within one relation for ORM_METADATA_COLUMN entries. */
  uint32_t ordinal;
  uint32_t reserved;
  orm_string_view_t catalog;
  orm_string_view_t schema;
  orm_string_view_t relation;
  orm_string_view_t name;
} orm_metadata_entry_t;

#define ORM_METADATA_ENTRY_INIT \
  { sizeof(orm_metadata_entry_t), ORM_METADATA_CATALOG, 0u, 0u, \
    {NULL, 0u}, {NULL, 0u}, {NULL, 0u}, {NULL, 0u} }


ORM_C_API uint32_t ORM_C_CALL orm_c_abi_version(void);
ORM_C_API const char *ORM_C_CALL orm_status_message(orm_status_t status);
ORM_C_API void ORM_C_CALL orm_error_init(orm_error_t *error);
ORM_C_API void ORM_C_CALL orm_config(orm_config_t *config);
ORM_C_API void ORM_C_CALL orm_flow_config(orm_flow_config_t *config,
                                         const cmeta_data_desc *row_shape);
ORM_C_API void ORM_C_CALL orm_object_flow_config(
    orm_object_flow_config_t *config,
    const cmeta_data_desc *row_shape,
    const DataBindMessagePlan *message_plan,
    const orm_object_row_factory_t *factory);

ORM_C_API orm_status_t ORM_C_CALL orm_connect(
    const orm_config_t *config, orm_connection_t **out_connection,
    orm_error_t *error);

/*
 * Retained ownership for opaque connection handles.
 *
 * Every ownership operation requires the caller already to own a valid hold.
 * The API does not validate stale/freed pointers; each concurrent participant
 * must keep its own hold, and double release is a caller contract violation.
 *
 * retain() adds one caller hold and returns a status instead of terminating the
 * process when the handle cannot accept another hold. release() consumes one
 * previously owned caller hold; NULL release is a no-op.
 *
 * close() is checked and does not consume the caller hold. It returns BUSY
 * without changing state while dependent Query/Transaction/Publisher/native
 * work exists. On success, native resources are closed and the held handle
 * remains valid for retain/close/release only; no new business work is admitted.
 *
 * Legacy orm_disconnect() remains source/binary compatible and is equivalent
 * to releasing one caller hold. Cleanup may therefore be deferred until
 * already-admitted dependents complete.
 */
ORM_C_API orm_status_t ORM_C_CALL
orm_connection_close(orm_connection_t *connection, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL
orm_connection_retain(orm_connection_t *connection);
ORM_C_API void ORM_C_CALL orm_connection_release(
    orm_connection_t *connection);
ORM_C_API void ORM_C_CALL orm_disconnect(orm_connection_t *connection);

/*
 * Returns one bounded, owned metadata snapshot for the active SQL connection.
 * Provider-specific discovery remains inside TurboDB. Unsupported providers
 * return ORM_STATUS_UNSUPPORTED without attempting a fallback.
 *
 * Entry string views remain valid until orm_metadata_snapshot_destroy().
 * The total entry count and copied string bytes are bounded by the connection's
 * max_result_rows and max_result_bytes limits.
 */
ORM_C_API orm_status_t ORM_C_CALL orm_connection_metadata_snapshot(
    orm_connection_t *connection, orm_metadata_snapshot_t **out_snapshot,
    orm_error_t *error);
ORM_C_API void ORM_C_CALL orm_metadata_snapshot_destroy(
    orm_metadata_snapshot_t *snapshot);
ORM_C_API orm_status_t ORM_C_CALL orm_metadata_snapshot_count(
    const orm_metadata_snapshot_t *snapshot, uint64_t *out_count,
    orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_metadata_snapshot_get(
    const orm_metadata_snapshot_t *snapshot, uint64_t index,
    orm_metadata_entry_t *out_entry, orm_error_t *error);


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
/*
 * Checked transaction close is valid only after commit/rollback (or an
 * explicitly recorded COMMIT_UNKNOWN outcome). It returns BUSY while a
 * dependent Publisher/native operation remains active and does not consume the
 * caller hold. retain/release follow the same opaque-handle rules as
 * connections. Legacy destroy consumes one caller hold and preserves automatic
 * rollback-on-final-release compatibility for an ACTIVE transaction. If that
 * mandatory final rollback fails, the owner is quarantined and the default
 * no-handler policy fails fast rather than pretending cleanup succeeded.
 */
ORM_C_API orm_status_t ORM_C_CALL
orm_transaction_close(orm_transaction_t *transaction, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL
orm_transaction_retain(orm_transaction_t *transaction);
ORM_C_API void ORM_C_CALL orm_transaction_release(
    orm_transaction_t *transaction);
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
/*
 * As with connections, these functions require an existing valid caller hold.
 * Checked Query close returns BUSY while an admitted Publisher/cursor still
 * owns the Query. retain/release are explicit caller holds. A successful close
 * leaves the held opaque handle available for retain/close/release only.
 * Legacy destroy consumes one caller hold and may defer final cleanup.
 */
ORM_C_API orm_status_t ORM_C_CALL
orm_query_close(orm_query_t *query, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_query_retain(orm_query_t *query);
ORM_C_API void ORM_C_CALL orm_query_release(orm_query_t *query);
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
/* Returns an owned materialized column name when row metadata supplied one.
 * Empty result sets whose cursor exposes only a count return UNSUPPORTED rather
 * than synthesizing a provider-specific label. The view lives with result. */
ORM_C_API orm_status_t ORM_C_CALL orm_result_column_name(
    const orm_result_t *result, uint64_t column,
    orm_string_view_t *out_name, orm_error_t *error);
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

/*
 * Provider-backed object row flow.
 *
 * message_plan must be an object MessagePlan compiled against row_shape.
 * factory.create constructs one fresh caller-defined output carrier in
 * out_value and returns a live cmeta_object_ref to its logical row object.
 * TurboDB decodes the row with data_bind_message_plan_decode_object().
 *
 * On decode failure TurboDB calls factory.destroy() on the staging output.
 * On success ownership of the output value transfers to the Publisher caller;
 * TurboDB releases only the temporary cmeta_object_ref borrow. WAIT/DONE do
 * not construct an output value.
 *
 * query/connection, row_shape, message_plan, state_provider, output_type and
 * factory callbacks/context must outlive Publisher destruction.
 */
ORM_C_API orm_status_t ORM_C_CALL orm_query_open_object_flow(
    orm_query_t *query, const orm_object_flow_config_t *config,
    cflow_publisher *out_publisher, orm_error_t *error);

/* Opens a non-transactional row query with asynchronous progress: native
 * nonblocking I/O for MySQL/PostgreSQL, a serial worker for SQLite.
 * Connection establishment and transaction control keep their synchronous APIs.
 * Unsupported drivers return UNSUPPORTED without issuing a query. The async
 * scheduler must also drive the Subscription and outlive publisher destruction.
 * Cancellation abandons the active native query; it does not roll back effects
 * of arbitrary SQL functions or authorize an automatic retry. */
ORM_C_API orm_status_t ORM_C_CALL orm_query_open_async_flow(
    orm_query_t *query, const orm_flow_config_t *config,
    const orm_async_config_t *async_config,
    cflow_publisher *out_publisher, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_query_open_flow_in_transaction(
    orm_query_t *query, orm_transaction_t *transaction,
    const orm_flow_config_t *config, cflow_publisher *out_publisher,
    orm_error_t *error);

/*
 * Optional canonical DataBind validation path.
 *
 * message_plan is a control-plane artifact compiled by SaltsUtils from one
 * exact logical DataBind contract and its generated native binding. The plan,
 * row_shape and reachable metadata are borrowed through Publisher destruction.
 * Runtime execution performs no schema/constraint/reflection lookup.
 *
 * Existing orm_query_open_flow* entry points remain unchanged and perform
 * native DataBind decoding without logical ValidationPlan execution.
 */
struct DataBindMessagePlan;
ORM_C_API orm_status_t ORM_C_CALL orm_query_open_validated_flow(
    orm_query_t *query, const orm_flow_config_t *config,
    const struct DataBindMessagePlan *message_plan,
    cflow_publisher *out_publisher, orm_error_t *error);
ORM_C_API orm_status_t ORM_C_CALL orm_query_open_validated_flow_in_transaction(
    orm_query_t *query, orm_transaction_t *transaction,
    const orm_flow_config_t *config,
    const struct DataBindMessagePlan *message_plan,
    cflow_publisher *out_publisher, orm_error_t *error);
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

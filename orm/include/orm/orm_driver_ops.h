#ifndef ORM_DRIVER_OPS_H
#define ORM_DRIVER_OPS_H

#include "orm_driver_plan.h"
#include <cserde/reader.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct orm_driver_host_v1 orm_driver_host_v1;
typedef struct orm_driver_api_v1 orm_driver_api_v1;

#define ORM_DRIVER_CAP_SELECT (UINT64_C(1) << 0)
#define ORM_DRIVER_CAP_INSERT (UINT64_C(1) << 1)
#define ORM_DRIVER_CAP_UPDATE (UINT64_C(1) << 2)
#define ORM_DRIVER_CAP_DELETE (UINT64_C(1) << 3)
#define ORM_DRIVER_CAP_RAW_SQL (UINT64_C(1) << 4)
#define ORM_DRIVER_CAP_TRANSACTION (UINT64_C(1) << 5)
#define ORM_DRIVER_CAP_SAVEPOINT (UINT64_C(1) << 6)
#define ORM_DRIVER_CAP_INCREMENTAL_ROWS (UINT64_C(1) << 7)
#define ORM_DRIVER_CAP_READ_UNCOMMITTED (UINT64_C(1) << 8)
#define ORM_DRIVER_CAP_READ_COMMITTED (UINT64_C(1) << 9)
#define ORM_DRIVER_CAP_REPEATABLE_READ (UINT64_C(1) << 10)
#define ORM_DRIVER_CAP_SNAPSHOT (UINT64_C(1) << 11)
#define ORM_DRIVER_CAP_SERIALIZABLE (UINT64_C(1) << 12)
#define ORM_DRIVER_CAP_ISOLATION_MASK \
  (ORM_DRIVER_CAP_READ_UNCOMMITTED | ORM_DRIVER_CAP_READ_COMMITTED | \
   ORM_DRIVER_CAP_REPEATABLE_READ | ORM_DRIVER_CAP_SNAPSHOT | \
   ORM_DRIVER_CAP_SERIALIZABLE)
#define ORM_DRIVER_CAP_KNOWN_MASK \
  (ORM_DRIVER_CAP_SELECT | ORM_DRIVER_CAP_INSERT | ORM_DRIVER_CAP_UPDATE | \
   ORM_DRIVER_CAP_DELETE | ORM_DRIVER_CAP_RAW_SQL | ORM_DRIVER_CAP_TRANSACTION | \
   ORM_DRIVER_CAP_SAVEPOINT | ORM_DRIVER_CAP_INCREMENTAL_ROWS | \
   ORM_DRIVER_CAP_ISOLATION_MASK)
#define ORM_DRIVER_EXEC_CALLER_BLOCKING (UINT64_C(1) << 0)
#define ORM_DRIVER_EXEC_OWNER_EXECUTOR (UINT64_C(1) << 1)
#define ORM_DRIVER_EXEC_NATIVE_WAIT (UINT64_C(1) << 2)
#define ORM_DRIVER_EXEC_KNOWN_MASK \
  (ORM_DRIVER_EXEC_CALLER_BLOCKING | ORM_DRIVER_EXEC_OWNER_EXECUTOR | \
   ORM_DRIVER_EXEC_NATIVE_WAIT)
#define ORM_DRIVER_STEP_ROW UINT32_C(0)
#define ORM_DRIVER_STEP_ROW_AND_DONE UINT32_C(1)
#define ORM_DRIVER_STEP_WAIT UINT32_C(2)
#define ORM_DRIVER_STEP_DONE UINT32_C(3)
#define ORM_DRIVER_STEP_ERROR UINT32_C(4)

typedef struct orm_driver_connection_v1 {
  orm_driver_header_v1 header;
  void *context;
  orm_driver_table_v1 ops;
} orm_driver_connection_v1;
typedef struct orm_driver_transaction_v1 {
  orm_driver_header_v1 header;
  void *context;
  orm_driver_table_v1 ops;
} orm_driver_transaction_v1;
typedef struct orm_driver_cursor_v1 {
  orm_driver_header_v1 header;
  void *context;
  orm_driver_table_v1 ops;
} orm_driver_cursor_v1;
typedef struct orm_driver_step_v1 {
  orm_driver_header_v1 header;
  uint32_t kind;
  uint32_t reserved;
  cflow_waitable waitable;
} orm_driver_step_v1;

typedef orm_status_t (ORM_DRIVER_CALL *orm_driver_initialize_fn)(
    const orm_driver_host_v1 *, void **, orm_error_t *);
typedef orm_status_t (ORM_DRIVER_CALL *orm_driver_finalize_fn)(void *, orm_error_t *);
typedef void (ORM_DRIVER_CALL *orm_driver_destroy_fn)(void *);
typedef orm_status_t (ORM_DRIVER_CALL *orm_driver_open_cursor_fn)(
    void *, const orm_driver_plan_view_v1 *, const orm_driver_limits_v1 *,
    orm_driver_cursor_v1 *, orm_error_t *);
typedef orm_status_t (ORM_DRIVER_CALL *orm_driver_command_fn)(
    void *, const orm_driver_plan_view_v1 *, const orm_driver_limits_v1 *,
    uint64_t *, orm_error_t *);
typedef orm_status_t (ORM_DRIVER_CALL *orm_driver_begin_fn)(
    void *, orm_isolation_t, orm_driver_transaction_v1 *, orm_error_t *);
typedef orm_status_t (ORM_DRIVER_CALL *orm_driver_finish_fn)(void *, orm_error_t *);
typedef orm_status_t (ORM_DRIVER_CALL *orm_driver_savepoint_fn)(
    void *, orm_driver_bytes_v1, orm_error_t *);
typedef orm_status_t (ORM_DRIVER_CALL *orm_driver_next_fn)(
    void *, cserde_reader *, orm_driver_step_v1 *, orm_error_t *);
typedef void (ORM_DRIVER_CALL *orm_driver_cancel_fn)(void *);
typedef orm_status_t (ORM_DRIVER_CALL *orm_driver_shape_fn)(
    void *, const cmeta_data_desc *, orm_error_t *);
typedef orm_status_t (ORM_DRIVER_CALL *orm_driver_column_count_fn)(
    void *, uint64_t *, orm_error_t *);
typedef orm_status_t (ORM_DRIVER_CALL *orm_driver_acquire_fn)(
    void *, void **, orm_error_t *);
typedef void (ORM_DRIVER_CALL *orm_driver_release_fn)(void *);
typedef orm_status_t (ORM_DRIVER_CALL *orm_driver_work_fn)(void *);
typedef void (ORM_DRIVER_CALL *orm_driver_complete_fn)(void *, orm_status_t);
typedef orm_status_t (ORM_DRIVER_CALL *orm_driver_submit_fn)(
    void *, void *, orm_driver_work_fn, orm_driver_complete_fn,
    void *, void **, orm_error_t *);
typedef orm_status_t (ORM_DRIVER_CALL *orm_driver_create_fn)(
    void *, const orm_config_t *, const orm_driver_limits_v1 *,
    orm_driver_connection_v1 *, orm_error_t *);

typedef struct orm_driver_module_ops_v1 {
  orm_driver_header_v1 header;
  orm_driver_initialize_fn initialize;
  orm_driver_finalize_fn finalize;
} orm_driver_module_ops_v1;
typedef struct orm_driver_connection_ops_v1 {
  orm_driver_header_v1 header;
  orm_driver_destroy_fn destroy;
  orm_driver_open_cursor_fn open_cursor;
  orm_driver_command_fn execute_command;
  orm_driver_begin_fn begin_transaction;
} orm_driver_connection_ops_v1;
typedef struct orm_driver_transaction_ops_v1 {
  orm_driver_header_v1 header;
  orm_driver_destroy_fn destroy;
  orm_driver_open_cursor_fn open_cursor;
  orm_driver_command_fn execute_command;
  orm_driver_finish_fn commit;
  orm_driver_finish_fn rollback;
  orm_driver_savepoint_fn savepoint;
  orm_driver_savepoint_fn rollback_to_savepoint;
  orm_driver_savepoint_fn release_savepoint;
} orm_driver_transaction_ops_v1;
typedef struct orm_driver_cursor_ops_v1 {
  orm_driver_header_v1 header;
  orm_driver_next_fn next;
  orm_driver_cancel_fn cancel;
  orm_driver_destroy_fn destroy;
  orm_driver_shape_fn configure_shape;
  orm_driver_column_count_fn column_count;
} orm_driver_cursor_ops_v1;
typedef struct orm_driver_lifetime_ops_v1 {
  orm_driver_header_v1 header;
  orm_driver_acquire_fn acquire;
  orm_driver_release_fn release;
} orm_driver_lifetime_ops_v1;
typedef struct orm_driver_execution_ops_v1 {
  orm_driver_header_v1 header;
  orm_driver_submit_fn submit;
  orm_driver_cancel_fn request_cancel;
  orm_driver_release_fn release_task;
} orm_driver_execution_ops_v1;

/* Successful creates transfer context ownership to matching destroy ops.
 * Failed creates clean up locally and clear outputs. Cancel only requests
 * cancellation: context and module leases survive all in-flight callbacks.
 * ROW reader storage survives the enclosing Publisher resume; WAIT backing
 * storage survives disarm and callback completion. No native handle crosses
 * this interface. Real host lifetime/execution services are separate work. */
#ifdef __cplusplus
}
#endif
#endif

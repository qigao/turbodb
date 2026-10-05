#ifndef ORM_TIDESDB_SQL_SUBQUERY_H
#define ORM_TIDESDB_SQL_SUBQUERY_H
#include "scan.h"

typedef enum orm_sql_subquery_state {
  ORM_SQL_SUBQUERY_PENDING, ORM_SQL_SUBQUERY_READY, ORM_SQL_SUBQUERY_FAILED, ORM_SQL_SUBQUERY_CANCELLED
} orm_sql_subquery_state;

/* Private complete value/cardinality stage over an already bound row source;
 * not SQL admission. Stable address, one synchronous execution owner. Borrows
 * input until close, owns Scan and deep-copied cache. No rows read during open.
 * A SQL EXISTS caller must supply an existence plan without unused projection
 * evaluation. This stage cannot remove side effects from its source callback. */
typedef struct orm_sql_subquery {
  orm_tidesdb_sql_budget *budget;
  orm_sql_subquery_kind kind;
  orm_sql_subquery_state state;
  orm_sql_type result;
  orm_sql_predicate equality;
  orm_sql_scan input;
  orm_sql_rows cache;
  turbodb_value_t scalar;
  turbodb_error_t failure;
  size_t metadata_bytes;
  orm_sql_expr_query_source source;
  bool evaluating, fixed_probe;
} orm_sql_subquery;

/* Empty output required. SCALAR/IN/NOT IN require one column (SQL_ERROR),
 * EXISTS accepts any positive width. probe_type is required only for IN/NOT IN.
 * Shape/type/capacity validation applies even to empty or unused sources.
 * Comparisons follow value.h, including I64/U64 and byte comparisons. Descriptors
 * are copied; source descriptor/context stay stable until close. Failed open
 * restores all new work and leases and leaves out empty. */
turbodb_status_t orm_tidesdb_sql_subquery_open(orm_sql_row_source *source,
    orm_sql_subquery_kind kind, const orm_sql_type *probe_type,
    orm_sql_subquery *out, turbodb_error_t *error);
/* IN/NOT IN source for compiled consumers; owns the same cache without fixing
 * an outer probe type. Each expression supplies its bound EQUAL descriptor.
 * Direct eval is INVALID_STATE; use the returned lazy expression source. */
turbodb_status_t orm_sql_subquery_open_set(orm_sql_row_source *source,
    orm_sql_subquery_kind kind, orm_sql_subquery *out, turbodb_error_t *error);
/* Borrowed lazy expression source, NULL if closed. Source references prevent
 * direct eval/cancel/close with BUSY; close expression runs first. */
orm_sql_expr_query_source *orm_tidesdb_sql_subquery_source(orm_sql_subquery *run);
/* Lazy first evaluation, reusable only within this execution. SCALAR observes
 * at most two rows, returns NULL/one value or SQL_ERROR for multiple rows.
 * EXISTS observes at most one row, never returns NULL. IN materializes the whole
 * set once; later probes read no source rows. Empty IN is FALSE even for NULL,
 * otherwise an equality match wins over UNKNOWN. NOT forms negate only BOOL.
 * probe is required exactly for IN/NOT IN and validated before source reads.
 * Output may alias probe; TEXT/BLOB scalar output borrows owned cache through
 * cancel/close. Failure preserves output and locks first operational error;
 * invalid API arguments do not start execution. Cancelled eval -> INVALID_STATE.
 * O(N * comparison cost) per IN probe; O(N + payload) retained work bounded by
 * WORK/MATERIALIZED_ROWS/steps, no implicit spill or source replay. */
turbodb_status_t orm_tidesdb_sql_subquery_eval(orm_sql_subquery *run,
    const turbodb_value_t *probe, turbodb_value_t *out, turbodb_error_t *error);
/* Idempotent cancellation; preserves first failure. Retains source lease/cache
 * until close. No source callbacks, and cannot resume the cancelled execution. */
turbodb_status_t orm_tidesdb_sql_subquery_cancel(orm_sql_subquery *run, turbodb_error_t *error);
/* NULL/empty is a no-op. Close before destroying input or ending its budget;
 * releases Scan, cached payloads and metadata, never commits a transaction. */
turbodb_status_t orm_tidesdb_sql_subquery_close(orm_sql_subquery *run, turbodb_error_t *error);
#endif

#ifndef ORM_TIDESDB_SQL_WINDOW_H
#define ORM_TIDESDB_SQL_WINDOW_H
#include "scan.h"
#include "aggregate.h"
#define ORM_SQL_WINDOW_MAX_BUCKETS (UINT64_C(1)<<63)
#define ORM_SQL_WINDOW_MAX_OFFSET ORM_SQL_WINDOW_MAX_BUCKETS
#define ORM_SQL_WINDOW_MAX_NTH ((uint64_t)INT64_MAX)

typedef enum orm_sql_window_kind {
  ORM_SQL_ROW_NUMBER, ORM_SQL_RANK, ORM_SQL_DENSE_RANK,
  ORM_SQL_PERCENT_RANK, ORM_SQL_CUME_DIST, ORM_SQL_NTILE, ORM_SQL_LAG, ORM_SQL_LEAD,
  ORM_SQL_FIRST_VALUE, ORM_SQL_LAST_VALUE, ORM_SQL_NTH_VALUE,
  ORM_SQL_WINDOW_COUNT_ALL, ORM_SQL_WINDOW_COUNT_VALUE, ORM_SQL_WINDOW_MIN,
  ORM_SQL_WINDOW_MAX, ORM_SQL_WINDOW_SUM, ORM_SQL_WINDOW_AVG,
  ORM_SQL_WINDOW_VAR_POP, ORM_SQL_WINDOW_VAR_SAMP, ORM_SQL_WINDOW_STDDEV_POP, ORM_SQL_WINDOW_STDDEV_SAMP,
  ORM_SQL_WINDOW_BIT_AND, ORM_SQL_WINDOW_BIT_OR, ORM_SQL_WINDOW_BIT_XOR
} orm_sql_window_kind;
static inline bool orm_sql_window_offset_kind(orm_sql_window_kind kind) {
  return kind == ORM_SQL_LAG || kind == ORM_SQL_LEAD;
}
static inline bool orm_sql_window_aggregate_kind(orm_sql_window_kind kind) {
  return kind >= ORM_SQL_WINDOW_COUNT_ALL && kind <= ORM_SQL_WINDOW_BIT_XOR;
}
static inline orm_sql_aggregate_kind orm_sql_window_reduction_kind(orm_sql_window_kind kind) {
  return (orm_sql_aggregate_kind)(kind-ORM_SQL_WINDOW_COUNT_ALL);
}
static inline bool orm_sql_window_frame_kind(orm_sql_window_kind kind) {
  return kind == ORM_SQL_FIRST_VALUE || kind == ORM_SQL_LAST_VALUE || kind == ORM_SQL_NTH_VALUE || orm_sql_window_aggregate_kind(kind);
}
static inline bool orm_sql_window_value_kind(orm_sql_window_kind kind) {
  return orm_sql_window_offset_kind(kind) || (orm_sql_window_frame_kind(kind) && kind != ORM_SQL_WINDOW_COUNT_ALL);
}
typedef enum orm_sql_frame_unit { ORM_SQL_FRAME_DEFAULT, ORM_SQL_FRAME_ROWS, ORM_SQL_FRAME_RANGE } orm_sql_frame_unit;
typedef enum orm_sql_frame_boundary_kind {
  ORM_SQL_BOUND_UNBOUNDED_PRECEDING, ORM_SQL_BOUND_PRECEDING, ORM_SQL_BOUND_CURRENT_ROW,
  ORM_SQL_BOUND_FOLLOWING, ORM_SQL_BOUND_UNBOUNDED_FOLLOWING
} orm_sql_frame_boundary_kind;
enum { ORM_SQL_FRAME_BOUNDARIES=2 };
typedef struct orm_sql_frame_boundary {
  orm_sql_frame_boundary_kind kind;
  turbodb_value_t distance; /* Validated nonnegative scalar, no borrowed payload. */
} orm_sql_frame_boundary;
typedef struct orm_sql_window_frame {
  orm_sql_frame_unit unit;
  orm_sql_frame_boundary boundaries[ORM_SQL_FRAME_BOUNDARIES];
} orm_sql_window_frame;

typedef struct orm_sql_window_spec {
  orm_sql_window_kind kind;
  const size_t *partitions;
  size_t partition_count;
  const orm_sql_scan_order *orders; /* Only column slots, no expressions. */
  size_t order_count;
  uint64_t buckets; /* NTILE: 1..2^63; other functions: zero. */
  uint64_t offset; /* LAG/LEAD: 0..2^63; NTH_VALUE: 1..INT64_MAX; otherwise zero. */
  size_t value_slot, default_slot; /* Precomputed input slots, never output slots. */
  bool has_default;
  orm_sql_window_frame frame; /* DEFAULT resolves from order_count at open. */
} orm_sql_window_spec;

/* Private type join for LAG/LEAD. Exact same kinds or one static NULL only;
 * supports carried TEXT/BLOB without coercion. Missing default is NULL type.
 * Leaves out unchanged on invalid/unsupported input, allocates nothing. */
turbodb_status_t orm_sql_window_offset_type(orm_sql_type value, orm_sql_type fallback,
    orm_sql_type *out, turbodb_error_t *error);

/* Private stage, not installed. One synchronous owner, zero initialized and
 * stable through close. Owns copied spec/types, sorting Scan and immutable row
 * snapshots. Borrows input descriptor/context/types through close. Output
 * appends I64 ranks/buckets, DOUBLE distributions or nullable value results.
 * Byte results borrow this owner's immutable target payload.
 * SELECT owns key binding and orders this stage after WHERE/GROUP/HAVING. */
typedef struct orm_sql_window {
  orm_tidesdb_sql_budget *budget;
  orm_sql_scan scan;
  orm_sql_rows rows;
  orm_sql_row_source source;
  vec_t orders, types;
  size_t order_bytes, type_bytes, metadata_bytes;
  size_t partition_count, input_columns, position;
  orm_sql_window_kind kind;
  uint64_t buckets;
  uint64_t offset;
  size_t value_slot, default_slot;
  bool has_default;
  orm_sql_window_frame frame;
  orm_sql_scan_state state;
  turbodb_error_t failure;
  bool prepared, evaluating;
} orm_sql_window;

/* Copies spec at open; no reads. Input must have positive width. All keys must
 * be numeric/BOOL/NULL; carried TEXT/BLOB payloads are deep copied. Empty input
 * still validates schema/spec. Failed construction preserves empty out and
 * restores new WORK/source admission. LAG/LEAD result uses a target value slot
 * or the current-row default slot (NULL if omitted); target NULL is respected.
 * Slot kinds must agree or one must be static NULL; there is no coercion.
 * FIRST/LAST/NTH use value_slot and the frame; NTH uses offset=1..INT64_MAX.
 * Empty/short frames return NULL, including for nonnullable input. Default RANGE
 * ends at current peers with ORDER, or partition end without ORDER. ROWS accepts
 * I64/U64 distances; RANGE also finite DOUBLE and requires one numeric order key
 * for distance bounds. Explicit frame directions are validated even for ranks.
 * Aggregate windows use aggregate_type and the shared scalar reduction: COUNT_ALL
 * ignores value_slot (must be zero), other aggregates require one value slot.
 * They forbid defaults and offsets, and do not consume GROUPS budget.
 * Work and plan shape use shared limits.
 * Example: spec={.kind=ORM_SQL_RANK,.orders=&order,.order_count=1}; open(source,
 * &spec,&run,error), next(&run,&row,error), close(&run,error). */
turbodb_status_t orm_tidesdb_sql_window_open(orm_sql_row_source *source,
    const orm_sql_window_spec *spec, orm_sql_window *out, turbodb_error_t *error);
/* First next materializes and computes complete partitions before publication.
 * Rows/sort scratch/copies charge shared WORK/MATERIALIZED_ROWS/steps; only the
 * input charges physical I/O. Preparation errors publish no prefix; delivery
 * steps may exhaust after earlier successful rows. Errors preserve out and
 * lock first error without retry. DONE/CANCELLED are idempotent. Output borrows until next,
 * cancel or close. Window sorting promises no final SQL order. NULLs/signed
 * zeros are peers. No ORDER means all partition rows are peers.
 * O(N*(K+O)*log(N)+N*(K+O+C+payload)) time, O(N*(C+K+O)+payload) storage;
 * numeric RANGE endpoints add O(N*log(N)) comparisons, without frame storage.
 * Aggregate fixed-start frames reuse their fold as the end grows; COUNT_ALL uses
 * frame width. Moving-start folds cost the sum of new frame widths, worst O(N^2),
 * and charge each row to the execution budget. O(1) extra reduction state.
 * Reentry and an active downstream consumer give BUSY without mutation. */
turbodb_status_t orm_tidesdb_sql_window_next(orm_sql_window *run,
    orm_sql_scan_row *out, turbodb_error_t *error);
/* Stable borrowed source until close; NULL for unopened run. Consumers must
 * close before direct next/cancel/close. Chains preserve prior result columns. */
orm_sql_row_source *orm_tidesdb_sql_window_source(orm_sql_window *run);
/* No drain/new reads. Keeps ERROR/DONE and retains work/input lease to close. */
turbodb_status_t orm_tidesdb_sql_window_cancel(orm_sql_window *run, turbodb_error_t *error);
/* Idempotent for empty run. BUSY preserves owner; otherwise closes input Scan
 * and all owned storage without charging steps or ending the transaction. */
turbodb_status_t orm_tidesdb_sql_window_close(orm_sql_window *run, turbodb_error_t *error);
#endif

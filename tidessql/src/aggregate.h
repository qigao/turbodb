#ifndef ORM_TIDESDB_SQL_AGGREGATE_H
#define ORM_TIDESDB_SQL_AGGREGATE_H
#include "scan.h"

typedef enum orm_sql_aggregate_kind {
  ORM_SQL_COUNT_ALL, ORM_SQL_COUNT_VALUE, ORM_SQL_MIN, ORM_SQL_MAX, ORM_SQL_SUM, ORM_SQL_AVG,
  ORM_SQL_VAR_POP, ORM_SQL_VAR_SAMP, ORM_SQL_STDDEV_POP, ORM_SQL_STDDEV_SAMP,
  ORM_SQL_BIT_AND, ORM_SQL_BIT_OR, ORM_SQL_BIT_XOR
} orm_sql_aggregate_kind;
static inline bool orm_sql_aggregate_moment_kind(orm_sql_aggregate_kind kind) {
  return kind >= ORM_SQL_VAR_POP && kind <= ORM_SQL_STDDEV_SAMP;
}
static inline bool orm_sql_aggregate_bit_kind(orm_sql_aggregate_kind kind) {
  return kind >= ORM_SQL_BIT_AND && kind <= ORM_SQL_BIT_XOR;
}

/* Shared pure type rule for Binder and reduction. input may be NULL only for
 * COUNT_ALL; failure preserves out. SUM/AVG require DOUBLE or NULL: exact-value
 * arguments need DECIMAL support and are rejected instead of losing precision.
 * Statistical moments accept numeric/BOOL/NULL and return nullable DOUBLE.
 * Numeric bit aggregates accept those kinds and return nonnullable U64;
 * TEXT conversion and binary-string evaluation are unsupported. */
turbodb_status_t orm_tidesdb_sql_aggregate_type(orm_sql_aggregate_kind kind,
    const orm_sql_type *input, orm_sql_type *out, turbodb_error_t *error);

typedef struct orm_sql_reduction {
  turbodb_value_t value;
  uint64_t count; /* AVG/moments non-NULL inputs; AVG value remains its sum. */
  double mean, squared_deviation; /* Moments own these until result publication. */
} orm_sql_reduction;

/* Private scalar fold shared by groups and frames. Caller supplies a supported
 * kind and validated same-kind input admitted by aggregate_type; NULL input
 * pointer only for COUNT_ALL. No allocation, budget charge or borrowed bytes:
 * callers charge each admitted input/result and own the reduction state.
 * COUNT starts at I64 zero; bits at U64 AND all-ones or OR/XOR zero; others at NULL.
 * add preserves state on overflow;
 * result does not mutate state. AVG divides the result copy; empty COUNT is zero,
 * while empty bits retain their identity and other empty results are NULL.
 * Bit DOUBLE input uses rint then checked I64-to-U64; signed values wrap modulo 2^64.
 * Moments use count/mean/squared_deviation;
 * population singleton is zero and sample count < 2 gives NULL.
 * Example: state=orm_sql_reduction_begin(ORM_SQL_SUM), add per DOUBLE input,
 * result(kind,&state,&out,error). All errors propagate to the owning stage. */
orm_sql_reduction orm_sql_reduction_begin(orm_sql_aggregate_kind kind);
turbodb_status_t orm_sql_reduction_add(orm_sql_aggregate_kind kind, orm_sql_reduction *state,
    const turbodb_value_t *input, turbodb_error_t *error);
turbodb_status_t orm_sql_reduction_result(orm_sql_aggregate_kind kind, const orm_sql_reduction *state,
    turbodb_value_t *out, turbodb_error_t *error);

typedef struct orm_sql_aggregate_item {
  orm_sql_aggregate_kind kind;
  size_t slot; /* Input column; ignored for COUNT_ALL. */
  size_t argument_count; /* Zero means one; contiguous slots, multiple only for DISTINCT COUNT. */
  bool distinct; /* COUNT/SUM/AVG de-duplicate numeric tuples; MIN/MAX are unchanged. */
} orm_sql_aggregate_item;
static inline size_t orm_sql_aggregate_arguments(const orm_sql_aggregate_item *item) {
  return item->argument_count ? item->argument_count : 1;
}
static inline bool orm_sql_aggregate_distinct(const orm_sql_aggregate_item *item) {
  return item->distinct && item->kind != ORM_SQL_MIN && item->kind != ORM_SQL_MAX;
}

typedef struct orm_sql_aggregate_spec {
  const size_t *keys; /* Group-key input columns, in output order. */
  size_t key_count;
  const orm_sql_aggregate_item *items;
  size_t item_count;
} orm_sql_aggregate_spec;

/* Private execution stage used by the SQL SELECT Binder, not installed.
 * One synchronous owner; zero initialize and keep at a stable address through
 * close. Owns sorting scan, copied spec/types and scalar group output. Borrows
 * source descriptor/context/types through close. pending borrows the owned
 * scan output until its next pull; callers must not advance that scan directly. */
typedef struct orm_sql_aggregate {
  orm_tidesdb_sql_budget *budget;
  orm_sql_scan scan;
  orm_sql_row_source source;
  vec_t keys, items, types, output, counts, moments;
  size_t key_bytes, item_bytes, type_bytes, output_bytes, metadata_bytes;
  size_t count_bytes, moment_bytes;
  uint64_t groups;
  vec_t distinct_slots, distinct_input, distinct_order;
  size_t distinct_slot_bytes, distinct_input_bytes, distinct_order_bytes;
  orm_sql_rows distinct_rows;
  const turbodb_value_t *pending;
  bool eof;
  orm_sql_scan_state state;
  turbodb_error_t failure;
} orm_sql_aggregate;

/* Validates even for empty input; source must have at least one column, spec at
 * least one key or aggregate. Copies spec; caller may release its arrays after
 * success. Keys/MIN/MAX accept numeric, BOOL, NULL, never TEXT/BLOB. COUNT_VALUE
 * accepts any validated scalar. Counts return nonnullable I64, extrema nullable
 * input kind. SUM/AVG accept DOUBLE or NULL, return nullable input kind; exact
 * numeric inputs require DECIMAL and are unsupported. AVG owns one fixed count
 * vector, bounded by item_count and WORK. Statistical moments additionally own
 * item_count scalar states under WORK, with DOUBLE output for numeric/BOOL/NULL.
 * Numeric bit aggregates return nonnullable U64 from numeric/BOOL/NULL input;
 * no additional state vector. DOUBLE conversion must fit the rounded I64 range.
 * DISTINCT COUNT supports numeric/BOOL/NULL tuples, excluding any NULL input.
 * DISTINCT SUM/AVG use one DOUBLE/NULL slot; MIN/MAX DISTINCT is a no-op.
 * Other DISTINCT kinds and byte tuple comparisons are unsupported.
 * No reads at open; only the input source
 * owns physical read charging.
 * Construction failure leaves out empty and restores new source admission/work;
 * rejecting an already active source leaves its existing lease unchanged. */
turbodb_status_t orm_tidesdb_sql_aggregate_open(orm_sql_row_source *source,
    const orm_sql_aggregate_spec *spec, orm_sql_aggregate *out, turbodb_error_t *error);

/* Returns group keys followed by aggregates, borrowed until next/cancel/close.
 * NULL and signed-zero keys group together. COUNT ignores NULL except COUNT_ALL;
 * extrema/SUM/AVG/bits ignore NULL. Empty global input emits one identity/NULL row, keyed
 * input none. Ordinary SUM adds in source order, AVG divides by its nonnull count once
 * before publication. Nonfinite inputs fail TYPE_ERROR; nonfinite intermediate
 * sums or count overflow fail LIMIT_EXCEEDED, even if a later mean could fit.
 * Moments use an online mean and squared deviations, ignoring NULL; sample
 * results require two inputs. Nonfinite moment intermediates fail before publishing.
 * Empty/all-NULL bits use U64 AND all-ones or OR/XOR zero. Bit DOUBLE conversion
 * overflow fails LIMIT_EXCEEDED before changing the scalar; I64 wraps modulo 2^64.
 * DISTINCT snapshots only the current group's numeric argument tuples, stable
 * sorts and folds unique non-NULL values before result publication, then frees
 * rows/sort records. Ascending distinct DOUBLE order is independent of ordinary
 * source order. Snapshots and sort charge shared rows/work/steps; no spill.
 * Keyed input sorts on first next, then folds adjacent groups; no-key input folds
 * incrementally with constant workspace unless DISTINCT needs snapshots.
 * No order promised to SQL callers.
 * GROUPS charges every admitted group (including empty global), retained until
 * close; sort snapshots charge MATERIALIZED_ROWS. Steps/work are bounded.
 * O(K*N*log(N)+N*(K+A)) keyed time; O(N*A) global time. Storage is owned by scan
 * plus O(K+A) metadata/output. Failures preserve out, lock error, may follow
 * previously emitted groups; repeated next never retries. Terminals idempotent.
 * BUSY if a downstream scan holds source_view's lease. */
turbodb_status_t orm_tidesdb_sql_aggregate_next(orm_sql_aggregate *run,
    orm_sql_scan_row *out, turbodb_error_t *error);

/* Borrowed derived row source. Types/context stay stable to close. Its next
 * delegates physical reads to the input, never charges derived rows as I/O.
 * A downstream scan claims its active lease; direct next/cancel/close then
 * return BUSY. Close downstream first. NULL for an unopened run. */
orm_sql_row_source *orm_tidesdb_sql_aggregate_source(orm_sql_aggregate *run);
/* Before first next, cancel reads nothing; retains errors/DONE. Close owns all
 * release work, including EOF/cancel/error, but never closes input or transaction.
 * Close is idempotent for NULL/empty. Example: aggregate_open, scan_open_source
 * on aggregate_source, scan_next, scan_close, aggregate_close, input close. */
turbodb_status_t orm_tidesdb_sql_aggregate_cancel(orm_sql_aggregate *run, turbodb_error_t *error);
turbodb_status_t orm_tidesdb_sql_aggregate_close(orm_sql_aggregate *run, turbodb_error_t *error);
#endif

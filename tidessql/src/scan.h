#ifndef ORM_TIDESDB_SQL_SCAN_H
#define ORM_TIDESDB_SQL_SCAN_H
#include "expr.h"
#include "rows.h"

typedef struct orm_sql_memory_source {
  const turbodb_value_t *values; /* row-major, rows * columns values */
  size_t rows, columns;
  const orm_sql_type *types;
} orm_sql_memory_source;

/* Private synchronous source. next returns one row or NULL for EOF, unchanged
 * on error. Source charges read rows/physical bytes before decoding, and bounds
 * its own workspace. A row has columns values, borrowed until next/source close.
 * Keep this descriptor/context/types stable through scan close. One active scan
 * only; active is managed by scan open/close, never by the source callback. */
typedef struct orm_sql_row_source {
  orm_tidesdb_sql_budget *budget;
  const orm_sql_type *types;
  size_t columns;
  void *context;
  turbodb_status_t (*next)(void *context, const turbodb_value_t **row, turbodb_error_t *error);
  bool active;
} orm_sql_row_source;

typedef struct orm_sql_scan_expression {
  orm_sql_expr *program; /* NULL selects the ordinary source-column slot. */
  const size_t *slots; /* columns followed by parameters */
  size_t count;
  const size_t *query_slots; /* Indices into spec.queries, in program order. */
  size_t query_count;
} orm_sql_scan_expression;

typedef struct orm_sql_scan_spec {
  orm_sql_expr *filter; /* Optional; must use the scan's active budget. */
  const size_t *filter_slots; /* columns followed by parameters */
  size_t filter_count;
  const size_t *projection;
  size_t projection_count;
  uint64_t offset, limit; /* Explicit limit, UINT64_MAX allowed. */
  const turbodb_value_t *parameters;
  const orm_sql_type *parameter_types;
  size_t parameter_count;
  const orm_sql_scan_expression *expressions; /* Optional projection_count entries. */
  const struct orm_sql_scan_order *orders;
  size_t order_count;
  bool distinct; /* Numeric/BOOL/NULL output tuples; checked even for LIMIT 0. */
  orm_sql_expr_query_source *const *queries;
  size_t query_count;
  const size_t *filter_query_slots;
  size_t filter_query_count;
  orm_sql_evaluation evaluation;
  /* Optional source->columns result types, borrowed unchanged through close.
   * Ordinary projection and order slots may promote numeric values to DOUBLE;
   * expression programs and filters keep their own types. No narrowing. */
  const orm_sql_type *coerce_types;
} orm_sql_scan_spec;

typedef struct orm_sql_scan_order {
  size_t slot;
  orm_sql_scan_expression expression;
  bool descending;
} orm_sql_scan_order;

typedef enum orm_sql_scan_state {
  ORM_SQL_SCAN_CLOSED, ORM_SQL_SCAN_OPEN, ORM_SQL_SCAN_ROW,
  ORM_SQL_SCAN_DONE, ORM_SQL_SCAN_ERROR, ORM_SQL_SCAN_CANCELLED
} orm_sql_scan_state;

typedef struct orm_sql_scan_row {
  orm_sql_scan_state state; /* ROW, DONE or CANCELLED on successful next */
  const turbodb_value_t *values;
  size_t count;
} orm_sql_scan_row;

/* Private fields. Zero initialize before open; one synchronous owner. Source
 * values/payloads remain immutable and borrowed through close. Schema and spec
 * arrays are needed only by open, except coerce_types borrowed through close.
 * Filter and computed projection programs stay
 * at stable addresses through close. Their scalar byte results borrow source,
 * parameter snapshots, program literals or leased query sources through
 * next/cancel/close. Query descriptors/context stay at stable addresses through
 * close; their pointer arrays and all mappings are needed only during open.
 * Sources are shared synchronously; close consumers before query sources.
 * ORDER BY/DISTINCT retain owned snapshots of matching source rows and numeric keys/outputs;
 * payloads are copied before pulling again. Native I/O belongs exclusively to
 * an optional borrowed pull source. No public ORM connection ownership. */
typedef struct orm_sql_scan {
  orm_tidesdb_sql_budget *budget;
  orm_sql_memory_source source;
  orm_sql_row_source *pull;
  orm_sql_expr_run expression;
  vec_t slots, inputs, projection, output, parameters, parameter_payload;
  size_t slot_bytes, input_bytes, projection_bytes, output_bytes, metadata_bytes;
  size_t parameter_bytes, payload_bytes;
  size_t position;
  uint64_t offset_left, limit_left;
  orm_sql_scan_state state;
  turbodb_error_t failure;
  vec_t orders, sorted;
  orm_sql_rows snapshots;
  size_t order_bytes, sorted_bytes;
  size_t sorted_position;
  bool sorted_ready;
  bool distinct;
  size_t sorted_count;
  bool evaluating;
  const orm_sql_type *coerce_types;
} orm_sql_scan;

/* Copies mappings and preallocates evaluators. columns/projection_count must be
 * positive and no greater than the statement plan-node limit. Validates even
 * for LIMIT 0/empty source. All parameters are validated and deep copied; their
 * input arrays/types/payloads are needed only through open. Parameter copies
 * consume work/step budget, not row-read counters. The combined column/parameter
 * count must fit the plan limit. Failure leaves output empty and work restored.
 * Source address range and row-byte arithmetic are checked, not memory ownership.
 * Caller guarantees the complete source array/payloads are accessible. */
turbodb_status_t orm_tidesdb_sql_scan_open(const orm_sql_memory_source *source,
    const orm_sql_scan_spec *spec, orm_tidesdb_sql_budget *budget,
    orm_sql_scan *out, turbodb_error_t *error);
/* Same execution contract, borrowing a preconfigured row source. Does not call
 * next during open; LIMIT 0/cancelled scans never pull. Source lifetime ends only
 * after scan_close. The source retains ownership of its native handles. */
turbodb_status_t orm_tidesdb_sql_scan_open_source(orm_sql_row_source *source,
    const orm_sql_scan_spec *spec, orm_tidesdb_sql_budget *budget,
    orm_sql_scan *out, turbodb_error_t *error);

/* Unordered ALL scans allocate no own workspace after open; pull and query
 * sources have their own bounded allocation contracts. Queries run only when
 * their expression instruction is reached, never while collecting row inputs.
 * Ordered/DISTINCT scans materialize on first next, admit every
 * matching row to WORK/MATERIALIZED_ROWS, stable-sort with charged CSTL scratch,
 * then apply offset/limit. Numeric keys place NULL first ASC, last DESC. Registry
 * growth charges simultaneous old/new capacity; each snapshot owns its payload.
 * Copy/evaluation/sort errors publish no sorted prefix. Projection errors retain
 * the ordinary per-row error contract for ALL. DISTINCT evaluates and saves all
 * numeric/BOOL/NULL outputs, sorts full tuples, compacts unique records, then
 * sorts by user keys and applies pagination. NULLs and signed zeros compare equal.
 * Output copying consumes steps and may fail at delivery. LIMIT 0/cancel-before-next reads nothing.
 * Consumes rejected rows until one match or terminal.
 * Only TRUE matches; OFFSET counts matches (unique tuples for DISTINCT). ALL
 * computed outputs execute after OFFSET, inside LIMIT; all columns must succeed before publishing a row.
 * Charges all scanned rows/bytes,
 * including rejects. Borrowed output expires at next/cancel/close. Failure keeps
 * *out unchanged and locks the first error; repeated next does no more work.
 * DONE/CANCELLED are idempotent and return empty output. Unordered ALL: O(rows * expression and
 * projection work), memory independent of row count. Ordered: O(S*M*log(M))
 * comparisons plus filter/key work, O(M*(columns+S)+payload) retained storage;
 * M is matching rows, S is key count. DISTINCT adds O(P*M*log(M)) comparisons
 * and O(M*P) values, P being output width. All retained storage releases on close.
 * Outer ORM result limits remain the adapter's responsibility. */
turbodb_status_t orm_tidesdb_sql_scan_next(orm_sql_scan *run,
    orm_sql_scan_row *out, turbodb_error_t *error);
/* next/cancel/close return BUSY during a next callback; no concurrent access.
 * Cancel preserves an existing ERROR/DONE; close releases all resources without
 * destroying the borrowed program or ending the statement/native transaction. */
turbodb_status_t orm_tidesdb_sql_scan_cancel(orm_sql_scan *run, turbodb_error_t *error);
turbodb_status_t orm_tidesdb_sql_scan_close(orm_sql_scan *run, turbodb_error_t *error);
#endif

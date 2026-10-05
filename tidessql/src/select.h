#ifndef ORM_TIDESDB_SQL_SELECT_H
#define ORM_TIDESDB_SQL_SELECT_H
#include "scan.h"
#include "schema.h"
#include "aggregate.h"
#include "from.h"
#include "window.h"

typedef struct orm_sql_select_column {
  char name[ORM_SQL_SELECT_NAME_BYTES + 1];
  orm_sql_type type;
} orm_sql_select_column;
typedef struct orm_sql_select_bound {
  uint64_t literal;
  size_t parameter;
  bool is_parameter;
} orm_sql_select_bound;

/* Private, zero initialized, single synchronous owner. After bind, owns all
 * metadata/code; document/schema may be destroyed. Keep at a stable address
 * while runs exist. Budget must remain active until every run and plan closes. */
typedef struct orm_sql_select {
  orm_tidesdb_sql_budget *budget;
  orm_sql_expr filter;
  vec_t types, columns, projection, slots, parameter_types, parameter_offsets;
  vec_t computations, expressions;
  size_t type_bytes, column_bytes, projection_bytes, slot_bytes, metadata_bytes;
  size_t parameter_type_bytes, parameter_offset_bytes;
  size_t computation_bytes, expression_bytes;
  size_t active_runs;
  orm_sql_select_bound offset, limit;
  vec_t orders, order_computations;
  size_t order_bytes, order_computation_bytes;
  bool distinct;
  bool grouped;
  bool correlated;
  orm_sql_expr pre_filter;
  vec_t pre_slots, group_keys, aggregate_items, group_computations, pre_projection, pre_expressions, pre_types;
  size_t pre_slot_bytes, group_key_bytes, aggregate_item_bytes, group_computation_bytes;
  size_t pre_projection_bytes, pre_expression_bytes, pre_type_bytes;
  vec_t key_computations;
  size_t key_computation_bytes;
  vec_t query_slots, pre_query_slots;
  size_t query_slot_bytes, pre_query_slot_bytes;
  vec_t windows, window_computations, window_projection, window_expressions, window_types, window_orders, window_keys;
  size_t window_bytes, window_computation_bytes, window_projection_bytes, window_expression_bytes, window_type_bytes;
  size_t window_order_bytes, window_key_bytes;
  size_t window_base_columns;
  vec_t window_frames;
  size_t window_frame_bytes;
} orm_sql_select;
typedef struct orm_sql_select_run {
  orm_sql_select *program;
  orm_sql_scan scan;
  vec_t group_run;
  size_t group_run_bytes;
  vec_t window_run;
  size_t window_run_bytes;
} orm_sql_select_run;

/* Private SELECT over a FROM plan bound from the same live document. Completes
 * projection/filter/group/order/limit binding using the plan's logical column
 * order and nullable output types. Owns its metadata/code after return; neither
 * document nor FROM plan is retained. Duplicate output names remain rejected;
 * table.* expands only that relation, plain * expands logical SQL column order.
 * Caller supplies the FROM executor's logical row source to select_open_source;
 * this entry only binds SELECT/default or TRADITIONAL EXPLAIN SELECT. */
turbodb_status_t orm_tidesdb_sql_select_bind_from(const sqlparser_document *document,
    const orm_sql_from *from, size_t max_depth, orm_sql_select *out, turbodb_error_t *error);

/* Private compound-query binding. SELECT targets scope.root; tail binds only
 * ORDER/LIMIT over already resolved output columns, copying names and types.
 * Same lifetime/error/budget contract as other SELECT binders. Tail schema has
 * a valid internal name but no table qualifier visible to expressions.
 * scope.queries enables trusted dependencies in all expression stages. Nested
 * query bodies are excluded from aggregate/alias discovery; their own binding
 * remains the dependency owner's responsibility. Grouped/window expression consumers
 * provide dependency capture hooks with source-column-to-input-slot mappings;
 * WHERE, key and aggregate-argument consumers preserve original row layouts.
 * No implicit correlation. */
turbodb_status_t orm_tidesdb_sql_select_bind_at(const orm_sql_query_scope *scope,
    const orm_sql_table_schema *schema, const orm_sql_from *from, orm_sql_select *out, turbodb_error_t *error);
turbodb_status_t orm_tidesdb_sql_select_bind_tail(const orm_sql_query_scope *scope,
    const orm_sql_table_schema *schema, sqlparser_list orders, sqlparser_id limit,
    orm_sql_select *out, turbodb_error_t *error);

/* Bind exactly one MySQL SELECT over the supplied table schema. Supports column
 * projection/aliases, table aliases, qualified names, * and table.*, existing
 * predicate subset, literal nonnegative LIMIT/OFFSET. Names are case-sensitive
 * ASCII identifiers of at most NAME_BYTES; backticks accepted, comments inside
 * qualified names and string-quoted aliases unsupported. Duplicate output names
 * rejected. Computed projections accept the existing scalar expression subset
 * and require explicit aliases; standalone ORDER BY keys resolve output names
 * before source columns. Numeric/BOOL/NULL ORDER BY accepts source expressions,
 * qualified source columns, standalone output aliases and positive output
 * positions, with independent ASC/DESC. TEXT/BLOB ordering, signed numeric
 * keys and output aliases inside compound keys are explicitly unsupported.
 * DISTINCT accepts numeric/BOOL/NULL output tuples. DISTINCT ORDER BY must
 * equal an output expression or refer only to source columns selected as plain
 * columns; compares bound programs/slots without algebraic rewriting.
 * GROUP BY accepts numeric/BOOL/NULL columns or supported scalar expressions,
 * standalone output aliases and positive output positions. Source names take
 * precedence over aliases. Complete or nested SELECT/HAVING/ORDER and window
 * value/key expressions equal to a group
 * key compile to that key's slot; no algebraic or functional-dependency inference.
 * COUNT(*)/COUNT(expr), numeric/BOOL/NULL MIN/MAX(expr), and DOUBLE/NULL
 * SUM/AVG(expr) work in projections, HAVING and ordering. Exact SUM/AVG inputs
 * require DECIMAL and remain unsupported; no implicit numeric conversion.
 * HAVING resolves group columns, calls and direct key/aggregate output
 * aliases. Ordinary COUNT DISTINCT accepts numeric/BOOL/NULL tuples; DOUBLE
 * SUM/AVG DISTINCT accept one argument, and MIN/MAX DISTINCT are unchanged.
 * Rejects ungrouped columns, grouped stars, nested aggregates,
 * aliases inside compound grouping keys and other compound HAVING aliases.
 * Noncolumn key subexpressions do not make their source columns accessible.
 * ROW_NUMBER/RANK/DENSE_RANK/PERCENT_RANK/CUME_DIST/NTILE/LAG/LEAD support inline OVER
 * in SELECT/final ORDER BY. Keys bind numeric/BOOL/NULL expressions in the
 * original row or group scope; named windows and supported frames are described
 * below. No same-SELECT aliases, nested windows or DISTINCT window aggregates.
 * WHERE/GROUP/HAVING reject windows.
 * NTILE accepts positive integer literals/markers through 2^63; validates
 * runtime values at open/EXPLAIN even when LIMIT/cardinality prunes results.
 * LAG/LEAD offsets accept integer literals/markers in 0..2^63 (default 1).
 * Value/default are precomputed row/group expressions, same kind or static
 * NULL only; omitted default is NULL, missing target uses the current default.
 * Offset values may carry TEXT/BLOB; only partition/order keys are numeric.
 * Windows follow WHERE/GROUP/HAVING and precede DISTINCT/final order/page.
 * No joins.
 * This convenience entry binds zero parameters.
 * All clauses checked even for LIMIT 0. No Catalog lookup/schema inference.
 * Failure leaves empty output and refunds work, retains consumed counters.
 * O((computations + 1) * nodes + schema^2 + outputs^2 + references * (schema + log(references))) time,
 * including expression input lookup; bounded
 * O((computations + 1) * document nodes + schema + outputs) workspace;
 * comparisons consume steps. Group binding adds O(nodes^2 + aggregates^2 *
 * expression size + keys * outputs * expression size) time for alias scope
 * and equivalent key/call checks, charged
 * to steps. Query-scope isolation adds O(nested queries * nodes) time and
 * O(nodes) scratch when nested bodies exist, charged to steps/work.
 * Window admission adds O(nodes^2), equivalence O(windows^2 * key expression
 * size) time; hidden key/result slots add O(keys + windows + schema) storage.
 * The existing node/plan/work limits bound all stages. */
turbodb_status_t orm_tidesdb_sql_select_bind(const sqlparser_document *document,
    const orm_sql_table_schema *schema, size_t max_depth,
    orm_tidesdb_sql_budget *budget, orm_sql_select *out, turbodb_error_t *error);

/* Same contract, with explicit parameter types in SQL source-offset order.
 * Exact marker count required (SQL_ERROR on mismatch); descriptors copied.
 * Projection, WHERE, group keys, aggregate arguments, HAVING, ORDER BY and standalone
 * LIMIT/OFFSET markers supported. Pagination types must
 * be I64/U64; runtime NULL/negative values rejected. No implicit conversions.
 * Sorting adds bounded O(parameters * log(parameters)) time/work steps and
 * O(parameters) scratch space, charged before CSTL sort. */
turbodb_status_t orm_tidesdb_sql_select_bind_parameters(const sqlparser_document *document,
    const orm_sql_table_schema *schema, const orm_sql_type *parameter_types, size_t parameter_count,
    size_t max_depth, orm_tidesdb_sql_budget *budget, orm_sql_select *out, turbodb_error_t *error);

/* Explicit private EXPLAIN binding, same ownership/type contracts; accepts one
 * default/TRADITIONAL MySQL EXPLAIN of SELECT only. Does not execute its child. */
turbodb_status_t orm_tidesdb_sql_select_bind_explain(const sqlparser_document *document,
    const orm_sql_table_schema *schema, const orm_sql_type *parameter_types, size_t parameter_count,
    size_t max_depth, orm_tidesdb_sql_budget *budget, orm_sql_select *out, turbodb_error_t *error);
/* Read-only parameter/page validation for plan inspection. No copies, row reads
 * or expression evaluation; outputs unchanged on failure. */
turbodb_status_t orm_tidesdb_sql_select_validate_parameters(const orm_sql_select *program,
    const turbodb_value_t *parameters, size_t count, uint64_t *offset, uint64_t *limit, turbodb_error_t *error);

/* Read-only owned output metadata, valid through destroy; NULL on invalid plan
 * or ordinal. No allocation. Name is NUL terminated. */
const orm_sql_select_column *orm_tidesdb_sql_select_column_at(
    const orm_sql_select *program, size_t ordinal);

/* Rows are immutable row-major values in bound schema order, borrowed through
 * close. Open checks shape and preallocates the scan. Use scan_next/cancel on
 * &run.scan; release only via select_close. Output row expires at next/cancel/
 * close. Call bind, select_open, scan_next/cancel, select_close, then destroy in
 * that order and check each status. An open run blocks plan destruction, including
 * plans with no WHERE; terminal runs still require close. Grouped runs own the
 * input scan, aggregate and final scan; close releases them in reverse order.
 * GROUPS and MATERIALIZED_ROWS quotas are shared across these stages; only the
 * original source charges physical reads. LIMIT 0/cancel before next reads none. */
turbodb_status_t orm_tidesdb_sql_select_open(orm_sql_select *program,
    const turbodb_value_t *rows, size_t row_count, orm_sql_select_run *out, turbodb_error_t *error);
/* Same run lifecycle, exact parameter count required (INVALID_ARGUMENT on
 * mismatch). Values/payloads borrow only this call and are deep copied before
 * success. Validates every parameter even for LIMIT 0/empty/dead predicates.
 * Invalid values return TYPE_ERROR; failure leaves an empty run, no rows read,
 * and the plan reusable. Parameter work/steps are charged to the plan budget. */
turbodb_status_t orm_tidesdb_sql_select_open_parameters(orm_sql_select *program,
    const turbodb_value_t *rows, size_t row_count, const turbodb_value_t *parameters, size_t parameter_count,
    orm_sql_select_run *out, turbodb_error_t *error);
/* Private streaming counterpart. Source columns must be in the bound schema
 * order with identical descriptors. Borrows source through select_close, copies
 * parameters, performs no source reads during open. See row_source contract. */
turbodb_status_t orm_tidesdb_sql_select_open_source(orm_sql_select *program,
    orm_sql_row_source *source, const turbodb_value_t *parameters, size_t parameter_count,
    orm_sql_select_run *out, turbodb_error_t *error);
/* Same streaming contract with explicit dependencies supplied in bind_at's
 * registry order. All grouped stages share sources, preserving lazy calls.
 * Close SELECT before dependencies; registry arrays borrow only this call.
 * Ordinary open entries reject programs that require query sources. Reentrant
 * select_close during scan_next returns BUSY without releasing any stage. */
turbodb_status_t orm_tidesdb_sql_select_open_source_queries(orm_sql_select *program,
    orm_sql_row_source *source, const turbodb_value_t *parameters, size_t parameter_count,
    const orm_sql_expr_query_sources *queries, orm_sql_select_run *out, turbodb_error_t *error);
/* Private cardinality consumer for EXISTS planning, not SQL admission. Same
 * source/parameter/close contract; callers observe only scan state, never use
 * its internal row payload as SELECT output or consult SELECT column metadata.
 * Preserves WHERE/group/HAVING/DISTINCT/pagination, drops sorting. Non-DISTINCT
 * also skips projection and aggregate arguments unused by HAVING. DISTINCT
 * keeps tuple evaluation because it determines the number of surviving rows.
 * Bound plans remain immutable and reusable by ordinary SELECT runs. Fixed
 * derived group specs cost O(pre-columns + aggregates) work and
 * O(aggregates * HAVING inputs) charged steps; no reads during open. */
turbodb_status_t orm_sql_select_open_cardinality(orm_sql_select *program,
    orm_sql_row_source *source, const turbodb_value_t *parameters, size_t parameter_count,
    const orm_sql_expr_query_sources *queries, orm_sql_select_run *out, turbodb_error_t *error);
/* Internal query demand. EXISTENCE observes at most one row, preserving LIMIT
 * zero, and can discard DISTINCT when OFFSET is zero. Non-value modes expose
 * only scan state, as with the cardinality entry above. */
turbodb_status_t orm_sql_select_open_demand(orm_sql_select *program,
    orm_sql_row_source *source, const turbodb_value_t *parameters, size_t parameter_count,
    const orm_sql_expr_query_sources *queries, orm_sql_query_demand demand,
    orm_sql_select_run *out, turbodb_error_t *error);
turbodb_status_t orm_tidesdb_sql_select_close(orm_sql_select_run *run, turbodb_error_t *error);
/* NULL/empty is a no-op; BUSY leaves an active plan unchanged. */
turbodb_status_t orm_tidesdb_sql_select_destroy(orm_sql_select *program, turbodb_error_t *error);
#endif

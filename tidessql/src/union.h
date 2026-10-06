#ifndef ORM_TIDESDB_SQL_UNION_H
#define ORM_TIDESDB_SQL_UNION_H
#include "scan.h"

typedef enum orm_sql_union_kind {
  ORM_SQL_UNION_ALL, ORM_SQL_UNION_DISTINCT,
  ORM_SQL_INTERSECT_ALL, ORM_SQL_INTERSECT_DISTINCT,
  ORM_SQL_EXCEPT_ALL, ORM_SQL_EXCEPT_DISTINCT
} orm_sql_union_kind;
enum { ORM_SQL_UNION_INPUTS = 2 };
/* Private complete execution stage, not SQL admission. Stable address and one
 * synchronous owner. Owns scans/types; borrows independent input descriptors,
 * contexts and type arrays through close. Output borrows until next/cancel/close.
 * Downstream closes first, then this run, then the input native owners. */
typedef struct orm_sql_union {
  orm_tidesdb_sql_budget *budget;
  orm_sql_scan inputs[ORM_SQL_UNION_INPUTS], output;
  orm_sql_row_source concat, source;
  vec_t types;
  size_t type_bytes, metadata_bytes, position;
  orm_sql_union_kind kind;
  const turbodb_value_t *pending[ORM_SQL_UNION_INPUTS];
  bool prepared, advance[ORM_SQL_UNION_INPUTS];
} orm_sql_union;
/* Shared bind/runtime type rule: identical kinds, NULL plus the other kind, or
 * DOUBLE with I64/U64; nullability is their union. A pure I64/U64 pair requires
 * DECIMAL and other mixed kinds are UNSUPPORTED; malformed types are TYPE_ERROR.
 * Failure preserves output, no allocation or budget consumption. */
turbodb_status_t orm_tidesdb_sql_union_type(orm_sql_type left, orm_sql_type right,
    orm_sql_type *out, turbodb_error_t *error);
/* Result nullability for a validated set kind; independent of input coercion. */
bool orm_sql_union_nullable(orm_sql_union_kind kind,orm_sql_type left,orm_sql_type right);
/* Shared execution/EXPLAIN admission. Validates shape, types and capacity,
 * charges validation steps; never acquires leases, allocates or calls next. */
turbodb_status_t orm_tidesdb_sql_union_validate(const orm_sql_row_source *left, const orm_sql_row_source *right,
    orm_sql_union_kind kind, turbodb_error_t *error);
/* Internal Compound admission with query-wide common kinds and node nullability.
 * types has left->columns entries, borrowed only for this call. NULL uses the
 * binary merge rule. Rejects narrowed result constraints or unsupported kinds. */
turbodb_status_t orm_sql_union_validate_as(const orm_sql_row_source *left, const orm_sql_row_source *right,
    orm_sql_union_kind kind, const orm_sql_type *types, turbodb_error_t *error);
/* Zero output required. Equal positive widths and shared active budget; source
 * and shape validation even if inputs are empty. No reads during open, both input
 * leases held through close. ALL streams left then right, including duplicates;
 * DISTINCT reuses numeric/BOOL/NULL Scan sort/dedup (TEXT/BLOB UNSUPPORTED).
 * INTERSECT/EXCEPT support numeric/BOOL/NULL only. Both inputs sort complete
 * tuples, with per-input DISTINCT when requested, then merge. ALL multiplicity
 * is min(left,right) for INTERSECT, max(left-right,0) for EXCEPT. NULL and signed
 * DOUBLE zero compare equal. Both sorts finish before the first result.
 * Result nullable is OR for UNION, AND for INTERSECT, left for EXCEPT.
 * Input conversion targets retain input nullability independently of results.
 * Sorting costs O(N log(N) * columns) time/O(N * columns) retained workspace,
 * merging O(N * columns) time; all candidates/work/comparisons charge budget.
 * Pending rows borrow each input Scan until its own next/close; advancement
 * after a published row is deferred until the next pull. No output order promise.
 * No native I/O or row charges here, source owns those. Work/materialized/step
 * limits shared; failure unwinds work/leases, consumed counters remain. */
turbodb_status_t orm_tidesdb_sql_union_open(orm_sql_row_source *left, orm_sql_row_source *right,
    orm_sql_union_kind kind, orm_sql_union *out, turbodb_error_t *error);
/* Same lifecycle; copies validated common types during open. The inputs retain
 * original validators; promoted projection/sort keys precede dedup and merge. */
turbodb_status_t orm_sql_union_open_as(orm_sql_row_source *left, orm_sql_row_source *right,
    orm_sql_union_kind kind, const orm_sql_type *types, orm_sql_union *out, turbodb_error_t *error);
orm_sql_row_source *orm_tidesdb_sql_union_source(orm_sql_union *run);
/* Ordinary Scan row/EOF/error contract, including locked first error and unchanged
 * output on failure. ALL may have returned a prefix before an input error;
 * DISTINCT publishes nothing until materialization/dedup succeed.
 * Direct next/cancel/close is BUSY when a consumer holds the output lease. */
turbodb_status_t orm_tidesdb_sql_union_next(orm_sql_union *run, orm_sql_scan_row *out, turbodb_error_t *error);
turbodb_status_t orm_tidesdb_sql_union_cancel(orm_sql_union *run, turbodb_error_t *error);
/* NULL/empty no-op. Does not destroy input owners or end the transaction. */
turbodb_status_t orm_tidesdb_sql_union_close(orm_sql_union *run, turbodb_error_t *error);
#endif

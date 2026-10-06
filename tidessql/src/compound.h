#ifndef ORM_TIDESDB_SQL_COMPOUND_H
#define ORM_TIDESDB_SQL_COMPOUND_H
#include "select.h"
#include "explain.h"
#include "catalog_store.h"
#include "cte_bind.h"

/* Private iterative query-tree owner. Stable address through close; owns every
 * SELECT branch, set operator and query tail. All branches borrow one Catalog
 * owner/active budget. Parent consumers close before child sources. The AST and
 * parameter buffers may die after open; no relation rows read until next.
 * count/depth/work/steps bounded; native transaction belongs to caller. */
typedef struct orm_sql_compound {
  orm_tidesdb_sql_budget *budget;
  vec_t nodes;
  size_t node_bytes, metadata_bytes, count;
  orm_sql_scan *scan;
  const orm_sql_select *plan;
  const orm_sql_schema_column *columns;
  orm_sql_row_source explanation;
  orm_sql_scan explained;
  size_t position;
  bool describe;
} orm_sql_compound;
/* SELECT leaves, UNION/INTERSECT/EXCEPT ALL/DISTINCT and query groups. Type/name
 * rules documented in union.h; each tail uses only its child's output scope.
 * Root and all descendants validated before publishing; failed open closes all
 * partial branches and restores work. No implicit transaction or budget reset.
 * explain binds every node and opens metadata scans only. Explanation rows visit
 * children before parents; the explanation owns no business execution runs. */
turbodb_status_t orm_tidesdb_sql_compound_open(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner, const turbodb_value_t *parameters, bool explain, orm_sql_compound *out, turbodb_error_t *error);
turbodb_status_t orm_sql_compound_open_queries(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner, const turbodb_value_t *parameters, bool explain,
    const orm_sql_expr_query_sources *sources, orm_sql_compound *out, turbodb_error_t *error);
/* Private CTE construction: compile only INITIAL or RECURSIVE leaves from a
 * validated shape for this same document/root/budget. Scope.root is the CTE's
 * query, demand must be VALUES. Preserve within-part UNION and local tails;
 * mixed-node tails belong to the recursive owner and are deliberately excluded.
 * The owner must validate both parts, apply cumulative DISTINCT/global limits,
 * and provide independent typed self sources via scope.derived before execution.
 * Does not discover dependencies or authorize raw recursive SQL. AST/shape borrow
 * only this call; normal compound close/reopen and failure contracts apply.
 * O(shape nodes) projection work/storage, charged before traversal/allocation.
 * Example: shape_bind, open_cte_part(INITIAL), schema_bind, install self sources,
 * open_cte_part(RECURSIVE), schema_member, close member execution, open store. */
turbodb_status_t orm_sql_compound_open_cte_part(const orm_sql_query_scope *scope,
    const orm_sql_cte_shape *shape, orm_sql_cte_parts part,
    orm_sql_catalog_store *owner, const turbodb_value_t *parameters, bool explain,
    const orm_sql_expr_query_sources *sources, orm_sql_compound *out, turbodb_error_t *error);
turbodb_status_t orm_sql_compound_explain_ids(orm_sql_compound *run, orm_sql_explain_block block,
    int64_t *next_id, turbodb_error_t *error);
turbodb_status_t orm_tidesdb_sql_compound_close(orm_sql_compound *run, turbodb_error_t *error);
/* Internal counterparts owned by runtime's round state machine. Close parent
 * consumers first; open children first. Keep plans, schema and source addresses.
 * No AST access, parameter retention or dependency reset here. */
turbodb_status_t orm_sql_compound_execution_close(orm_sql_compound *run, turbodb_error_t *error);
turbodb_status_t orm_sql_compound_execution_open(orm_sql_compound *run, const turbodb_value_t *parameters,
    size_t parameter_count, const orm_sql_expr_query_sources *sources, turbodb_error_t *error);
#endif

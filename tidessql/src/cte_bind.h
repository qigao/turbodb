#ifndef ORM_TIDESDB_SQL_CTE_BIND_H
#define ORM_TIDESDB_SQL_CTE_BIND_H
#include "binding.h"

/* Pure lexical binding, no Catalog access or query execution. Each TABLE AST id
 * indexes a CTE definition id at id-1; zero means a Catalog/derived occurrence.
 * Nearest WITH scope wins; definitions see earlier siblings and outer scopes.
 * Recursive self references are identified but rejected until recursive runs
 * exist. RECURSIVE without self-reference is legal. Explicit column names are
 * validated here; output width is checked after binding the definition query.
 * Owns a fixed vector, O(AST nodes^2) work worst case, charged to work/steps.
 * Caller releases via orm_sql_work_release. Failure/absent WITH leaves zero out. */
turbodb_status_t orm_sql_cte_bind(const orm_sql_query_scope *scope,
    vec_t *references, size_t *bytes, turbodb_error_t *error);
/* Internal preparation counterpart: preserves lexical self references for
 * recursive shape/schema binding. Does not authorize execution. Same ownership
 * and budgets; callers must validate every recursive definition before use. */
turbodb_status_t orm_sql_cte_resolve(const orm_sql_query_scope *scope,
    vec_t *references, size_t *bytes, turbodb_error_t *error);

typedef enum orm_sql_cte_parts {
  ORM_SQL_CTE_INITIAL_PART=1, ORM_SQL_CTE_RECURSIVE_PART=2,
  ORM_SQL_CTE_MIXED_PARTS=3
} orm_sql_cte_parts;
/* Preorder nodes retain the original UNION/group/WITH tree and tail locations.
 * A SELECT leaf has left=right=SIZE_MAX; self is its direct recursive TABLE id.
 * union_before records the edge preceding this SELECT in SQL member order.
 * No borrowed node pointers; ids require the original document during binding. */
typedef struct orm_sql_cte_shape_node {
  sqlparser_id ast,self,union_before;
  size_t parent,left,right;
  orm_sql_cte_parts parts;
} orm_sql_cte_shape_node;
typedef struct orm_sql_cte_shape {
  orm_tidesdb_sql_budget *budget;
  vec_t nodes;
  size_t bytes,count,initial_members,recursive_members;
} orm_sql_cte_shape;
/* Definition is a CTE AST id within scope; references comes from resolve for
 * that same scope/document. Pure structural validation, not expression/name/type
 * admission. Initial members precede recursive members, each with exactly one
 * direct FROM reference, never on an outer join's null-extended side. Checks
 * recursive grouping/ordering/distinct/window/known aggregate and UNION rules.
 * No SQL/AST rewriting, schema inference or parameter renumbering.
 * O(AST nodes^2) bounded work, O(AST nodes) storage; iterative depth checks and
 * charged comparisons. Zero output required; all work refunded on failure.
 * Example: resolve(scope), shape_bind(scope,cte,refs), bind seed/member plans,
 * shape_close, release refs. Runtime execution still needs its own admission. */
turbodb_status_t orm_sql_cte_shape_bind(const orm_sql_query_scope *scope,sqlparser_id definition,
    const vec_t *references,orm_sql_cte_shape *out,turbodb_error_t *error);
turbodb_status_t orm_sql_cte_shape_close(orm_sql_cte_shape *shape,turbodb_error_t *error);
struct orm_sql_select;
typedef struct orm_sql_cte_schema {
  orm_tidesdb_sql_budget *budget;
  orm_sql_table_schema schema;
  vec_t columns, types, names;
  size_t column_bytes, type_bytes, name_bytes, metadata_bytes;
  char name[ORM_SQL_SELECT_NAME_BYTES+1];
} orm_sql_cte_schema;
/* Owns a stable CTE schema copied from an already bound seed plan (including any
 * seed UNION type resolution). Applies explicit column names, checks count and
 * duplicates. Recursive results are nullable; member types never widen seed.
 * Ordinary CTEs preserve seed nullability. AST and seed plan may die afterwards.
 * No SQL execution/admission or row reads. O(columns^2) name validation and
 * O(columns) owned metadata, charged to shared steps/work. Empty stable output;
 * failure refunds work and clears output. Close only after schema consumers. */
turbodb_status_t orm_sql_cte_schema_bind(const orm_sql_query_scope *scope,sqlparser_id definition,
    const struct orm_sql_select *seed,bool recursive,orm_sql_cte_schema *out,turbodb_error_t *error);
/* TABLE query counterpart sharing the same immutable schema owner and close.
 * Applies same-order derived column aliases with exact width and unique names;
 * preserves query types/nullability. Same budget, scope, failure and lifetime
 * contract as CTE schema binding; no rows read and no query-plan mutation. */
turbodb_status_t orm_sql_derived_schema_bind(const orm_sql_query_scope *scope,sqlparser_id definition,
    const struct orm_sql_select *query,orm_sql_cte_schema *out,turbodb_error_t *error);
/* Construction-time validation: same positive width and same kind or member
 * NULL at each position. No implicit conversions, mutation or name matching.
 * O(columns) charged steps; shared budget required. */
turbodb_status_t orm_sql_cte_schema_member(const orm_sql_cte_schema *schema,
    const struct orm_sql_select *member,turbodb_error_t *error);
turbodb_status_t orm_sql_cte_schema_close(orm_sql_cte_schema *schema,turbodb_error_t *error);
#endif

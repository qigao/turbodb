#ifndef ORM_TIDESDB_SQL_CHANGE_H
#define ORM_TIDESDB_SQL_CHANGE_H
#include "catalog_store.h"
#include "diagnostics.h"
#include "binding.h"

/* Private, synchronous type-only validation for one single-table
 * UPDATE/DELETE. scope borrows its MySQL AST and actual parameter types for
 * the call; root must be the document's sole statement. No parameter values,
 * row reads, expression evaluation, diagnostics mutation, writes or transaction
 * transitions. Catalog metadata is read from the supplied ready owner, even
 * if read-only. Temporary plans/work are released before return; cleanup
 * failure poisons the owner. Nonrecursive WITH, scalar/IN/EXISTS dependencies,
 * nested correlated and LATERAL query blocks use schema-only dependency binding.
 * Recursive CTEs reject even with a positive iteration limit. No retained
 * prepared handle is produced.
 * Execute remains responsible for actual NULL/range/LIMIT validation.
 * Usage: active budget + Catalog owner; scope={document,statement,types,count,
 * depth,budget}; change_bind(&scope,owner,error); destroy document; end budget.
 * Executable examples and failure matrix: ../tests/integration/runtime_test.c. */
turbodb_status_t orm_sql_change_bind(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *store, turbodb_error_t *error);

/* Private synchronous MySQL UPDATE/DELETE, optionally wrapped by WITH, over one
 * Catalog table. Borrows AST
 * and immutable parameters through return. Supports the existing expression
 * subset, optional WHERE, and multiple assignments evaluated left to right.
 * PK updates replace old keys atomically in selected row order; destinations
 * still occupied at that step return CONSTRAINT. Source rows are selected only
 * once from the original snapshot. Parameters use source order. Numeric assignments
 * use MySQL conversion rules. UPDATE IGNORE adjusts invalid values with diagnostics
 * and skips conflicting rows in selected order. Multi-table operations and priority
 * modifiers remain unsupported. DEFAULT uses the
 * Catalog scalar or implicit NULL for nullable columns.
 * Scalar/IN/EXISTS dependencies and named CTE references are available in
 * WHERE, assignments and computed ordering. Direct children may correlate on
 * an explicitly target-table-qualified column. Recursive CTEs require
 * an explicit positive max_iterations; zero keeps recursive writes disabled.
 * All dependency relation sources close after materialization and before writes.
 * ORDER BY accepts numeric columns or existing scalar expressions returning
 * numeric/BOOL/NULL values, ASC/DESC with NULL first/last respectively. Text,
 * blob and bare/signed numeric position items are unsupported. Computed keys
 * are copied scalars evaluated once per matched original row before sorting.
 * LIMIT accepts a nonnegative integer literal or I64/U64 parameter, without
 * offset, and counts matches including unchanged rows. Ties and unordered
 * selection have no promised SQL order. LIMIT 0 skips data reads. max_depth > 0.
 * All expressions/parameters bind even for empty tables or LIMIT 0.
 * Two snapshot scans: count matching rows then materialize changes; ordered
 * writes copy all matches and compute/sort keys before LIMIT/assignment.
 * Key errors on any matching candidate fail even if LIMIT would exclude it.
 * No writes until all sources close. Fixed workspace/MATERIALIZED_ROWS bound
 * the batch; any excess rejects the whole statement. Changed values/keys are copied.
 * One savepoint, one version increment when rows actually change. No-op leaves
 * version unchanged. Success sets affected to changed/deleted rows. With
 * client_found_rows, UPDATE reports selected matches including no-op rows;
 * DELETE is unchanged. Failures
 * preserve it. Owner commit/rollback remains caller responsibility. Single
 * owner, no concurrent operations or live sources; budget must remain active.
 * Reproducible call examples: integration/tidesdb/sql/relation_test.c. */
turbodb_status_t orm_tidesdb_sql_change_execute(const sqlparser_document *document,
    orm_sql_catalog_store *store, const turbodb_value_t *parameters, size_t parameter_count,
    size_t max_depth, uint64_t max_iterations, bool client_found_rows,
    orm_sql_diagnostics *diagnostics,
    size_t *affected,
    turbodb_error_t *error);
/* Context-aware form; copies session and receiver, selects the command's
 * existing evaluation policy. Same ownership/failure contract. */
turbodb_status_t orm_sql_change_execute_evaluation(const sqlparser_document *document,
    orm_sql_catalog_store *store, const turbodb_value_t *parameters, size_t parameter_count,
    size_t max_depth, uint64_t max_iterations, bool client_found_rows,
    orm_sql_evaluation evaluation, size_t *affected, turbodb_error_t *error);
#endif

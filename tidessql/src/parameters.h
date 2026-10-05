#ifndef ORM_TIDESDB_SQL_PARAMETERS_H
#define ORM_TIDESDB_SQL_PARAMETERS_H
#include "binding.h"
#include "catalog_store.h"

/* Private source-ordered parameter metadata. An unresolved entry has no type:
 * resolved, never the zeroed type bytes, is its state. No parameter values.
 * One single-threaded owner and active statement budget through close. AST is
 * borrowed through the last infer call; ready metadata survives AST destruction.
 * Fixed document marker capacity; all storage/steps use the existing budget. */
typedef struct orm_sql_parameters {
  const sqlparser_document *document;
  orm_tidesdb_sql_budget *budget;
  vec_t offsets, types, resolved;
  size_t offset_bytes, type_bytes, resolved_bytes;
} orm_sql_parameters;
/* document is borrowed through inference, budget through close. out must be
 * zero initialized; success owns metadata, failure releases partial output.
 * INVALID_ARGUMENT for malformed admission, UNSUPPORTED for dialect/batch,
 * INVALID_STATE for inactive budget, LIMIT_EXCEEDED/OUT_OF_MEMORY for capacity.
 * All calls return OK only on complete success; no allocation fallback. */
turbodb_status_t orm_sql_parameters_open(const sqlparser_document *document,
    orm_tidesdb_sql_budget *budget, orm_sql_parameters *out, turbodb_error_t *error);
/* Infer one scalar tree against a resolved local schema. Supports leaves,
 * arithmetic, binary comparisons and numeric CAST; other AST kinds explicitly
 * reject. Optional context is a real non-NULL target type (assignment/CAST);
 * no context derives TEXT for a bare marker, DOUBLE for untyped arithmetic.
 * Direct comparison/arithmetic markers use the peer type; comparison does not
 * provide an arithmetic context. All derived markers are nullable.
 * scope contains no supplied parameter types, dependencies, substitutions or
 * outer/capture frames. Reuses the ordinary Binder/compiler for final checks,
 * without evaluation, storage access or diagnostics. Binding/allocation failure
 * preserves metadata; cleanup failure or an invalid budget consumes the metadata
 * owner and its views.
 * a different type for an already inferred marker fails SQL_ERROR.
 * N=document nodes, M=markers, C=columns, B=SQL bytes:
 * O(N^2*(C+log(M+1)+1)+N*B) time and O(N+M+B) temporary space, charged and
 * bounded by WORK/steps/depth. root must be a real expression.
 * Call once per expression, then types() only after every marker is inferred.
 * For example: open(doc,budget,&p,error); infer(&p,&local,expression,NULL,depth,
 * error); types(&p,&types,&count,error); use types; close(&p,error). Each status
 * must be checked. Formal examples: ../tests/unit/parameters_test.c.
 * This is a scalar building block, not a statement prepare API. */
turbodb_status_t orm_sql_parameters_infer(orm_sql_parameters *parameters,
    const orm_sql_binding_scope *scope, sqlparser_id root,
    const orm_sql_type *context, size_t max_depth, turbodb_error_t *error);
/* Immutable borrowed types through close; unresolved metadata returns
 * INVALID_STATE and leaves both outputs unchanged. */
turbodb_status_t orm_sql_parameters_types(const orm_sql_parameters *parameters,
    const orm_sql_type **out, size_t *count, turbodb_error_t *error);
/* Private whole-statement inference and validation from SQL + Catalog schema.
 * One MySQL SELECT (no FROM or one plain table), INSERT/REPLACE VALUES/SET,
 * or single-table UPDATE/DELETE. SELECT GROUP/HAVING/windows, query dependencies,
 * INSERT SELECT and incoming-row aliases reject pending #206. Marker-bearing
 * expressions use infer's scalar subset; marker-free expressions use the full
 * existing Binder. Assignments derive target-column types, pagination U64.
 * Catalog must be ready, including read-only; max_depth positive and not SIZE_MAX.
 * Borrows document/owner only for construction, never values. Publishes complete
 * metadata only after the ordinary statement Binder succeeds. Zero stable out;
 * failure empties it, occupied out unchanged. All temporary Catalog leases close
 * before return; no business rows/evaluation/writes/diagnostic/transaction changes.
 * Success retains only offsets/types/resolved in the active owner budget, through
 * parameters_close. AST may die on success; no further infer after AST destruction.
 * This does not retain a prepared plan or authorize a later schema snapshot.
 * E=expression roots: O(E*(N^2*(C+log(M+1)+1)+N*B)+E*M) plus existing schema/
 * statement binding; O(N+M+B+C) inference workspace plus the existing Binders'
 * storage. All existing quotas apply. No cache.
 * Example: statement(doc,owner,depth,&p,error); types(&p,&types,&count,error);
 * consume metadata; close(&p,error). Check every status. Native formal examples:
 * ../tests/integration/runtime_test.c. Public SDK/ABI remains unchanged. */
turbodb_status_t orm_sql_parameters_statement(const sqlparser_document *document,
    orm_sql_catalog_store *owner, size_t max_depth, orm_sql_parameters *out,
    turbodb_error_t *error);
/* Zero/partial close allowed. Refunds WORK, retains consumed steps/nodes. */
turbodb_status_t orm_sql_parameters_close(orm_sql_parameters *parameters,
    turbodb_error_t *error);
#endif

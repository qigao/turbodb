#ifndef ORM_TIDESDB_SQL_RUNTIME_H
#define ORM_TIDESDB_SQL_RUNTIME_H
#include "relation.h"
#include "select.h"
#include "show.h"
#include "explain.h"
#include "compound.h"
#include "dependencies.h"
#include "diagnostics.h"

typedef enum orm_sql_query_kind { ORM_SQL_QUERY_CLOSED, ORM_SQL_QUERY_SELECT, ORM_SQL_QUERY_SHOW, ORM_SQL_QUERY_EXPLAIN, ORM_SQL_QUERY_COMPOUND } orm_sql_query_kind;
/* Private synchronous query owner; zero initialize and keep at a stable address
 * through close. All fields are read-only to callers. The Catalog owner and its
 * active budget outlive this object. No transaction/budget begin/end here.
 * Close the query before budget_end; a later budget_begin starts fresh statement
 * limits on the same Catalog transaction while retaining cumulative limits. */
typedef struct orm_sql_query {
  orm_sql_catalog_store *owner;
  orm_sql_query_kind kind;
  size_t columns, metadata_bytes;
  orm_sql_dependencies dependencies;
  orm_sql_query_demand demand;
  bool execution_closed;
  turbodb_error_t execution_failure;
  orm_sql_snapshot parameters;
  size_t parameter_count;
  bool statement_parameters;
  orm_sql_evaluation evaluation;
  union {
    struct {
      orm_sql_relation_source source; orm_sql_select plan;
      vec_t relations;
      size_t relation_bytes;
      vec_t inputs;
      size_t input_bytes;
      vec_t dependent_inputs;
      size_t dependent_input_bytes;
      orm_sql_from from;
      orm_sql_from_run from_run;
      /* One immutable physical witness; hidden from SQL name resolution.
       * The source budget also marks its owner lease through close. */
      struct { orm_sql_row_source source; orm_sql_type type; turbodb_value_t value; bool done; } unit;
      union { orm_sql_select_run run; struct { orm_sql_explain_source source; orm_sql_scan scan; } explain; };
    } select;
    struct {
      orm_sql_show_source source; orm_sql_scan scan;
      orm_sql_expr filter;
      vec_t filter_slots;
      size_t filter_bytes;
    } show;
    orm_sql_compound compound;
  } as;
} orm_sql_query;

/* Private metadata-only FROM owner for a lexical SELECT frame. No ON/expression
 * compilation or business reads. Borrowed schema remains valid through close;
 * caller closes it with runtime_close on success and partial failure. All
 * Catalog leases, plan nodes, workspace and steps use the statement budget. */
turbodb_status_t orm_sql_runtime_schema_open(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner,orm_sql_query *out,
    const orm_sql_table_schema **schema,vstr *qualifier,turbodb_error_t *error);
/* Same metadata-only lifetime, selecting an actual FROM TABLE/JOIN subtree.
 * Requires a nonzero subtree ID; unrelated or nested-query IDs are rejected
 * before Catalog access. Only selected leaf occurrences need prepared derived
 * bindings. Includes internal LEFT/RIGHT nullability and source qualifiers,
 * without touching other branches or granting LATERAL execution. */
turbodb_status_t orm_sql_runtime_subtree_schema_open(const orm_sql_query_scope *scope,
    sqlparser_id subtree,orm_sql_catalog_store *owner,orm_sql_query *out,
    const orm_sql_table_schema **schema,vstr *qualifier,turbodb_error_t *error);

/* Private construction owner for one LATERAL table's same-SELECT frame. Stable
 * inputs own schema names and Catalog leases; columns borrow those names until
 * close. Prefix IDs follow contextualization order; each input retains logical
 * SQL column order and NULL extension. All columns share lexical depth zero.
 * No executable query, ON compilation, business reads or LATERAL admission.
 * Zero initialize, keep at a stable address and close before owner/budget end.
 * AST/prepared derived schemas may die after success. Construction errors leave
 * empty output; close failure permits close retry only. Empty prefix succeeds
 * with count zero. O(prefixes * AST nodes + input metadata + columns^2), bounded by
 * the statement work/plan/depth/step budgets. */
typedef struct orm_sql_lateral_schema {
  orm_tidesdb_sql_budget *budget;
  vec_t prefixes,inputs,columns;
  size_t prefix_bytes,input_bytes,column_bytes,metadata_bytes;
  orm_sql_table_schema schema;
} orm_sql_lateral_schema;
turbodb_status_t orm_sql_runtime_lateral_schema_open(const orm_sql_query_scope *scope,
    sqlparser_id lateral,orm_sql_catalog_store *owner,orm_sql_lateral_schema *out,turbodb_error_t *error);
turbodb_status_t orm_sql_runtime_lateral_schema_close(orm_sql_lateral_schema *frame,turbodb_error_t *error);

/* Private dependent-query owner for JOIN right_binding. Compiles an already
 * prepared SELECT/compound scope and its complete outer frame; no dependency
 * discovery or raw SQL admission. Zero initialize and keep at a stable address.
 * Owns the child plan, copied SQL markers, fixed capture slots and output types.
 * AST/frame/marker inputs may die after success; external FROM/query sources and
 * the supplied query registry remain borrowed until final close. The caller
 * reopens external dependencies before each binding.open and closes consumers
 * before binding.close. open takes exactly outer_schema->count borrowed values,
 * retained until successful round close; no parameter replacement or row reads
 * during open. Failure clears captures once partial execution is closed. Runtime
 * execution failures latch until final close. BUSY close retains all ownership
 * for retry; an active source scan forbids round/final close. All allocations and
 * rounds use existing work/plan/step/row budgets. No cache or quota reset.
 * Construction is O(marker payload + ordinary query binding), each round adds
 * O(capture width) to runtime open/close. Space is bounded by the compiled query,
 * marker snapshot, capture slots and output types, without retained result rows.
 * Example: compile, join_open(...,&out->source,{.right_binding=out->binding}),
 * join_next, join_close, lateral_query_close. Composite prefixes/ancestors must
 * be assembled by the FROM owner into the compiled frame order before open. */
typedef struct orm_sql_lateral_query {
  orm_tidesdb_sql_budget *budget;
  orm_sql_query query;
  orm_sql_snapshot arguments;
  vec_t types;
  size_t type_bytes,metadata_bytes,marker_count,capture_count;
  orm_sql_expr_query_sources queries;
  orm_sql_row_source source;
  orm_sql_join_right_binding binding;
  bool bound;
} orm_sql_lateral_query;
turbodb_status_t orm_sql_lateral_query_open(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner,const turbodb_value_t *parameters,
    const orm_sql_expr_query_sources *queries,orm_sql_lateral_query *out,turbodb_error_t *error);
turbodb_status_t orm_sql_lateral_query_close(orm_sql_lateral_query *run,turbodb_error_t *error);

/* One parsed MySQL CREATE/DROP TABLE/INDEX, TRUNCATE TABLE, supported ALTER TABLE,
 * INSERT/UPDATE/DELETE, or WITH-wrapped UPDATE/DELETE. Borrows document and
 * parameters for the call. max_depth must be positive. Exact parameter count,
 * same grammar/type/budget contracts as the underlying private modules. No
 * implicit commit; DDL reports affected=0. DML reports changed rows unless
 * client_found_rows requests MySQL matched-row reporting for UPDATE and no-op
 * duplicate-key updates.
 * Noncorrelated query dependencies and CTEs in UPDATE/DELETE share the command
 * snapshot. Recursive CTE writes require positive max_iterations.
 * Failure preserves affected; a failed owner requires rollback. Queries, SQL
 * transaction commands and all other statements return UNSUPPORTED. */
turbodb_status_t orm_tidesdb_sql_runtime_execute(const sqlparser_document *document,
    orm_sql_catalog_store *owner, const turbodb_value_t *parameters, size_t parameter_count,
    size_t max_depth, uint64_t max_iterations, bool client_found_rows,
    orm_sql_diagnostics *diagnostics, size_t *affected,
    turbodb_error_t *error);
/* Same command contract, with a copied connection snapshot. DML selects its
 * own strict/IGNORE policy while preserving the snapshot and receiver. */
turbodb_status_t orm_sql_runtime_execute_evaluation(const sqlparser_document *document,
    orm_sql_catalog_store *owner, const turbodb_value_t *parameters, size_t parameter_count,
    size_t max_depth, uint64_t max_iterations, bool client_found_rows,
    orm_sql_evaluation evaluation, size_t *affected, turbodb_error_t *error);

/* One parsed MySQL SELECT/UNION/query group, supported SHOW or
 * default/TRADITIONAL EXPLAIN SELECT, including compound query trees and
 * scalar/IN/NOT IN dependencies, each with one output column. Direct
 * scalar/IN/EXISTS children of a plain-table SELECT, a single CTE source, or a
 * JOIN containing plain tables and prepared nonrecursive CTE sources may bind qualified
 * outer columns, or unqualified names absent from their local FROM schema, and
 * are reopened for each outer row. Those children
 * may contain nested scalar/IN/EXISTS chains capturing both parent and ancestor
 * rows, with nearest-scope name resolution and alias shadowing. Plain/JOIN,
 * derived and nonrecursive CTE input schemas are supported within these chains.
 * may themselves read one plain table or a plain-table JOIN; outer references
 * are valid in ON and the remaining SELECT expressions. Other dependency shapes
 * retain noncorrelated caching.
 * EXISTS/NOT EXISTS accepts arbitrary output width and skips unused output
 * evaluation while preserving row existence. Internal output may omit aliases.
 * Noncorrelated FROM derived tables require an alias and unique named output
 * columns; computed columns require explicit aliases. Their query sources open
 * before consumers and stream on demand in the same Catalog snapshot.
 * Nonrecursive WITH supports nested scopes, earlier definitions, optional unique
 * column lists and independent references to one lazily materialized result;
 * root JOIN occurrences publish one combined qualified row schema before their
 * direct expression dependencies bind.
 * Recursive references and WITH commands remain unsupported. Named CTE columns
 * permit anonymous internal projections; outer output rules remain unchanged.
 * SELECT without FROM consumes one private unit row, without storage reads.
 * Existing predicates, grouping, pagination and dependencies apply to that row;
 * column names and stars require FROM. Unaliased top-level expressions use their
 * source text as bounded output labels (NAME_BYTES); use aliases for longer
 * expressions or to make their output addressable as derived/CTE columns.
 * EXPLAIN binds and validates the SELECT but never opens its execution run or
 * reads relation rows, including during next. Catalog metadata reads still
 * consume the shared read budget. Retains the same owner lease.
 * Names/schema resolve in owner's
 * snapshot. SHOW TABLES/COLUMNS/VARIABLES support LIKE or WHERE; INDEX supports
 * WHERE. WHERE binds actual output column names with ASCII case folding and
 * accepts typed parameters; subqueries and aggregates remain unsupported.
 * database_name is SHOW's display label, unused by SELECT. Input AST,
 * parameters/payloads and label may be released after return; they are copied
 * or compiled as needed. Failed open refunds work and leaves output empty;
 * occupied output is unchanged. No reads of relation data until next. */
turbodb_status_t orm_tidesdb_sql_runtime_open(const sqlparser_document *document,
    orm_sql_catalog_store *owner, vstr database_name, const turbodb_value_t *parameters,
    size_t parameter_count, size_t max_depth, orm_sql_query *out, turbodb_error_t *error);
turbodb_status_t orm_tidesdb_sql_runtime_open_diagnostics(
    const sqlparser_document *document, orm_sql_catalog_store *owner,
    vstr database_name, const turbodb_value_t *parameters, size_t parameter_count,
    size_t max_depth, orm_sql_diagnostics *diagnostics,
    orm_sql_query *out, turbodb_error_t *error);
/* Top-level statement policy, copied into queries and every dependency run.
 * max_iterations zero disables recursion. Receiver must be initialized and
 * outlive runtime_close; no reset while query consumers live. Invalid mode or
 * receiver fails before query construction, including SHOW and EXPLAIN. */
turbodb_status_t orm_sql_runtime_open_evaluation(const sqlparser_document *document,
    orm_sql_catalog_store *owner, vstr database_name, const turbodb_value_t *parameters,
    size_t parameter_count, size_t max_depth, uint64_t max_iterations,
    orm_sql_evaluation evaluation, orm_sql_query *out, turbodb_error_t *error);
/* Private bounded recursive construction, with the same statement/parameter
 * ownership and failure contract as runtime_open. max_iterations must be positive
 * and is independent of max_depth. Owns recursive dependency graphs in place;
 * EXPLAIN composes seed/member/dependency metadata without executing recursion.
 * Whole-definition pagination bounds generation; subset-member pages reject.
 * The driver selects this entry only when sql_max_recursive_iterations is
 * explicitly configured; its ordinary entry retains recursive rejection.
 * Example: open_recursive, destroy document, next, execution_close/resume for
 * business queries, close. EXPLAIN uses next/close only. */
turbodb_status_t orm_sql_runtime_open_recursive(const sqlparser_document *document,
    orm_sql_catalog_store *owner, vstr database_name, const turbodb_value_t *parameters,
    size_t parameter_count, size_t max_depth, uint64_t max_iterations,
    orm_sql_query *out, turbodb_error_t *error);
/* Internal SELECT-block construction for the compound owner. Uses the same
 * Catalog snapshot and full-document parameters; scope.root must be SELECT.
 * explain selects binding and metadata only, without opening a SELECT run.
 * Zero stable output, same close/error/no-row-read contract as runtime_open. */
turbodb_status_t orm_tidesdb_sql_runtime_block_open(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner, const turbodb_value_t *parameters, bool explain, orm_sql_query *out, turbodb_error_t *error);
/* Internal construction from already bound dependencies; never recursively
 * discovers dependencies. Supports SELECT/UNION/query groups. The registry is
 * borrowed only for open, its referenced sources through consumer close. */
turbodb_status_t orm_sql_runtime_scope_open(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner, const turbodb_value_t *parameters, bool explain,
    const orm_sql_expr_query_sources *sources, orm_sql_query *out, turbodb_error_t *error);
/* Private type-only SELECT/compound/query-group construction, including an
 * INSERT SELECT query root. No parameter values, operator runs, business reads
 * or diagnostics mutation. Complete-document parameter types in source order;
 * VALUES root demand, nonrecursive WITH/derived/LATERAL/scalar/IN/EXISTS and
 * correlated dependencies; no externally supplied registries/outer frame.
 * Recursive definitions reject pending #206 type-only recursive binding.
 * Stable zero output owns compiled names/types, workspace and Catalog leases
 * through runtime_close; AST/types may die on success. Column views expire at
 * close. Owner/budget outlive it; finish remains BUSY while leases exist.
 * next/cancel reject. With no dependency graph, execution_open may explicitly
 * supply real parameters; metadata dependency graphs cannot execute/replay.
 * Failure closes partial ownership; cleanup failure poisons the owner.
 * Example: active Catalog owner; scope={doc,root,types,count,depth,budget};
 * query_bind(&scope,owner,&query,error); destroy doc; runtime_column; close.
 * Formal usage/ownership/fault cases: ../tests/integration/runtime_test.c. */
turbodb_status_t orm_sql_runtime_query_bind(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner, orm_sql_query *out, turbodb_error_t *error);
/* Statement-owned query root embedded in another command, such as INSERT
 * SELECT. Discovers and owns its dependencies, snapshots parameters, and
 * supports SELECT/UNION/query groups and WITH. A positive scope.max_iterations
 * enables bounded recursion; zero rejects recursive references. scope parameter types
 * and values cover the complete containing document in source order. The root
 * query and all sources close through runtime_close before the command writes. */
turbodb_status_t orm_sql_runtime_query_open(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner, const turbodb_value_t *parameters,
    orm_sql_query *out, turbodb_error_t *error);
/* Business output plan even for EXPLAIN; immutable through query close. */
const orm_sql_select *orm_sql_runtime_plan(const orm_sql_query *query);
/* Construction-only EXPLAIN ids. next_id is advanced on success; no row reads. */
turbodb_status_t orm_sql_runtime_explain_ids(orm_sql_query *query, orm_sql_explain_block block,
    int64_t *next_id, turbodb_error_t *error);
/* Base output scan, bypassing the optional dependency EXPLAIN wrapper. */
orm_sql_scan *orm_sql_runtime_base_scan(orm_sql_query *query);
/* Private round lifecycle for fully bound SELECT/compound queries.
 * SHOW and EXPLAIN reject. Open with external parameters rejects a query owning
 * dependencies; use resume for same-statement replay of such a query.
 * Close external consumers first. execution_close releases runs, retaining plans,
 * schema, inputs and Catalog leases; repeated close is inert. Borrowed rows expire.
 * execution_open requires closed execution, rewinds owned native/unit sources and
 * opens operators without AST or data reads. Caller rewinds external FROM sources
 * and supplies the same statement's parameters/query registry each round; registry
 * arrays/parameter bytes borrow only the call. Retained FROM descriptors and their
 * owners outlive final runtime_close; expression sources outlive each execution.
 * No quota reset. Execution/reopen failures stay latched through final
 * query close; failed reopen releases partial runs. EOF/cancel may be reopened.
 * O(query nodes + FROM inputs + ordinary operator open costs), bounded by the
 * shared work/step quotas. Use execution_close, reset dependencies, execution_open,
 * next, and finally runtime_close (which also destroys retained plans). */
turbodb_status_t orm_sql_runtime_execution_close(orm_sql_query *query, turbodb_error_t *error);
turbodb_status_t orm_sql_runtime_execution_open(orm_sql_query *query, const turbodb_value_t *parameters,
    size_t parameter_count, const orm_sql_expr_query_sources *sources, turbodb_error_t *error);
/* Reopens closed execution using the original runtime_open parameter snapshot
 * and dependency registry. Scope-open queries have external owners and reject.
 * Reopens streaming derived queries, rewinds CTE readers, retains noncorrelated
 * scalar/set/CTE caches. No parameter replacement, AST access or row reads.
 * Same failure latch/cleanup contract as execution_open. Example: runtime_open,
 * next, execution_close, execution_resume, next, runtime_close. */
turbodb_status_t orm_sql_runtime_execution_resume(orm_sql_query *query, turbodb_error_t *error);
/* Borrowed column metadata until close. Invalid ordinal/output ->
 * INVALID_ARGUMENT, closed query -> INVALID_STATE; out unchanged on failure. */
turbodb_status_t orm_tidesdb_sql_runtime_column(const orm_sql_query *query,
    size_t ordinal, orm_sql_schema_column *out, turbodb_error_t *error);
/* Unified scan contract: row borrows until next/cancel/close; error preserves
 * out and locks first failure. EOF and cancellation are idempotent. */
turbodb_status_t orm_tidesdb_sql_runtime_next(orm_sql_query *query, orm_sql_scan_row *out, turbodb_error_t *error);
turbodb_status_t orm_tidesdb_sql_runtime_cancel(orm_sql_query *query, turbodb_error_t *error);
/* NULL/empty is a no-op; releases run, plan, source and workspace, in that order.
 * Does not commit/rollback or reset budgets. Close even after EOF/error/cancel.
 * BUSY retains outstanding owners, parameter snapshots and leases. Some inner
 * consumers may already have closed; release external consumers and retry close
 * only. Successful close releases each receipt once and clears the query.
 * Example: execute(CREATE), execute(INSERT), open(SELECT), next(...), close(...),
 * catalog_finish(owner,true); see integration/tidesdb/sql/runtime_test.c. */
turbodb_status_t orm_tidesdb_sql_runtime_close(orm_sql_query *query, turbodb_error_t *error);
#endif

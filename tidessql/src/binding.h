#ifndef ORM_TIDESDB_SQL_BINDING_H
#define ORM_TIDESDB_SQL_BINDING_H
#include "expr.h"
#include "schema.h"
#include "aggregate.h"

/* Borrowed query-block input inside one live MySQL document. root identifies a
 * SELECT/compound block; parameters always describe every marker in the whole
 * document, including siblings and outer pagination, in source order.
 * Optional queries are already validated noncorrelated dependencies, strictly
 * AST-id ordered. SELECT/FROM retain only compiled types and registry slots;
 * the dependency owner must supply corresponding sources at execution open. */
typedef enum orm_sql_query_demand {
  ORM_SQL_QUERY_VALUES, ORM_SQL_QUERY_CARDINALITY, ORM_SQL_QUERY_EXISTENCE
} orm_sql_query_demand;

struct orm_sql_row_source;
struct orm_sql_from_input;
/* Statement-owned derived/CTE occurrence, published after its query is bound.
 * Schema and source remain stable until all FROM consumers have closed.
 * Metadata/EXPLAIN bindings have schema only (source=NULL), never execute rows. */
typedef struct orm_sql_derived_binding {
  sqlparser_id node;
  const orm_sql_table_schema *schema;
  struct orm_sql_row_source *source;
  const struct orm_sql_from_input *input; /* Optional stable dependent-FROM provider. */
} orm_sql_derived_binding;

struct orm_sql_query_scope;
typedef turbodb_status_t (*orm_sql_dependency_schema_prepare)(
    struct orm_sql_query_scope *scope,void *context,turbodb_error_t *error);
typedef turbodb_status_t (*orm_sql_query_parameters_prepare)(
    struct orm_sql_query_scope *scope,void *context,turbodb_error_t *error);
typedef bool (*orm_sql_query_output_parameters_pending)(
    const struct orm_sql_query_scope *scope,void *context);
typedef struct orm_sql_query_scope {
  const sqlparser_document *document;
  sqlparser_id root;
  const orm_sql_type *parameter_types;
  size_t parameter_count, max_depth;
  orm_tidesdb_sql_budget *budget;
  const orm_sql_expr_query_binding *queries;
  size_t query_count;
  bool in_query; /* IN query blocks reject LIMIT per MySQL restrictions. */
  orm_sql_query_demand demand;
  bool anonymous_output; /* EXISTS or explicit source column lists supply output names. */
  bool scalar_output; /* Internal single-column result; no implicit SQL alias. */
  const orm_sql_derived_binding *derived;
  size_t derived_count;
  bool unit_input; /* Runtime supplies exactly one private BOOL witness, no FROM. */
  uint64_t max_iterations; /* Zero keeps recursive CTEs disabled. */
  /* outer_* is the immutable nearest-first lexical frame while this query
   * block binds. Columns carry lexical_depth to preserve shadowing between
   * query blocks. dependency_* exposes the current callback's complete row.
   * A zero qualifier means columns carry their own relation qualifiers. */
  const orm_sql_table_schema *outer_schema, *dependency_schema;
  vstr outer_qualifier, dependency_qualifier;
  bool defer_execution;
  /* Pure metadata construction has no evaluable dependency sources. Separate
   * from deferred execution, which still owns inputs needed for later reopen. */
  bool binding_only;
  /* Optional root-runtime hook. Dependency construction invokes it once after
   * every root FROM query source is published and before the first top-level
   * expression dependency. It may publish the immutable combined row schema
   * used by direct correlated children. Nested scopes must clear the hook. */
  orm_sql_dependency_schema_prepare prepare_dependency_schema;
  void *dependency_schema_context;
  bool force_dependency_schema; /* Lexical captures need the complete root row. */
  orm_sql_evaluation evaluation;
  /* Optional statement-inference state. A false marker has no type and must
   * fail before compilation; zeroed orm_sql_type storage is never semantic.
   * The callback may resolve markers in this query block before it binds. */
  const bool *parameter_resolved;
  orm_sql_query_parameters_prepare prepare_parameters;
  void *parameter_context;
  /* Compound binding may prepare marker-free leaves first, then use their
   * immutable result kinds to constrain corresponding projection markers.
   * output_root prevents a copied scope from leaking the context into nested
   * dependencies. The pending hook only classifies projection markers; all
   * actual inference remains owned by prepare_parameters. */
  orm_sql_query_output_parameters_pending output_parameters_pending;
  const orm_sql_type *parameter_output_types;
  size_t parameter_output_count;
  sqlparser_id parameter_output_root;
} orm_sql_query_scope;

/* Basic private type-only admission; validates borrowed document/root/types,
 * requires one MySQL statement and charges a complete AST traversal. Query
 * dependency kinds and lexical contracts are checked by their own binders. */
turbodb_status_t orm_sql_bind_statement_scope(const orm_sql_query_scope *scope,
    turbodb_error_t *error);
/* Shared private write admission. Root must be the complete MySQL statement;
 * dependencies and command kinds are validated by the consuming binder.
 * Explicit parameter types, no parameter values. O(nodes), O(1) AST charge;
 * not used by existing execution admission. */
turbodb_status_t orm_sql_bind_write_scope(const orm_sql_query_scope *scope,
    turbodb_error_t *error);

/* Private borrowed immutable scope. Offsets are strictly source ordered;
 * parameter_types has parameter_count entries. No ownership transfer.
 * Optional substitutions replace validated scalar keys, aggregate CALLs, WINDOWs or
 * HAVING alias NAMEs with schema slots; their children are opaque to this scope.
 * The SELECT Binder validates keys/arguments in the original row scope first. */
typedef struct orm_sql_binding_scope {
  const sqlparser_document *document;
  const orm_sql_table_schema *schema;
  vstr qualifier;
  const orm_sql_type *parameter_types;
  const uint64_t *parameter_offsets;
  size_t parameter_count;
  size_t parameter_marker_count;
  const orm_sql_table_schema *outer_schema;
  vstr outer_qualifier;
  bool *correlated;
  orm_tidesdb_sql_budget *budget;
  const size_t *substitutions; /* Optional node_count entries: result slot+1, zero absent. */
  size_t substitution_count;
  /* Already validated dependencies in strictly increasing AST-id order. Inner
   * query bodies are opaque here; parameter offsets still cover the document. */
  const orm_sql_expr_query_binding *queries;
  size_t query_count;
  bool hidden_input; /* Physical unit-row witness has no SQL-visible columns. */
  const size_t *capture_slots; /* NULL for original rows; source-to-group/window input slots otherwise. */
  size_t capture_count;
  bool ascii_insensitive_names; /* SHOW result labels; ordinary schemas retain exact names. */
  const bool *parameter_resolved; /* NULL means every supplied marker type is resolved. */
} orm_sql_binding_scope;
typedef struct orm_sql_expression_target {
  orm_sql_expr *program;
  vec_t *slots;
  size_t *slot_bytes;
  vec_t *query_slots;
  size_t *query_slot_bytes;
} orm_sql_expression_target;
/* Shared supported aggregate-name classification only; arguments, placement and
 * types require full binding. No allocation; false preserves kind. */
bool orm_sql_bind_aggregate_kind(const sqlparser_document *document,const sqlparser_node *node,
    orm_sql_aggregate_kind *kind);
/* Zero target storage required; slots index schema columns then parameters.
 * query_slots map local query inputs into scope->queries, independently of row
 * slots. Query output storage is required only when an expression uses queries.
 * Caller destroys program and releases both mappings on success AND partial failure.
 * Iterative O(document nodes*log(query_count+1) + document nodes +
 * references*(columns + log(parameters+1))), O(document nodes) workspace,
 * charged to scope budget. No row/storage access. */
turbodb_status_t orm_sql_bind_expression(const orm_sql_binding_scope *scope, sqlparser_id root,
    size_t max_depth, orm_sql_expression_target target, bool predicate, turbodb_error_t *error);
/* Resolves a plain or table-qualified column; per-column qualifiers override the
 * scope qualifier. Unqualified duplicates fail SQL_ERROR (ambiguous), qualified
 * lookup never falls back to another table. Leaves slot unchanged on error. */
turbodb_status_t orm_sql_bind_column(const orm_sql_binding_scope *scope,
    sqlparser_id name, size_t *slot, turbodb_error_t *error);
/* Exact marker count, fixed uint64_t source offsets, sorted with charged CSTL
 * scratch. Release offsets/bytes even on partial failure; borrows document. */
turbodb_status_t orm_sql_bind_parameter_offsets(const sqlparser_document *document,
    size_t count, orm_tidesdb_sql_budget *budget, vec_t *offsets, size_t *bytes, turbodb_error_t *error);
#endif

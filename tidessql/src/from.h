#ifndef ORM_TIDESDB_SQL_FROM_H
#define ORM_TIDESDB_SQL_FROM_H
#include "binding.h"
#include "join.h"

/* Private plan nodes, fixed addresses until whole-plan destruction. Logical
 * physical columns always follow SQL left/right order. Common JOIN columns
 * retain original qualified slots; qualified_only/star_order describe their
 * merged unqualified scope and star output separately. For reversed RIGHT joins the
 * executor must exchange children, use remapped ON slots, then project physical
 * right/left columns back to logical order. Parameters follow total pair width.
 * A leaf's table ordinal indexes caller-provided schemas/sources, not table ID. */
typedef struct orm_sql_from_node {
  sqlparser_id ast;
  size_t left, right, table;
  bool leaf, reversed, lateral;
  orm_sql_join_kind kind;
  char name[ORM_SQL_SELECT_NAME_BYTES+1], qualifier[ORM_SQL_SELECT_NAME_BYTES+1];
  orm_sql_table_schema schema;
  vec_t columns, names, slots, query_slots;
  size_t column_bytes, name_bytes, slot_bytes, query_slot_bytes;
  orm_sql_expr condition;
  bool common;
  vec_t keys;
  size_t key_bytes, key_count;
} orm_sql_from_node;
typedef struct orm_sql_from {
  orm_tidesdb_sql_budget *budget;
  vec_t nodes, parameter_types;
  size_t node_bytes, parameter_bytes, metadata_bytes, count, tables, active_runs;
  size_t parameter_marker_count;
  sqlparser_id root;
  bool correlated;
  sqlparser_id statement_from;
  bool conditions_bound, contains_lateral;
} orm_sql_from;

typedef struct orm_sql_from_table {
  char name[ORM_SQL_SELECT_NAME_BYTES+1];
  sqlparser_id ast;
  sqlparser_id derived; /* TABLE AST identity, zero for Catalog relations. */
} orm_sql_from_table;
/* Validate FROM shape before Catalog access. Owns plain table names or derived
 * aliases in SQL occurrence order; derived AST identities never name Catalog
 * relations. Caller releases the vector with orm_sql_work_release and returned bytes.
 * Accepts SELECT and default/TRADITIONAL EXPLAIN SELECT. Failure leaves out empty. */
turbodb_status_t orm_tidesdb_sql_from_tables(const sqlparser_document *document,
    size_t max_depth, orm_tidesdb_sql_budget *budget, vec_t *out, size_t *bytes, turbodb_error_t *error);

/* Stable address, one synchronous owner. Borrows plan and independent leaf
 * sources through close, owns operators and RIGHT output projections. Sources
 * and plan may not be changed while open. No native handles owned here. */
typedef struct orm_sql_from_run {
  orm_sql_from *plan;
  vec_t nodes;
  size_t node_bytes, metadata_bytes;
  orm_sql_row_source *source;
  vec_t dependent_state;
  size_t dependent_bytes;
} orm_sql_from_run;
/* Source order matches table occurrences. Opens bottom-up without pulling rows;
 * copies parameters in operators. Bounded by shared plan/work/step/pair limits.
 * Failed open releases all new leases. Close consumer before run (otherwise
 * BUSY unchanged), then plan, then native sources. Reentrant close during
 * evaluation is also BUSY unchanged. NULL/empty close is inert. */
turbodb_status_t orm_tidesdb_sql_from_open(orm_sql_from *plan,
    orm_sql_row_source *const *sources, size_t count, const turbodb_value_t *parameters,
    size_t parameter_count, orm_sql_from_run *out, turbodb_error_t *error);
/* Same contract, with ON dependencies in bind_at's registry order. Query slots
 * are never remapped by RIGHT JOIN. Sources outlive the entire FROM run. */
turbodb_status_t orm_tidesdb_sql_from_open_queries(orm_sql_from *plan,
    orm_sql_row_source *const *sources, size_t count, const turbodb_value_t *parameters,
    size_t parameter_count, const orm_sql_expr_query_sources *queries,
    orm_sql_from_run *out, turbodb_error_t *error);
turbodb_status_t orm_tidesdb_sql_from_close(orm_sql_from_run *run, turbodb_error_t *error);

/* Explicit private dependent execution of a complete, condition-bound FROM.
 * Every occurrence has an independent, stable source. Ordinary inputs require
 * rewind (also called before their first pull); lateral inputs require paired
 * binding callbacks and capture types in completed-prefix then outer-frame order.
 * Capture types include all visible columns, even unused ones. Callbacks and
 * source owners outlive run close. No AST or provider calls during construction.
 * binding follows join_right_binding's failed-open/borrow contract; provider
 * close cannot reenter owner close (BUSY), including a single root lateral leaf.
 * Opens operators lazily; every right subtree containing LATERAL reopens per left
 * row. Provider close follows its scan closure, before ancestors advance. Close
 * failure retains storage/borrows for retry. Construction arrays/parameters may
 * die after success; query-source owners remain borrowed. Same existing budgets,
 * first-error latch and consumer-before-close contract. No raw SQL admission.
 * Scope-subtree plans reject: external same-SELECT prefixes require the complete
 * tree. Setup O(nodes*depth + tables^2 + schema/frame widths), storage
 * O(nodes*depth + schema/frame widths), O(frame width) per capture round,
 * plus ordinary operator costs. Example: bind subtree schema for full FROM,
 * bind conditions, open_dependent, consume run.source, from_close, destroy plan. */
typedef struct orm_sql_from_input {
  orm_sql_row_source *source;
  void *context;
  turbodb_status_t (*rewind)(void *context,turbodb_error_t *error);
  orm_sql_join_right_binding binding;
  const orm_sql_type *capture_types;
  size_t capture_count;
} orm_sql_from_input;
turbodb_status_t orm_sql_from_open_dependent(orm_sql_from *plan,
    const orm_sql_from_input *inputs,size_t count,const turbodb_value_t *parameters,
    size_t parameter_count,const orm_sql_expr_query_sources *queries,
    orm_sql_from_run *out,turbodb_error_t *error);
/* Internal close dispatch; callers use from_close for either execution mode. */
turbodb_status_t orm_sql_from_dependent_close(orm_sql_from_run *run,turbodb_error_t *error);

/* Bind only FROM and ON of one MySQL SELECT (or default/TRADITIONAL EXPLAIN
 * SELECT). Each plain or derived table occurrence needs
 * its own schema entry, in left-to-right SQL order, including self joins. Plain
 * tables, aliased derived query sources and INNER/LEFT/RIGHT/CROSS chains supported;
 * USING/NATURAL/FULL and table modifiers rejected. Caller binds derived queries
 * independently before supplying their output schemas. Other SELECT clauses are not validated
 * by this API and must pass the SELECT Binder before execution. All statement
 * markers count toward parameter_count and retain source order. A scoped
 * correlated bind appends outer-column types after those markers while keeping
 * parameter_marker_count separate for source-offset validation.
 * Zero output required, one synchronous owner. Owns compiled ON, mappings,
 * names, columns and parameter types; document/schemas can die after success.
 * O(nodes + tables^2 + columns^2 + ON binding work), bounded by depth/plan/work/
 * execution budgets. No recursion, storage access, or schema inference.
 * Failure leaves out empty and refunds all work; consumed counters remain. */
turbodb_status_t orm_tidesdb_sql_from_bind(const sqlparser_document *document,
    const orm_sql_table_schema *const *schemas, size_t schema_count,
    const orm_sql_type *parameter_types, size_t parameter_count, size_t max_depth,
    orm_tidesdb_sql_budget *budget, orm_sql_from *out, turbodb_error_t *error);
/* Same ownership/validation contracts, targeting a SELECT block in a compound
 * document. Full-document parameter numbering is preserved. */
turbodb_status_t orm_tidesdb_sql_from_tables_at(const orm_sql_query_scope *scope,
    vec_t *out, size_t *bytes, turbodb_error_t *error);
turbodb_status_t orm_tidesdb_sql_from_bind_at(const orm_sql_query_scope *scope,
    const orm_sql_table_schema *const *schemas, size_t count, orm_sql_from *out, turbodb_error_t *error);
/* Two-phase private binding for correlation discovery. The schema phase owns a
 * complete immutable FROM tree and combined root schema but deliberately leaves
 * every ON expression empty. This lets a dependency graph bind against the
 * outer joined row before query-source slots exist. After dependencies publish
 * their registry, conditions_at completes every ON expression exactly once;
 * scoped outer columns become appended parameter slots and mark the plan
 * correlated.
 * A schema-only plan is nonexecutable, including unconditional joins: execution
 * requires successful conditions_at exactly once. Destroy handles either phase;
 * neither entry reads rows or opens executors. */
turbodb_status_t orm_tidesdb_sql_from_bind_schema_at(const orm_sql_query_scope *scope,
    const orm_sql_table_schema *const *schemas,size_t count,orm_sql_from *out,turbodb_error_t *error);
/* Metadata preparation for an actual TABLE/JOIN subtree of scope's SELECT FROM.
 * Never traverses TABLE query bodies as sibling sources. Only the selected tree
 * needs schemas, in its SQL occurrence order; statement markers keep full-document
 * numbering. LATERAL leaves are accepted as metadata with caller-declared output
 * schemas, without granting execution. Other FROM branches are not bound.
 * Identity/depth/shape failures leave output empty; same owned-name/schema,
 * budget and destroy contracts as the full-tree schema phase. Conditions_at
 * can complete ordinary subtrees before execution while the AST is still alive. */
turbodb_status_t orm_sql_from_subtree_tables_at(const orm_sql_query_scope *scope,
    sqlparser_id subtree,vec_t *out,size_t *bytes,turbodb_error_t *error);
turbodb_status_t orm_sql_from_subtree_schema_at(const orm_sql_query_scope *scope,
    sqlparser_id subtree,const orm_sql_table_schema *const *schemas,size_t count,
    orm_sql_from *out,turbodb_error_t *error);
/* Construction-only LATERAL visibility in one SELECT FROM. Returns disjoint,
 * completed sibling subtree IDs preceding this exact LATERAL TABLE. JOIN order
 * is left/right except RIGHT, which visits right/left. TABLE query bodies are
 * opaque. Prefixes share one lexical depth; each subtree keeps its SQL column
 * order. No self, later source, ON binding or Catalog access. Empty prefix is
 * valid. Requires zero output and nonzero target; errors refund all WORK.
 * Caller releases the ID vector with work_release before budget_end. O(AST
 * nodes), O(AST nodes) temporary space, bounded by depth/work/plan/steps. */
turbodb_status_t orm_sql_from_lateral_prefixes_at(const orm_sql_query_scope *scope,
    sqlparser_id lateral,vec_t *out,size_t *bytes,turbodb_error_t *error);
turbodb_status_t orm_tidesdb_sql_from_bind_conditions_at(const orm_sql_query_scope *scope,
    orm_sql_from *plan,turbodb_error_t *error);
/* Root at ordinal zero; child ordinals exceed parent. NULL if out of bounds. */
const orm_sql_from_node *orm_tidesdb_sql_from_at(const orm_sql_from *plan, size_t ordinal);
/* Caller must first close every executor that borrows this plan. Active ON runs
 * and FROM runs reject the entire destroy with BUSY before releasing any node. */
turbodb_status_t orm_tidesdb_sql_from_destroy(orm_sql_from *plan, turbodb_error_t *error);
#endif

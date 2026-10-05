#ifndef ORM_TIDESDB_SQL_DEPENDENCIES_H
#define ORM_TIDESDB_SQL_DEPENDENCIES_H
#include "binding.h"
#include "catalog_store.h"
#include "explain.h"

struct orm_sql_query;
/* Private statement-owned expression and FROM query dependencies, stable through close.
 * Nodes contain independent queries in one Catalog snapshot, cache owners and
 * borrowed row adapters. Expression/FROM registries use AST-id order. Nodes also
 * include CTE definitions, and order is topological over containment plus CTE
 * reference edges. CTE definitions own caches; occurrences own independent readers.
 * Caller closes its consuming SELECT/compound before this owner. No data reads
 * during construction (Catalog metadata is read and charged). AST/parameters
 * may die after successful runtime open. */
typedef struct orm_sql_dependencies {
  orm_tidesdb_sql_budget *budget;
  vec_t nodes, bindings, sources, order, derived;
  size_t node_bytes, binding_bytes, source_bytes, order_bytes, metadata_bytes, derived_bytes;
  size_t count, query_count, derived_count, prepared, first, position;
  bool describe,lateral_frames,binding_only;
  struct orm_sql_query *root;
  orm_sql_row_source explanation;
  orm_sql_scan explained;
  orm_sql_schema_column columns[ORM_SQL_EXPLAIN_COLUMNS];
  orm_sql_type types[ORM_SQL_EXPLAIN_COLUMNS];
  orm_sql_evaluation evaluation;
} orm_sql_dependencies;

/* Finds and independently binds scalar/IN/EXISTS, derived and CTE dependencies; nested queries
 * precede consumers. EXISTS requests only row existence, with arbitrary logical
 * output width. Derived queries publish declared output schemas and row sources.
 * A direct scalar/IN/EXISTS child composed from SELECT/UNION/query groups whose
 * leaves have no FROM, plain tables, plain-table JOINs, owned nested derived
 * inputs or nonrecursive CTEs may bind the root's current row. Root FROM query
 * sources publish their schemas before top-level expression dependencies; a
 * single CTE or a JOIN containing CTE occurrences therefore exposes its complete
 * qualified row contract to direct scalar/IN/EXISTS children. Correlated query
 * descendants reopen from inner to outer for each callback and close in reverse
 * order. Correlated CTE definitions rebuild one shared bounded store per outer row;
 * their readers, including occurrences in nested callbacks, close before the store
 * and reopen only for the consuming callback. Stable CTE caches keep their existing
 * readers and rewind only the current callback's occurrences. The compiled
 * dependency plan reopens and closes once
 * per callback; byte results are retained in bounded owned snapshots until
 * consumers close. Noncorrelated nodes retain their one-query lazy caches.
 * Nested expression-query chains compose the immediate SELECT input schema
 * with ancestor frames, resolving names nearest first. Each nested callback
 * combines the synchronous parent row with the active parent's captured values;
 * SQL markers retain their original slots. Metadata-only FROM owners close
 * after binding; compiled plans and fixed argument storage remain through close.
 * Grouped consumers map captured input columns to direct GROUP BY keys after
 * descendants mark their actual references. Ungrouped captures fail SQL_ERROR;
 * WHERE/key/aggregate-argument consumers retain their original input rows.
 * Derived/CTE definitions inherit the enclosing outer frame without
 * adding their SELECT's sibling inputs. Expression children add the definition's
 * local input row; per-node capture flags propagate actual ancestor references
 * before consumer binding. Recursive definitions may capture their enclosing
 * frame directly in seed/member plans, including through preceding CTEs. Each
 * correlated callback reopens seed and a fresh recursive cache with the original
 * marker prefix. Seed dependencies bind before the nullable self schema is
 * published; member dependencies then bind against that schema before completing
 * the same recursive owner. Expression children may capture each frontier row
 * and inherited ancestors through owned derived/CTE definitions, including nested
 * recursive definitions. Self table occurrences remain direct member FROM inputs.
 * LATERAL definitions additionally bind completed same-SELECT prefixes and
 * publish stable FROM input callbacks. Their compiled queries and owned marker
 * snapshots reopen for each prefix row, with nested LATERAL as a separate
 * callback boundary. Ordinary derived and CTE providers offer rewind; recursive
 * self providers rewind only their current immutable frontier. Close needs no
 * remaining execution-step quota; BUSY retains the owner for explicit retry.
 * Empty dependency set leaves zero output. Errors leave closeable partial output;
 * runtime closes consumers then dependencies and preserves the original error.
 * CTE lexical binding is O(AST nodes^2); ordinary discovery/sort O(dependencies^2).
 * Nested frame discovery is O(dependencies * AST nodes); source prerequisites
 * can add O(dependencies^3 * depth) topological checks, charged to the step limit.
 * O(AST nodes + dependencies) construction registry space;
 * each child additionally consumes its normal bind/open resources. All bounded
 * by the same AST/plan/work/step/depth limits. */
turbodb_status_t orm_sql_dependencies_open(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner, const turbodb_value_t *parameters, bool describe,
    orm_sql_dependencies *out, turbodb_error_t *error);
/* Private construction with an explicit positive recursive iteration
 * limit. Same graph/registry ownership and failure cleanup as open. Validates
 * every recursive shape before binding dependencies; only direct validated self
 * occurrences bypass reference edges. Member streaming descendants replay each
 * round, cache descendants and seed inputs do not. Describe opens metadata scans
 * for seed/members and global pagination without execution or cache creation.
 * Pages over a subset of recursive members reject; ordinary open keeps its
 * existing recursive rejection. Does not change runtime's raw SQL admission.
 * Additional per-definition shape checks are O(AST^2), member classification
 * O(dependencies * (depth + shape nodes)), all charged and bounded.
 * Example: open_recursive, publish registries to runtime_scope_open, destroy AST,
 * consume root, close root, dependencies_close. */
turbodb_status_t orm_sql_dependencies_open_recursive(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner,const turbodb_value_t *parameters,bool describe,uint64_t max_iterations,
    orm_sql_dependencies *out,turbodb_error_t *error);
/* Explicit private compilation-only graph, also admitting LATERAL definitions.
 * LATERAL frames add only completed same-SELECT prefixes, then inherited outer
 * frames; prefix schemas publish before their consumers. Nested scalar/IN/EXISTS,
 * derived/CTE definitions share existing capture propagation. max_iterations=0
 * rejects recursion; positive values retain the existing recursive constraints.
 * No business reads/cache, evaluable sources or replay; derived bindings publish
 * schemas with source=NULL. Construction frames close after binding. Fixed plans,
 * capture metadata and parameter snapshots remain stable through close; AST and
 * input parameters may die after success. Same partial-failure cleanup/budgets
 * as open. LATERAL prefix prerequisites add at most one depth factor to source
 * sorting: O(dependencies^3 * depth^2), with each scan charged to the step limit.
 * Production runtime does not call this entry. */
turbodb_status_t orm_sql_dependencies_lateral_metadata_open(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner,const turbodb_value_t *parameters,uint64_t max_iterations,
    orm_sql_dependencies *out,turbodb_error_t *error);
/* Type-only metadata graph for #206. No parameter values/snapshots, runtime
 * caches, EXPLAIN operators or business reads. Nonrecursive WITH/derived,
 * LATERAL and scalar/IN/EXISTS share existing lexical/capture/shape checks;
 * recursive definitions reject. Publishes schemas/binding types only, with
 * no evaluable sources; execution reuse rejects. AST/types may die on success.
 * Stable graph/owner/budget live through reverse-topological close. Partial
 * failure remains closeable, following open's cleanup and bounded complexity.
 * Usage: query_bind owns this graph; close root consumers before graph close.
 * Formal examples/fault tests: ../tests/integration/runtime_test.c. */
turbodb_status_t orm_sql_dependencies_bind(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner,orm_sql_dependencies *out,turbodb_error_t *error);
/* Construction-only metadata composition; no business runs or query eval.
 * Root must be fully bound, at its final address, before attaching this scan. */
turbodb_status_t orm_sql_dependencies_explain(struct orm_sql_query *root, turbodb_error_t *error);
/* Reverse-topological destruction. BUSY can follow completed node closures;
 * prepared records the remaining prefix so a retry never releases it twice.
 * After a partial close, release outstanding consumers and retry close only. */
turbodb_status_t orm_sql_dependencies_close(orm_sql_dependencies *run, turbodb_error_t *error);
/* Same-statement replay, coordinated by runtime after root consumers close.
 * Only streaming derived queries and CTE references outside cache boundaries
 * rewind. Scalar/IN/EXISTS and CTE producers/caches remain PENDING/READY/FAILED.
 * Close in reverse dependency order, open in forward order; no AST or row reads.
 * All sources/metadata retain addresses. Parameters must be the original owned
 * statement values. Open failure requires execution_close and final close; no
 * automatic retry. O(nodes + query reopen costs), charged work/steps. */
turbodb_status_t orm_sql_dependencies_execution_close(orm_sql_dependencies *run, turbodb_error_t *error);
turbodb_status_t orm_sql_dependencies_execution_open(orm_sql_dependencies *run, const turbodb_value_t *parameters,
    size_t parameter_count, turbodb_error_t *error);
#endif

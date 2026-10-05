#ifndef ORM_TIDESDB_SQL_CTE_QUERY_H
#define ORM_TIDESDB_SQL_CTE_QUERY_H
#include "compound.h"
#include "cte_store.h"

/* Borrowed external dependency owner. open prepares/rewinds member-only streaming
 * inputs before member execution opens; close releases their execution after all
 * member consumers close, including partial open failure. Both callbacks and
 * context required together; close must be retryable and need no remaining quota.
 * Do not reset seed inputs, unrelated consumers or scalar/set/CTE caches. Parameters
 * borrow this call. Descriptor is copied, context survives the CTE query.
 * No callbacks during construction or EXPLAIN. Already prepared inputs must allow
 * construction-time member binding/open without consuming rows. */
typedef struct orm_sql_cte_query_rounds {
  void *context;
  turbodb_status_t (*open)(void *context,const turbodb_value_t *parameters,size_t count,turbodb_error_t *error);
  turbodb_status_t (*close)(void *context,turbodb_error_t *error);
} orm_sql_cte_query_rounds;
typedef struct orm_sql_cte_query_spec {
  uint64_t max_iterations;
  bool describe;
  const orm_sql_expr_query_sources *sources;
  orm_sql_cte_query_rounds rounds;
} orm_sql_cte_query_spec;

/* Private single-threaded recursive definition owner, stable and zero before open.
 * Owns both compound plans, nullable seed schema, SQL marker payloads, independent
 * self proxies, copied expression-source registry and one lazy recursive cache.
 * Immutable evaluation policy survives seed/member execution reopens; its
 * diagnostic receiver borrows the statement owner through query_close.
 * Borrowed Catalog, budget and external dependencies outlive this owner. Fields
 * are read-only to callers. Only cache readers may consume business output;
 * describe exposes the two compound metadata scans, without a business cache.
 * Close all readers/metadata consumers first. No concurrent or reentrant mutation. */
typedef struct orm_sql_cte_query {
  orm_tidesdb_sql_budget *budget;
  orm_sql_compound initial,recursive;
  orm_sql_cte_schema schema;
  orm_sql_cte_store cache;
  orm_sql_snapshot parameters;
  orm_sql_cte_recursion recursion;
  vec_t proxies,sources;
  size_t proxy_bytes,source_bytes,metadata_bytes,parameter_count,marker_count;
  orm_sql_row_source initial_source,recursive_source;
  orm_sql_explain_source pagination;
  orm_sql_scan explained_page;
  orm_sql_cte_query_rounds rounds;
  orm_sql_evaluation evaluation;
  const sqlparser_document *binding_document;
  sqlparser_id binding_definition;
  bool inputs_open,describe,execution_closed,complete;
} orm_sql_cte_query;

/* scope contains definition; references is resolve(scope)'s lexical mapping.
 * Validates shape, compiles complete seed, binds schema/self occurrences, then
 * compiles/type-checks members before opening the lazy cache. Infers cumulative
 * DISTINCT from recursive UNION edges, never from seed-only DISTINCT.
 * SQL marker buffers/AST/registry arrays may die after return; external source
 * descriptors and callback contexts may not. If scope.outer_schema is supplied,
 * parameters additionally includes its complete typed capture suffix; those
 * values borrow through execution_close. defer_execution binds both parts without
 * opening seed execution; its initial cache must close before execution_open.
 * External FROM registry requires round callbacks
 * for execution; the owner selects member-only dependencies, not the CTE query.
 * Positive explicit iteration limit; no implicit default or max_depth reuse.
 * Whole-definition LIMIT/OFFSET bounds generation and external output. Nested
 * pages on its outer unary group chain compose; pages over a subset of members
 * reject UNSUPPORTED. Does not discover dependencies or admit driver raw SQL.
 * O(AST^2) structural binding plus ordinary compound binding; O(AST + bindings +
 * parameters/payload) fixed owner work, shared quotas and existing cache bounds.
 * No business reads at open. Failure refunds owned work and zeroes out; if cleanup
 * is BUSY, retain the closeable owner until its consumers release.
 * Example: resolve, query_open, reader_open(&query.cache), pull, reader_close,
 * query_close. EXPLAIN consumes initial.scan, recursive.scan, then explained_page
 * when its budget is non-NULL. */
turbodb_status_t orm_sql_cte_query_open(const orm_sql_query_scope *scope,sqlparser_id definition,
    const vec_t *references,orm_sql_catalog_store *owner,const turbodb_value_t *parameters,
    const orm_sql_cte_query_spec *spec,orm_sql_cte_query *out,turbodb_error_t *error);
turbodb_status_t orm_sql_cte_query_close(orm_sql_cte_query *query,turbodb_error_t *error);
/* Private staged binding for dependency frames. Seed open publishes the only
 * nullable schema without business execution. Member open completes that same
 * owner after member expression sources have bound. Keep the original AST and
 * scope parameter layout through both calls; registries may publish additional
 * entries in between. Self entries must be unique, source-less and borrow the
 * exact published seed columns/count; member binding replaces them with frontier
 * proxies. External FROM entries require the original round callbacks.
 * Partial owners are closeable, never executable. Invalid phase inputs return
 * INVALID_ARGUMENT without altering a valid partial owner; binding failures
 * close owned work unless consumers keep it BUSY. Example: seed_open, publish
 * query.schema.schema, bind member expression dependencies, member_open. */
turbodb_status_t orm_sql_cte_query_seed_open(const orm_sql_query_scope *scope,
    sqlparser_id definition,const vec_t *references,orm_sql_catalog_store *owner,
    const turbodb_value_t *parameters,const orm_sql_cte_query_spec *spec,
    orm_sql_cte_query *out,turbodb_error_t *error);
turbodb_status_t orm_sql_cte_query_member_open(const orm_sql_query_scope *scope,
    sqlparser_id definition,const vec_t *references,orm_sql_catalog_store *owner,
    const turbodb_value_t *parameters,const orm_sql_expr_query_sources *sources,
    orm_sql_cte_query *out,turbodb_error_t *error);
/* Private same-statement execution lifecycle. Close all cache readers first;
 * compiled plans/schema and marker payloads retain their addresses. Open borrows
 * only the appended outer-frame values through execution_close, which clears
 * those slots. SQL markers must be the original statement parameters. Open
 * failure requires close before another open; close needs no remaining quota.
 * Does not rediscover AST, change recursion rules or reset external caches. */
turbodb_status_t orm_sql_cte_query_execution_close(orm_sql_cte_query *query,turbodb_error_t *error);
turbodb_status_t orm_sql_cte_query_execution_open(orm_sql_cte_query *query,
    const turbodb_value_t *parameters,size_t count,turbodb_error_t *error);
#endif

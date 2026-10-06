#ifndef ORM_SQL_PREPARED_H
#define ORM_SQL_PREPARED_H
#include "parameters.h"
#include <tidessql/tidessql.h>

/* Private durable metadata owner, single-threaded at a stable address. One
 * dedicated active budget owns fixed SQL/types/columns/name/table/schema Vec storage.
 * No AST, parameter values, Catalog lease or physical plan survives open.
 * Each physical Catalog source occurrence, including sources below query
 * dependencies, has one name + table_id + canonical schema slice; lexical CTE
 * references are excluded. DML versions are deliberately excluded. Any mismatch
 * latches invalidated.
 * DDL owns SQL only; its zero-result command intent binds the current Catalog
 * on every execution and does not cache schema. All fields read-only to
 * consumers except check's invalidation transition. */
typedef struct orm_sql_prepared_table {
  char name[ORM_SQL_SELECT_NAME_BYTES+1];
  uint64_t id;
  size_t schema_offset,schema_size;
} orm_sql_prepared_table;
typedef struct orm_sql_prepared {
  orm_tidesdb_sql_budget budget;
  vec_t sql,types,columns,names,tables,schema;
  size_t sql_bytes,type_bytes,column_bytes,name_bytes,table_bytes,schema_bytes,owner_bytes;
  bool rows,invalidated;
} orm_sql_prepared;

/* Zero output. metadata_limits must already bound the remaining session bytes;
 * owner_bytes includes the embedding opaque SDK object. SQL/AST/owner and the
 * SHOW display label and evaluation context borrow only this call. Ordinary
 * table/JOIN and supported bounded recursive/nonrecursive CTE/derived/scalar/IN/EXISTS inference,
 * scalar-inferable SHOW WHERE/EXPLAIN metadata, plus zero-parameter DDL using read-only
 * structural validators; expressions and defaults never evaluate during
 * preparation and query sources never advance.
 * Failure closes all partial ownership and empties out; source cleanup failure
 * poisons the Catalog owner. Owned WORK remains active until prepared_close.
 * Open costs ordinary statement inference/binding + O(SQL + source schema +
 * column bytes). Retained space is O(SQL + source schemas + parameters +
 * columns/name bytes). */
turbodb_status_t orm_sql_prepared_open(const sqlparser_document *document,vstr sql,vstr database_name,
    orm_sql_evaluation evaluation,orm_sql_catalog_store *owner,const orm_sql_budget_limits *metadata_limits,
    const tdsql_limits *limits,size_t max_depth,uint64_t max_iterations,size_t owner_bytes,
    orm_sql_prepared *out,turbodb_error_t *error);
/* Current execution snapshot, before reading any parameters or business rows.
 * Same-table DML leaves the descriptor valid; any missing/recreated/changed
 * source schema returns INVALID_STATE and latches invalidated, without altering
 * the Catalog.
 * No automatic reprepare, writes, expression evaluation or diagnostics. O(schema
 * bytes), bounded temporary WORK/read/step costs in the execution budget. */
turbodb_status_t orm_sql_prepared_check(orm_sql_prepared *prepared,
    orm_sql_catalog_store *owner,turbodb_error_t *error);
/* Consumes zero/partial/invalidated metadata, refunds all retained WORK and ends
 * its dedicated budget. Never accesses native DB. Caller frees embedding object
 * and quarantines its session on failure. */
turbodb_status_t orm_sql_prepared_close(orm_sql_prepared *prepared,turbodb_error_t *error);
#endif

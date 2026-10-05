#ifndef ORM_SQL_PREPARED_H
#define ORM_SQL_PREPARED_H
#include "parameters.h"
#include <tidessql/tidessql.h>

/* Private durable metadata owner, single-threaded at a stable address. One
 * dedicated active budget owns fixed SQL/types/columns/name/schema Vec storage.
 * No AST, parameter values, Catalog lease or physical plan survives open.
 * table_id + canonical schema bytes identify one plain table; DML versions are
 * deliberately excluded. A schema mismatch latches invalidated until close.
 * DDL owns SQL only; its zero-result command intent binds the current Catalog
 * on every execution and does not cache schema. All fields read-only to
 * consumers except check's invalidation transition. */
typedef struct orm_sql_prepared {
  orm_tidesdb_sql_budget budget;
  vec_t sql,types,columns,names,schema;
  size_t sql_bytes,type_bytes,column_bytes,name_bytes,schema_bytes,owner_bytes;
  char table[ORM_SQL_SELECT_NAME_BYTES+1];
  uint64_t table_id;
  bool rows,invalidated;
} orm_sql_prepared;

/* Zero output. metadata_limits must already bound the remaining session bytes;
 * owner_bytes includes the embedding opaque SDK object. SQL/AST/owner borrow
 * only this call. Single-table inference scope plus zero-parameter DDL using
 * read-only structural validators; defaults never evaluate during preparation.
 * Failure closes all partial ownership and empties out; source cleanup failure
 * poisons the Catalog owner. Owned WORK remains active until prepared_close.
 * Open costs ordinary statement inference/binding + O(SQL/schema/column bytes).
 * Retained space is O(SQL + schema + parameters + columns/name bytes). */
turbodb_status_t orm_sql_prepared_open(const sqlparser_document *document,vstr sql,
    orm_sql_catalog_store *owner,const orm_sql_budget_limits *metadata_limits,
    const tdsql_limits *limits,size_t max_depth,size_t owner_bytes,
    orm_sql_prepared *out,turbodb_error_t *error);
/* Current execution snapshot, before reading any parameters or business rows.
 * Same-table DML leaves the descriptor valid; missing/recreated/changed schema
 * returns INVALID_STATE and latches invalidated, without altering the Catalog.
 * No automatic reprepare, writes, expression evaluation or diagnostics. O(schema
 * bytes), bounded temporary WORK/read/step costs in the execution budget. */
turbodb_status_t orm_sql_prepared_check(orm_sql_prepared *prepared,
    orm_sql_catalog_store *owner,turbodb_error_t *error);
/* Consumes zero/partial/invalidated metadata, refunds all retained WORK and ends
 * its dedicated budget. Never accesses native DB. Caller frees embedding object
 * and quarantines its session on failure. */
turbodb_status_t orm_sql_prepared_close(orm_sql_prepared *prepared,turbodb_error_t *error);
#endif

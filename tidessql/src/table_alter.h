#ifndef ORM_TIDESDB_SQL_TABLE_ALTER_H
#define ORM_TIDESDB_SQL_TABLE_ALTER_H
#include "catalog_store.h"

/* One MySQL ALTER TABLE rename-table/rename-column, SET/DROP DEFAULT or ADD/DROP
 * COLUMN on a persistent unqualified
 * table. Synchronous single owner; requires writable active statement with no
 * live sources. Borrows document through return. Rename preserves ordinal/type
 * and physical entries. ADD inserts BIGINT/[UNSIGNED]/DOUBLE with NULL/NOT NULL,
 * optionally FIRST or AFTER an existing column, otherwise at the end;
 * DROP requires a non-PK, non-indexed column and remaps remaining ordinals.
 * ADD also remaps surviving PK/index ordinals; missing AFTER target -> SQL_ERROR.
 * SET/DROP DEFAULT changes metadata only, preserving old rows/index keys.
 * Reuses CREATE's finite numeric/NULL constant defaults; failed binding preserves
 * old metadata. DROP DEFAULT leaves strict NOT NULL omissions as errors.
 * ADD fills old rows with the explicit default, or NULL when nullable without a
 * default; nonempty ADD NOT NULL without default -> UNSUPPORTED. ADD/DROP
 * validate the complete old row/index snapshot, then atomically rewrite Data
 * and schema. Orphan/missing/corrupt entries -> DATASTORE_ERROR and poison.
 * IDs and Data/index wire preserved; schema reuses existing v1/v2 default
 * encoding rules. No implicit commit.
 * Uses bounded WORK/MATERIALIZED_ROWS/PLAN/AST/READ/WRITE/STEP and one rollbackable
 * store batch. Missing names/duplicate column -> SQL_ERROR; occupied table ->
 * CONSTRAINT; unsupported ALTER -> UNSUPPORTED; exhausted version -> LIMIT.
 * Native cleanup failure poisons owner; full transaction rollback required.
 * See index_store_test.c and relational_test.c for executable use. */
turbodb_status_t orm_sql_table_alter(const sqlparser_document *, orm_sql_catalog_store *, turbodb_error_t *);
/* PREPARE: same structural rules using Catalog/index directories only. No
 * DEFAULT evaluation, business rows, physical index audit, writes or retained
 * definitions. Current metadata is checked again during EXECUTE. */
turbodb_status_t orm_sql_table_alter_validate(const sqlparser_document *, orm_sql_catalog_store *, turbodb_error_t *);
#endif

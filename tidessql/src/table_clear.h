#ifndef ORM_TIDESDB_SQL_TABLE_CLEAR_H
#define ORM_TIDESDB_SQL_TABLE_CLEAR_H
#include "catalog_store.h"
/* One MySQL DROP TABLE [IF EXISTS] name [, name] ... [RESTRICT|CASCADE] or TRUNCATE [TABLE] name,
 * no parameters. Requires writable owner, no active sources. Fully validates
 * every target's Data and indexes before one atomic batch. DROP removes directories/version; TRUNCATE
 * preserves definitions/IDs and advances version even when empty. No implicit
 * commit, format migration or ID reuse. O(R*C+E log E*K) time, O(R*C+E*K) work,
 * admitted by existing row/work/read/write/step quotas. Missing -> SQL_ERROR
 * unless DROP IF EXISTS. Corruption poisons owner; batch failure rolls back
 * only this command unless cleanup fails. Without IF EXISTS any missing DROP
 * target rejects the whole statement before writes. Caller commits for durability.
 * Public SQL examples and transaction rollback tests: relational_test.c. */
turbodb_status_t orm_sql_table_clear(const sqlparser_document *, orm_sql_catalog_store *, turbodb_error_t *);
/* Read-only PREPARE validation: names/existence only, no row/index reads or
 * mutation; IF EXISTS keeps its ordinary command intent. Owns no output. */
turbodb_status_t orm_sql_table_clear_validate(const sqlparser_document *, orm_sql_catalog_store *, turbodb_error_t *);
#endif

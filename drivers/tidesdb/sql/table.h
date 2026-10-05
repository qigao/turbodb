#ifndef ORM_TIDESDB_SQL_TABLE_H
#define ORM_TIDESDB_SQL_TABLE_H
#include <orm.h>
#include "select.h"
#include "bridge.h"

/* Private read-only adapter for existing ORMTDB v1 records. Explicit schema
 * and full table key prefix required, never inferred from data. The caller
 * keeps transaction/CF live and unchanged through close (no commit, rollback,
 * savepoint rollback, mutation or concurrent use while a scan is active).
 * Zero initialize; stable address after open. One synchronous owner. */
typedef struct orm_sql_table_source {
  orm_sql_row_source source;
  orm_tidesdb_transaction_t *transaction;
  orm_tidesdb_column_family_t *family;
  orm_tidesdb_iterator_t *iterator;
  vec_t prefix, names, types, fields, values;
  size_t prefix_bytes, name_bytes, type_bytes, field_bytes, value_bytes, metadata_bytes;
  size_t max_row_bytes;
  bool advance, done;
  orm_error_t failure;
} orm_sql_table_source;

/* Copies prefix/schema, preallocates fixed workspace, no native read. Bound
 * plan and schema must describe the same ordered columns. Prefix must delimit
 * exactly one table key range. Native key order has no numeric/SQL ordering
 * guarantee. First pull creates/seeks iterator. Rejects missing/extra fields,
 * corrupt records and type/nullability mismatch, including unprojected fields.
 * Charges one read row and key+value bytes before decoding. No driver workspace
 * allocation during pulls; native engine memory is outside this work budget.
 * Failure leaves empty output and refunds workspace. No data writes or format
 * changes. Use table_open, select_open_source(&table.source), scan_next/cancel,
 * select_close, table_close, then end the caller's transaction. */
orm_status_t orm_tidesdb_sql_table_open(orm_tidesdb_transaction_t *transaction,
    orm_tidesdb_column_family_t *family, vstr prefix, const orm_sql_table_schema *schema,
    size_t max_row_bytes, orm_tidesdb_sql_budget *budget, orm_sql_table_source *out, orm_error_t *error);
/* BUSY preserves an active source. NULL/empty no-op. Frees iterator/workspace,
 * never commits/rolls back/frees the caller's transaction. Close and reopen this
 * adapter to restart at the prefix; a source otherwise keeps its read position. */
orm_status_t orm_tidesdb_sql_table_close(orm_sql_table_source *source, orm_error_t *error);
#endif

#ifndef ORM_TIDESDB_SQL_ROWS_H
#define ORM_TIDESDB_SQL_ROWS_H
#include "value.h"
#include <cstl/vec.h>

typedef struct orm_sql_snapshot { vec_t values; size_t bytes; } orm_sql_snapshot;
/* Fixed owned scalar/payload copy, no MATERIALIZED_ROWS charge (also used for
 * parameters). Empty snapshot required; caller releases values/bytes through
 * work_release on success or partial failure. Caller owns scalar/schema checks;
 * this helper validates byte-copy shape and capacity only. */
turbodb_status_t orm_sql_snapshot_copy(orm_sql_snapshot *snapshot, const turbodb_value_t *row,
    size_t columns, size_t extra, orm_tidesdb_sql_budget *budget, turbodb_error_t *error);

/* Private owned row snapshots shared by sort and join. Single synchronous owner;
 * initialize only budget, keep it active through close. Metadata is included in
 * the containing operator's budget. Append deep copies borrowed TEXT/BLOB bytes;
 * extra reserves zeroed value slots for caller-computed sort keys/outputs.
 * Scalar/schema validation belongs to the caller; malformed payload pointers
 * and byte arithmetic fail before copy. No read or native transaction charges.
 * Every admitted snapshot charges MATERIALIZED_ROWS, copy steps and full WORK;
 * registry growth charges old+new capacity. No implicit spill or unbounded growth.
 * Append failure leaves out unchanged, latches failed, and requires close.
 * Returned values stay stable across appends until close; no mutation once
 * consumed by readers. O(columns+extra+payload) append, amortized O(1) registry growth;
 * total space O(rows*(columns+extra)+payload). Close refunds retained storage. */
typedef struct orm_sql_rows {
  orm_tidesdb_sql_budget *budget;
  vec_t snapshots;
  size_t snapshot_bytes, materialized;
  bool failed;
} orm_sql_rows;
turbodb_status_t orm_sql_rows_append(orm_sql_rows *run, const turbodb_value_t *row,
    size_t columns, size_t extra, turbodb_value_t **out, turbodb_error_t *error);
/* NULL for invalid index or failed store; O(1), no allocation or budget charge. */
const turbodb_value_t *orm_sql_rows_at(const orm_sql_rows *run, size_t index);
turbodb_status_t orm_sql_rows_close(orm_sql_rows *run, turbodb_error_t *error);
#endif

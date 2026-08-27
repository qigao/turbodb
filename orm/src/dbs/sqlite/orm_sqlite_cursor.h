#ifndef ORM_SQLITE_CURSOR_H
#define ORM_SQLITE_CURSOR_H

#include "orm_cbind_source.h"

#include <sqlite3.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * On success, moves *statement into out_cursor and clears *statement. On
 * failure, the caller retains the statement. The cursor must stay in the
 * SQLite connection's single-thread domain and finalizes the statement once.
 */
orm_status_t orm_sqlite_cursor_from_statement(orm_row_cursor *out_cursor,
                                              sqlite3_stmt **statement,
                                              orm_error_t *error);

/*
 * Legacy result projection: non-NULL, non-BLOB values are emitted as the
 * exact byte view returned by sqlite3_column_text(). This keeps the old
 * index-based result accessors byte-compatible while sharing cursor stepping,
 * cancellation, and statement ownership with the typed Source path.
 */
orm_status_t orm_sqlite_text_cursor_from_statement(
    orm_row_cursor *out_cursor, sqlite3_stmt **statement, orm_error_t *error);

#ifdef __cplusplus
}
#endif

#endif

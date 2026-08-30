#ifndef ORM_SQLITE_CURSOR_H
#define ORM_SQLITE_CURSOR_H

#include "orm_cbind_publisher.h"

#include <sqlite3.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct orm_sqlite_cursor_config {
  uint64_t max_result_rows;
  uint64_t max_result_bytes;
} orm_sqlite_cursor_config;

#define ORM_SQLITE_CURSOR_CONFIG_INIT(max_result_rows_, max_result_bytes_) \
  { (max_result_rows_), (max_result_bytes_) }

/* On success, moves *statement into out_cursor and clears *statement. */
orm_status_t orm_sqlite_cursor_from_statement(orm_row_cursor *out_cursor,
                                              sqlite3_stmt **statement,
                                              const orm_sqlite_cursor_config *config,
                                              orm_error_t *error);

#ifdef __cplusplus
}
#endif

#endif

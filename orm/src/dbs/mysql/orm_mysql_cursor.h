#ifndef ORM_MYSQL_CURSOR_H
#define ORM_MYSQL_CURSOR_H

#include "orm_row_publisher.h"

#include <mysql.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct orm_mysql_cursor_config {
  size_t max_columns;
  uint64_t max_result_rows;
  size_t max_result_bytes;
  uint64_t *affected_rows;
} orm_mysql_cursor_config;

#define ORM_MYSQL_CURSOR_CONFIG_INIT(max_columns_, max_rows_, max_bytes_, affected_) \
  { (max_columns_), (max_rows_), (max_bytes_), (affected_) }

orm_status_t orm_mysql_cursor_from_statement(
    orm_row_cursor *out_cursor,
    MYSQL_STMT **statement,
    const orm_mysql_cursor_config *config,
    orm_error_t *error);

#ifdef __cplusplus
}
#endif

#endif

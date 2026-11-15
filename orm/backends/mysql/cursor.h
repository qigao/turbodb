#ifndef TURBODB_ORM_MYSQL_CURSOR_H
#define TURBODB_ORM_MYSQL_CURSOR_H

#include "cursor_row.h"
#include "source.h"
#include "session_async.h"
#include "orm_row_publisher.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  MYSQL_CURSOR_CONFIG_ABI_VERSION = 1u
};

typedef struct mysql_cursor_config_t {
  size_t struct_size;
  uint32_t abi_version;
  uint64_t max_result_rows;
  uint64_t max_result_bytes;
  size_t max_columns;
  size_t max_metadata_bytes;
  size_t max_row_bytes;
} mysql_cursor_config_t;

#define MYSQL_CURSOR_CONFIG_INIT(max_rows_, max_result_bytes_, max_columns_,                                  max_metadata_bytes_, max_row_bytes_)           { sizeof(mysql_cursor_config_t), MYSQL_CURSOR_CONFIG_ABI_VERSION,              (max_rows_), (max_result_bytes_), (max_columns_),                            (max_metadata_bytes_), (max_row_bytes_) }

/*
 * Success moves source ownership into out_cursor and clears source.
 * Column metadata is copied into cursor-owned storage before return.
 * Each source row is borrowed only for the source->next() call; cursor copies
 * it immediately into one bounded current-row buffer.
 */
orm_status_t mysql_cursor_start(
    orm_row_cursor *out_cursor,
    mysql_cursor_source_t *source,
    const mysql_column_definition_t *columns,
    size_t column_count,
    const mysql_cursor_config_t *config,
    orm_error_t *error);

typedef struct mysql_cursor_async_owner {
  void *context;
  void (*release)(void *);
} mysql_cursor_async_owner;
/* Success moves source and its input owner. Failure leaves both caller-owned. */
orm_status_t mysql_cursor_start_async(orm_row_cursor *out,
    mysql_async_source *source, const mysql_cursor_config_t *config,
    const orm_async_config_t *async_config, mysql_cursor_async_owner owner,
    orm_error_t *error);

#ifdef __cplusplus
}
#endif

#endif

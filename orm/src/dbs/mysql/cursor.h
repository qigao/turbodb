#ifndef TURBODB_ORM_MYSQL_CURSOR_H
#define TURBODB_ORM_MYSQL_CURSOR_H

#include "cursor_row.h"
#include "orm_row_publisher.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  MYSQL_CURSOR_SOURCE_OPS_ABI_VERSION = 1u,
  MYSQL_CURSOR_CONFIG_ABI_VERSION = 1u
};

typedef enum mysql_cursor_source_step_kind_t {
  MYSQL_CURSOR_SOURCE_DONE = 0,
  MYSQL_CURSOR_SOURCE_ROW,
  MYSQL_CURSOR_SOURCE_ERROR
} mysql_cursor_source_step_kind_t;

typedef struct mysql_cursor_source_step_t {
  mysql_cursor_source_step_kind_t kind;
  orm_status_t status;
  const char *message;
  const uint8_t *row;
  size_t row_size;
} mysql_cursor_source_step_t;

#define MYSQL_CURSOR_SOURCE_STEP_INIT   { MYSQL_CURSOR_SOURCE_DONE, ORM_STATUS_OK, NULL, NULL, 0u }

typedef struct mysql_cursor_source_ops_t {
  size_t struct_size;
  uint32_t abi_version;
  mysql_cursor_source_step_t (*next)(void *context);
  void (*cancel)(void *context);
  void (*destroy)(void *context);
} mysql_cursor_source_ops_t;

typedef struct mysql_cursor_source_t {
  const mysql_cursor_source_ops_t *ops;
  void *context;
} mysql_cursor_source_t;

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

#ifdef __cplusplus
}
#endif

#endif

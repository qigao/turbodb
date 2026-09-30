#ifndef TURBODB_MYSQL_CURSOR_ROW_H
#define TURBODB_MYSQL_CURSOR_ROW_H

#include "row_reader.h"
#include "wire/row.h"

#include <cserde/reader.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mysql_cursor_row_store_t {
  mysql_column_definition_t *columns;
  mysql_binary_value_t *values;
  unsigned char *column_names;
  unsigned char *row_storage;
  size_t column_count;
  size_t column_name_bytes;
  size_t row_capacity;
  size_t row_size;
  mysql_row_reader_state reader_state;
} mysql_cursor_row_store_t;

mysql_wire_status_t mysql_cursor_row_store_init(
    mysql_cursor_row_store_t *store,
    const mysql_column_definition_t *columns,
    size_t column_count,
    size_t max_columns,
    size_t max_metadata_bytes,
    size_t max_row_bytes);

void mysql_cursor_row_store_destroy(mysql_cursor_row_store_t *store);

mysql_wire_status_t mysql_cursor_row_store_load(
    mysql_cursor_row_store_t *store,
    const uint8_t *payload,
    size_t payload_size);

cserde_status mysql_cursor_row_store_reader(
    mysql_cursor_row_store_t *store,
    cserde_reader *out_reader);

#ifdef __cplusplus
}
#endif

#endif

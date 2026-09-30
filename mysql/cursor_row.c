#include "cursor_row.h"

#include <stdlib.h>
#include <string.h>

static void mysql_cursor_row_store_clear(
    mysql_cursor_row_store_t *store) {
  if (store == NULL)
    return;
  free(store->row_storage);
  free(store->column_names);
  free(store->values);
  free(store->columns);
  memset(store, 0, sizeof(*store));
}

mysql_wire_status_t mysql_cursor_row_store_init(
    mysql_cursor_row_store_t *store,
    const mysql_column_definition_t *columns,
    size_t column_count,
    size_t max_columns,
    size_t max_metadata_bytes,
    size_t max_row_bytes) {
  size_t name_bytes = 0u;
  size_t offset = 0u;
  size_t i;

  if (store == NULL ||
      (columns == NULL && column_count != 0u) ||
      max_columns == 0u ||
      max_metadata_bytes == 0u ||
      max_row_bytes == 0u)
    return MYSQL_WIRE_STATUS_INVALID;

  memset(store, 0, sizeof(*store));

  if (column_count > max_columns)
    return MYSQL_WIRE_STATUS_LIMIT;

  for (i = 0u; i < column_count; ++i) {
    const mysql_wire_bytes_t name = columns[i].name;
    if (name.is_null ||
        (name.data == NULL && name.length != 0u))
      return MYSQL_WIRE_STATUS_INVALID;
    if (name.length > max_metadata_bytes - name_bytes)
      return MYSQL_WIRE_STATUS_LIMIT;
    name_bytes += name.length;
  }

  if (column_count != 0u) {
    if (column_count > SIZE_MAX / sizeof(*store->columns) ||
        column_count > SIZE_MAX / sizeof(*store->values))
      return MYSQL_WIRE_STATUS_LIMIT;

    store->columns = (mysql_column_definition_t *)calloc(
        column_count, sizeof(*store->columns));
    store->values = (mysql_binary_value_t *)calloc(
        column_count, sizeof(*store->values));
    if (store->columns == NULL || store->values == NULL) {
      mysql_cursor_row_store_clear(store);
      return MYSQL_WIRE_STATUS_LIMIT;
    }
  }

  if (name_bytes != 0u) {
    store->column_names = (unsigned char *)malloc(name_bytes);
    if (store->column_names == NULL) {
      mysql_cursor_row_store_clear(store);
      return MYSQL_WIRE_STATUS_LIMIT;
    }
  }

  store->row_storage = (unsigned char *)malloc(max_row_bytes);
  if (store->row_storage == NULL) {
    mysql_cursor_row_store_clear(store);
    return MYSQL_WIRE_STATUS_LIMIT;
  }

  for (i = 0u; i < column_count; ++i) {
    mysql_column_definition_t copied = columns[i];
    const size_t size = columns[i].name.length;

    copied.catalog = (mysql_wire_bytes_t){0};
    copied.schema = (mysql_wire_bytes_t){0};
    copied.table = (mysql_wire_bytes_t){0};
    copied.org_table = (mysql_wire_bytes_t){0};
    copied.org_name = (mysql_wire_bytes_t){0};

    if (size != 0u)
      memcpy(store->column_names + offset,
             columns[i].name.data, size);
    copied.name.data = size != 0u
                           ? store->column_names + offset
                           : NULL;
    copied.name.length = size;
    copied.name.is_null = false;
    store->columns[i] = copied;
    offset += size;
  }

  store->column_count = column_count;
  store->column_name_bytes = name_bytes;
  store->row_capacity = max_row_bytes;
  return MYSQL_WIRE_STATUS_OK;
}

void mysql_cursor_row_store_destroy(mysql_cursor_row_store_t *store) {
  mysql_cursor_row_store_clear(store);
}

mysql_wire_status_t mysql_cursor_row_store_load(
    mysql_cursor_row_store_t *store,
    const uint8_t *payload,
    size_t payload_size) {
  mysql_wire_status_t status;

  if (store == NULL || store->row_storage == NULL ||
      (payload == NULL && payload_size != 0u))
    return MYSQL_WIRE_STATUS_INVALID;
  if (payload_size > store->row_capacity)
    return MYSQL_WIRE_STATUS_LIMIT;

  if (payload_size != 0u)
    memcpy(store->row_storage, payload, payload_size);
  store->row_size = payload_size;

  if (store->column_count != 0u)
    memset(store->values, 0,
           store->column_count * sizeof(*store->values));

  status = mysql_wire_decode_binary_row(
      store->row_storage, store->row_size,
      store->columns, store->column_count,
      store->values, store->column_count);
  if (status != MYSQL_WIRE_STATUS_OK) {
    store->row_size = 0u;
    if (store->column_count != 0u)
      memset(store->values, 0,
             store->column_count * sizeof(*store->values));
  }
  return status;
}

cserde_status mysql_cursor_row_store_reader(
    mysql_cursor_row_store_t *store,
    cserde_reader *out_reader) {
  if (store == NULL || out_reader == NULL ||
      store->row_storage == NULL ||
      store->row_size == 0u)
    return CSERDE_INVALID_STATE;

  return mysql_row_reader_init(
      &store->reader_state,
      store->columns,
      store->values,
      store->column_count,
      out_reader);
}

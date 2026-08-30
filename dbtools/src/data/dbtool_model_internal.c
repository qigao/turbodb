#include "data/dbtool_model_internal.h"

#include <stdint.h>

int dbtool_column_storage_matches(const dbtool_column_v1 *column) {
  if (column == NULL) return 0;
  switch (column->scalar_kind) {
    case DBTOOL_SCALAR_INT64:
      return column->storage_kind >= DBTOOL_STORAGE_INTEGER16 &&
             column->storage_kind <= DBTOOL_STORAGE_INTEGER64;
    case DBTOOL_SCALAR_UINT64:
      return (column->storage_kind >= DBTOOL_STORAGE_INTEGER16 &&
              column->storage_kind <= DBTOOL_STORAGE_INTEGER64) ||
             column->storage_kind == DBTOOL_STORAGE_UINT64_DECIMAL;
    case DBTOOL_SCALAR_DOUBLE:
      return column->storage_kind == DBTOOL_STORAGE_FLOAT32 ||
             column->storage_kind == DBTOOL_STORAGE_FLOAT64;
    case DBTOOL_SCALAR_BOOLEAN:
      return column->storage_kind == DBTOOL_STORAGE_BOOLEAN;
    case DBTOOL_SCALAR_TEXT:
      return column->storage_kind == DBTOOL_STORAGE_TEXT;
    case DBTOOL_SCALAR_BYTES:
      return column->storage_kind == DBTOOL_STORAGE_BYTES;
    case DBTOOL_SCALAR_UUID:
      return column->storage_kind == DBTOOL_STORAGE_UUID;
    default:
      return 0;
  }
}

int dbtool_table_metadata_valid(const dbtool_table_v1 *table,
                                size_t table_index) {
  static const uint32_t known_flags =
      DBTOOL_COLUMN_OPTIONAL | DBTOOL_COLUMN_HAS_DEFAULT |
      DBTOOL_COLUMN_GENERATED;
  size_t index;
  if (table == NULL || table->struct_size < sizeof(*table) ||
      table->abi_version != DBTOOL_MODEL_ABI_VERSION ||
      table->index != table_index || table->name == NULL ||
      table->name[0] == '\0' || table->database_name == NULL ||
      table->database_name[0] == '\0' || table->columns == NULL ||
      table->column_count == 0u ||
      table->column_count > SIZE_MAX / sizeof(dbtool_cell))
    return 0;
  for (index = 0u; index < table->column_count; ++index) {
    const dbtool_column_v1 *column = &table->columns[index];
    if (column->struct_size < sizeof(*column) ||
        column->abi_version != DBTOOL_MODEL_ABI_VERSION ||
        column->index != index || column->name == NULL ||
        column->name[0] == '\0' || column->database_name == NULL ||
        column->database_name[0] == '\0' ||
        !dbtool_column_storage_matches(column) ||
        (column->flags & ~known_flags) != 0u ||
        ((column->flags & DBTOOL_COLUMN_GENERATED) != 0u &&
         (column->flags &
          (DBTOOL_COLUMN_OPTIONAL | DBTOOL_COLUMN_HAS_DEFAULT)) != 0u))
      return 0;
  }
  return 1;
}

const dbtool_table_v1 *dbtool_model_table_valid(
    const dbtool_model_v1 *model, size_t table_index) {
  const dbtool_table_v1 *table;
  if (model == NULL || model->struct_size < sizeof(*model) ||
      model->abi_version != DBTOOL_MODEL_ABI_VERSION || model->tables == NULL ||
      model->table_count == 0u || table_index >= model->table_count)
    return NULL;
  table = &model->tables[table_index];
  return dbtool_table_metadata_valid(table, table_index) ? table : NULL;
}

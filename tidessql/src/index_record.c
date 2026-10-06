#include "index_record.h"
#include "index_internal.h"
#include "name.h"
#include "wire.h"
#include "work.h"
#include "error.h"
#include <string.h>

enum { INDEX_RECORD_VERSION = 2, INDEX_RECORD_FLAGS = 3, INDEX_RECORD_TABLE = 4,
       INDEX_RECORD_TABLE_GENERATION = 12, INDEX_RECORD_ID = 20, INDEX_RECORD_GENERATION = 28,
       INDEX_RECORD_COUNT = 36, INDEX_RECORD_HEADER = 40, INDEX_RECORD_PART = 6,
       INDEX_RECORD_TYPE = 4, INDEX_RECORD_PART_FLAGS = 5, INDEX_RECORD_NAME_LENGTHS = 2,
       INDEX_RECORD_I64 = 1, INDEX_RECORD_U64 = 2, INDEX_RECORD_F64 = 3,
       INDEX_RECORD_WIRE_INTEGER = 1, INDEX_RECORD_WIRE_REAL = 2, INDEX_RECORD_UNIQUE = 1,
       INDEX_RECORD_NULLABLE = 1, INDEX_RECORD_DESC = 2 };
static const uint8_t index_record_magic[] = {'S', 'I', 1};

static turbodb_status_t record_error(turbodb_error_t *error, turbodb_status_t status, const char *reason) {
  tdsql_error_set(error, status, reason); return status;
}
static turbodb_status_t record_charge(orm_tidesdb_sql_budget *budget, orm_sql_budget_resource resource,
    size_t count, turbodb_error_t *error) {
  orm_sql_budget_amount amount = {0}; amount.value[resource] = count;
  return orm_tidesdb_sql_budget_reserve(budget, &amount, error);
}
static bool record_schema(const orm_sql_table_schema *schema) {
  return schema && schema->name.data && schema->name.len && schema->columns && schema->count;
}
static turbodb_status_t record_validate(const orm_sql_index_record *record, const orm_sql_table_schema *schema,
    turbodb_status_t invalid, turbodb_error_t *error) {
  const orm_sql_index_definition *definition = &record->definition;
  if (!record->identity.table_id || !record->identity.table_generation || !record->identity.index_id || !record->identity.generation)
    return record_error(error, invalid, "SQL index record requires nonzero identities and generations");
  const char *reason = NULL;
  const vstr name = {definition->name, definition->name_size}, table = {definition->table, definition->table_size};
  if (orm_sql_name_validate(name, &reason) != TURBODB_STATUS_OK || orm_sql_index_primary_name(name) ||
      orm_sql_name_validate(table, &reason) != TURBODB_STATUS_OK)
    return record_error(error, invalid, "SQL index record has an invalid index or table name");
  if (table.len != schema->name.len || memcmp(table.data, schema->name.data, table.len))
    return record_error(error, invalid, "SQL index record table does not match its Catalog schema");
  const size_t count = vec_size(&definition->parts);
  if (!count || count > schema->count)
    return record_error(error, invalid, "SQL index record has an invalid key part count");
  for (size_t i = 0; i < count; ++i) {
    turbodb_status_t status = record_charge(definition->budget, ORM_SQL_BUDGET_EXECUTION_STEPS, 1, error);
    if (status != TURBODB_STATUS_OK) return status;
    const orm_sql_index_part *part = vec_at_const(&definition->parts, i);
    if (part->column >= schema->count || part->column > UINT32_MAX ||
        (part->type.kind != TURBODB_VALUE_INT64 && part->type.kind != TURBODB_VALUE_UINT64 && part->type.kind != TURBODB_VALUE_DOUBLE) ||
        part->type.kind != schema->columns[part->column].type.kind ||
        part->type.nullable != schema->columns[part->column].type.nullable)
      return record_error(error, invalid, "SQL index record column type or ordinal does not match its Catalog schema");
    for (size_t j = 0; j < i; ++j) {
      status = record_charge(definition->budget, ORM_SQL_BUDGET_EXECUTION_STEPS, 1, error);
      if (status != TURBODB_STATUS_OK) return status;
      const orm_sql_index_part *other = vec_at_const(&definition->parts, j);
      if (part->column == other->column) return record_error(error, invalid, "SQL index record has duplicate key columns");
    }
  }
  return TURBODB_STATUS_OK;
}
static void record_write_name(uint8_t *data, size_t *offset, const char *name, size_t size) {
  data[(*offset)++] = (uint8_t)size;
  memcpy(data + *offset, name, size); *offset += size;
}
static void record_write(const orm_sql_index_record *record, uint8_t *data) {
  const orm_sql_index_definition *definition = &record->definition;
  memcpy(data, index_record_magic, sizeof(index_record_magic));
  if (orm_sql_index_has_double(definition)) data[INDEX_RECORD_VERSION] = INDEX_RECORD_WIRE_REAL;
  data[INDEX_RECORD_FLAGS] = definition->unique ? INDEX_RECORD_UNIQUE : 0;
  orm_sql_wire_write(data + INDEX_RECORD_TABLE, ORM_SQL_WIRE_U64, record->identity.table_id);
  orm_sql_wire_write(data + INDEX_RECORD_TABLE_GENERATION, ORM_SQL_WIRE_U64, record->identity.table_generation);
  orm_sql_wire_write(data + INDEX_RECORD_ID, ORM_SQL_WIRE_U64, record->identity.index_id);
  orm_sql_wire_write(data + INDEX_RECORD_GENERATION, ORM_SQL_WIRE_U64, record->identity.generation);
  orm_sql_wire_write(data + INDEX_RECORD_COUNT, ORM_SQL_WIRE_U32, vec_size(&definition->parts));
  size_t offset = INDEX_RECORD_HEADER;
  record_write_name(data, &offset, definition->name, definition->name_size);
  record_write_name(data, &offset, definition->table, definition->table_size);
  for (size_t i = 0; i < vec_size(&definition->parts); ++i, offset += INDEX_RECORD_PART) {
    const orm_sql_index_part *part = vec_at_const(&definition->parts, i);
    orm_sql_wire_write(data + offset, ORM_SQL_WIRE_U32, part->column);
    data[offset + INDEX_RECORD_TYPE] = part->type.kind == TURBODB_VALUE_INT64 ? INDEX_RECORD_I64 :
        part->type.kind == TURBODB_VALUE_DOUBLE ? INDEX_RECORD_F64 : INDEX_RECORD_U64;
    data[offset + INDEX_RECORD_PART_FLAGS] = (part->type.nullable ? INDEX_RECORD_NULLABLE : 0) |
        (part->descending ? INDEX_RECORD_DESC : 0);
  }
}
turbodb_status_t orm_tidesdb_sql_index_record_encode(const orm_sql_index_record *record,
    const orm_sql_table_schema *schema, size_t max_bytes, vec_t *out,
    size_t *reserved, turbodb_error_t *error) {
  if (!record || !record->definition.budget || !record->definition.parts.initialized ||
      !record_schema(schema) || !max_bytes || !out || out->initialized || !reserved || *reserved)
    return record_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "SQL index record encoding requires a definition, schema and empty output");
  const orm_sql_index_definition *definition = &record->definition;
  turbodb_status_t status = record_validate(record, schema, TURBODB_STATUS_INVALID_ARGUMENT, error);
  if (status != TURBODB_STATUS_OK) return status;
  const size_t header = INDEX_RECORD_HEADER + INDEX_RECORD_NAME_LENGTHS + definition->name_size + definition->table_size;
  const size_t count = vec_size(&definition->parts);
  if (header > max_bytes || count > UINT32_MAX || count > (max_bytes - header) / INDEX_RECORD_PART)
    return record_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL index record exceeds byte capacity");
  const size_t size = header + count * INDEX_RECORD_PART;
  status = record_charge(definition->budget, ORM_SQL_BUDGET_EXECUTION_STEPS, size, error);
  vec_t bytes = {0}; size_t work = 0;
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&bytes, size, sizeof(uint8_t), _Alignof(uint8_t),
      definition->budget, &work, error);
  if (status == TURBODB_STATUS_OK) {
    record_write(record, vec_data(&bytes)); *out = bytes; *reserved = work; return TURBODB_STATUS_OK;
  }
  const turbodb_status_t released = orm_sql_work_release(&bytes, work, definition->budget, NULL);
  return released == TURBODB_STATUS_OK ? status : released;
}
static turbodb_status_t record_read_name(const uint8_t *data, size_t size, size_t *offset,
    char *out, size_t *length, turbodb_error_t *error) {
  if (*offset >= size) return record_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL index record has a truncated name");
  const size_t count = data[(*offset)++];
  if (!count || count > ORM_SQL_SELECT_NAME_BYTES || count > size - *offset)
    return record_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL index record has an invalid name length");
  const char *reason = NULL;
  if (orm_sql_name_validate((vstr){(const char *)data + *offset, count}, &reason) != TURBODB_STATUS_OK)
    return record_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL index record has an invalid name");
  memcpy(out, data + *offset, count); *length = count; *offset += count; return TURBODB_STATUS_OK;
}
static turbodb_status_t record_read_parts(const uint8_t *data, size_t size, size_t offset, size_t count,
    orm_sql_index_record *record, turbodb_error_t *error) {
  if (count > (size - offset) / INDEX_RECORD_PART || count * INDEX_RECORD_PART != size - offset)
    return record_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL index record has truncated or trailing key parts");
  turbodb_status_t status = orm_sql_work_zero(&record->definition.parts, count, sizeof(orm_sql_index_part),
      _Alignof(orm_sql_index_part), record->definition.budget, &record->definition.part_bytes, error);
  if (status != TURBODB_STATUS_OK) return status;
  for (size_t i = 0; i < count; ++i, offset += INDEX_RECORD_PART) {
    const uint8_t kind = data[offset + INDEX_RECORD_TYPE], flags = data[offset + INDEX_RECORD_PART_FLAGS];
    if ((kind != INDEX_RECORD_I64 && kind != INDEX_RECORD_U64 &&
         !(kind == INDEX_RECORD_F64 && data[INDEX_RECORD_VERSION] == INDEX_RECORD_WIRE_REAL)) ||
        (flags & ~(INDEX_RECORD_NULLABLE | INDEX_RECORD_DESC)))
      return record_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL index record has invalid key type or flags");
    orm_sql_index_part *part = vec_at(&record->definition.parts, i);
    *part = (orm_sql_index_part){(size_t)orm_sql_wire_read(data + offset, ORM_SQL_WIRE_U32),
        {kind == INDEX_RECORD_I64 ? TURBODB_VALUE_INT64 : kind == INDEX_RECORD_F64 ? TURBODB_VALUE_DOUBLE : TURBODB_VALUE_UINT64,
            (flags & INDEX_RECORD_NULLABLE) != 0},
        (flags & INDEX_RECORD_DESC) != 0};
  }
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_tidesdb_sql_index_record_decode(const uint8_t *data, size_t size,
    const orm_sql_table_schema *schema, size_t max_bytes, orm_tidesdb_sql_budget *budget,
    orm_sql_index_record *out, turbodb_error_t *error) {
  if (!data || !record_schema(schema) || !max_bytes || !budget || !out ||
      out->definition.budget || out->definition.parts.initialized)
    return record_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "SQL index record decoding requires bytes, schema and empty output");
  if (size > max_bytes) return record_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL index record exceeds byte capacity");
  if (size < INDEX_RECORD_HEADER + INDEX_RECORD_NAME_LENGTHS || memcmp(data, index_record_magic, INDEX_RECORD_VERSION))
    return record_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL index record has a truncated or invalid header");
  if (data[INDEX_RECORD_VERSION] != INDEX_RECORD_WIRE_INTEGER && data[INDEX_RECORD_VERSION] != INDEX_RECORD_WIRE_REAL)
    return record_error(error, TURBODB_STATUS_UNSUPPORTED, "SQL index record has an unknown wire version");
  const size_t count = (size_t)orm_sql_wire_read(data + INDEX_RECORD_COUNT, ORM_SQL_WIRE_U32);
  if ((data[INDEX_RECORD_FLAGS] & ~INDEX_RECORD_UNIQUE) || !count || count > schema->count ||
      count > (size - INDEX_RECORD_HEADER - INDEX_RECORD_NAME_LENGTHS) / INDEX_RECORD_PART)
    return record_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL index record has invalid flags or key count");
  orm_sql_index_record record = {.definition = {.budget = budget, .unique = (data[INDEX_RECORD_FLAGS] & INDEX_RECORD_UNIQUE) != 0},
      .identity = {orm_sql_wire_read(data + INDEX_RECORD_TABLE, ORM_SQL_WIRE_U64),
        orm_sql_wire_read(data + INDEX_RECORD_TABLE_GENERATION, ORM_SQL_WIRE_U64),
        orm_sql_wire_read(data + INDEX_RECORD_ID, ORM_SQL_WIRE_U64),
        orm_sql_wire_read(data + INDEX_RECORD_GENERATION, ORM_SQL_WIRE_U64)}};
  turbodb_status_t status = record_charge(budget, ORM_SQL_BUDGET_EXECUTION_STEPS, size, error);
  if (status == TURBODB_STATUS_OK) status = record_charge(budget, ORM_SQL_BUDGET_PLAN_NODES, count + 1, error);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_budget_reserve_capacity(budget, 1,
      sizeof(record), 0, &record.definition.metadata_bytes, error);
  size_t offset = INDEX_RECORD_HEADER;
  if (status == TURBODB_STATUS_OK) status = record_read_name(data, size, &offset,
      record.definition.name, &record.definition.name_size, error);
  if (status == TURBODB_STATUS_OK) status = record_read_name(data, size, &offset,
      record.definition.table, &record.definition.table_size, error);
  if (status == TURBODB_STATUS_OK) status = record_read_parts(data, size, offset, count, &record, error);
  if (status == TURBODB_STATUS_OK && (data[INDEX_RECORD_VERSION] == INDEX_RECORD_WIRE_REAL) != orm_sql_index_has_double(&record.definition))
    status = record_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL index record version does not match key types");
  if (status == TURBODB_STATUS_OK) status = record_validate(&record, schema, TURBODB_STATUS_DATASTORE_ERROR, error);
  if (status == TURBODB_STATUS_OK) { *out = record; return TURBODB_STATUS_OK; }
  const turbodb_status_t released = orm_tidesdb_sql_index_record_destroy(&record, NULL);
  return released == TURBODB_STATUS_OK ? status : released;
}
turbodb_status_t orm_tidesdb_sql_index_record_destroy(orm_sql_index_record *record, turbodb_error_t *error) {
  if (!record) return TURBODB_STATUS_OK;
  const turbodb_status_t status = orm_tidesdb_sql_index_destroy(&record->definition, error);
  *record = (orm_sql_index_record){0}; return status;
}

#include "orm_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct orm_metadata_owned_entry {
  orm_metadata_kind_t kind;
  uint32_t ordinal;
  tstr catalog;
  tstr schema;
  tstr relation;
  tstr name;
} orm_metadata_owned_entry;

struct orm_metadata_snapshot {
  orm_metadata_owned_entry *entries;
  uint64_t count;
  uint64_t capacity;
  uint64_t copied_bytes;
  uint64_t max_entries;
  uint64_t max_bytes;
};

static orm_string_view_t metadata_empty_view(void) {
  orm_string_view_t value = {NULL, 0u};
  return value;
}

static orm_string_view_t metadata_owned_view(tstr value) {
  return value != NULL ? tstr_to_v(value) : metadata_empty_view();
}

static int metadata_view_equal(orm_string_view_t left,
                               orm_string_view_t right) {
  return left.len == right.len &&
         (left.len == 0u || memcmp(left.data, right.data, left.len) == 0);
}

static int metadata_owned_equal(tstr left, orm_string_view_t right) {
  return metadata_view_equal(metadata_owned_view(left), right);
}

static void metadata_owned_entry_destroy(orm_metadata_owned_entry *entry) {
  if (entry == NULL) return;
  tstr_free(entry->catalog);
  tstr_free(entry->schema);
  tstr_free(entry->relation);
  tstr_free(entry->name);
  memset(entry, 0, sizeof(*entry));
}

void ORM_C_CALL orm_metadata_snapshot_destroy(
    orm_metadata_snapshot_t *snapshot) {
  if (snapshot == NULL) return;
  for (uint64_t i = 0u; i < snapshot->count; ++i)
    metadata_owned_entry_destroy(&snapshot->entries[i]);
  free(snapshot->entries);
  free(snapshot);
}

static orm_status_t metadata_fail(orm_error_t *error, orm_status_t status,
                                  const char *message) {
  orm_error_set(error, status, message);
  return status;
}

static orm_status_t metadata_copy_string(
    orm_metadata_snapshot_t *snapshot, orm_string_view_t input,
    tstr *out, orm_error_t *error) {
  *out = NULL;
  if (input.len == 0u) return ORM_STATUS_OK;
  if (input.data == NULL)
    return metadata_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                         "metadata string view is invalid");
  if (input.len > SIZE_MAX ||
      snapshot->copied_bytes > snapshot->max_bytes ||
      (uint64_t)input.len > snapshot->max_bytes - snapshot->copied_bytes)
    return metadata_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                         "metadata string budget exceeded");
  *out = tstr_new_len(input.data, input.len);
  if (*out == NULL)
    return metadata_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                         "copy metadata string");
  snapshot->copied_bytes += (uint64_t)input.len;
  return ORM_STATUS_OK;
}

static int metadata_snapshot_has(
    const orm_metadata_snapshot_t *snapshot, orm_metadata_kind_t kind,
    orm_string_view_t catalog, orm_string_view_t schema,
    orm_string_view_t relation, orm_string_view_t name) {
  for (uint64_t i = 0u; i < snapshot->count; ++i) {
    const orm_metadata_owned_entry *entry = &snapshot->entries[i];
    if (entry->kind == kind &&
        metadata_owned_equal(entry->catalog, catalog) &&
        metadata_owned_equal(entry->schema, schema) &&
        metadata_owned_equal(entry->relation, relation) &&
        metadata_owned_equal(entry->name, name))
      return 1;
  }
  return 0;
}

static orm_status_t metadata_snapshot_reserve(
    orm_metadata_snapshot_t *snapshot, orm_error_t *error) {
  if (snapshot->count >= snapshot->max_entries)
    return metadata_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                         "metadata entry budget exceeded");
  if (snapshot->count < snapshot->capacity) return ORM_STATUS_OK;

  uint64_t next = snapshot->capacity == 0u ? 16u : snapshot->capacity * 2u;
  if (next > snapshot->max_entries) next = snapshot->max_entries;
  if (next <= snapshot->capacity ||
      next > (uint64_t)(SIZE_MAX / sizeof(orm_metadata_owned_entry)))
    return metadata_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                         "metadata entry storage exceeds platform range");

  void *resized = realloc(
      snapshot->entries, (size_t)next * sizeof(orm_metadata_owned_entry));
  if (resized == NULL)
    return metadata_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                         "grow metadata snapshot");
  snapshot->entries = (orm_metadata_owned_entry *)resized;
  memset(snapshot->entries + snapshot->capacity, 0,
         (size_t)(next - snapshot->capacity) *
             sizeof(orm_metadata_owned_entry));
  snapshot->capacity = next;
  return ORM_STATUS_OK;
}

static orm_status_t metadata_snapshot_add(
    orm_metadata_snapshot_t *snapshot, orm_metadata_kind_t kind,
    orm_string_view_t catalog, orm_string_view_t schema,
    orm_string_view_t relation, orm_string_view_t name, uint32_t ordinal,
    orm_error_t *error) {
  orm_status_t status;
  orm_metadata_owned_entry entry = {0};
  const uint64_t bytes_before = snapshot->copied_bytes;

  if (metadata_snapshot_has(snapshot, kind, catalog, schema, relation, name))
    return ORM_STATUS_OK;

  status = metadata_snapshot_reserve(snapshot, error);
  if (status != ORM_STATUS_OK) return status;

  entry.kind = kind;
  entry.ordinal = ordinal;
  status = metadata_copy_string(snapshot, catalog, &entry.catalog, error);
  if (status == ORM_STATUS_OK)
    status = metadata_copy_string(snapshot, schema, &entry.schema, error);
  if (status == ORM_STATUS_OK)
    status = metadata_copy_string(snapshot, relation, &entry.relation, error);
  if (status == ORM_STATUS_OK)
    status = metadata_copy_string(snapshot, name, &entry.name, error);
  if (status != ORM_STATUS_OK) {
    metadata_owned_entry_destroy(&entry);
    snapshot->copied_bytes = bytes_before;
    return status;
  }

  snapshot->entries[snapshot->count++] = entry;
  return ORM_STATUS_OK;
}

static orm_status_t metadata_query(
    orm_connection_t *connection, const char *sql,
    const orm_string_view_t *binds, uint32_t bind_count,
    orm_result_t **out_result, orm_error_t *error) {
  orm_query_t *query = NULL;
  orm_status_t status;

  *out_result = NULL;
  status = orm_raw(connection, orm_view(sql), &query, error);
  for (uint32_t i = 0u; status == ORM_STATUS_OK && i < bind_count; ++i)
    status = orm_query_bind(query, orm_text_v(binds[i]), error);
  if (status == ORM_STATUS_OK)
    status = orm_query_execute(query, out_result, error);
  orm_query_destroy(query);
  return status;
}

static orm_status_t metadata_rows(const orm_result_t *result,
                                  uint64_t *out_rows,
                                  orm_error_t *error) {
  return orm_result_row_count(result, out_rows, error);
}

static orm_status_t metadata_text(const orm_result_t *result,
                                  uint64_t row, uint64_t column,
                                  orm_string_view_t *out,
                                  orm_error_t *error) {
  uint8_t is_null = 0u;
  orm_status_t status =
      orm_result_is_null(result, row, column, &is_null, error);
  if (status != ORM_STATUS_OK) return status;
  if (is_null) {
    *out = metadata_empty_view();
    return ORM_STATUS_OK;
  }
  return orm_result_get_text(result, row, column, out, error);
}

static orm_status_t metadata_ordinal(const orm_result_t *result,
                                     uint64_t row, uint64_t column,
                                     uint32_t *out,
                                     orm_error_t *error) {
  orm_value_kind_t kind = ORM_VALUE_NULL;
  orm_status_t status =
      orm_result_value_kind(result, row, column, &kind, error);
  if (status != ORM_STATUS_OK) return status;

  if (kind == ORM_VALUE_INT64) {
    int64_t value = 0;
    status = orm_result_get_int64(result, row, column, &value, error);
    if (status != ORM_STATUS_OK) return status;
    if (value < 0 || (uint64_t)value > UINT32_MAX)
      return metadata_fail(error, ORM_STATUS_OUT_OF_RANGE,
                           "metadata ordinal is out of range");
    *out = (uint32_t)value;
    return ORM_STATUS_OK;
  }
  if (kind == ORM_VALUE_UINT64) {
    uint64_t value = 0u;
    status = orm_result_get_uint64(result, row, column, &value, error);
    if (status != ORM_STATUS_OK) return status;
    if (value > UINT32_MAX)
      return metadata_fail(error, ORM_STATUS_OUT_OF_RANGE,
                           "metadata ordinal is out of range");
    *out = (uint32_t)value;
    return ORM_STATUS_OK;
  }
  return metadata_fail(error, ORM_STATUS_TYPE_ERROR,
                       "metadata ordinal is not an integer");
}

static int metadata_text_equals(orm_string_view_t value, const char *text) {
  const size_t size = strlen(text);
  return value.len == size &&
         (size == 0u || memcmp(value.data, text, size) == 0);
}

static orm_status_t metadata_load_sqlite(
    orm_connection_t *connection, orm_metadata_snapshot_t *snapshot,
    orm_error_t *error) {
  orm_result_t *result = NULL;
  orm_status_t status;
  uint64_t rows = 0u;

  status = metadata_query(
      connection,
      "SELECT name FROM pragma_database_list ORDER BY seq",
      NULL, 0u, &result, error);
  if (status != ORM_STATUS_OK) return status;
  status = metadata_rows(result, &rows, error);
  for (uint64_t row = 0u; status == ORM_STATUS_OK && row < rows; ++row) {
    orm_string_view_t name = metadata_empty_view();
    status = metadata_text(result, row, 0u, &name, error);
    if (status == ORM_STATUS_OK)
      status = metadata_snapshot_add(
          snapshot, ORM_METADATA_CATALOG, metadata_empty_view(),
          metadata_empty_view(), metadata_empty_view(), name, 0u, error);
  }
  orm_result_destroy(result);
  result = NULL;
  if (status != ORM_STATUS_OK) return status;

  status = metadata_query(
      connection,
      "SELECT schema, name, type FROM pragma_table_list "
      "WHERE type IN ('table','view') AND name NOT LIKE 'sqlite_%' "
      "ORDER BY schema, name",
      NULL, 0u, &result, error);
  if (status != ORM_STATUS_OK) return status;
  status = metadata_rows(result, &rows, error);
  for (uint64_t row = 0u; status == ORM_STATUS_OK && row < rows; ++row) {
    orm_string_view_t catalog = metadata_empty_view();
    orm_string_view_t name = metadata_empty_view();
    orm_string_view_t type = metadata_empty_view();
    status = metadata_text(result, row, 0u, &catalog, error);
    if (status == ORM_STATUS_OK)
      status = metadata_text(result, row, 1u, &name, error);
    if (status == ORM_STATUS_OK)
      status = metadata_text(result, row, 2u, &type, error);
    if (status == ORM_STATUS_OK)
      status = metadata_snapshot_add(
          snapshot,
          metadata_text_equals(type, "view") ? ORM_METADATA_VIEW
                                              : ORM_METADATA_TABLE,
          catalog, metadata_empty_view(), metadata_empty_view(), name,
          0u, error);
  }
  orm_result_destroy(result);
  result = NULL;
  if (status != ORM_STATUS_OK) return status;

  const uint64_t relation_count = snapshot->count;
  for (uint64_t index = 0u;
       status == ORM_STATUS_OK && index < relation_count; ++index) {
    const orm_metadata_owned_entry *entry = &snapshot->entries[index];
    if (entry->kind != ORM_METADATA_TABLE &&
        entry->kind != ORM_METADATA_VIEW)
      continue;

    const orm_string_view_t binds[] = {
        metadata_owned_view(entry->name),
        metadata_owned_view(entry->catalog)};
    status = metadata_query(
        connection,
        "SELECT name, cid FROM pragma_table_info(?1, ?2) ORDER BY cid",
        binds, 2u, &result, error);
    if (status != ORM_STATUS_OK) break;
    status = metadata_rows(result, &rows, error);
    for (uint64_t row = 0u; status == ORM_STATUS_OK && row < rows; ++row) {
      orm_string_view_t column = metadata_empty_view();
      uint32_t ordinal = 0u;
      status = metadata_text(result, row, 0u, &column, error);
      if (status == ORM_STATUS_OK)
        status = metadata_ordinal(result, row, 1u, &ordinal, error);
      if (status == ORM_STATUS_OK)
        status = metadata_snapshot_add(
            snapshot, ORM_METADATA_COLUMN,
            metadata_owned_view(entry->catalog), metadata_empty_view(),
            metadata_owned_view(entry->name), column, ordinal, error);
    }
    orm_result_destroy(result);
    result = NULL;
  }
  orm_result_destroy(result);
  return status;
}

static orm_status_t metadata_load_mysql(
    orm_connection_t *connection, orm_metadata_snapshot_t *snapshot,
    orm_error_t *error) {
  orm_result_t *result = NULL;
  orm_status_t status;
  uint64_t rows = 0u;
  orm_string_view_t current = metadata_empty_view();

  status = metadata_query(
      connection,
      "SELECT schema_name FROM information_schema.schemata "
      "ORDER BY schema_name",
      NULL, 0u, &result, error);
  if (status != ORM_STATUS_OK) return status;
  status = metadata_rows(result, &rows, error);
  for (uint64_t row = 0u; status == ORM_STATUS_OK && row < rows; ++row) {
    orm_string_view_t name = metadata_empty_view();
    status = metadata_text(result, row, 0u, &name, error);
    if (status == ORM_STATUS_OK)
      status = metadata_snapshot_add(
          snapshot, ORM_METADATA_CATALOG, metadata_empty_view(),
          metadata_empty_view(), metadata_empty_view(), name, 0u, error);
  }
  orm_result_destroy(result);
  result = NULL;
  if (status != ORM_STATUS_OK) return status;

  status = metadata_query(
      connection, "SELECT DATABASE()", NULL, 0u, &result, error);
  if (status != ORM_STATUS_OK) return status;
  status = metadata_rows(result, &rows, error);
  if (status == ORM_STATUS_OK && rows != 0u)
    status = metadata_text(result, 0u, 0u, &current, error);
  if (status == ORM_STATUS_OK && current.len != 0u) {
    tstr owned = tstr_new_len(current.data, current.len);
    if (owned == NULL)
      status = metadata_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                             "copy current MySQL catalog");
    orm_result_destroy(result);
    result = NULL;
    if (status != ORM_STATUS_OK) return status;
    current = tstr_to_v(owned);

    status = metadata_query(
        connection,
        "SELECT table_name, table_type FROM information_schema.tables "
        "WHERE table_schema = DATABASE() ORDER BY table_name",
        NULL, 0u, &result, error);
    if (status == ORM_STATUS_OK)
      status = metadata_rows(result, &rows, error);
    for (uint64_t row = 0u; status == ORM_STATUS_OK && row < rows; ++row) {
      orm_string_view_t name = metadata_empty_view();
      orm_string_view_t type = metadata_empty_view();
      status = metadata_text(result, row, 0u, &name, error);
      if (status == ORM_STATUS_OK)
        status = metadata_text(result, row, 1u, &type, error);
      if (status == ORM_STATUS_OK)
        status = metadata_snapshot_add(
            snapshot,
            metadata_text_equals(type, "VIEW") ? ORM_METADATA_VIEW
                                                : ORM_METADATA_TABLE,
            current, metadata_empty_view(), metadata_empty_view(),
            name, 0u, error);
    }
    orm_result_destroy(result);
    result = NULL;

    if (status == ORM_STATUS_OK)
      status = metadata_query(
          connection,
          "SELECT table_name, column_name, ordinal_position "
          "FROM information_schema.columns "
          "WHERE table_schema = DATABASE() "
          "ORDER BY table_name, ordinal_position",
          NULL, 0u, &result, error);
    if (status == ORM_STATUS_OK)
      status = metadata_rows(result, &rows, error);
    for (uint64_t row = 0u; status == ORM_STATUS_OK && row < rows; ++row) {
      orm_string_view_t relation = metadata_empty_view();
      orm_string_view_t name = metadata_empty_view();
      uint32_t ordinal = 0u;
      status = metadata_text(result, row, 0u, &relation, error);
      if (status == ORM_STATUS_OK)
        status = metadata_text(result, row, 1u, &name, error);
      if (status == ORM_STATUS_OK)
        status = metadata_ordinal(result, row, 2u, &ordinal, error);
      if (status == ORM_STATUS_OK) {
        if (ordinal == 0u)
          status = metadata_fail(error, ORM_STATUS_OUT_OF_RANGE,
                                 "MySQL metadata ordinal is zero");
        else
          --ordinal;
      }
      if (status == ORM_STATUS_OK)
        status = metadata_snapshot_add(
            snapshot, ORM_METADATA_COLUMN, current,
            metadata_empty_view(), relation, name, ordinal, error);
    }
    orm_result_destroy(result);
    tstr_free(owned);
    return status;
  }

  orm_result_destroy(result);
  return status;
}

static orm_status_t metadata_load_postgresql(
    orm_connection_t *connection, orm_metadata_snapshot_t *snapshot,
    orm_error_t *error) {
  orm_result_t *result = NULL;
  orm_status_t status;
  uint64_t rows = 0u;
  tstr catalog_owned = NULL;
  orm_string_view_t catalog = metadata_empty_view();

  status = metadata_query(
      connection, "SELECT current_database()", NULL, 0u, &result, error);
  if (status != ORM_STATUS_OK) return status;
  status = metadata_rows(result, &rows, error);
  if (status == ORM_STATUS_OK && rows != 0u)
    status = metadata_text(result, 0u, 0u, &catalog, error);
  if (status == ORM_STATUS_OK && catalog.len != 0u) {
    catalog_owned = tstr_new_len(catalog.data, catalog.len);
    if (catalog_owned == NULL)
      status = metadata_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                             "copy PostgreSQL catalog");
  }
  orm_result_destroy(result);
  result = NULL;
  if (status != ORM_STATUS_OK) return status;
  catalog = metadata_owned_view(catalog_owned);
  if (catalog.len != 0u)
    status = metadata_snapshot_add(
        snapshot, ORM_METADATA_CATALOG, metadata_empty_view(),
        metadata_empty_view(), metadata_empty_view(), catalog, 0u, error);

  if (status == ORM_STATUS_OK)
    status = metadata_query(
        connection,
        "SELECT schema_name FROM information_schema.schemata "
        "WHERE schema_name <> 'information_schema' "
        "AND schema_name NOT LIKE 'pg_%' ORDER BY schema_name",
        NULL, 0u, &result, error);
  if (status == ORM_STATUS_OK)
    status = metadata_rows(result, &rows, error);
  for (uint64_t row = 0u; status == ORM_STATUS_OK && row < rows; ++row) {
    orm_string_view_t schema = metadata_empty_view();
    status = metadata_text(result, row, 0u, &schema, error);
    if (status == ORM_STATUS_OK)
      status = metadata_snapshot_add(
          snapshot, ORM_METADATA_SCHEMA, catalog,
          metadata_empty_view(), metadata_empty_view(), schema, 0u, error);
  }
  orm_result_destroy(result);
  result = NULL;

  if (status == ORM_STATUS_OK)
    status = metadata_query(
        connection,
        "SELECT table_schema, table_name, table_type "
        "FROM information_schema.tables "
        "WHERE table_schema <> 'information_schema' "
        "AND table_schema NOT LIKE 'pg_%' "
        "ORDER BY table_schema, table_name",
        NULL, 0u, &result, error);
  if (status == ORM_STATUS_OK)
    status = metadata_rows(result, &rows, error);
  for (uint64_t row = 0u; status == ORM_STATUS_OK && row < rows; ++row) {
    orm_string_view_t schema = metadata_empty_view();
    orm_string_view_t name = metadata_empty_view();
    orm_string_view_t type = metadata_empty_view();
    status = metadata_text(result, row, 0u, &schema, error);
    if (status == ORM_STATUS_OK)
      status = metadata_text(result, row, 1u, &name, error);
    if (status == ORM_STATUS_OK)
      status = metadata_text(result, row, 2u, &type, error);
    if (status == ORM_STATUS_OK)
      status = metadata_snapshot_add(
          snapshot,
          metadata_text_equals(type, "VIEW") ? ORM_METADATA_VIEW
                                              : ORM_METADATA_TABLE,
          catalog, schema, metadata_empty_view(), name, 0u, error);
  }
  orm_result_destroy(result);
  result = NULL;

  if (status == ORM_STATUS_OK)
    status = metadata_query(
        connection,
        "SELECT table_schema, table_name, column_name, ordinal_position "
        "FROM information_schema.columns "
        "WHERE table_schema <> 'information_schema' "
        "AND table_schema NOT LIKE 'pg_%' "
        "ORDER BY table_schema, table_name, ordinal_position",
        NULL, 0u, &result, error);
  if (status == ORM_STATUS_OK)
    status = metadata_rows(result, &rows, error);
  for (uint64_t row = 0u; status == ORM_STATUS_OK && row < rows; ++row) {
    orm_string_view_t schema = metadata_empty_view();
    orm_string_view_t relation = metadata_empty_view();
    orm_string_view_t name = metadata_empty_view();
    uint32_t ordinal = 0u;
    status = metadata_text(result, row, 0u, &schema, error);
    if (status == ORM_STATUS_OK)
      status = metadata_text(result, row, 1u, &relation, error);
    if (status == ORM_STATUS_OK)
      status = metadata_text(result, row, 2u, &name, error);
    if (status == ORM_STATUS_OK)
      status = metadata_ordinal(result, row, 3u, &ordinal, error);
    if (status == ORM_STATUS_OK) {
      if (ordinal == 0u)
        status = metadata_fail(error, ORM_STATUS_OUT_OF_RANGE,
                               "PostgreSQL metadata ordinal is zero");
      else
        --ordinal;
    }
    if (status == ORM_STATUS_OK)
      status = metadata_snapshot_add(
          snapshot, ORM_METADATA_COLUMN, catalog, schema,
          relation, name, ordinal, error);
  }
  orm_result_destroy(result);
  tstr_free(catalog_owned);
  return status;
}

orm_status_t ORM_C_CALL orm_connection_metadata_snapshot(
    orm_connection_t *connection, orm_metadata_snapshot_t **out_snapshot,
    orm_error_t *error) {
  orm_metadata_snapshot_t *snapshot;
  orm_status_t status;

  if (out_snapshot != NULL) *out_snapshot = NULL;
  if (connection == NULL || out_snapshot == NULL)
    return metadata_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                         "invalid metadata snapshot request");

  snapshot = (orm_metadata_snapshot_t *)calloc(1u, sizeof(*snapshot));
  if (snapshot == NULL)
    return metadata_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                         "allocate metadata snapshot");
  snapshot->max_entries = connection->limits.max_result_rows;
  snapshot->max_bytes = connection->limits.max_result_bytes;
  if (snapshot->max_entries == 0u || snapshot->max_bytes == 0u) {
    orm_metadata_snapshot_destroy(snapshot);
    return metadata_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                         "metadata snapshot limits are zero");
  }

  if (strcmp(connection->driver_id, "sqlite") == 0)
    status = metadata_load_sqlite(connection, snapshot, error);
  else if (strcmp(connection->driver_id, "mysql") == 0)
    status = metadata_load_mysql(connection, snapshot, error);
  else if (strcmp(connection->driver_id, "postgresql") == 0)
    status = metadata_load_postgresql(connection, snapshot, error);
  else
    status = metadata_fail(error, ORM_STATUS_UNSUPPORTED,
                           "driver does not expose SQL metadata");

  if (status != ORM_STATUS_OK) {
    orm_metadata_snapshot_destroy(snapshot);
    return status;
  }

  *out_snapshot = snapshot;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

orm_status_t ORM_C_CALL orm_metadata_snapshot_count(
    const orm_metadata_snapshot_t *snapshot, uint64_t *out_count,
    orm_error_t *error) {
  if (snapshot == NULL || out_count == NULL)
    return metadata_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                         "invalid metadata count request");
  *out_count = snapshot->count;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

orm_status_t ORM_C_CALL orm_metadata_snapshot_get(
    const orm_metadata_snapshot_t *snapshot, uint64_t index,
    orm_metadata_entry_t *out_entry, orm_error_t *error) {
  if (snapshot == NULL || out_entry == NULL ||
      out_entry->struct_size != sizeof(*out_entry))
    return metadata_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                         "invalid metadata entry request");
  if (index >= snapshot->count)
    return metadata_fail(error, ORM_STATUS_OUT_OF_RANGE,
                         "metadata entry index is out of range");

  const orm_metadata_owned_entry *source = &snapshot->entries[index];
  out_entry->kind = source->kind;
  out_entry->ordinal = source->ordinal;
  out_entry->reserved = 0u;
  out_entry->catalog = metadata_owned_view(source->catalog);
  out_entry->schema = metadata_owned_view(source->schema);
  out_entry->relation = metadata_owned_view(source->relation);
  out_entry->name = metadata_owned_view(source->name);
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

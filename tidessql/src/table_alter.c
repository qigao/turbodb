#include "table_alter.h"
#include "relation.h"
#include "index_directory.h"
#include "index_change.h"
#include "name.h"
#include "work.h"
#include "error.h"
#include <string.h>

enum { RENAME_COLUMN_WRITES = 3, RENAME_TABLE_WRITES = 4 };
typedef struct alter_record {
  vec_t bytes;
  size_t work;
  uint8_t key[INDEX_DIRECTORY_HEADER + ORM_SQL_SELECT_NAME_BYTES];
} alter_record;
typedef struct table_alter {
  orm_sql_catalog_store *store;
  orm_sql_table_definition table;
  orm_sql_index_set indexes;
  vec_t records, writes, encoded, rows, row_records, scratch;
  size_t record_work, write_work, encoded_work, metadata;
  size_t row_work, row_record_work, scratch_work, row_count, row_width;
  uint8_t prefix[ORM_SQL_RELATION_PREFIX_BYTES];
  uint64_t id, version;
  vstr name, column, replacement, after;
  sqlparser_column_position position;
  const sqlparser_document *document;
  sqlparser_id added, default_expression;
  sqlparser_alter_action action;
  bool rename_table;
} table_alter;
static turbodb_status_t alter_error(turbodb_error_t *error, turbodb_status_t status, const char *message) {
  tdsql_error_set(error, status, message); return status;
}
static bool alter_equal(vstr a, vstr b) { return a.len == b.len && !memcmp(a.data, b.data, a.len); }
static turbodb_status_t alter_bind(table_alter *r, const sqlparser_document *doc, turbodb_error_t *error) {
  const sqlparser_node *s = sqlparser_get_node(doc, sqlparser_statements(doc).first);
  if (sqlparser_get_dialect(doc) != SQLPARSER_MYSQL || sqlparser_statements(doc).count != 1 ||
      !s || s->kind != SQLPARSER_ALTER_TABLE ||
      (s->as.alter.action != SQLPARSER_RENAME_TABLE && s->as.alter.action != SQLPARSER_RENAME_COLUMN &&
       s->as.alter.action != SQLPARSER_ADD_COLUMN && s->as.alter.action != SQLPARSER_DROP_COLUMN &&
       s->as.alter.action != SQLPARSER_SET_COLUMN_DEFAULT && s->as.alter.action != SQLPARSER_DROP_COLUMN_DEFAULT))
    return alter_error(error, TURBODB_STATUS_UNSUPPORTED, "ALTER TABLE supports rename, column defaults and bounded ADD/DROP COLUMN");
  r->document = doc; r->action = s->as.alter.action;
  r->rename_table = s->as.alter.action == SQLPARSER_RENAME_TABLE;
  if (r->action == SQLPARSER_SET_COLUMN_DEFAULT) r->default_expression = s->as.alter.value;
  if (r->action == SQLPARSER_ADD_COLUMN) r->added = s->as.alter.column;
  r->position = s->as.alter.position;
  const char *reason = NULL;
  turbodb_status_t status = orm_sql_name_node(doc, s->as.alter.table, &r->name, &reason);
  if (status == TURBODB_STATUS_OK && r->added && r->position == SQLPARSER_COLUMN_AFTER)
    status = orm_sql_name_node(doc, s->as.alter.after, &r->after, &reason);
  if (status == TURBODB_STATUS_OK && (r->rename_table || r->action == SQLPARSER_RENAME_COLUMN))
    status = orm_sql_name_node(doc, s->as.alter.new_name, &r->replacement, &reason);
  if (status == TURBODB_STATUS_OK && !r->rename_table && !r->added)
    status = orm_sql_name_node(doc, s->as.alter.column, &r->column, &reason);
  return status == TURBODB_STATUS_OK ? status : alter_error(error, status, reason);
}
static turbodb_status_t alter_snapshot(table_alter *r, turbodb_error_t *error) {
  orm_sql_relation_source source = {0};
  turbodb_status_t status = orm_tidesdb_sql_relation_open(r->store, r->name, &source, error);
  if (status == TURBODB_STATUS_OK) {
    memcpy(r->prefix, source.prefix, sizeof(r->prefix));
    status = orm_sql_relation_snapshot(&source, &r->rows, &r->row_work, &r->row_count, error);
  }
  const turbodb_status_t closed = orm_tidesdb_sql_relation_close(&source, status == TURBODB_STATUS_OK ? error : NULL);
  if (closed != TURBODB_STATUS_OK) r->store->failed = true;
  if (status == TURBODB_STATUS_OK) status = closed;
  if (status != TURBODB_STATUS_OK) return status;
  const orm_sql_store_audit data = {r->prefix[0], r->id, 1, ORM_SQL_RELATION_KEY_BYTES, 1, r->store->max_record_bytes, r->row_count};
  status = orm_sql_store_audit_prefix(r->store, &data, error);
  const orm_sql_index_change snapshot = {vec_data_const(&r->rows), NULL, r->row_count,
      vec_size(&r->table.columns), r->table.primary_key};
  orm_sql_index_delta validation = {0};
  if (status == TURBODB_STATUS_OK && r->row_count && vec_size(&r->indexes.records))
    status = orm_sql_index_delta_prepare(r->store, &r->indexes, &snapshot, &validation, error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_index_snapshot_audit(r->store, &r->indexes, &snapshot, error);
  const turbodb_status_t released = orm_sql_index_delta_close(&validation, status == TURBODB_STATUS_OK ? error : NULL);
  if (released != TURBODB_STATUS_OK) r->store->failed = true;
  if (status == TURBODB_STATUS_OK) status = released;
  return status;
}
static turbodb_status_t alter_encode_rows(table_alter *r, const orm_sql_table_definition *changed, size_t ordinal, turbodb_error_t *error) {
  if (!r->row_count) return TURBODB_STATUS_OK;
  orm_sql_table_schema schema = {0};
  turbodb_status_t status = orm_tidesdb_sql_catalog_schema(changed, &schema, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (r->added && !schema.columns[ordinal].type.nullable && !schema.defaults[ordinal].specified)
    return alter_error(error, TURBODB_STATUS_UNSUPPORTED, "nonempty ADD NOT NULL requires a supported explicit default");
  size_t size = 0;
  status = orm_sql_relation_row_size(schema.count, r->store->max_record_bytes, &size, error);
  if (status == TURBODB_STATUS_OK && size > SIZE_MAX - ORM_SQL_RELATION_KEY_BYTES)
    status = alter_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "ALTER row record width overflow");
  if (status != TURBODB_STATUS_OK) return status;
  r->row_width = ORM_SQL_RELATION_KEY_BYTES + size;
  status = orm_sql_work_zero(&r->row_records, r->row_count, r->row_width, 1, r->store->budget, &r->row_record_work, error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&r->scratch, schema.count, sizeof(turbodb_value_t), _Alignof(turbodb_value_t),
      r->store->budget, &r->scratch_work, error);
  const size_t old_count = vec_size(&r->table.columns);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < r->row_count; ++i) {
    const turbodb_value_t *before = vec_at_const(&r->rows, i); turbodb_value_t *after = vec_data(&r->scratch);
    orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = old_count;
    status = orm_tidesdb_sql_budget_reserve(r->store->budget, &amount, error);
    if (status != TURBODB_STATUS_OK) break;
    for (size_t c = 0; c < schema.count; ++c) {
      if (r->added && c == ordinal) after[c] = schema.defaults[c].specified ? schema.defaults[c].value : turbodb_null();
      else after[c] = before[r->added ? c - (c > ordinal) : c + (c >= ordinal)];
    }
    status = orm_sql_relation_encode_record(&schema, changed->primary_key, after, r->prefix,
        vec_at(&r->row_records, i), r->row_width, r->store->budget, error);
  }
  return status;
}
static turbodb_status_t alter_dependencies(table_alter *r, size_t ordinal, turbodb_error_t *error) {
  for (size_t i = 0; !r->added && i < vec_size(&r->indexes.records); ++i) {
    const orm_sql_index_record *index = vec_at_const(&r->indexes.records, i);
    orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = vec_size(&index->definition.parts);
    const turbodb_status_t charged = orm_tidesdb_sql_budget_reserve(r->store->budget, &amount, error);
    if (charged != TURBODB_STATUS_OK) return charged;
    for (size_t p = 0; p < vec_size(&index->definition.parts); ++p)
      if (((const orm_sql_index_part *)vec_at_const(&index->definition.parts, p))->column == ordinal)
        return alter_error(error, TURBODB_STATUS_UNSUPPORTED, "DROP COLUMN requires dropping dependent indexes first");
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t alter_columns(table_alter *r, size_t slot, turbodb_error_t *error) {
  const size_t ordinal = r->added ? slot : slot ? slot - 1 : SIZE_MAX;
  orm_sql_table_definition changed = {0};
  turbodb_status_t status = alter_dependencies(r,ordinal,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_catalog_columns(&r->table, r->document, r->added, ordinal, &changed, error);
  if (status == TURBODB_STATUS_OK) status = alter_snapshot(r, error);
  if (status == TURBODB_STATUS_OK) status = alter_encode_rows(r, &changed, ordinal, error);
  if (status == TURBODB_STATUS_OK) {
    status = orm_tidesdb_sql_catalog_destroy(&r->table, error);
    if (status == TURBODB_STATUS_OK) { r->table = changed; changed = (orm_sql_table_definition){0}; }
    else r->store->failed = true;
  }
  const turbodb_status_t closed = orm_tidesdb_sql_catalog_destroy(&changed, status == TURBODB_STATUS_OK ? error : NULL);
  if (closed != TURBODB_STATUS_OK) r->store->failed = true;
  if (status == TURBODB_STATUS_OK) status = closed;
  if (status == TURBODB_STATUS_OK) for (size_t i = 0; i < vec_size(&r->indexes.records); ++i) {
    orm_sql_index_record *index = vec_at(&r->indexes.records, i);
    for (size_t p = 0; p < vec_size(&index->definition.parts); ++p) {
      orm_sql_index_part *part = vec_at(&index->definition.parts, p);
      if (r->added && part->column >= ordinal) ++part->column;
      else if (!r->added && part->column > ordinal) --part->column;
    }
  }
  return status;
}
static turbodb_status_t alter_prepare(table_alter *r, bool validate_only, turbodb_error_t *error) {
  orm_sql_catalog_snapshot snapshot = {0}; bool found = false;
  turbodb_status_t status = orm_sql_store_lookup(r->store, r->name, &r->table, &r->id, &r->version, &found, &snapshot, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (!found) return alter_error(error, TURBODB_STATUS_SQL_ERROR, "ALTER TABLE target does not exist");
  if (r->version == UINT64_MAX) return alter_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "ALTER TABLE version exhausted");
  orm_sql_table_schema schema = {0}; size_t slot = 0;
  status = orm_tidesdb_sql_catalog_schema(&r->table, &schema, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (r->added) {
    slot = r->position == SQLPARSER_COLUMN_FIRST ? 0 : schema.count;
    if (r->position == SQLPARSER_COLUMN_AFTER) {
      slot = 0;
      orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = schema.count;
      status = orm_tidesdb_sql_budget_reserve(r->store->budget, &amount, error);
      for (size_t i = 0; status == TURBODB_STATUS_OK && i < schema.count; ++i)
        if (alter_equal(schema.columns[i].name, r->after)) { slot = i + 1; break; }
      if (status == TURBODB_STATUS_OK && !slot) status = alter_error(error, TURBODB_STATUS_SQL_ERROR, "AFTER column does not exist");
      if (status != TURBODB_STATUS_OK) return status;
    }
  }
  if (r->rename_table && !alter_equal(r->name, r->replacement)) {
    orm_sql_table_definition existing = {0}; uint64_t id = 0, version = 0;
    status = orm_tidesdb_sql_catalog_lookup(r->store, r->replacement, &existing, &id, &version, &found, error);
    const turbodb_status_t closed = orm_tidesdb_sql_catalog_destroy(&existing, status == TURBODB_STATUS_OK ? error : NULL);
    if (closed != TURBODB_STATUS_OK) r->store->failed = true;
    if (status == TURBODB_STATUS_OK) status = closed;
    if (status == TURBODB_STATUS_OK && found) status = alter_error(error, TURBODB_STATUS_CONSTRAINT, "rename destination table already exists");
  } else if (!r->rename_table && !r->added) {
    orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = schema.count;
    status = orm_tidesdb_sql_budget_reserve(r->store->budget, &amount, error);
    for (size_t i = 0; status == TURBODB_STATUS_OK && i < schema.count; ++i)
      if (alter_equal(schema.columns[i].name, r->column)) { slot = i + 1; break; }
    if (status == TURBODB_STATUS_OK && !slot) status = alter_error(error, TURBODB_STATUS_SQL_ERROR, "ALTER source column does not exist");
  }
  if (status == TURBODB_STATUS_OK) status = orm_sql_index_set_load(r->store, &schema, r->id, snapshot, &r->indexes, error);
  if (status == TURBODB_STATUS_OK && snapshot.format == ORM_SQL_STORE_FORMAT_BASE && vec_size(&r->indexes.records))
    status = alter_error(error, TURBODB_STATUS_DATASTORE_ERROR, "base table has orphan index directory");
  const bool columns = r->action == SQLPARSER_ADD_COLUMN || r->action == SQLPARSER_DROP_COLUMN;
  const bool defaults = r->action == SQLPARSER_SET_COLUMN_DEFAULT || r->action == SQLPARSER_DROP_COLUMN_DEFAULT;
  if (validate_only) {
    const size_t ordinal=r->added?slot:slot?slot-1:SIZE_MAX;
    if (status==TURBODB_STATUS_OK && columns) status=alter_dependencies(r,ordinal,error);
    if (status==TURBODB_STATUS_OK) status=columns?
        orm_sql_catalog_validate_columns(&r->table,r->document,r->added,ordinal,error):defaults?
        orm_sql_catalog_validate_default(&r->table,r->document,ordinal,r->default_expression,error):
        orm_sql_catalog_rename(&r->table,slot,r->replacement,error);
    return status;
  }
  if (status == TURBODB_STATUS_OK) status = columns ? alter_columns(r, slot, error) : defaults ?
      orm_sql_catalog_default(&r->table,r->document,slot-1,r->default_expression,error) :
      orm_sql_catalog_rename(&r->table, slot, r->replacement, error);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_catalog_schema(&r->table, &schema, error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_store_encode_table(&r->table, r->id, r->store->max_record_bytes,
      &r->encoded, &r->encoded_work, error);
  const size_t indexes = r->rename_table || columns ? vec_size(&r->indexes.records) : 0;
  const size_t fixed = r->rename_table && !alter_equal(r->name, r->replacement) ? RENAME_TABLE_WRITES : RENAME_COLUMN_WRITES;
  if (status == TURBODB_STATUS_OK && (indexes > SIZE_MAX - fixed || r->row_count > SIZE_MAX - fixed - indexes))
    status = alter_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "ALTER write count overflow");
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&r->records, indexes, sizeof(alter_record), _Alignof(alter_record),
      r->store->budget, &r->record_work, error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&r->writes, indexes + fixed + r->row_count, sizeof(orm_sql_store_write),
      _Alignof(orm_sql_store_write), r->store->budget, &r->write_work, error);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < indexes; ++i) {
    orm_sql_index_record *index = vec_at(&r->indexes.records, i);
    if (r->rename_table) {
      memcpy(index->definition.table, r->replacement.data, r->replacement.len);
      index->definition.table[r->replacement.len] = 0; index->definition.table_size = r->replacement.len;
    }
    alter_record *record = vec_at(&r->records, i);
    status = orm_tidesdb_sql_index_record_encode(index, &schema, r->store->max_record_bytes, &record->bytes, &record->work, error);
    if (status == TURBODB_STATUS_OK) *(orm_sql_store_write *)vec_at(&r->writes, i) = (orm_sql_store_write){
        record->key, vec_data_const(&record->bytes), orm_sql_index_directory_key(index, record->key), vec_size(&record->bytes), ORM_SQL_STORE_PUT};
  }
  return status;
}
static turbodb_status_t alter_publish(table_alter *r, turbodb_error_t *error) {
  uint8_t old_key[ORM_SQL_STORE_NAME_KEY_BYTES], new_key[ORM_SQL_STORE_NAME_KEY_BYTES];
  uint8_t version_key[ORM_SQL_STORE_VERSION_KEY_BYTES], version[ORM_SQL_WIRE_U64];
  uint8_t manifest_key[ORM_SQL_STORE_MANIFEST_KEY_BYTES], manifest[ORM_SQL_STORE_MANIFEST_BYTES];
  const vstr target = r->rename_table ? r->replacement : r->name;
  turbodb_status_t status = orm_sql_store_name_key(target, new_key, error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_store_name_key(r->name, old_key, error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_store_catalog_barrier(r->store, manifest_key, manifest, error);
  if (status != TURBODB_STATUS_OK) return status;
  orm_sql_store_version_key(r->id, version_key); orm_sql_wire_write(version, sizeof(version), r->version + 1);
  size_t position = vec_size(&r->records);
  for (size_t i = 0; i < r->row_count; ++i) {
    const uint8_t *record = vec_at_const(&r->row_records, i);
    *(orm_sql_store_write *)vec_at(&r->writes, position++) = (orm_sql_store_write){record, record + ORM_SQL_RELATION_KEY_BYTES,
        ORM_SQL_RELATION_KEY_BYTES, r->row_width - ORM_SQL_RELATION_KEY_BYTES, ORM_SQL_STORE_PUT};
  }
  *(orm_sql_store_write *)vec_at(&r->writes, position++) = (orm_sql_store_write){new_key, vec_data_const(&r->encoded),
      2 + target.len, vec_size(&r->encoded), ORM_SQL_STORE_PUT};
  if (r->rename_table && !alter_equal(r->name, r->replacement))
    *(orm_sql_store_write *)vec_at(&r->writes, position++) = (orm_sql_store_write){old_key, NULL, 2 + r->name.len, 0, ORM_SQL_STORE_DELETE};
  *(orm_sql_store_write *)vec_at(&r->writes, position++) = (orm_sql_store_write){version_key, version, sizeof(version_key), sizeof(version), ORM_SQL_STORE_PUT};
  *(orm_sql_store_write *)vec_at(&r->writes, position++) = (orm_sql_store_write){manifest_key, manifest, sizeof(manifest_key), sizeof(manifest), ORM_SQL_STORE_PUT};
  return orm_sql_store_batch(r->store, vec_data_const(&r->writes), position, error);
}
static turbodb_status_t alter_close(table_alter *r, turbodb_error_t *error) {
  turbodb_status_t status = TURBODB_STATUS_OK;
  for (size_t i = 0; i < vec_size(&r->records); ++i) {
    alter_record *record = vec_at(&r->records, i);
    const turbodb_status_t closed = orm_sql_work_release(&record->bytes, record->work, r->store->budget, status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = closed;
  }
  const turbodb_status_t indexes = orm_sql_index_set_close(&r->indexes, status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = indexes;
  const turbodb_status_t table = orm_tidesdb_sql_catalog_destroy(&r->table, status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = table;
  vec_t *vectors[] = {&r->records, &r->writes, &r->encoded, &r->rows, &r->row_records, &r->scratch};
  const size_t bytes[] = {r->record_work, r->write_work, r->encoded_work, r->row_work, r->row_record_work, r->scratch_work};
  for (size_t i = 0; i < sizeof(vectors)/sizeof(vectors[0]); ++i) {
    const turbodb_status_t closed = orm_sql_work_release(vectors[i], bytes[i], r->store->budget, status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = closed;
  }
  const turbodb_status_t metadata = r->metadata ? orm_tidesdb_sql_budget_release(r->store->budget, ORM_SQL_BUDGET_WORK_BYTES,
      r->metadata, status == TURBODB_STATUS_OK ? error : NULL) : TURBODB_STATUS_OK;
  if (status == TURBODB_STATUS_OK) status = metadata;
  const turbodb_status_t rows = r->row_count ? orm_tidesdb_sql_budget_release(r->store->budget, ORM_SQL_BUDGET_MATERIALIZED_ROWS,
      r->row_count, status == TURBODB_STATUS_OK ? error : NULL) : TURBODB_STATUS_OK;
  if (status == TURBODB_STATUS_OK) status = rows;
  if (status != TURBODB_STATUS_OK) r->store->failed = true;
  return status;
}
turbodb_status_t orm_sql_table_alter(const sqlparser_document *document, orm_sql_catalog_store *store, turbodb_error_t *error) {
  if (!document) return alter_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "ALTER document required");
  turbodb_status_t status = orm_sql_store_writable(store, error);
  if (status != TURBODB_STATUS_OK) return status;
  table_alter r = {.store = store}; status = alter_bind(&r, document, error);
  if (status != TURBODB_STATUS_OK) return status;
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_AST_NODES] = sqlparser_node_count(document);
  status = orm_tidesdb_sql_budget_reserve(store->budget, &amount, error);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_budget_reserve_capacity(store->budget, 1, sizeof(r), 0, &r.metadata, error);
  if (status == TURBODB_STATUS_OK) status = alter_prepare(&r, false, error);
  if (status == TURBODB_STATUS_DATASTORE_ERROR) store->failed = true;
  if (status == TURBODB_STATUS_OK) status = alter_publish(&r, error);
  const turbodb_status_t closed = alter_close(&r, status == TURBODB_STATUS_OK ? error : NULL);
  return status == TURBODB_STATUS_OK ? closed : status;
}

turbodb_status_t orm_sql_table_alter_validate(const sqlparser_document *document,
    orm_sql_catalog_store *store, turbodb_error_t *error) {
  if (!document) return alter_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"ALTER document required");
  turbodb_status_t status=orm_sql_store_ready(store,error);
  if (status!=TURBODB_STATUS_OK) return status;
  table_alter r={.store=store}; status=alter_bind(&r,document,error);
  if (status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_budget_reserve_capacity(store->budget,1,sizeof(r),0,&r.metadata,error);
  if (status==TURBODB_STATUS_OK) status=alter_prepare(&r,true,error);
  const turbodb_status_t closed=alter_close(&r,status==TURBODB_STATUS_OK?error:NULL);
  return closed==TURBODB_STATUS_OK?status:closed;
}

#include "table_clear.h"
#include "index_change.h"
#include "relation.h"
#include "name.h"
#include "work.h"
#include "error.h"
#include <string.h>

enum { CLEAR_DROP_METADATA = 3, CLEAR_TRUNCATE_METADATA = 1 };
typedef struct table_clear {
  orm_sql_catalog_store *store;
  orm_sql_table_definition table;
  orm_sql_table_schema schema;
  orm_sql_relation_source source;
  orm_sql_index_set indexes;
  orm_sql_index_delta delta;
  vec_t rows, keys, directories, writes;
  size_t row_bytes, key_bytes, directory_bytes, write_bytes, metadata, count;
  uint64_t id, version;
  uint8_t prefix[ORM_SQL_RELATION_PREFIX_BYTES];
  uint8_t name_key[ORM_SQL_STORE_NAME_KEY_BYTES];
  uint8_t version_key[ORM_SQL_STORE_VERSION_KEY_BYTES];
  uint8_t version_value[ORM_SQL_WIRE_U64];
  uint8_t manifest_key[ORM_SQL_STORE_MANIFEST_KEY_BYTES];
  uint8_t manifest[ORM_SQL_STORE_MANIFEST_BYTES];
  bool drop, found;
} table_clear;
static turbodb_status_t clear_error(turbodb_error_t *error, turbodb_status_t status, const char *message) {
  tdsql_error_set(error, status, message); return status;
}
static turbodb_status_t clear_steps(table_clear *c, size_t count, turbodb_error_t *error) {
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = count;
  return orm_tidesdb_sql_budget_reserve(c->store->budget, &amount, error);
}
static turbodb_status_t clear_command(const sqlparser_document *doc, bool *drop,
    bool *missing_ok, sqlparser_list *targets, turbodb_error_t *error) {
  const sqlparser_node *statement = sqlparser_get_node(doc, sqlparser_statements(doc).first);
  if (sqlparser_get_dialect(doc) != SQLPARSER_MYSQL || sqlparser_statements(doc).count != 1 || !statement)
    return clear_error(error, TURBODB_STATUS_UNSUPPORTED, "table clear requires one MySQL statement");
  if (statement->kind == SQLPARSER_DROP_TABLE) {
    if (statement->as.drop_table.temporary || !statement->as.drop_table.tables.count)
      return clear_error(error, TURBODB_STATUS_UNSUPPORTED, "DROP TABLE requires persistent tables");
    *drop = true; *missing_ok = statement->as.drop_table.if_exists;
    *targets = statement->as.drop_table.tables;
    return TURBODB_STATUS_OK;
  } else if (statement->kind == SQLPARSER_TRUNCATE_TABLE) {
    const sqlparser_id target = statement->as.maintenance.target;
    *targets = (sqlparser_list){target, target, 1};
    return TURBODB_STATUS_OK;
  }
  else return clear_error(error, TURBODB_STATUS_UNSUPPORTED, "expected DROP TABLE or TRUNCATE TABLE");
}
static turbodb_status_t clear_load(table_clear *c, vstr name, bool missing_ok, turbodb_error_t *error) {
  orm_sql_catalog_snapshot snapshot = {0};
  turbodb_status_t status = orm_sql_store_lookup(c->store, name, &c->table, &c->id, &c->version, &c->found, &snapshot, error);
  if (status != TURBODB_STATUS_OK || !c->found)
    return status != TURBODB_STATUS_OK || missing_ok ? status : clear_error(error, TURBODB_STATUS_SQL_ERROR, "table clear target does not exist");
  if (!c->drop && c->version == UINT64_MAX)
    return clear_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "TRUNCATE table version exhausted");
  status = orm_tidesdb_sql_catalog_schema(&c->table, &c->schema, error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_index_set_load(c->store, &c->schema, c->id, snapshot, &c->indexes, error);
  if (status == TURBODB_STATUS_OK && snapshot.format == ORM_SQL_STORE_FORMAT_BASE && vec_size(&c->indexes.records))
    status = clear_error(error, TURBODB_STATUS_DATASTORE_ERROR, "base table has orphan index directory");
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_relation_open(c->store, name, &c->source, error);
  if (status == TURBODB_STATUS_OK) memcpy(c->prefix, c->source.prefix, sizeof(c->prefix));
  return status;
}
static turbodb_status_t clear_validate(table_clear *c, turbodb_error_t *error) {
  const orm_sql_store_audit data = {c->prefix[0], c->id, 1, ORM_SQL_RELATION_KEY_BYTES, 1, c->store->max_record_bytes, c->count};
  turbodb_status_t status = orm_sql_store_audit_prefix(c->store, &data, error);
  if (status == TURBODB_STATUS_OK && c->count && vec_size(&c->indexes.records)) {
    const orm_sql_index_change change = {vec_data_const(&c->rows), NULL, c->count, c->schema.count, c->table.primary_key};
    status = orm_sql_index_delta_prepare(c->store, &c->indexes, &change, &c->delta, error);
  }
  const orm_sql_index_change snapshot = {vec_data_const(&c->rows), NULL, c->count, c->schema.count, c->table.primary_key};
  if (status == TURBODB_STATUS_OK) status = orm_sql_index_snapshot_audit(c->store, &c->indexes, &snapshot, error);
  return status;
}
static turbodb_status_t clear_prepare(table_clear *c, turbodb_error_t *error) {
  const size_t directories = c->drop ? vec_size(&c->indexes.records) : 0;
  const size_t metadata = c->drop ? CLEAR_DROP_METADATA : CLEAR_TRUNCATE_METADATA;
  if (directories > SIZE_MAX - metadata || c->count > SIZE_MAX - directories - metadata ||
      c->delta.count > SIZE_MAX - c->count - directories - metadata)
    return clear_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "table clear write count overflow");
  const size_t count = c->count + c->delta.count + directories + metadata;
  turbodb_status_t status = orm_sql_work_zero(&c->writes, count, sizeof(orm_sql_store_write), _Alignof(orm_sql_store_write),
      c->store->budget, &c->write_bytes, error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&c->keys, c->count, ORM_SQL_RELATION_KEY_BYTES, 1,
      c->store->budget, &c->key_bytes, error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&c->directories, directories, INDEX_DIRECTORY_HEADER + ORM_SQL_SELECT_NAME_BYTES, 1,
      c->store->budget, &c->directory_bytes, error);
  if (status == TURBODB_STATUS_OK) status = clear_steps(c, count, error);
  if (status != TURBODB_STATUS_OK) return status;
  size_t position = 0;
  for (size_t r = 0; r < c->count; ++r) {
    const turbodb_value_t *row = vec_at_const(&c->rows, r), primary = row[c->table.primary_key];
    uint8_t *key = vec_at(&c->keys, r); memcpy(key, c->prefix, sizeof(c->prefix));
    const uint64_t word = primary.kind == TURBODB_VALUE_INT64 ? orm_sql_wire_signed_order(primary.data.int64_value) : primary.data.uint64_value;
    orm_sql_wire_order_write(key + sizeof(c->prefix), word);
    *(orm_sql_store_write *)vec_at(&c->writes, position++) = (orm_sql_store_write){key, NULL, ORM_SQL_RELATION_KEY_BYTES, 0, ORM_SQL_STORE_DELETE};
  }
  for (size_t i = 0; i < c->delta.count; ++i)
    *(orm_sql_store_write *)vec_at(&c->writes, position++) = ((const orm_sql_index_mutation *)vec_at_const(&c->delta.mutations, i))->write;
  for (size_t i = 0; i < directories; ++i) {
    uint8_t *key = vec_at(&c->directories, i);
    const size_t size = orm_sql_index_directory_key(vec_at_const(&c->indexes.records, i), key);
    *(orm_sql_store_write *)vec_at(&c->writes, position++) = (orm_sql_store_write){key, NULL, size, 0, ORM_SQL_STORE_DELETE};
  }
  orm_sql_store_version_key(c->id, c->version_key);
  if (c->drop) {
    status = orm_sql_store_name_key(c->schema.name, c->name_key, error);
    if (status == TURBODB_STATUS_OK)
      status = orm_sql_store_catalog_barrier(c->store, c->manifest_key, c->manifest, error);
    if (status != TURBODB_STATUS_OK) return status;
    *(orm_sql_store_write *)vec_at(&c->writes, position++) = (orm_sql_store_write){c->name_key, NULL, 2 + c->schema.name.len, 0, ORM_SQL_STORE_DELETE};
    *(orm_sql_store_write *)vec_at(&c->writes, position++) = (orm_sql_store_write){c->version_key, NULL, sizeof(c->version_key), 0, ORM_SQL_STORE_DELETE};
    *(orm_sql_store_write *)vec_at(&c->writes, position++) = (orm_sql_store_write){c->manifest_key, c->manifest,
        sizeof(c->manifest_key), sizeof(c->manifest), ORM_SQL_STORE_PUT};
  } else {
    orm_sql_wire_write(c->version_value, sizeof(c->version_value), c->version + 1);
    *(orm_sql_store_write *)vec_at(&c->writes, position++) = (orm_sql_store_write){c->version_key, c->version_value,
        sizeof(c->version_key), sizeof(c->version_value), ORM_SQL_STORE_PUT};
  }
  return position == count ? TURBODB_STATUS_OK : clear_error(error, TURBODB_STATUS_INTERNAL_ERROR, "table clear write count mismatch");
}
static turbodb_status_t clear_close(table_clear *c, turbodb_error_t *error) {
  turbodb_status_t status = orm_tidesdb_sql_relation_close(&c->source, error);
  const turbodb_status_t delta = orm_sql_index_delta_close(&c->delta, status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = delta;
  const turbodb_status_t indexes = orm_sql_index_set_close(&c->indexes, status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = indexes;
  const turbodb_status_t table = orm_tidesdb_sql_catalog_destroy(&c->table, status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = table;
  vec_t *vectors[] = {&c->rows, &c->keys, &c->directories, &c->writes};
  const size_t bytes[] = {c->row_bytes, c->key_bytes, c->directory_bytes, c->write_bytes};
  for (size_t i = 0; i < sizeof(vectors)/sizeof(vectors[0]); ++i) {
    const turbodb_status_t closed = orm_sql_work_release(vectors[i], bytes[i], c->store->budget, status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = closed;
  }
  const size_t amounts[] = {c->count, c->metadata};
  const orm_sql_budget_resource resources[] = {ORM_SQL_BUDGET_MATERIALIZED_ROWS, ORM_SQL_BUDGET_WORK_BYTES};
  for (size_t i = 0; i < sizeof(amounts)/sizeof(amounts[0]); ++i) {
    const turbodb_status_t closed = amounts[i] ? orm_tidesdb_sql_budget_release(c->store->budget, resources[i], amounts[i],
        status == TURBODB_STATUS_OK ? error : NULL) : TURBODB_STATUS_OK;
    if (status == TURBODB_STATUS_OK) status = closed;
  }
  if (status != TURBODB_STATUS_OK) c->store->failed = true;
  return status;
}
static turbodb_status_t clear_target(const sqlparser_document *document, sqlparser_id target,
    bool missing_ok, table_clear *c, turbodb_error_t *error) {
  vstr name = {0}; const char *reason = NULL;
  turbodb_status_t status = orm_sql_name_node(document, target, &name, &reason);
  if (status != TURBODB_STATUS_OK) return clear_error(error, status, reason);
  status = clear_load(c, name, missing_ok, error);
  if (status == TURBODB_STATUS_OK && c->found)
    status = orm_sql_relation_snapshot(&c->source, &c->rows, &c->row_bytes, &c->count, error);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_relation_close(&c->source, error);
  if (status == TURBODB_STATUS_OK && c->found) status = clear_validate(c, error);
  if (status == TURBODB_STATUS_DATASTORE_ERROR) c->store->failed = true;
  if (status == TURBODB_STATUS_OK && c->found) status = clear_prepare(c, error);
  return status;
}
static turbodb_status_t clear_single(const sqlparser_document *document, sqlparser_id target,
    bool drop, bool missing_ok, orm_sql_catalog_store *store, turbodb_error_t *error) {
  table_clear c = {.store=store,.drop=drop};
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve_capacity(store->budget, 1, sizeof(c), 0,
      &c.metadata, error);
  if (status == TURBODB_STATUS_OK) status = clear_target(document, target, missing_ok, &c, error);
  if (status == TURBODB_STATUS_OK && c.found)
    status = orm_sql_store_batch(store, vec_data_const(&c.writes), vec_size(&c.writes), error);
  const turbodb_status_t closed = clear_close(&c, status == TURBODB_STATUS_OK ? error : NULL);
  return status == TURBODB_STATUS_OK ? closed : status;
}
static turbodb_status_t clear_multiple(const sqlparser_document *document, sqlparser_list targets,
    bool missing_ok, orm_sql_catalog_store *store, turbodb_error_t *error) {
  vec_t contexts = {0}, combined = {0}; size_t context_bytes = 0, combined_bytes = 0;
  turbodb_status_t status = orm_sql_work_zero(&contexts, targets.count, sizeof(table_clear),
      _Alignof(table_clear), store->budget, &context_bytes, error);
  for (size_t i = 0; i < vec_size(&contexts); ++i) {
    table_clear *c = vec_at(&contexts, i); c->store = store; c->drop = true;
  }
  sqlparser_id target = targets.first; size_t prepared = 0, write_count = 0;
  for (; status == TURBODB_STATUS_OK && prepared < targets.count; ++prepared) {
    const sqlparser_node *node = sqlparser_get_node(document, target);
    if (!node) { status = clear_error(error, TURBODB_STATUS_INTERNAL_ERROR, "invalid DROP TABLE target list"); break; }
    table_clear *c = vec_at(&contexts, prepared);
    status = clear_target(document, target, missing_ok, c, error);
    if (status == TURBODB_STATUS_OK && c->found) {
      const size_t count = vec_size(&c->writes);
      if (count > SIZE_MAX - write_count)
        status = clear_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "DROP TABLE write count overflow");
      else write_count += count;
    }
    target = node->next;
  }
  if (status == TURBODB_STATUS_OK && (prepared != targets.count || target != SQLPARSER_NONE))
    status = clear_error(error, TURBODB_STATUS_INTERNAL_ERROR, "invalid DROP TABLE target count");
  if (status == TURBODB_STATUS_OK && write_count)
    status = orm_sql_work_zero(&combined, write_count, sizeof(orm_sql_store_write),
        _Alignof(orm_sql_store_write), store->budget, &combined_bytes, error);
  if (status == TURBODB_STATUS_OK && write_count) {
    size_t position = 0;
    for (size_t i = 0; i < targets.count; ++i) {
      const table_clear *c = vec_at_const(&contexts, i);
      const size_t count = vec_size(&c->writes);
      if (count) memcpy(vec_at(&combined, position), vec_data_const(&c->writes), count * sizeof(orm_sql_store_write));
      position += count;
    }
    status = orm_sql_store_batch(store, vec_data_const(&combined), write_count, error);
  }
  const turbodb_status_t combined_closed = orm_sql_work_release(&combined, combined_bytes, store->budget,
      status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = combined_closed;
  for (size_t i = 0; i < vec_size(&contexts); ++i) {
    table_clear *c = vec_at(&contexts, i);
    const turbodb_status_t closed = clear_close(c, status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = closed;
  }
  const turbodb_status_t contexts_closed = orm_sql_work_release(&contexts, context_bytes, store->budget,
      status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = contexts_closed;
  if (status != TURBODB_STATUS_OK && (combined_closed != TURBODB_STATUS_OK || contexts_closed != TURBODB_STATUS_OK))
    store->failed = true;
  return status;
}
turbodb_status_t orm_sql_table_clear(const sqlparser_document *document, orm_sql_catalog_store *store, turbodb_error_t *error) {
  if (!document) return clear_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "table clear document required");
  turbodb_status_t status = orm_sql_store_writable(store, error);
  if (status != TURBODB_STATUS_OK) return status;
  bool drop = false, missing_ok = false; sqlparser_list targets = {0};
  status = clear_command(document, &drop, &missing_ok, &targets, error);
  if (status != TURBODB_STATUS_OK) return status;
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_AST_NODES] = sqlparser_node_count(document);
  status = orm_tidesdb_sql_budget_reserve(store->budget, &amount, error);
  if (status != TURBODB_STATUS_OK) return status;
  return targets.count == 1 ? clear_single(document, targets.first, drop, missing_ok, store, error) :
      clear_multiple(document, targets, missing_ok, store, error);
}

turbodb_status_t orm_sql_table_clear_validate(const sqlparser_document *document,
    orm_sql_catalog_store *store, turbodb_error_t *error) {
  if (!document) return clear_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"table clear document required");
  turbodb_status_t status=orm_sql_store_ready(store,error);
  bool drop=false,missing_ok=false; sqlparser_list targets={0};
  if (status==TURBODB_STATUS_OK) status=clear_command(document,&drop,&missing_ok,&targets,error);
  sqlparser_id id=targets.first; size_t count=0;
  for (;status==TURBODB_STATUS_OK && count<targets.count;++count) {
    const sqlparser_node *node=sqlparser_get_node(document,id);
    if (!node) { status=clear_error(error,TURBODB_STATUS_INTERNAL_ERROR,"invalid table clear targets"); break; }
    vstr name={0}; const char *reason=NULL;
    status=orm_sql_name_node(document,id,&name,&reason);
    if (status!=TURBODB_STATUS_OK) { status=clear_error(error,status,reason); break; }
    orm_sql_table_definition table={0}; uint64_t table_id=0,version=0; bool found=false;
    status=orm_tidesdb_sql_catalog_lookup(store,name,&table,&table_id,&version,&found,error);
    if (status==TURBODB_STATUS_OK && !found && !missing_ok)
      status=clear_error(error,TURBODB_STATUS_SQL_ERROR,"table clear target does not exist");
    const turbodb_status_t closed=orm_tidesdb_sql_catalog_destroy(&table,status==TURBODB_STATUS_OK?error:NULL);
    if (closed!=TURBODB_STATUS_OK) store->failed=true;
    if (status==TURBODB_STATUS_OK) status=closed;
    id=node->next;
  }
  if (status==TURBODB_STATUS_OK && (count!=targets.count || id))
    status=clear_error(error,TURBODB_STATUS_INTERNAL_ERROR,"invalid table clear target count");
  return status;
}

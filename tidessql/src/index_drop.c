#include "index_store.h"
#include "index_change.h"
#include "index_internal.h"
#include "name.h"
#include "relation.h"
#include "work.h"
#include "error.h"
#include <string.h>

enum { DROP_INDEX_METADATA_WRITES = 2 };
typedef struct index_drop {
  orm_sql_catalog_store *store;
  orm_sql_table_definition table;
  orm_sql_table_schema schema;
  orm_sql_index_set selected;
  orm_sql_relation_source source;
  orm_sql_index_delta delta;
  vec_t rows, writes;
  size_t row_bytes, write_bytes, metadata, count, tuple;
  uint64_t table_id, version, index_id;
} index_drop;
static turbodb_status_t drop_index_error(turbodb_error_t *error, turbodb_status_t status, const char *message) {
  tdsql_error_set(error, status, message); return status;
}
static turbodb_status_t drop_index_charge(index_drop *d, orm_sql_budget_resource resource, size_t count, turbodb_error_t *error) {
  orm_sql_budget_amount amount = {0}; amount.value[resource] = count;
  return orm_tidesdb_sql_budget_reserve(d->store->budget, &amount, error);
}
static turbodb_status_t drop_index_load(index_drop *d, vstr table, vstr name, bool validate_only, turbodb_error_t *error) {
  orm_sql_catalog_snapshot snapshot = {0}; bool found = false; orm_sql_index_set all = {0};
  turbodb_status_t status = orm_sql_store_lookup(d->store, table, &d->table, &d->table_id, &d->version, &found, &snapshot, error);
  if (status == TURBODB_STATUS_OK && !found) status = drop_index_error(error, TURBODB_STATUS_SQL_ERROR, "DROP INDEX table does not exist");
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_catalog_schema(&d->table, &d->schema, error);
  if (status == TURBODB_STATUS_OK && snapshot.format < ORM_SQL_STORE_FORMAT_INDEXED)
    status = drop_index_error(error, TURBODB_STATUS_SQL_ERROR, "DROP INDEX index does not exist");
  if (status == TURBODB_STATUS_OK) status = orm_sql_index_set_load(d->store, &d->schema, d->table_id, snapshot, &all, error);
  size_t position = SIZE_MAX;
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < vec_size(&all.records); ++i) {
    const orm_sql_index_record *record = vec_at_const(&all.records, i);
    status = drop_index_charge(d, ORM_SQL_BUDGET_EXECUTION_STEPS, name.len, error);
    if (record->definition.name_size == name.len && !memcmp(record->definition.name, name.data, name.len)) position = i;
  }
  if (status == TURBODB_STATUS_OK && position == SIZE_MAX)
    status = drop_index_error(error, TURBODB_STATUS_SQL_ERROR, "DROP INDEX index does not exist on this table");
  if (status == TURBODB_STATUS_OK && d->version == UINT64_MAX)
    status = drop_index_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "DROP INDEX table version exhausted");
  if (status == TURBODB_STATUS_OK) {
    d->selected.budget = d->store->budget;
    status = orm_sql_work_zero(&d->selected.records, 1, sizeof(orm_sql_index_record), _Alignof(orm_sql_index_record),
        d->store->budget, &d->selected.bytes, error);
  }
  if (status == TURBODB_STATUS_OK) {
    orm_sql_index_record *source = vec_at(&all.records, position), *target = vec_at(&d->selected.records, 0);
    *target = *source; *source = (orm_sql_index_record){0};
    d->index_id = target->identity.index_id;
    status = orm_tidesdb_sql_index_key_size(&target->definition, &d->tuple, error);
    if (status == TURBODB_STATUS_OK && (d->tuple > SIZE_MAX - INDEX_PREFIX_BYTES - ORM_SQL_WIRE_U64 ||
        INDEX_PREFIX_BYTES + d->tuple + ORM_SQL_WIRE_U64 > d->store->max_record_bytes))
      status = drop_index_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "DROP INDEX physical key exceeds capacity");
  }
  const turbodb_status_t closed = orm_sql_index_set_close(&all, status == TURBODB_STATUS_OK ? error : NULL);
  if (closed != TURBODB_STATUS_OK) d->store->failed = true;
  if (status == TURBODB_STATUS_OK) status = closed;
  if (status == TURBODB_STATUS_OK && !validate_only) status = orm_tidesdb_sql_relation_open(d->store, table, &d->source, error);
  return status;
}
/* Delta point validation plus complete physical counts proves no extra keys
 * survive deletion of the directory. All generations of the ID are audited. */
static turbodb_status_t drop_index_probe(index_drop *d, uint8_t space, size_t expected, turbodb_error_t *error) {
  const orm_sql_store_audit audit = {space, d->index_id, 1,
      INDEX_PREFIX_BYTES + d->tuple + (space == INDEX_DATA_NS ? ORM_SQL_WIRE_U64 : 0),
      ORM_SQL_WIRE_U64, ORM_SQL_WIRE_U64, expected};
  return orm_sql_store_audit_prefix(d->store, &audit, error);
}
static turbodb_status_t drop_index_publish(index_drop *d, turbodb_error_t *error) {
  if (d->delta.count > SIZE_MAX - DROP_INDEX_METADATA_WRITES)
    return drop_index_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "DROP INDEX write count overflow");
  const size_t count = d->delta.count + DROP_INDEX_METADATA_WRITES;
  turbodb_status_t status = orm_sql_work_zero(&d->writes, count, sizeof(orm_sql_store_write), _Alignof(orm_sql_store_write),
      d->store->budget, &d->write_bytes, error);
  if (status != TURBODB_STATUS_OK) return status;
  status = drop_index_charge(d, ORM_SQL_BUDGET_EXECUTION_STEPS, count, error);
  if (status != TURBODB_STATUS_OK) return status;
  for (size_t i = 0; i < d->delta.count; ++i)
    *(orm_sql_store_write *)vec_at(&d->writes, i) = ((const orm_sql_index_mutation *)vec_at_const(&d->delta.mutations, i))->write;
  uint8_t directory[INDEX_DIRECTORY_HEADER + ORM_SQL_SELECT_NAME_BYTES];
  const size_t directory_size = orm_sql_index_directory_key(vec_at_const(&d->selected.records, 0), directory);
  *(orm_sql_store_write *)vec_at(&d->writes, d->delta.count) = (orm_sql_store_write){directory, NULL, directory_size, 0, ORM_SQL_STORE_DELETE};
  uint8_t version_key[ORM_SQL_STORE_VERSION_KEY_BYTES], version[ORM_SQL_WIRE_U64];
  orm_sql_store_version_key(d->table_id, version_key); orm_sql_wire_write(version, sizeof(version), d->version + 1);
  *(orm_sql_store_write *)vec_at(&d->writes, count - 1) = (orm_sql_store_write){version_key, version, sizeof(version_key), sizeof(version), ORM_SQL_STORE_PUT};
  return orm_sql_store_batch(d->store, vec_data_const(&d->writes), count, error);
}
static turbodb_status_t drop_index_close(index_drop *d, turbodb_error_t *error) {
  turbodb_status_t status = orm_tidesdb_sql_relation_close(&d->source, error);
  const turbodb_status_t delta = orm_sql_index_delta_close(&d->delta, status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = delta;
  const turbodb_status_t indexes = orm_sql_index_set_close(&d->selected, status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = indexes;
  const turbodb_status_t table = orm_tidesdb_sql_catalog_destroy(&d->table, status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = table;
  vec_t *vectors[] = {&d->rows, &d->writes}; const size_t bytes[] = {d->row_bytes, d->write_bytes};
  for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
    const turbodb_status_t closed = orm_sql_work_release(vectors[i], bytes[i], d->store->budget, status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = closed;
  }
  const size_t amounts[] = {d->count, d->metadata};
  const orm_sql_budget_resource resources[] = {ORM_SQL_BUDGET_MATERIALIZED_ROWS, ORM_SQL_BUDGET_WORK_BYTES};
  for (size_t i = 0; i < sizeof(amounts) / sizeof(amounts[0]); ++i) {
    const turbodb_status_t closed = amounts[i] ? orm_tidesdb_sql_budget_release(d->store->budget,
        resources[i], amounts[i], status == TURBODB_STATUS_OK ? error : NULL) : TURBODB_STATUS_OK;
    if (status == TURBODB_STATUS_OK) status = closed;
  }
  if (status != TURBODB_STATUS_OK) d->store->failed = true;
  return status;
}
static turbodb_status_t drop_index_bind(const sqlparser_document *document,
    vstr *table, vstr *name, turbodb_error_t *error) {
  const sqlparser_node *statement = sqlparser_get_node(document, sqlparser_statements(document).first);
  if (sqlparser_get_dialect(document) != SQLPARSER_MYSQL || sqlparser_statements(document).count != 1 ||
      !statement || statement->kind != SQLPARSER_DROP_INDEX || statement->as.drop_object.if_exists)
    return drop_index_error(error, TURBODB_STATUS_UNSUPPORTED, "expected one MySQL DROP INDEX name ON table");
  const char *reason = NULL;
  turbodb_status_t status = orm_sql_name_node(document, statement->as.drop_object.name, name, &reason);
  if (status == TURBODB_STATUS_OK) status = orm_sql_name_node(document, statement->as.drop_object.table, table, &reason);
  if (status != TURBODB_STATUS_OK) return drop_index_error(error, status, reason);
  if (orm_sql_index_primary_name(*name)) return drop_index_error(error, TURBODB_STATUS_UNSUPPORTED, "DROP PRIMARY index is unsupported");
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_tidesdb_sql_index_drop(const sqlparser_document *document,
    orm_sql_catalog_store *store, uint64_t *index_id, turbodb_error_t *error) {
  if (!document || !index_id) return drop_index_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "DROP INDEX requires document and output");
  turbodb_status_t status = orm_sql_store_writable(store, error);
  if (status != TURBODB_STATUS_OK) return status;
  vstr table={0},name={0}; status=drop_index_bind(document,&table,&name,error);
  if (status!=TURBODB_STATUS_OK) return status;
  index_drop d = {.store = store};
  status = drop_index_charge(&d, ORM_SQL_BUDGET_AST_NODES, sqlparser_node_count(document), error);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_budget_reserve_capacity(store->budget, 1, sizeof(d), 0, &d.metadata, error);
  if (status == TURBODB_STATUS_OK) status = drop_index_load(&d, table, name, false, error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_relation_snapshot(&d.source, &d.rows, &d.row_bytes, &d.count, error);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_relation_close(&d.source, error);
  if (status == TURBODB_STATUS_OK && d.count) {
    const orm_sql_index_change change = {vec_data_const(&d.rows), NULL, d.count, d.schema.count, d.table.primary_key};
    status = orm_sql_index_delta_prepare(store, &d.selected, &change, &d.delta, error);
  }
  if (status == TURBODB_STATUS_OK) status = drop_index_probe(&d, INDEX_DATA_NS, d.count, error);
  if (status == TURBODB_STATUS_OK) status = drop_index_probe(&d, INDEX_UNIQUE_NS, d.delta.count - d.count, error);
  if (status == TURBODB_STATUS_DATASTORE_ERROR) store->failed = true;
  if (status == TURBODB_STATUS_OK) status = drop_index_publish(&d, error);
  const turbodb_status_t closed = drop_index_close(&d, status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = closed;
  if (status == TURBODB_STATUS_OK) *index_id = d.index_id;
  return status;
}
turbodb_status_t orm_sql_index_drop_validate(const sqlparser_document *document,
    orm_sql_catalog_store *store, turbodb_error_t *error) {
  if (!document) return drop_index_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"DROP INDEX document required");
  turbodb_status_t status=orm_sql_store_ready(store,error);
  vstr table={0},name={0};
  if (status==TURBODB_STATUS_OK) status=drop_index_bind(document,&table,&name,error);
  index_drop d={.store=store};
  if (status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_budget_reserve_capacity(store->budget,1,sizeof(d),0,&d.metadata,error);
  if (status==TURBODB_STATUS_OK) status=drop_index_load(&d,table,name,true,error);
  const turbodb_status_t closed=drop_index_close(&d,status==TURBODB_STATUS_OK?error:NULL);
  return closed==TURBODB_STATUS_OK?status:closed;
}

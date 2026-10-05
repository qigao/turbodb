#include "index_store.h"
#include "index_record.h"
#include "index_internal.h"
#include "index_directory.h"
#include "name.h"
#include "relation.h"
#include "store_internal.h"
#include "work.h"
#include "error.h"
#include <string.h>

enum { INDEX_METADATA_WRITES = 3 };
typedef enum index_workspace { INDEX_RECORD, INDEX_KEYS, INDEX_SLOTS, INDEX_WRITES, INDEX_WORKSPACES } index_workspace;
typedef struct index_slot { const uint8_t *tuple; size_t size; bool occupied; } index_slot;
typedef struct index_build {
  orm_sql_catalog_store *store;
  orm_sql_index_record record;
  orm_sql_table_definition table;
  orm_sql_table_schema schema;
  orm_sql_relation_source source;
  orm_sql_index_publication publication;
  vec_t work[INDEX_WORKSPACES];
  size_t bytes[INDEX_WORKSPACES], metadata, rows, tuple_size, key_size, stride, writes;
  uint64_t table_id, version;
} index_build;

static turbodb_status_t index_store_error(turbodb_error_t *error, turbodb_status_t status, const char *message) {
  tdsql_error_set(error, status, message); return status;
}
static turbodb_status_t index_build_charge(index_build *b, orm_sql_budget_resource resource,
    uint64_t count, turbodb_error_t *error) {
  orm_sql_budget_amount amount = {0}; amount.value[resource] = count;
  return orm_tidesdb_sql_budget_reserve(b->store->budget, &amount, error);
}
static int index_slot_compare(const void *a, const void *b) {
  const index_slot *x = a, *y = b;
  return memcmp(x->tuple, y->tuple, x->size);
}
static bool index_slot_copy(void *to, const void *from) { *(index_slot *)to = *(const index_slot *)from; return true; }
static void index_slot_move(void *to, void *from) { *(index_slot *)to = *(index_slot *)from; }
/* Slots borrow keys in fixed workspace, never own them. */
static void index_slot_destroy(void *value) { (void)value; }
static const cmeta_type_traits index_slot_traits = {
  CMETA_TRAIT_COMPARE | CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY |
      CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY,
  NULL, NULL, index_slot_compare, index_slot_copy, index_slot_move, index_slot_destroy};
static const cmeta_type_desc index_slot_type = {"sql_index_build_slot", sizeof(index_slot),
  _Alignof(index_slot), CMETA_T_OBJECT, NULL, &index_slot_traits};

static turbodb_status_t index_build_admit(index_build *b, turbodb_error_t *error) {
  if (b->publication.previous_format == ORM_SQL_STORE_FORMAT_BASE) {
    const uint8_t prefix = INDEX_UNIQUE_NS;
    return orm_sql_store_unused_prefix(b->store, &prefix, sizeof(prefix), error);
  }
  orm_sql_index_set set = {0};
  turbodb_status_t status = orm_sql_index_set_load(b->store, &b->schema, b->table_id,
      (orm_sql_catalog_snapshot){b->publication.id, b->publication.previous_format}, &set, error);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < vec_size(&set.records); ++i) {
    const orm_sql_index_record *existing = vec_at_const(&set.records, i);
    status = index_build_charge(b, ORM_SQL_BUDGET_EXECUTION_STEPS, b->record.definition.name_size, error);
    if (status == TURBODB_STATUS_OK && existing->definition.name_size == b->record.definition.name_size &&
        !memcmp(existing->definition.name, b->record.definition.name, existing->definition.name_size))
      status = index_store_error(error, TURBODB_STATUS_CONSTRAINT, "SQL index name already exists on this table");
  }
  const turbodb_status_t closed = orm_sql_index_set_close(&set, status == TURBODB_STATUS_OK ? error : NULL);
  if (closed != TURBODB_STATUS_OK) b->store->failed = true;
  if (status == TURBODB_STATUS_OK) status = closed;
  uint8_t prefix[1 + ORM_SQL_WIRE_U64]; orm_sql_wire_write(prefix + 1, ORM_SQL_WIRE_U64, b->publication.id);
  for (uint8_t space = INDEX_UNIQUE_NS; status == TURBODB_STATUS_OK && space <= INDEX_DATA_NS; ++space) {
    prefix[0] = space; status = orm_sql_store_unused_prefix(b->store, prefix, sizeof(prefix), error);
  }
  return status;
}
static turbodb_status_t index_build_prepare(index_build *b, const sqlparser_document *document,
    vstr table, turbodb_error_t *error) {
  turbodb_status_t status = orm_sql_store_prepare_index(b->store, &b->publication, error);
  bool found = false;
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_catalog_lookup(b->store, table, &b->table,
      &b->table_id, &b->version, &found, error);
  if (status == TURBODB_STATUS_OK && !found)
    status = index_store_error(error, TURBODB_STATUS_SQL_ERROR, "CREATE INDEX table does not exist");
  if (status == TURBODB_STATUS_OK && b->version == UINT64_MAX)
    status = index_store_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "CREATE INDEX table version exhausted");
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_catalog_schema(&b->table, &b->schema, error);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_index_bind_create(document, &b->schema,
      b->store->budget, &b->record.definition, error);
  if (status == TURBODB_STATUS_OK && orm_sql_index_has_double(&b->record.definition))
    b->publication.data[ORM_SQL_STORE_FORMAT_OFFSET] = ORM_SQL_STORE_FORMAT_REAL_INDEXED;
  b->record.identity = (orm_sql_index_identity){b->table_id, 1, b->publication.id, 1};
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_index_record_encode(&b->record, &b->schema,
      b->store->max_record_bytes, &b->work[INDEX_RECORD], &b->bytes[INDEX_RECORD], error);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_index_key_size(&b->record.definition, &b->tuple_size, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (b->tuple_size > SIZE_MAX - INDEX_PREFIX_BYTES - ORM_SQL_WIRE_U64)
    return index_store_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "CREATE INDEX key size overflow");
  b->key_size = INDEX_PREFIX_BYTES + b->tuple_size + ORM_SQL_WIRE_U64;
  const size_t unique_size = b->record.definition.unique ? b->key_size - ORM_SQL_WIRE_U64 : 0;
  if (b->key_size > b->store->max_record_bytes || unique_size > SIZE_MAX - b->key_size ||
      INDEX_DIRECTORY_HEADER + b->record.definition.name_size > b->store->max_record_bytes)
    return index_store_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "CREATE INDEX physical key exceeds byte limit");
  b->stride = b->key_size + unique_size;
  status = index_build_admit(b, error);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_relation_open(b->store, table, &b->source, error);
  return status;
}
turbodb_status_t orm_sql_index_create_validate(const sqlparser_document *document,
    orm_sql_catalog_store *store, turbodb_error_t *error) {
  if (!document) return index_store_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"CREATE INDEX document required");
  turbodb_status_t status=orm_sql_store_ready(store,error);
  if (status!=TURBODB_STATUS_OK) return status;
  const sqlparser_node *root=sqlparser_get_node(document,sqlparser_statements(document).first);
  if (!root || root->kind!=SQLPARSER_CREATE_INDEX)
    return index_store_error(error,TURBODB_STATUS_UNSUPPORTED,"CREATE INDEX required");
  vstr name={0}; const char *reason=NULL;
  status=orm_sql_name_node(document,root->as.create_index.table,&name,&reason);
  if (status!=TURBODB_STATUS_OK) return index_store_error(error,status,reason);
  orm_sql_table_definition table={0}; orm_sql_index_definition index={0};
  uint64_t id=0,version=0; bool found=false;
  status=orm_tidesdb_sql_catalog_lookup(store,name,&table,&id,&version,&found,error);
  if (status==TURBODB_STATUS_OK && !found)
    status=index_store_error(error,TURBODB_STATUS_SQL_ERROR,"CREATE INDEX table does not exist");
  orm_sql_table_schema schema={0};
  if (status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_catalog_schema(&table,&schema,error);
  if (status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_index_bind_create(document,&schema,store->budget,&index,error);
  turbodb_status_t closed=orm_tidesdb_sql_index_destroy(&index,status==TURBODB_STATUS_OK?error:NULL);
  if (closed!=TURBODB_STATUS_OK) store->failed=true;
  if (status==TURBODB_STATUS_OK) status=closed;
  closed=orm_tidesdb_sql_catalog_destroy(&table,status==TURBODB_STATUS_OK?error:NULL);
  if (closed!=TURBODB_STATUS_OK) store->failed=true;
  return closed==TURBODB_STATUS_OK?status:closed;
}
static turbodb_status_t index_build_count(index_build *b, turbodb_error_t *error) {
  for (;;) {
    const turbodb_value_t *row = NULL;
    turbodb_status_t status = b->source.source.next(b->source.source.context, &row, error);
    if (status != TURBODB_STATUS_OK || !row) return status;
    if (b->rows == SIZE_MAX)
      return index_store_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "CREATE INDEX row count overflow");
    status = index_build_charge(b, ORM_SQL_BUDGET_MATERIALIZED_ROWS, 1, error);
    if (status != TURBODB_STATUS_OK) return status;
    ++b->rows;
  }
}
static turbodb_status_t index_build_allocate(index_build *b, turbodb_error_t *error) {
  const size_t per_row = b->record.definition.unique ? 2 : 1;
  if (b->rows > (SIZE_MAX - INDEX_METADATA_WRITES) / per_row)
    return index_store_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "CREATE INDEX write count overflow");
  turbodb_status_t status = orm_sql_work_zero(&b->work[INDEX_KEYS], b->rows, b->stride, 1,
      b->store->budget, &b->bytes[INDEX_KEYS], error);
  if (status == TURBODB_STATUS_OK && b->record.definition.unique)
    status = orm_sql_work_zero(&b->work[INDEX_SLOTS], b->rows, sizeof(index_slot), _Alignof(index_slot),
        b->store->budget, &b->bytes[INDEX_SLOTS], error);
  if (status == TURBODB_STATUS_OK)
    status = orm_sql_work_zero(&b->work[INDEX_WRITES], b->rows * per_row + INDEX_METADATA_WRITES,
        sizeof(orm_sql_store_write), _Alignof(orm_sql_store_write), b->store->budget, &b->bytes[INDEX_WRITES], error);
  return status;
}
static void index_build_write(index_build *b, const uint8_t *key, size_t size,
    const uint8_t *value, size_t value_size) {
  *(orm_sql_store_write *)vec_at(&b->work[INDEX_WRITES], b->writes++) =
      (orm_sql_store_write){key, value, size, value_size, ORM_SQL_STORE_PUT};
}
static turbodb_status_t index_build_row(index_build *b, size_t position, const turbodb_value_t *row, turbodb_error_t *error) {
  turbodb_status_t status = index_build_charge(b, ORM_SQL_BUDGET_EXECUTION_STEPS, b->stride, error);
  if (status != TURBODB_STATUS_OK) return status;
  uint8_t *key = vec_at(&b->work[INDEX_KEYS], position);
  uint8_t *primary = key + b->key_size - ORM_SQL_WIRE_U64;
  bool contains_null = false;
  status = orm_tidesdb_sql_index_key_encode(&b->record.definition, row, b->schema.count,
      key + INDEX_PREFIX_BYTES, b->tuple_size, &contains_null, error);
  if (status != TURBODB_STATUS_OK) return status;
  orm_sql_index_prefix(key, INDEX_DATA_NS, &b->record.identity);
  const turbodb_value_t *pk = &row[b->table.primary_key];
  orm_sql_wire_order_write(primary, pk->kind == TURBODB_VALUE_INT64 ?
      orm_sql_wire_signed_order(pk->data.int64_value) : pk->data.uint64_value);
  index_build_write(b, key, b->key_size, primary, ORM_SQL_WIRE_U64);
  if (b->record.definition.unique) {
    *(index_slot *)vec_at(&b->work[INDEX_SLOTS], position) =
        (index_slot){key + INDEX_PREFIX_BYTES, b->tuple_size, !contains_null};
    if (!contains_null) {
      uint8_t *unique = key + b->key_size;
      orm_sql_index_prefix(unique, INDEX_UNIQUE_NS, &b->record.identity);
      memcpy(unique + INDEX_PREFIX_BYTES, key + INDEX_PREFIX_BYTES, b->tuple_size);
      index_build_write(b, unique, b->key_size - ORM_SQL_WIRE_U64, primary, ORM_SQL_WIRE_U64);
    }
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t index_build_fill(index_build *b, turbodb_error_t *error) {
  turbodb_status_t status = orm_sql_relation_rewind(&b->source, error);
  size_t position = 0;
  while (status == TURBODB_STATUS_OK) {
    const turbodb_value_t *row = NULL;
    status = b->source.source.next(b->source.source.context, &row, error);
    if (status != TURBODB_STATUS_OK || !row) break;
    if (position == b->rows) {
      b->store->failed = true;
      return index_store_error(error, TURBODB_STATUS_DATASTORE_ERROR, "CREATE INDEX snapshot row count grew");
    }
    status = index_build_row(b, position++, row, error);
  }
  if (status == TURBODB_STATUS_OK && position != b->rows) {
    b->store->failed = true;
    status = index_store_error(error, TURBODB_STATUS_DATASTORE_ERROR, "CREATE INDEX snapshot row count shrank");
  }
  return status;
}
static turbodb_status_t index_build_unique(index_build *b, turbodb_error_t *error) {
  if (!b->record.definition.unique || b->rows < 2) return TURBODB_STATUS_OK;
  turbodb_status_t status = orm_sql_work_sort(vec_data(&b->work[INDEX_SLOTS]), b->rows,
      &index_slot_type, b->tuple_size, b->store->budget, error);
  for (size_t i = 1; status == TURBODB_STATUS_OK && i < b->rows; ++i) {
    status = index_build_charge(b, ORM_SQL_BUDGET_EXECUTION_STEPS, b->tuple_size, error);
    if (status != TURBODB_STATUS_OK) break;
    const index_slot *before = vec_at_const(&b->work[INDEX_SLOTS], i - 1), *after = vec_at_const(&b->work[INDEX_SLOTS], i);
    if (before->occupied && after->occupied && !index_slot_compare(before, after))
      status = index_store_error(error, TURBODB_STATUS_CONSTRAINT, "CREATE UNIQUE INDEX duplicate non-NULL key");
  }
  return status;
}
static turbodb_status_t index_build_publish(index_build *b, turbodb_error_t *error) {
  uint8_t directory[INDEX_DIRECTORY_HEADER + ORM_SQL_SELECT_NAME_BYTES];
  const size_t directory_size = orm_sql_index_directory_key(&b->record, directory);
  uint8_t version_key[ORM_SQL_STORE_VERSION_KEY_BYTES], version[ORM_SQL_WIRE_U64];
  orm_sql_store_version_key(b->table_id, version_key);
  orm_sql_wire_write(version, sizeof(version), b->version + 1);
  index_build_write(b, directory, directory_size,
      vec_data_const(&b->work[INDEX_RECORD]), vec_size(&b->work[INDEX_RECORD]));
  index_build_write(b, version_key, sizeof(version_key), version, sizeof(version));
  index_build_write(b, b->publication.key, sizeof(b->publication.key), b->publication.data, sizeof(b->publication.data));
  return orm_sql_store_batch(b->store, vec_data_const(&b->work[INDEX_WRITES]), b->writes, error);
}
static turbodb_status_t index_build_close(index_build *b, turbodb_error_t *error) {
  turbodb_status_t status = orm_tidesdb_sql_relation_close(&b->source, error);
  for (size_t i = 0; i < INDEX_WORKSPACES; ++i) {
    const turbodb_status_t released = orm_sql_work_release(&b->work[i], b->bytes[i], b->store->budget,
        status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  const turbodb_status_t index = orm_tidesdb_sql_index_record_destroy(&b->record, status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = index;
  const turbodb_status_t table = orm_tidesdb_sql_catalog_destroy(&b->table, status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = table;
  const uint64_t amounts[] = {b->rows, b->metadata};
  const orm_sql_budget_resource resources[] = {ORM_SQL_BUDGET_MATERIALIZED_ROWS, ORM_SQL_BUDGET_WORK_BYTES};
  for (size_t i = 0; i < sizeof(amounts) / sizeof(amounts[0]); ++i) {
    const turbodb_status_t released = amounts[i] ? orm_tidesdb_sql_budget_release(b->store->budget, resources[i],
        amounts[i], status == TURBODB_STATUS_OK ? error : NULL) : TURBODB_STATUS_OK;
    if (status == TURBODB_STATUS_OK) status = released;
  }
  if (status != TURBODB_STATUS_OK) b->store->failed = true;
  return status;
}
turbodb_status_t orm_tidesdb_sql_index_build(const sqlparser_document *document,
    orm_sql_catalog_store *store, uint64_t *index_id, turbodb_error_t *error) {
  if (!document || !index_id)
    return index_store_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "CREATE INDEX requires document and output");
  turbodb_status_t status = orm_sql_store_writable(store, error);
  if (status != TURBODB_STATUS_OK) return status;
  const sqlparser_node *statement = sqlparser_get_node(document, sqlparser_statements(document).first);
  if (sqlparser_get_dialect(document) != SQLPARSER_MYSQL || sqlparser_statements(document).count != 1 ||
      !statement || statement->kind != SQLPARSER_CREATE_INDEX)
    return index_store_error(error, TURBODB_STATUS_UNSUPPORTED, "index build requires one MySQL CREATE INDEX");
  vstr table = {0}; const char *reason = NULL;
  status = orm_sql_name_node(document, statement->as.create_index.table, &table, &reason);
  if (status != TURBODB_STATUS_OK) return index_store_error(error, status, reason);
  index_build b = {.store = store};
  status = orm_tidesdb_sql_budget_reserve_capacity(store->budget, 1, sizeof(b), 0, &b.metadata, error);
  if (status == TURBODB_STATUS_OK) status = index_build_prepare(&b, document, table, error);
  if (status == TURBODB_STATUS_OK) status = index_build_count(&b, error);
  if (status == TURBODB_STATUS_OK) status = index_build_allocate(&b, error);
  if (status == TURBODB_STATUS_OK) status = index_build_fill(&b, error);
  if (status == TURBODB_STATUS_OK) status = index_build_unique(&b, error);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_relation_close(&b.source, error);
  if (status == TURBODB_STATUS_OK) status = index_build_publish(&b, error);
  const turbodb_status_t released = index_build_close(&b, status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  if (status == TURBODB_STATUS_OK) *index_id = b.publication.id;
  return status;
}

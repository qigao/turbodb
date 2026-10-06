#include "index.h"
#include "index_internal.h"
#include "name.h"
#include "work.h"
#include "error.h"
#include <stdio.h>
#include <string.h>

typedef struct index_binder {
  const sqlparser_document *document;
  const orm_sql_table_schema *schema;
  orm_sql_index_definition definition;
  size_t offset;
  turbodb_error_t *error;
} index_binder;

static turbodb_status_t index_error(turbodb_error_t *error, turbodb_status_t status, size_t offset, const char *reason) {
  char message[TURBODB_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message), "TidesDB SQL CREATE INDEX at byte %zu: %s", offset, reason);
  tdsql_error_set(error, status, message); return status;
}
static turbodb_status_t index_fail(index_binder *b, turbodb_status_t status, const char *reason) {
  return index_error(b->error, status, b->offset, reason);
}
static turbodb_status_t index_charge(index_binder *b, orm_sql_budget_resource resource, size_t count) {
  orm_sql_budget_amount amount = {0}; amount.value[resource] = count;
  return orm_tidesdb_sql_budget_reserve(b->definition.budget, &amount, b->error);
}
static turbodb_status_t index_identifier(index_binder *b, sqlparser_id id, vstr *out) {
  const sqlparser_node *node = sqlparser_get_node(b->document, id);
  if (node) b->offset = node->span.offset;
  const char *reason = NULL;
  const turbodb_status_t status = orm_sql_name_node(b->document, id, out, &reason);
  return status == TURBODB_STATUS_OK ? status : index_fail(b, status, reason);
}
static bool index_same_name(vstr a, vstr b) {
  return a.len == b.len && !memcmp(a.data, b.data, a.len);
}
static turbodb_status_t index_names_view(index_binder *b, vstr name, sqlparser_id table_name, bool generated) {
  vstr table = {0};
  if (!generated && orm_sql_index_primary_name(name))
    return index_fail(b, TURBODB_STATUS_SQL_ERROR, "PRIMARY is a reserved index name");
  turbodb_status_t status = index_identifier(b, table_name, &table);
  if (status != TURBODB_STATUS_OK) return status;
  if (!index_same_name(table, b->schema->name))
    return index_fail(b, TURBODB_STATUS_SQL_ERROR, "index table does not match Catalog schema");
  memcpy(b->definition.name, name.data, name.len); b->definition.name_size = name.len;
  memcpy(b->definition.table, table.data, table.len); b->definition.table_size = table.len;
  b->definition.generated_name = generated;
  return TURBODB_STATUS_OK;
}
static turbodb_status_t index_names(index_binder *b, sqlparser_id index_name, sqlparser_id table_name) {
  vstr name = {0};
  turbodb_status_t status = index_identifier(b, index_name, &name);
  return status == TURBODB_STATUS_OK ? index_names_view(b, name, table_name, false) : status;
}
static turbodb_status_t index_part(index_binder *b, const sqlparser_node *order, size_t slot) {
  if (!order || (order->kind != SQLPARSER_INDEX_PART && order->kind != SQLPARSER_ORDER))
    return index_fail(b, TURBODB_STATUS_UNSUPPORTED, "expected an ordered key column");
  b->offset = order->span.offset;
  const bool table_key = order->kind == SQLPARSER_ORDER;
  if (!table_key && (order->as.index_part.expression || order->as.index_part.length))
    return index_fail(b, TURBODB_STATUS_UNSUPPORTED, "functional and prefix index keys are unsupported");
  vstr name = {0};
  turbodb_status_t status = index_identifier(b, table_key ? order->as.order.expression : order->as.index_part.column, &name);
  if (status != TURBODB_STATUS_OK) return status;
  size_t ordinal = SIZE_MAX;
  for (size_t i = 0; i < b->schema->count; ++i) {
    status = index_charge(b, ORM_SQL_BUDGET_EXECUTION_STEPS, 1);
    if (status != TURBODB_STATUS_OK) return status;
    if (index_same_name(name, b->schema->columns[i].name)) { ordinal = i; break; }
  }
  if (ordinal == SIZE_MAX) return index_fail(b, TURBODB_STATUS_SQL_ERROR, "unknown index column");
  const orm_sql_type type = b->schema->columns[ordinal].type;
  if (type.kind != TURBODB_VALUE_INT64 && type.kind != TURBODB_VALUE_UINT64 && type.kind != TURBODB_VALUE_DOUBLE)
    return index_fail(b, TURBODB_STATUS_UNSUPPORTED, "index columns require BIGINT, BIGINT UNSIGNED or DOUBLE");
  for (size_t i = 0; i < slot; ++i) {
    status = index_charge(b, ORM_SQL_BUDGET_EXECUTION_STEPS, 1);
    if (status != TURBODB_STATUS_OK) return status;
    const orm_sql_index_part *part = vec_at_const(&b->definition.parts, i);
    if (part->column == ordinal) return index_fail(b, TURBODB_STATUS_SQL_ERROR, "duplicate index column");
  }
  orm_sql_index_part *part = vec_at(&b->definition.parts, slot);
  *part = (orm_sql_index_part){ordinal, type, table_key ? order->as.order.descending : order->as.index_part.descending};
  return TURBODB_STATUS_OK;
}
static turbodb_status_t index_parts(index_binder *b, sqlparser_list columns) {
  if (!columns.count) return index_fail(b, TURBODB_STATUS_UNSUPPORTED, "index requires key columns");
  turbodb_status_t status = index_charge(b, ORM_SQL_BUDGET_PLAN_NODES, columns.count);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&b->definition.parts, columns.count,
      sizeof(orm_sql_index_part), _Alignof(orm_sql_index_part), b->definition.budget,
      &b->definition.part_bytes, b->error);
  size_t slot = 0;
  for (sqlparser_id id = columns.first; status == TURBODB_STATUS_OK && id;) {
    status = index_charge(b, ORM_SQL_BUDGET_EXECUTION_STEPS, 1);
    if (status != TURBODB_STATUS_OK) break;
    if (slot == columns.count) return index_fail(b, TURBODB_STATUS_INVALID_ARGUMENT, "invalid key list length");
    const sqlparser_node *order = sqlparser_get_node(b->document, id);
    status = index_part(b, order, slot++);
    if (status == TURBODB_STATUS_OK) id = order->next;
  }
  if (status == TURBODB_STATUS_OK && slot != columns.count)
    status = index_fail(b, TURBODB_STATUS_INVALID_ARGUMENT, "invalid key list length");
  return status;
}
turbodb_status_t orm_tidesdb_sql_index_bind_create(const sqlparser_document *document,
    const orm_sql_table_schema *schema, orm_tidesdb_sql_budget *budget,
    orm_sql_index_definition *out, turbodb_error_t *error) {
  if (!document || !schema || !schema->name.data || !schema->name.len ||
      !schema->columns || !schema->count || !budget || !out || out->budget || out->parts.initialized)
    return index_error(error, TURBODB_STATUS_INVALID_ARGUMENT, 0, "invalid index binding arguments");
  if (sqlparser_get_dialect(document) != SQLPARSER_MYSQL || sqlparser_statements(document).count != 1)
    return index_error(error, TURBODB_STATUS_UNSUPPORTED, 0, "expected exactly one MySQL CREATE INDEX");
  const sqlparser_node *statement = sqlparser_get_node(document, sqlparser_statements(document).first);
  if (!statement || statement->kind != SQLPARSER_CREATE_INDEX ||
      statement->as.create_index.if_not_exists || statement->as.create_index.where)
    return index_error(error, TURBODB_STATUS_UNSUPPORTED, statement ? statement->span.offset : 0,
        "unsupported CREATE INDEX statement or options");
  index_binder b = {.document = document, .schema = schema, .error = error,
      .definition = {.budget = budget, .source_offset = statement->span.offset,
          .unique = statement->as.create_index.unique}};
  turbodb_status_t status = index_charge(&b, ORM_SQL_BUDGET_AST_NODES, sqlparser_node_count(document));
  if (status == TURBODB_STATUS_OK) status = index_charge(&b, ORM_SQL_BUDGET_PLAN_NODES, 1);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_budget_reserve_capacity(budget, 1,
      sizeof(b), 0, &b.definition.metadata_bytes, error);
  if (status == TURBODB_STATUS_OK) status = index_names(&b, statement->as.create_index.name, statement->as.create_index.table);
  if (status == TURBODB_STATUS_OK) status = index_parts(&b, statement->as.create_index.columns);
  if (status == TURBODB_STATUS_OK) { *out = b.definition; return TURBODB_STATUS_OK; }
  const turbodb_status_t released = orm_tidesdb_sql_index_destroy(&b.definition, NULL);
  return released == TURBODB_STATUS_OK ? status : released;
}
turbodb_status_t orm_sql_index_bind_table_key(const sqlparser_document *document, const sqlparser_node *key,
    const orm_sql_table_schema *schema, orm_tidesdb_sql_budget *budget,
    orm_sql_index_definition *out, turbodb_error_t *error) {
  if (!document || !key || !schema || !schema->columns || !schema->count || !budget || !out || out->budget || out->parts.initialized)
    return index_error(error, TURBODB_STATUS_INVALID_ARGUMENT, 0, "invalid table index binding arguments");
  if (sqlparser_get_dialect(document) != SQLPARSER_MYSQL || key->kind != SQLPARSER_CONSTRAINT ||
      (key->as.constraint.kind != SQLPARSER_INDEX && key->as.constraint.kind != SQLPARSER_UNIQUE) ||
      key->as.constraint.expression || key->as.constraint.table ||
      key->as.constraint.referenced_columns.count || key->as.constraint.declarations.count ||
      key->as.constraint.autoincrement || key->as.constraint.descending ||
      key->as.constraint.conflict != SQLPARSER_CONFLICT_DEFAULT ||
      !key->as.constraint.key_terms.count ||
      key->as.constraint.columns.count != key->as.constraint.key_terms.count)
    return index_error(error, TURBODB_STATUS_UNSUPPORTED, key->span.offset, "expected a numeric table index");
  index_binder b = {.document=document, .schema=schema, .error=error,
      .definition={.budget=budget, .source_offset=key->span.offset,
          .unique=key->as.constraint.kind == SQLPARSER_UNIQUE}};
  turbodb_status_t status = index_charge(&b, ORM_SQL_BUDGET_PLAN_NODES, 1);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_budget_reserve_capacity(budget, 1, sizeof(b), 0,
      &b.definition.metadata_bytes, error);
  const sqlparser_node *root=sqlparser_get_node(document,sqlparser_statements(document).first);
  if (status == TURBODB_STATUS_OK && key->as.constraint.name)
    status = index_names(&b, key->as.constraint.name, root->as.create_table.table);
  else if (status == TURBODB_STATUS_OK) {
    const sqlparser_node *first=sqlparser_get_node(document,key->as.constraint.key_terms.first);
    vstr generated={0};
    status=index_identifier(&b,first?first->as.order.expression:0,&generated);
    if (status==TURBODB_STATUS_OK) status=index_names_view(&b,generated,root->as.create_table.table,true);
  }
  if (status == TURBODB_STATUS_OK) status = index_parts(&b, key->as.constraint.key_terms);
  if (status == TURBODB_STATUS_OK) { *out=b.definition; return status; }
  const turbodb_status_t released = orm_tidesdb_sql_index_destroy(&b.definition, NULL);
  return released == TURBODB_STATUS_OK ? status : released;
}
turbodb_status_t orm_sql_index_bind_column_key(const sqlparser_document *document, const sqlparser_node *key,
    sqlparser_id column, const orm_sql_table_schema *schema, orm_tidesdb_sql_budget *budget,
    orm_sql_index_definition *out, turbodb_error_t *error) {
  if (!document || !key || !column || !schema || !schema->columns || !schema->count || !budget ||
      !out || out->budget || out->parts.initialized)
    return index_error(error,TURBODB_STATUS_INVALID_ARGUMENT,0,"invalid column index binding arguments");
  if (sqlparser_get_dialect(document)!=SQLPARSER_MYSQL || key->kind!=SQLPARSER_CONSTRAINT ||
      key->as.constraint.kind!=SQLPARSER_UNIQUE || key->as.constraint.name || key->as.constraint.expression ||
      key->as.constraint.table || key->as.constraint.columns.count || key->as.constraint.referenced_columns.count ||
      key->as.constraint.key_terms.count || key->as.constraint.declarations.count || key->as.constraint.autoincrement ||
      key->as.constraint.descending || key->as.constraint.conflict!=SQLPARSER_CONFLICT_DEFAULT)
    return index_error(error,TURBODB_STATUS_UNSUPPORTED,key->span.offset,"expected a column UNIQUE constraint");
  index_binder b={.document=document,.schema=schema,.offset=key->span.offset,.error=error,
      .definition={.budget=budget,.source_offset=key->span.offset,.unique=true}};
  turbodb_status_t status=index_charge(&b,ORM_SQL_BUDGET_PLAN_NODES,2);
  if (status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_budget_reserve_capacity(budget,1,sizeof(b),0,
      &b.definition.metadata_bytes,error);
  const sqlparser_node *root=sqlparser_get_node(document,sqlparser_statements(document).first);
  vstr generated={0};
  if (status==TURBODB_STATUS_OK) status=index_identifier(&b,column,&generated);
  if (status==TURBODB_STATUS_OK) status=index_names_view(&b,generated,root->as.create_table.table,true);
  if (status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&b.definition.parts,1,sizeof(orm_sql_index_part),
      _Alignof(orm_sql_index_part),budget,&b.definition.part_bytes,error);
  if (status==TURBODB_STATUS_OK) {
    sqlparser_node order={.kind=SQLPARSER_ORDER,.span=key->span,.as.order={.expression=column}};
    status=index_part(&b,&order,0);
  }
  if (status==TURBODB_STATUS_OK) { *out=b.definition; return status; }
  const turbodb_status_t released=orm_tidesdb_sql_index_destroy(&b.definition,NULL);
  return released==TURBODB_STATUS_OK?status:released;
}
turbodb_status_t orm_tidesdb_sql_index_destroy(orm_sql_index_definition *definition, turbodb_error_t *error) {
  if (!definition || !definition->budget) return TURBODB_STATUS_OK;
  turbodb_status_t status = orm_sql_work_release(&definition->parts, definition->part_bytes, definition->budget, error);
  if (definition->metadata_bytes) {
    const turbodb_status_t released = orm_tidesdb_sql_budget_release(definition->budget,
        ORM_SQL_BUDGET_WORK_BYTES, definition->metadata_bytes, status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  *definition = (orm_sql_index_definition){0}; return status;
}

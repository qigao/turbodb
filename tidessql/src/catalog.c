#include "catalog.h"
#include "expr.h"
#include "name.h"
#include "work.h"
#include "wire.h"
#include "error.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct catalog_name { char bytes[ORM_SQL_SELECT_NAME_BYTES + 1]; size_t size; } catalog_name;
typedef struct catalog_binder {
  const sqlparser_document *document;
  orm_sql_table_definition definition;
  size_t offset;
  turbodb_error_t *error;
  bool secondary;
  bool alter;
  bool validate_only;
} catalog_binder;

static turbodb_status_t catalog_error(turbodb_error_t *error, turbodb_status_t status, size_t offset, const char *reason) {
  char message[TURBODB_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message), "TidesDB SQL CREATE TABLE at byte %zu: %s", offset, reason);
  tdsql_error_set(error, status, message); return status;
}
static turbodb_status_t catalog_fail(catalog_binder *b, turbodb_status_t status, const char *reason) {
  if (b->alter) {
    char message[TURBODB_ERROR_MESSAGE_CAPACITY];
    (void)snprintf(message,sizeof(message),"TidesDB SQL ALTER TABLE at byte %zu: %s",b->offset,reason);
    tdsql_error_set(b->error,status,message); return status;
  }
  return catalog_error(b->error, status, b->offset, reason);
}
static turbodb_status_t catalog_charge(catalog_binder *b, orm_sql_budget_resource resource, size_t count) {
  orm_sql_budget_amount amount = {0}; amount.value[resource] = count;
  return orm_tidesdb_sql_budget_reserve(b->definition.budget, &amount, b->error);
}
static turbodb_status_t catalog_vector(catalog_binder *b, vec_t *v, size_t count,
    size_t size, size_t align, size_t *bytes) {
  return orm_sql_work_zero(v, count, size, align, b->definition.budget, bytes, b->error);
}
static turbodb_status_t catalog_identifier(catalog_binder *b, sqlparser_id id, vstr *out) {
  const sqlparser_node *node = sqlparser_get_node(b->document, id);
  if (!node || node->kind != SQLPARSER_NAME)
    return catalog_fail(b, TURBODB_STATUS_UNSUPPORTED, "expected an identifier");
  b->offset = node->span.offset;
  if (node->as.name.parts != 1)
    return catalog_fail(b, TURBODB_STATUS_UNSUPPORTED, "qualified names are unsupported");
  const char *reason = NULL;
  vstr text = {sqlparser_text(b->document, node->span), node->span.length};
  turbodb_status_t status = orm_sql_name_part(&text, out, &reason);
  if (status != TURBODB_STATUS_OK) return catalog_fail(b, status, reason);
  if (text.len) return catalog_fail(b, TURBODB_STATUS_UNSUPPORTED, "unsupported identifier quoting");
  return TURBODB_STATUS_OK;
}
static vstr catalog_copy_name(catalog_binder *b, size_t slot, vstr source) {
  catalog_name *name = vec_at(&b->definition.names, slot);
  memcpy(name->bytes, source.data, source.len); name->size = source.len;
  return (vstr){name->bytes, name->size};
}
static bool catalog_keyword(vstr text, const char *keyword) {
  if (text.len != strlen(keyword)) return false;
  for (size_t i = 0; i < text.len; ++i) {
    unsigned char c = (unsigned char)text.data[i];
    if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
    if (c != (unsigned char)keyword[i]) return false;
  }
  return true;
}
static turbodb_status_t catalog_type(catalog_binder *b, sqlparser_id id, orm_sql_type *out) {
  const sqlparser_node *node = sqlparser_get_node(b->document, id);
  if (!node || node->kind != SQLPARSER_TYPE)
    return catalog_fail(b, TURBODB_STATUS_UNSUPPORTED, "column requires an explicit type");
  b->offset = node->span.offset;
  if (node->as.type.arguments.count || node->as.type.zerofill)
    return catalog_fail(b, TURBODB_STATUS_UNSUPPORTED, "type arguments and ZEROFILL are unsupported");
  vstr name = {0};
  turbodb_status_t status = catalog_identifier(b, node->as.type.name, &name);
  if (status != TURBODB_STATUS_OK) return status;
  if (catalog_keyword(name, "BIGINT"))
    *out = (orm_sql_type){node->as.type.is_unsigned ? TURBODB_VALUE_UINT64 : TURBODB_VALUE_INT64, true};
  else if (catalog_keyword(name, "DOUBLE") && !node->as.type.is_unsigned)
    *out = (orm_sql_type){TURBODB_VALUE_DOUBLE, true};
  else return catalog_fail(b, TURBODB_STATUS_UNSUPPORTED, "supported types are BIGINT, BIGINT UNSIGNED and DOUBLE");
  return TURBODB_STATUS_OK;
}
static turbodb_status_t catalog_key(catalog_binder *b, size_t column) {
  if (b->definition.primary_key != SIZE_MAX)
    return catalog_fail(b, TURBODB_STATUS_SQL_ERROR, "multiple primary keys");
  b->definition.primary_key = column;
  return TURBODB_STATUS_OK;
}
static turbodb_status_t catalog_constraint(catalog_binder *b, const sqlparser_node *node, bool allow_name) {
  b->offset = node->span.offset;
  if (node->kind != SQLPARSER_CONSTRAINT || (!allow_name && node->as.constraint.name) ||
      (node->as.constraint.expression &&
        node->as.constraint.kind != SQLPARSER_DEFAULT) || node->as.constraint.table ||
      node->as.constraint.referenced_columns.count || node->as.constraint.key_terms.count ||
      node->as.constraint.declarations.count || node->as.constraint.descending ||
      node->as.constraint.autoincrement || node->as.constraint.conflict != SQLPARSER_CONFLICT_DEFAULT)
    return catalog_fail(b, TURBODB_STATUS_UNSUPPORTED, "unsupported constraint attributes");
  return catalog_charge(b, ORM_SQL_BUDGET_EXECUTION_STEPS, 1);
}
static turbodb_status_t catalog_default(catalog_binder *b,
    const orm_sql_schema_column *column, orm_sql_column_default *result,
    sqlparser_id id) {
  const sqlparser_node *node = sqlparser_get_node(b->document,id);
  if (!node) return catalog_fail(b,TURBODB_STATUS_INVALID_ARGUMENT,
      "missing column default expression");
  b->offset = node->span.offset;
  orm_sql_expr program = {0}; orm_sql_expr_run run = {0};
  turbodb_value_t value = turbodb_null();
  const size_t max_depth = sqlparser_node_count(b->document);
  turbodb_status_t status = orm_tidesdb_sql_expr_compile_value(b->document,id,
      NULL,0,max_depth,b->definition.budget,&program,b->error);
  if (b->validate_only) {
    /* A compiled default is not a resolved value and must never be published. */
    if (status == TURBODB_STATUS_OK && program.uses_session)
      status = catalog_fail(b,TURBODB_STATUS_UNSUPPORTED,"session-dependent defaults are unsupported");
    const turbodb_status_t released = orm_tidesdb_sql_expr_destroy(&program,
        status == TURBODB_STATUS_OK ? b->error : NULL);
    return released == TURBODB_STATUS_OK ? status : released;
  }
  if (status == TURBODB_STATUS_OK)
    status = orm_tidesdb_sql_expr_run_open_evaluation(&program,
        (orm_sql_evaluation){.mode=ORM_SQL_EVALUATION_WRITE},&run,b->error);
  if (status == TURBODB_STATUS_OK)
    status = orm_tidesdb_sql_expr_run_eval(&run,NULL,0,&value,b->error);
  turbodb_value_t converted = turbodb_null();
  orm_sql_assignment_adjustment adjustment = ORM_SQL_ASSIGNMENT_EXACT;
  if (status == TURBODB_STATUS_OK) {
    if (value.kind == TURBODB_VALUE_NULL && !column->type.nullable)
      status = catalog_fail(b,TURBODB_STATUS_SQL_ERROR,
          "NOT NULL column cannot default to NULL");
    else
      status = orm_tidesdb_sql_assignment_convert(column->type,&value,false,
          b->definition.budget,&converted,&adjustment,b->error);
  }
  turbodb_status_t released = orm_tidesdb_sql_expr_run_close(&run,
      status == TURBODB_STATUS_OK ? b->error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  released = orm_tidesdb_sql_expr_destroy(&program,
      status == TURBODB_STATUS_OK ? b->error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  if (status != TURBODB_STATUS_OK)
    return catalog_fail(b,status,"invalid or unsupported column default expression or assignment");
  result->value = converted;
  result->specified = true;
  return TURBODB_STATUS_OK;
}
static turbodb_status_t catalog_column(catalog_binder *b, const sqlparser_node *node, size_t ordinal) {
  vstr name = {0};
  turbodb_status_t status = catalog_identifier(b, node->as.column.name, &name);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < ordinal; ++i) {
    status = catalog_charge(b, ORM_SQL_BUDGET_EXECUTION_STEPS, 1);
    const orm_sql_schema_column *other = vec_at_const(&b->definition.columns, i);
    if (status == TURBODB_STATUS_OK && name.len == other->name.len && !memcmp(name.data, other->name.data, name.len))
      status = catalog_fail(b, TURBODB_STATUS_SQL_ERROR, "duplicate column name");
  }
  if (status != TURBODB_STATUS_OK) return status;
  orm_sql_schema_column *column = vec_at(&b->definition.columns, ordinal);
  orm_sql_column_default *result = vec_at(&b->definition.defaults,ordinal);
  column->name = catalog_copy_name(b, ordinal + 1, name);
  status = catalog_type(b, node->as.column.type, &column->type);
  bool nullability_seen = false;
  sqlparser_id default_value = 0;
  for (sqlparser_id id = node->as.column.constraints.first; status == TURBODB_STATUS_OK && id;) {
    const sqlparser_node *constraint = sqlparser_get_node(b->document, id);
    status = catalog_constraint(b, constraint, false);
    if (status != TURBODB_STATUS_OK) break;
    if (constraint->as.constraint.columns.count)
      status = catalog_fail(b, TURBODB_STATUS_UNSUPPORTED, "unsupported column constraint");
    else switch (constraint->as.constraint.kind) {
      case SQLPARSER_PRIMARY_KEY: status = catalog_key(b, ordinal); break;
      case SQLPARSER_UNIQUE:
        if (!b->secondary) status = catalog_fail(b, TURBODB_STATUS_UNSUPPORTED, "unsupported column constraint");
        break;
      case SQLPARSER_NOT_NULL:
      case SQLPARSER_NULLABLE:
        if (nullability_seen) status = catalog_fail(b, TURBODB_STATUS_SQL_ERROR, "repeated nullability constraint");
        else { nullability_seen = true; column->type.nullable = constraint->as.constraint.kind == SQLPARSER_NULLABLE; }
        break;
      case SQLPARSER_DEFAULT:
        if (default_value)
          status = catalog_fail(b,TURBODB_STATUS_SQL_ERROR,
              "repeated column default");
        else default_value = constraint->as.constraint.expression;
        break;
      default: status = catalog_fail(b, TURBODB_STATUS_UNSUPPORTED, "unsupported column constraint"); break;
    }
    id = constraint->next;
  }
  if (status == TURBODB_STATUS_OK && default_value)
    status = catalog_default(b,column,result,default_value);
  return status;
}
static turbodb_status_t catalog_table_key(catalog_binder *b, const sqlparser_node *node) {
  turbodb_status_t status = catalog_constraint(b, node, node->as.constraint.kind == SQLPARSER_PRIMARY_KEY);
  if (status != TURBODB_STATUS_OK) return status;
  if (node->as.constraint.kind != SQLPARSER_PRIMARY_KEY || node->as.constraint.columns.count != 1)
    return catalog_fail(b, TURBODB_STATUS_UNSUPPORTED, "expected a single-column primary key");
  if (node->as.constraint.name) {
    vstr constraint_name = {0};
    status = catalog_identifier(b, node->as.constraint.name, &constraint_name);
    if (status != TURBODB_STATUS_OK) return status;
  }
  vstr name = {0};
  status = catalog_identifier(b, node->as.constraint.columns.first, &name);
  if (status != TURBODB_STATUS_OK) return status;
  for (size_t i = 0; i < vec_size(&b->definition.columns); ++i) {
    status = catalog_charge(b, ORM_SQL_BUDGET_EXECUTION_STEPS, 1);
    if (status != TURBODB_STATUS_OK) return status;
    const orm_sql_schema_column *column = vec_at_const(&b->definition.columns, i);
    if (name.len == column->name.len && !memcmp(name.data, column->name.data, name.len)) return catalog_key(b, i);
  }
  return catalog_fail(b, TURBODB_STATUS_SQL_ERROR, "unknown primary key column");
}
static turbodb_status_t catalog_elements(catalog_binder *b, sqlparser_list elements) {
  size_t count = 0;
  turbodb_status_t status;
  for (sqlparser_id id = elements.first; id;) {
    status = catalog_charge(b, ORM_SQL_BUDGET_EXECUTION_STEPS, 1);
    if (status != TURBODB_STATUS_OK) return status;
    const sqlparser_node *node = sqlparser_get_node(b->document, id);
    if (node->kind == SQLPARSER_COLUMN) ++count;
    else if (node->kind != SQLPARSER_CONSTRAINT)
      return catalog_fail(b, TURBODB_STATUS_UNSUPPORTED, "unsupported table element");
    id = node->next;
  }
  if (!count) return catalog_fail(b, TURBODB_STATUS_UNSUPPORTED, "table requires declared columns");
  if (count == SIZE_MAX) return catalog_fail(b, TURBODB_STATUS_LIMIT_EXCEEDED, "column capacity overflow");
  status = catalog_charge(b, ORM_SQL_BUDGET_PLAN_NODES, count + 1);
  if (status == TURBODB_STATUS_OK) status = catalog_vector(b, &b->definition.names, count + 1,
      sizeof(catalog_name), _Alignof(catalog_name), &b->definition.name_bytes);
  if (status == TURBODB_STATUS_OK) status = catalog_vector(b, &b->definition.columns, count,
      sizeof(orm_sql_schema_column), _Alignof(orm_sql_schema_column), &b->definition.column_bytes);
  if (status == TURBODB_STATUS_OK) status = catalog_vector(b, &b->definition.defaults, count,
      sizeof(orm_sql_column_default), _Alignof(orm_sql_column_default),
      &b->definition.default_bytes);
  /* Resolve table keys only after every column, including forward references. */
  for (unsigned pass = 0; status == TURBODB_STATUS_OK && pass < 2; ++pass) {
    size_t ordinal = 0;
    for (sqlparser_id id = elements.first; status == TURBODB_STATUS_OK && id;) {
      const sqlparser_node *node = sqlparser_get_node(b->document, id);
      status = catalog_charge(b, ORM_SQL_BUDGET_EXECUTION_STEPS, 1);
      if (status == TURBODB_STATUS_OK && pass == 0 && node->kind == SQLPARSER_COLUMN)
        status = catalog_column(b, node, ordinal++);
      if (status == TURBODB_STATUS_OK && pass == 1 && node->kind == SQLPARSER_CONSTRAINT &&
          !(b->secondary && (node->as.constraint.kind == SQLPARSER_INDEX || node->as.constraint.kind == SQLPARSER_UNIQUE)))
        status = catalog_table_key(b, node);
      id = node->next;
    }
  }
  if (status != TURBODB_STATUS_OK) return status;
  if (b->definition.primary_key == SIZE_MAX)
    return catalog_fail(b, TURBODB_STATUS_UNSUPPORTED, "an explicit integer primary key is required");
  orm_sql_schema_column *key = vec_at(&b->definition.columns, b->definition.primary_key);
  if (key->type.kind != TURBODB_VALUE_INT64 && key->type.kind != TURBODB_VALUE_UINT64)
    return catalog_fail(b, TURBODB_STATUS_UNSUPPORTED, "primary key requires BIGINT or BIGINT UNSIGNED");
  key->type.nullable = false;
  return TURBODB_STATUS_OK;
}

static turbodb_status_t catalog_bind_create(const sqlparser_document *document,
    orm_tidesdb_sql_budget *budget, bool secondary, bool validate_only,
    orm_sql_table_definition *out, turbodb_error_t *error) {
  if (!document || !budget || !out || out->budget)
    return catalog_error(error, TURBODB_STATUS_INVALID_ARGUMENT, 0, "invalid definition binding arguments");
  if (sqlparser_get_dialect(document) != SQLPARSER_MYSQL || sqlparser_statements(document).count != 1)
    return catalog_error(error, TURBODB_STATUS_UNSUPPORTED, 0, "expected exactly one MySQL CREATE TABLE");
  const sqlparser_node *statement = sqlparser_get_node(document, sqlparser_statements(document).first);
  if (!statement || statement->kind != SQLPARSER_CREATE_TABLE || statement->as.create_table.temporary ||
      statement->as.create_table.options.count || statement->as.create_table.query || statement->as.create_table.like_table)
    return catalog_error(error, TURBODB_STATUS_UNSUPPORTED, statement ? statement->span.offset : 0,
        "unsupported CREATE TABLE statement or options");
  catalog_binder b = {.document = document, .definition = {.budget = budget, .primary_key = SIZE_MAX,
      .if_not_exists = statement->as.create_table.if_not_exists}, .error = error,
      .secondary = secondary, .validate_only = validate_only};
  turbodb_status_t status = catalog_charge(&b, ORM_SQL_BUDGET_AST_NODES, sqlparser_node_count(document));
  if (status == TURBODB_STATUS_OK)
    status = orm_tidesdb_sql_budget_reserve_capacity(budget, 1, sizeof(b), 0, &b.definition.metadata_bytes, error);
  vstr name = {0};
  if (status == TURBODB_STATUS_OK) status = catalog_identifier(&b, statement->as.create_table.table, &name);
  if (status == TURBODB_STATUS_OK) status = catalog_elements(&b, statement->as.create_table.elements);
  if (status == TURBODB_STATUS_OK) { (void)catalog_copy_name(&b, 0, name); *out = b.definition; return TURBODB_STATUS_OK; }
  const turbodb_status_t cleanup = orm_tidesdb_sql_catalog_destroy(&b.definition, NULL);
  return cleanup == TURBODB_STATUS_OK ? status : cleanup;
}
turbodb_status_t orm_tidesdb_sql_catalog_bind_create(const sqlparser_document *document,
    orm_tidesdb_sql_budget *budget, orm_sql_table_definition *out, turbodb_error_t *error) {
  return catalog_bind_create(document,budget,false,false,out,error);
}
turbodb_status_t orm_sql_catalog_destroy_all(orm_sql_create_definition *definition, turbodb_error_t *error) {
  if (!definition || !definition->table.budget) return TURBODB_STATUS_OK;
  turbodb_status_t status = TURBODB_STATUS_OK;
  for (size_t i=0; i<vec_size(&definition->indexes); ++i) {
    const turbodb_status_t released = orm_tidesdb_sql_index_destroy(vec_at(&definition->indexes,i),status==TURBODB_STATUS_OK?error:NULL);
    if (status==TURBODB_STATUS_OK) status=released;
  }
  const turbodb_status_t released = orm_sql_work_release(&definition->indexes,definition->index_bytes,
      definition->table.budget,status==TURBODB_STATUS_OK?error:NULL);
  if (status==TURBODB_STATUS_OK) status=released;
  const turbodb_status_t destroyed=orm_tidesdb_sql_catalog_destroy(&definition->table,status==TURBODB_STATUS_OK?error:NULL);
  if (status==TURBODB_STATUS_OK) status=destroyed;
  *definition=(orm_sql_create_definition){0}; return status;
}
static bool catalog_index_same(const orm_sql_index_definition *left, const orm_sql_index_definition *right) {
  return left->name_size==right->name_size && !memcmp(left->name,right->name,left->name_size);
}
/* Explicit names reserve the complete namespace regardless of source order;
 * generated names then claim the first free first-column[_N] name in SQL order. */
static turbodb_status_t catalog_index_name(orm_sql_create_definition *definition, size_t slot,
    size_t offset, turbodb_error_t *error) {
  orm_sql_index_definition *index=vec_at(&definition->indexes,slot);
  const size_t count=vec_size(&definition->indexes);
  char base[ORM_SQL_SELECT_NAME_BYTES+1];
  memcpy(base,index->name,index->name_size); const size_t base_size=index->name_size;
  for (size_t suffix=1; suffix<=count+2; ++suffix) {
    bool conflict=catalog_keyword((vstr){index->name,index->name_size},"PRIMARY");
    for (size_t i=0; !conflict && i<count; ++i) {
      if (i==slot) continue;
      const orm_sql_index_definition *other=vec_at_const(&definition->indexes,i);
      if (index->generated_name && other->generated_name && i>slot) continue;
      if (!index->generated_name && (other->generated_name || i>slot)) continue;
      orm_sql_budget_amount amount={0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=index->name_size;
      turbodb_status_t status=orm_tidesdb_sql_budget_reserve(definition->table.budget,&amount,error);
      if (status!=TURBODB_STATUS_OK) return status;
      conflict=catalog_index_same(index,other);
    }
    if (!conflict) return TURBODB_STATUS_OK;
    if (!index->generated_name)
      return catalog_error(error,TURBODB_STATUS_CONSTRAINT,offset,"duplicate table index name");
    char tail[32]; const int written=snprintf(tail,sizeof(tail),"_%zu",suffix+1);
    if (written<0 || (size_t)written>=sizeof(tail) || (size_t)written>ORM_SQL_SELECT_NAME_BYTES)
      return catalog_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,offset,"generated index name capacity exhausted");
    const size_t tail_size=(size_t)written;
    orm_sql_budget_amount amount={0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=tail_size+1;
    turbodb_status_t status=orm_tidesdb_sql_budget_reserve(definition->table.budget,&amount,error);
    if (status!=TURBODB_STATUS_OK) return status;
    const size_t prefix=base_size<ORM_SQL_SELECT_NAME_BYTES-tail_size?base_size:ORM_SQL_SELECT_NAME_BYTES-tail_size;
    memcpy(index->name,base,prefix); memcpy(index->name+prefix,tail,tail_size);
    index->name_size=prefix+tail_size; index->name[index->name_size]=0;
  }
  return catalog_error(error,TURBODB_STATUS_INTERNAL_ERROR,offset,"generated index name collision invariant failed");
}
static turbodb_status_t catalog_count_secondary(const sqlparser_document *document, const sqlparser_node *root,
    orm_tidesdb_sql_budget *budget, size_t *out, turbodb_error_t *error) {
  size_t count=0;
  for (sqlparser_id id=root->as.create_table.elements.first; id;) {
    orm_sql_budget_amount amount={0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=1;
    turbodb_status_t status=orm_tidesdb_sql_budget_reserve(budget,&amount,error);
    if (status!=TURBODB_STATUS_OK) return status;
    const sqlparser_node *node=sqlparser_get_node(document,id);
    if (node->kind==SQLPARSER_CONSTRAINT &&
        (node->as.constraint.kind==SQLPARSER_INDEX || node->as.constraint.kind==SQLPARSER_UNIQUE)) {
      if (count==SIZE_MAX) return catalog_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,node->span.offset,"index count overflow");
      ++count;
    } else if (node->kind==SQLPARSER_COLUMN) {
      for (sqlparser_id key_id=node->as.column.constraints.first; key_id;) {
        amount=(orm_sql_budget_amount){0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=1;
        status=orm_tidesdb_sql_budget_reserve(budget,&amount,error);
        if (status!=TURBODB_STATUS_OK) return status;
        const sqlparser_node *key=sqlparser_get_node(document,key_id);
        if (key->as.constraint.kind==SQLPARSER_UNIQUE) {
          if (count==SIZE_MAX) return catalog_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,key->span.offset,"index count overflow");
          ++count;
        }
        key_id=key->next;
      }
    }
    id=node->next;
  }
  *out=count; return TURBODB_STATUS_OK;
}
static turbodb_status_t catalog_bind_all(const sqlparser_document *document, orm_tidesdb_sql_budget *budget,
    bool validate_only, orm_sql_create_definition *out, turbodb_error_t *error) {
  if (!out || out->table.budget || out->indexes.initialized)
    return catalog_error(error,TURBODB_STATUS_INVALID_ARGUMENT,0,"empty complete CREATE definition required");
  orm_sql_create_definition definition={0};
  turbodb_status_t status=catalog_bind_create(document,budget,true,validate_only,&definition.table,error);
  if (status!=TURBODB_STATUS_OK) return status;
  const sqlparser_node *root=sqlparser_get_node(document,sqlparser_statements(document).first);
  size_t count=0;
  if (status==TURBODB_STATUS_OK) status=catalog_count_secondary(document,root,budget,&count,error);
  if (status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&definition.indexes,count,sizeof(orm_sql_index_definition),_Alignof(orm_sql_index_definition),
      budget,&definition.index_bytes,error);
  orm_sql_table_schema schema={0};
  if (status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_catalog_schema(&definition.table,&schema,error);
  size_t slot=0;
  for (sqlparser_id id=root->as.create_table.elements.first; status==TURBODB_STATUS_OK && id;) {
    orm_sql_budget_amount charge={0}; charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=1;
    status=orm_tidesdb_sql_budget_reserve(budget,&charge,error);
    if (status!=TURBODB_STATUS_OK) break;
    const sqlparser_node *node=sqlparser_get_node(document,id);
    if (node->kind==SQLPARSER_CONSTRAINT &&
        (node->as.constraint.kind==SQLPARSER_INDEX || node->as.constraint.kind==SQLPARSER_UNIQUE)) {
      orm_sql_index_definition *index=vec_at(&definition.indexes,slot);
      status=orm_sql_index_bind_table_key(document,node,&schema,budget,index,error);
      ++slot;
    } else if (node->kind==SQLPARSER_COLUMN) {
      for (sqlparser_id key_id=node->as.column.constraints.first; status==TURBODB_STATUS_OK && key_id;) {
        orm_sql_budget_amount amount={0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=1;
        status=orm_tidesdb_sql_budget_reserve(budget,&amount,error);
        if (status!=TURBODB_STATUS_OK) break;
        const sqlparser_node *key=sqlparser_get_node(document,key_id);
        if (key->as.constraint.kind==SQLPARSER_UNIQUE) {
          orm_sql_index_definition *index=vec_at(&definition.indexes,slot);
          status=orm_sql_index_bind_column_key(document,key,node->as.column.name,&schema,budget,index,error);
          ++slot;
        }
        key_id=key->next;
      }
    }
    id=node->next;
  }
  if (status==TURBODB_STATUS_OK && slot!=count)
    status=catalog_error(error,TURBODB_STATUS_INTERNAL_ERROR,root->span.offset,"bound index count invariant failed");
  for (size_t i=0; status==TURBODB_STATUS_OK && i<count; ++i) {
    const orm_sql_index_definition *index=vec_at_const(&definition.indexes,i);
    status=catalog_index_name(&definition,i,index->source_offset,error);
  }
  if (status==TURBODB_STATUS_OK) { *out=definition; return status; }
  const turbodb_status_t released=orm_sql_catalog_destroy_all(&definition,NULL);
  return released==TURBODB_STATUS_OK?status:released;
}
turbodb_status_t orm_sql_catalog_bind_create_all(const sqlparser_document *document,
    orm_tidesdb_sql_budget *budget, orm_sql_create_definition *out, turbodb_error_t *error) {
  return catalog_bind_all(document,budget,false,out,error);
}
turbodb_status_t orm_sql_catalog_validate_create(const sqlparser_document *document,
    orm_tidesdb_sql_budget *budget, turbodb_error_t *error) {
  orm_sql_create_definition definition={0};
  const turbodb_status_t status=catalog_bind_all(document,budget,true,&definition,error);
  const turbodb_status_t released=orm_sql_catalog_destroy_all(&definition,status==TURBODB_STATUS_OK?error:NULL);
  return released==TURBODB_STATUS_OK?status:released;
}
turbodb_status_t orm_tidesdb_sql_catalog_schema(const orm_sql_table_definition *definition,
    orm_sql_table_schema *out, turbodb_error_t *error) {
  if (!definition || !definition->budget || !out)
    return catalog_error(error, TURBODB_STATUS_INVALID_ARGUMENT, 0, "invalid schema view arguments");
  const catalog_name *name = vec_at_const(&definition->names, 0);
  *out = (orm_sql_table_schema){
      {name->bytes, name->size}, vec_data_const(&definition->columns),
      vec_size(&definition->columns), vec_data_const(&definition->defaults)};
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_sql_catalog_rename(orm_sql_table_definition *definition, size_t slot, vstr name, turbodb_error_t *error) {
  if (!definition || !definition->budget || slot >= vec_size(&definition->names))
    return catalog_error(error, TURBODB_STATUS_INVALID_ARGUMENT, 0, "invalid rename definition or slot");
  const char *reason = NULL;
  turbodb_status_t status = orm_sql_name_validate(name, &reason);
  if (status != TURBODB_STATUS_OK) return catalog_error(error, status, 0, reason);
  catalog_binder b = {.definition = {.budget = definition->budget}, .error = error};
  status = catalog_charge(&b, ORM_SQL_BUDGET_EXECUTION_STEPS, vec_size(&definition->columns) + name.len);
  if (status != TURBODB_STATUS_OK) return status;
  for (size_t i = 0; slot && i < vec_size(&definition->columns); ++i) {
    const orm_sql_schema_column *column = vec_at_const(&definition->columns, i);
    if (i + 1 != slot && column->name.len == name.len && !memcmp(column->name.data, name.data, name.len))
      return catalog_error(error, TURBODB_STATUS_SQL_ERROR, 0, "rename destination column already exists");
  }
  catalog_name *target = vec_at(&definition->names, slot);
  memcpy(target->bytes, name.data, name.len); target->bytes[name.len] = 0; target->size = name.len;
  const vstr copied = {target->bytes, target->size};
  if (slot) ((orm_sql_schema_column *)vec_at(&definition->columns, slot - 1))->name = copied;
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_sql_catalog_default(orm_sql_table_definition *definition, const sqlparser_document *document,
    size_t ordinal, sqlparser_id expression, turbodb_error_t *error) {
  if (!definition || !definition->budget || !document || ordinal >= vec_size(&definition->columns) ||
      vec_size(&definition->defaults) != vec_size(&definition->columns)) {
    tdsql_error_set(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid ALTER default definition or ordinal");
    return TURBODB_STATUS_INVALID_ARGUMENT;
  }
  if (sqlparser_get_dialect(document) != SQLPARSER_MYSQL) {
    tdsql_error_set(error,TURBODB_STATUS_UNSUPPORTED,"ALTER default requires MySQL dialect");
    return TURBODB_STATUS_UNSUPPORTED;
  }
  catalog_binder binder = {.document=document,.definition={.budget=definition->budget},.error=error,.alter=true};
  orm_sql_column_default replacement = {0};
  turbodb_status_t status = catalog_charge(&binder,ORM_SQL_BUDGET_EXECUTION_STEPS,1);
  if (status == TURBODB_STATUS_OK && expression)
    status = catalog_default(&binder,vec_at_const(&definition->columns,ordinal),&replacement,expression);
  if (status == TURBODB_STATUS_OK) *(orm_sql_column_default *)vec_at(&definition->defaults,ordinal) = replacement;
  return status;
}
static turbodb_status_t catalog_columns(const orm_sql_table_definition *source, const sqlparser_document *document,
    sqlparser_id added, size_t ordinal, bool validate_only, orm_sql_table_definition *out, turbodb_error_t *error) {
  if (!source || !source->budget || !document || !out || out->budget)
    return catalog_error(error, TURBODB_STATUS_INVALID_ARGUMENT, 0, "invalid column alteration arguments");
  const size_t old_count = vec_size(&source->columns);
  if (!added && (ordinal >= old_count || ordinal == source->primary_key))
    return catalog_error(error, TURBODB_STATUS_UNSUPPORTED, 0, "cannot remove primary or absent column");
  if (added && old_count >= UINT32_MAX)
    return catalog_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, 0, "column count exhausted");
  if (added && ordinal > old_count)
    return catalog_error(error, TURBODB_STATUS_INVALID_ARGUMENT, 0, "ADD position exceeds column count");
  const sqlparser_node *node = added ? sqlparser_get_node(document, added) : NULL;
  if (added && (!node || node->kind != SQLPARSER_COLUMN))
    return catalog_error(error, TURBODB_STATUS_INVALID_ARGUMENT, 0, "ADD requires a column definition");
  const size_t count = added ? old_count + 1 : old_count - 1;
  if (count == SIZE_MAX) return catalog_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, 0, "column capacity overflow");
  catalog_binder b = {.document = document, .definition = {.budget = source->budget,
      .primary_key = source->primary_key - (!added && ordinal < source->primary_key ? 1 : 0)},
      .error = error,.alter=true,.validate_only=validate_only};
  turbodb_status_t status = catalog_charge(&b, ORM_SQL_BUDGET_PLAN_NODES, count + 1);
  if (status == TURBODB_STATUS_OK) status = catalog_charge(&b, ORM_SQL_BUDGET_EXECUTION_STEPS, old_count);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_budget_reserve_capacity(source->budget, 1, sizeof(b), 0,
      &b.definition.metadata_bytes, error);
  if (status == TURBODB_STATUS_OK) status = catalog_vector(&b, &b.definition.names, count + 1,
      sizeof(catalog_name), _Alignof(catalog_name), &b.definition.name_bytes);
  if (status == TURBODB_STATUS_OK) status = catalog_vector(&b, &b.definition.columns, count,
      sizeof(orm_sql_schema_column), _Alignof(orm_sql_schema_column), &b.definition.column_bytes);
  if (status == TURBODB_STATUS_OK) status = catalog_vector(&b, &b.definition.defaults, count,
      sizeof(orm_sql_column_default), _Alignof(orm_sql_column_default),
      &b.definition.default_bytes);
  if (status == TURBODB_STATUS_OK) {
    const catalog_name *name = vec_at_const(&source->names, 0);
    (void)catalog_copy_name(&b, 0, (vstr){name->bytes, name->size});
    for (size_t i = 0, position = 0; i < old_count; ++i) {
      if (!added && i == ordinal) continue;
      const orm_sql_schema_column *old = vec_at_const(&source->columns, i);
      orm_sql_schema_column *column = vec_at(&b.definition.columns, position);
      orm_sql_column_default *result = vec_at(&b.definition.defaults,position);
      const orm_sql_column_default *old_default = vec_at_const(&source->defaults,i);
      column->name = catalog_copy_name(&b, position + 1, old->name);
      column->type = old->type;
      *result = *old_default;
      ++position;
    }
    if (added) status = catalog_column(&b, node, old_count);
    if (status == TURBODB_STATUS_OK && added && ordinal < old_count) {
      status = catalog_charge(&b, ORM_SQL_BUDGET_EXECUTION_STEPS, count);
      if (status == TURBODB_STATUS_OK) {
        catalog_name *names = vec_data(&b.definition.names);
        orm_sql_schema_column *columns = vec_data(&b.definition.columns);
        orm_sql_column_default *defaults = vec_data(&b.definition.defaults);
        const catalog_name last_name = names[count];
        const orm_sql_schema_column last_column = columns[old_count];
        const orm_sql_column_default last_default = defaults[old_count];
        memmove(names + ordinal + 2, names + ordinal + 1, (old_count - ordinal) * sizeof(*names));
        memmove(columns + ordinal + 1, columns + ordinal, (old_count - ordinal) * sizeof(*columns));
        memmove(defaults + ordinal + 1, defaults + ordinal,
            (old_count - ordinal) * sizeof(*defaults));
        names[ordinal + 1] = last_name; columns[ordinal] = last_column;
        defaults[ordinal] = last_default;
        for (size_t i = ordinal; i < count; ++i)
          columns[i].name = (vstr){names[i + 1].bytes, names[i + 1].size};
        if (b.definition.primary_key >= ordinal) ++b.definition.primary_key;
      }
    }
  }
  if (status == TURBODB_STATUS_OK) { *out = b.definition; return status; }
  const turbodb_status_t closed = orm_tidesdb_sql_catalog_destroy(&b.definition, NULL);
  return closed == TURBODB_STATUS_OK ? status : closed;
}
turbodb_status_t orm_sql_catalog_columns(const orm_sql_table_definition *source, const sqlparser_document *document,
    sqlparser_id added, size_t ordinal, orm_sql_table_definition *out, turbodb_error_t *error) {
  return catalog_columns(source,document,added,ordinal,false,out,error);
}
turbodb_status_t orm_sql_catalog_validate_columns(const orm_sql_table_definition *source,
    const sqlparser_document *document, sqlparser_id added, size_t ordinal, turbodb_error_t *error) {
  orm_sql_table_definition temporary={0};
  const turbodb_status_t status=catalog_columns(source,document,added,ordinal,true,&temporary,error);
  const turbodb_status_t released=orm_tidesdb_sql_catalog_destroy(&temporary,status==TURBODB_STATUS_OK?error:NULL);
  return released==TURBODB_STATUS_OK?status:released;
}
turbodb_status_t orm_sql_catalog_validate_default(const orm_sql_table_definition *source,
    const sqlparser_document *document, size_t ordinal, sqlparser_id expression, turbodb_error_t *error) {
  if (!source || !source->budget || !document || ordinal>=vec_size(&source->columns))
    return catalog_error(error,TURBODB_STATUS_INVALID_ARGUMENT,0,"invalid default validation inputs");
  if (!expression) return TURBODB_STATUS_OK;
  catalog_binder b={.document=document,.definition={.budget=source->budget},
      .error=error,.alter=true,.validate_only=true};
  return catalog_default(&b,vec_at_const(&source->columns,ordinal),NULL,expression);
}
turbodb_status_t orm_tidesdb_sql_catalog_destroy(orm_sql_table_definition *definition, turbodb_error_t *error) {
  if (!definition || !definition->budget) return TURBODB_STATUS_OK;
  turbodb_status_t status = TURBODB_STATUS_OK;
  vec_t *vectors[] = {&definition->names, &definition->columns,
      &definition->defaults};
  const size_t bytes[] = {definition->name_bytes, definition->column_bytes,
      definition->default_bytes};
  for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
    const turbodb_status_t released = orm_sql_work_release(vectors[i], bytes[i], definition->budget,
        status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  if (definition->metadata_bytes) {
    const turbodb_status_t released = orm_tidesdb_sql_budget_release(definition->budget,
        ORM_SQL_BUDGET_WORK_BYTES, definition->metadata_bytes, status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  *definition = (orm_sql_table_definition){0}; return status;
}

enum { CATALOG_WIRE_HEADER = 12, CATALOG_WIRE_COUNT = 4, CATALOG_WIRE_KEY = 8,
       CATALOG_WIRE_VERSION = 2, CATALOG_WIRE_FLAGS = 3, CATALOG_WIRE_COLUMN = 3,
       CATALOG_WIRE_DEFAULT = 9, CATALOG_WIRE_CURRENT = 2,
       CATALOG_WIRE_I64 = 1, CATALOG_WIRE_U64 = 2, CATALOG_WIRE_F64 = 3,
       CATALOG_DEFAULT_NONE = 0, CATALOG_DEFAULT_NULL = 1,
       CATALOG_DEFAULT_VALUE = 2 };
static const uint8_t catalog_wire_magic[] = {'S', 'C', 1, 0};

turbodb_status_t orm_tidesdb_sql_catalog_encode(const orm_sql_table_definition *definition,
    size_t max_bytes, vec_t *out, size_t *reserved, turbodb_error_t *error) {
  if (!definition || !definition->budget || !max_bytes || !out || out->initialized || !reserved || *reserved)
    return catalog_error(error, TURBODB_STATUS_INVALID_ARGUMENT, 0, "invalid schema encoding arguments");
  orm_sql_table_schema schema = {0};
  turbodb_status_t status = orm_tidesdb_sql_catalog_schema(definition, &schema, error);
  if (status != TURBODB_STATUS_OK) return status;
  size_t size = CATALOG_WIRE_HEADER + 1 + schema.name.len;
  if (schema.count > UINT32_MAX || size > max_bytes)
    return catalog_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, 0, "schema wire capacity exceeded");
  bool defaults = false;
  for (size_t i = 0; i < schema.count; ++i) {
    const orm_sql_schema_column *column = &schema.columns[i];
    const orm_sql_column_default *column_default = &schema.defaults[i];
    if (!column_default->specified) continue;
    defaults = true;
    if (column_default->value.reserved ||
        (column_default->value.kind == TURBODB_VALUE_NULL && !column->type.nullable) ||
        (column_default->value.kind != TURBODB_VALUE_NULL &&
         column_default->value.kind != column->type.kind) ||
        (column_default->value.kind == TURBODB_VALUE_DOUBLE &&
         !isfinite(column_default->value.data.double_value)))
      return catalog_error(error,TURBODB_STATUS_TYPE_ERROR,0,
          "invalid schema column default");
  }
  for (size_t i = 0; i < schema.count; ++i) {
    const turbodb_value_kind_t kind = schema.columns[i].type.kind;
    if (kind != TURBODB_VALUE_INT64 && kind != TURBODB_VALUE_UINT64 && kind != TURBODB_VALUE_DOUBLE)
      return catalog_error(error, TURBODB_STATUS_UNSUPPORTED, 0, "column type has no schema wire representation");
    const size_t fixed = CATALOG_WIRE_COLUMN +
        (defaults ? CATALOG_WIRE_DEFAULT : 0);
    const size_t extra = fixed + schema.columns[i].name.len;
    if (extra > max_bytes - size)
      return catalog_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, 0, "schema wire capacity exceeded");
    size += extra;
  }
  catalog_binder b = {.definition = {.budget = definition->budget}, .error = error};
  status = catalog_charge(&b, ORM_SQL_BUDGET_EXECUTION_STEPS, size);
  vec_t bytes = {0}; size_t work = 0;
  if (status == TURBODB_STATUS_OK) status = catalog_vector(&b, &bytes, size, sizeof(uint8_t), _Alignof(uint8_t), &work);
  if (status != TURBODB_STATUS_OK) {
    const turbodb_status_t released = orm_sql_work_release(&bytes, work, definition->budget, NULL);
    return released == TURBODB_STATUS_OK ? status : released;
  }
  uint8_t *data = vec_data(&bytes);
  memcpy(data, catalog_wire_magic, sizeof(catalog_wire_magic));
  data[CATALOG_WIRE_VERSION] = defaults ? CATALOG_WIRE_CURRENT :
      catalog_wire_magic[CATALOG_WIRE_VERSION];
  orm_sql_wire_write(data + CATALOG_WIRE_COUNT, ORM_SQL_WIRE_U32, schema.count);
  orm_sql_wire_write(data + CATALOG_WIRE_KEY, ORM_SQL_WIRE_U32, definition->primary_key);
  size_t offset = CATALOG_WIRE_HEADER;
  data[offset++] = (uint8_t)schema.name.len;
  memcpy(data + offset, schema.name.data, schema.name.len); offset += schema.name.len;
  for (size_t i = 0; i < schema.count; ++i) {
    const orm_sql_schema_column *column = &schema.columns[i];
    const orm_sql_column_default *column_default = &schema.defaults[i];
    data[offset++] = (uint8_t)column->name.len;
    data[offset++] = column->type.kind == TURBODB_VALUE_INT64 ? CATALOG_WIRE_I64 :
        column->type.kind == TURBODB_VALUE_UINT64 ? CATALOG_WIRE_U64 : CATALOG_WIRE_F64;
    data[offset++] = column->type.nullable ? 1 : 0;
    memcpy(data + offset, column->name.data, column->name.len); offset += column->name.len;
    if (defaults) {
      uint64_t bits = 0;
      data[offset++] = !column_default->specified ? CATALOG_DEFAULT_NONE :
          column_default->value.kind == TURBODB_VALUE_NULL ? CATALOG_DEFAULT_NULL :
          CATALOG_DEFAULT_VALUE;
      if (column_default->specified) switch (column_default->value.kind) {
        case TURBODB_VALUE_INT64:
          bits = (uint64_t)column_default->value.data.int64_value;
          break;
        case TURBODB_VALUE_UINT64:
          bits = column_default->value.data.uint64_value;
          break;
        case TURBODB_VALUE_DOUBLE:
          memcpy(&bits,&column_default->value.data.double_value,sizeof(bits));
          break;
        default: break;
      }
      orm_sql_wire_write(data + offset,ORM_SQL_WIRE_U64,bits);
      offset += ORM_SQL_WIRE_U64;
    }
  }
  *out = bytes; *reserved = work; return TURBODB_STATUS_OK;
}
static turbodb_status_t catalog_wire_name(catalog_binder *b, const uint8_t *data,
    size_t size, size_t *offset, size_t length, vstr *out) {
  if (length > size - *offset)
    return catalog_fail(b, TURBODB_STATUS_DATASTORE_ERROR, "truncated schema name");
  *out = (vstr){(const char *)data + *offset, length};
  const char *reason = NULL;
  if (orm_sql_name_validate(*out, &reason) != TURBODB_STATUS_OK)
    return catalog_fail(b, TURBODB_STATUS_DATASTORE_ERROR, "invalid persisted schema name");
  *offset += length; return TURBODB_STATUS_OK;
}
turbodb_status_t orm_tidesdb_sql_catalog_decode(const uint8_t *data, size_t size, size_t max_bytes,
    orm_tidesdb_sql_budget *budget, orm_sql_table_definition *out, turbodb_error_t *error) {
  if (!data || !max_bytes || !budget || !out || out->budget)
    return catalog_error(error, TURBODB_STATUS_INVALID_ARGUMENT, 0, "invalid schema decoding arguments");
  if (size > max_bytes) return catalog_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, 0, "schema wire capacity exceeded");
  if (size <= CATALOG_WIRE_HEADER || memcmp(data, catalog_wire_magic, CATALOG_WIRE_VERSION))
    return catalog_error(error, TURBODB_STATUS_DATASTORE_ERROR, 0, "truncated or invalid schema header");
  const uint8_t version = data[CATALOG_WIRE_VERSION];
  if (version != catalog_wire_magic[CATALOG_WIRE_VERSION] &&
      version != CATALOG_WIRE_CURRENT)
    return catalog_error(error, TURBODB_STATUS_UNSUPPORTED, 0, "unknown schema wire version");
  if (data[CATALOG_WIRE_FLAGS]) return catalog_error(error, TURBODB_STATUS_DATASTORE_ERROR, 0, "invalid schema flags");
  const size_t count = (size_t)orm_sql_wire_read(data + CATALOG_WIRE_COUNT, ORM_SQL_WIRE_U32);
  const size_t key = (size_t)orm_sql_wire_read(data + CATALOG_WIRE_KEY, ORM_SQL_WIRE_U32);
  const size_t column_fixed = CATALOG_WIRE_COLUMN +
      (version == CATALOG_WIRE_CURRENT ? CATALOG_WIRE_DEFAULT : 0);
  if (!count || key >= count ||
      count > (size - CATALOG_WIRE_HEADER) / (column_fixed + 1))
    return catalog_error(error, TURBODB_STATUS_DATASTORE_ERROR, 0, "invalid schema column count or primary key");
  catalog_binder b = {.definition = {.budget = budget, .primary_key = key}, .error = error};
  turbodb_status_t status = catalog_charge(&b, ORM_SQL_BUDGET_EXECUTION_STEPS, size);
  if (status == TURBODB_STATUS_OK) status = catalog_charge(&b, ORM_SQL_BUDGET_PLAN_NODES, count + 1);
  if (status == TURBODB_STATUS_OK)
    status = orm_tidesdb_sql_budget_reserve_capacity(budget, 1, sizeof(b), 0, &b.definition.metadata_bytes, error);
  if (status == TURBODB_STATUS_OK) status = catalog_vector(&b, &b.definition.names, count + 1,
      sizeof(catalog_name), _Alignof(catalog_name), &b.definition.name_bytes);
  if (status == TURBODB_STATUS_OK) status = catalog_vector(&b, &b.definition.columns, count,
      sizeof(orm_sql_schema_column), _Alignof(orm_sql_schema_column), &b.definition.column_bytes);
  if (status == TURBODB_STATUS_OK) status = catalog_vector(&b, &b.definition.defaults, count,
      sizeof(orm_sql_column_default), _Alignof(orm_sql_column_default),
      &b.definition.default_bytes);
  size_t offset = CATALOG_WIRE_HEADER; const size_t table_length = data[offset++]; vstr name = {0};
  if (status == TURBODB_STATUS_OK) status = catalog_wire_name(&b, data, size, &offset, table_length, &name);
  if (status == TURBODB_STATUS_OK) (void)catalog_copy_name(&b, 0, name);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < count; ++i) {
    if (size - offset < CATALOG_WIRE_COLUMN) { status = catalog_fail(&b, TURBODB_STATUS_DATASTORE_ERROR, "truncated schema column"); break; }
    const size_t length = data[offset++]; const uint8_t kind = data[offset++], nullable = data[offset++];
    if (kind < CATALOG_WIRE_I64 || kind > CATALOG_WIRE_F64 || nullable > 1 ||
        (i == key && (nullable || kind == CATALOG_WIRE_F64))) {
      status = catalog_fail(&b, TURBODB_STATUS_DATASTORE_ERROR, "invalid persisted column type or primary key"); break;
    }
    status = catalog_wire_name(&b, data, size, &offset, length, &name);
    for (size_t j = 0; status == TURBODB_STATUS_OK && j < i; ++j) {
      status = catalog_charge(&b, ORM_SQL_BUDGET_EXECUTION_STEPS, 1);
      const orm_sql_schema_column *other = vec_at_const(&b.definition.columns, j);
      if (status == TURBODB_STATUS_OK && name.len == other->name.len && !memcmp(name.data, other->name.data, name.len))
        status = catalog_fail(&b, TURBODB_STATUS_DATASTORE_ERROR, "duplicate persisted column");
    }
    if (status != TURBODB_STATUS_OK) break;
    orm_sql_schema_column *column = vec_at(&b.definition.columns, i);
    orm_sql_column_default *column_default = vec_at(&b.definition.defaults,i);
    column->name = catalog_copy_name(&b, i + 1, name);
    column->type = (orm_sql_type){kind == CATALOG_WIRE_I64 ? TURBODB_VALUE_INT64 :
        kind == CATALOG_WIRE_U64 ? TURBODB_VALUE_UINT64 : TURBODB_VALUE_DOUBLE, nullable != 0};
    if (version == CATALOG_WIRE_CURRENT) {
      if (size - offset < CATALOG_WIRE_DEFAULT) {
        status = catalog_fail(&b,TURBODB_STATUS_DATASTORE_ERROR,
            "truncated schema column default");
        break;
      }
      const uint8_t tag = data[offset++];
      const uint64_t bits = orm_sql_wire_read(data + offset,ORM_SQL_WIRE_U64);
      offset += ORM_SQL_WIRE_U64;
      if (tag > CATALOG_DEFAULT_VALUE ||
          (tag != CATALOG_DEFAULT_VALUE && bits) ||
          (tag == CATALOG_DEFAULT_NULL && !column->type.nullable)) {
        status = catalog_fail(&b,TURBODB_STATUS_DATASTORE_ERROR,
            "invalid persisted column default");
        break;
      }
      if (tag != CATALOG_DEFAULT_NONE) {
        column_default->specified = true;
        if (tag == CATALOG_DEFAULT_NULL) column_default->value = turbodb_null();
        else if (column->type.kind == TURBODB_VALUE_INT64) {
          const uint64_t sign = UINT64_C(1) << 63;
          const int64_t value = bits & sign ?
              INT64_MIN + (int64_t)(bits ^ sign) : (int64_t)bits;
          column_default->value = turbodb_i64(value);
        }
        else if (column->type.kind == TURBODB_VALUE_UINT64)
          column_default->value = turbodb_u64(bits);
        else {
          double value = 0.0;
          memcpy(&value,&bits,sizeof(value));
          if (!isfinite(value)) {
            status = catalog_fail(&b,TURBODB_STATUS_DATASTORE_ERROR,
                "nonfinite persisted column default");
            break;
          }
          column_default->value = turbodb_f64(value);
        }
      }
    }
  }
  if (status == TURBODB_STATUS_OK && offset != size) status = catalog_fail(&b, TURBODB_STATUS_DATASTORE_ERROR, "trailing schema bytes");
  if (status == TURBODB_STATUS_OK) { *out = b.definition; return TURBODB_STATUS_OK; }
  const turbodb_status_t released = orm_tidesdb_sql_catalog_destroy(&b.definition, NULL);
  return released == TURBODB_STATUS_OK ? status : released;
}

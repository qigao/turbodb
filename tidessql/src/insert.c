#include "insert.h"
#include "relation.h"
#include "expr.h"
#include "binding.h"
#include "name.h"
#include "runtime.h"
#include "rows.h"
#include "store_internal.h"
#include "work.h"
#include "error.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct insert_assignment {
  orm_sql_expr program;
  orm_sql_expr_run run;
  vec_t slots, arguments;
  size_t slot_bytes, argument_bytes, column;
  bool default_value;
} insert_assignment;
typedef struct insert_context {
  const sqlparser_document *document;
  orm_sql_catalog_store *store;
  const turbodb_value_t *parameters;
  const orm_sql_type *binding_types;
  size_t parameter_count, max_depth, offset, target_count;
  uint64_t max_iterations;
  bool client_found_rows;
  vec_t columns, offsets, inputs, arguments, values, parameter_types;
  vec_t augmented_columns, substitutions, assignments, existing, current;
  size_t column_bytes, offset_bytes, input_bytes, argument_bytes, value_bytes;
  size_t parameter_type_bytes, augmented_column_bytes, substitution_bytes;
  size_t assignment_bytes, existing_bytes, current_bytes, row_slots;
  size_t candidate_row;
  orm_sql_table_definition definition;
  orm_sql_table_schema schema;
  vstr table;
  orm_sql_rows query_rows;
  orm_sql_diagnostics *diagnostics;
  bool permissive, binding_only;
  orm_sql_evaluation evaluation;
  turbodb_error_t *error;
} insert_context;

static turbodb_status_t insert_error(insert_context *c, turbodb_status_t status, const char *reason) {
  char message[TURBODB_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message), "TidesDB SQL INSERT at byte %zu: %s", c->offset, reason);
  tdsql_error_set(c->error, status, message); return status;
}
static turbodb_status_t insert_charge(insert_context *c, orm_sql_budget_resource resource, size_t count) {
  orm_sql_budget_amount amount = {0}; amount.value[resource] = count;
  return orm_tidesdb_sql_budget_reserve(c->store->budget, &amount, c->error);
}
static turbodb_status_t insert_vector(insert_context *c, vec_t *v, size_t count,
    size_t width, size_t align, size_t *bytes) {
  return orm_sql_work_zero(v, count, width, align, c->store->budget, bytes, c->error);
}
static turbodb_status_t insert_name(insert_context *c, sqlparser_id id, vstr *out) {
  const sqlparser_node *node = sqlparser_get_node(c->document, id);
  if (node) c->offset = node->span.offset;
  const char *reason = NULL;
  const turbodb_status_t status = orm_sql_name_node(c->document, id, out, &reason);
  return status == TURBODB_STATUS_OK ? status : insert_error(c, status, reason);
}
static bool insert_name_equal(vstr left, vstr right) {
  return left.len == right.len && (!left.len || !memcmp(left.data,right.data,left.len));
}
static turbodb_status_t insert_reference(insert_context *c, sqlparser_id id,
    vstr *name, vstr *qualifier) {
  const sqlparser_node *node=sqlparser_get_node(c->document,id);
  if (!node || node->kind != SQLPARSER_NAME)
    return insert_error(c,TURBODB_STATUS_UNSUPPORTED,"expected a column name");
  c->offset=node->span.offset;
  vstr text={sqlparser_text(c->document,node->span),node->span.length};
  const char *reason=NULL;
  turbodb_status_t status=orm_sql_name_part(&text,name,&reason);
  *qualifier=(vstr){0};
  if (status == TURBODB_STATUS_OK && text.len && text.data[0] == '.') {
    *qualifier=*name; ++text.data; --text.len;
    status=orm_sql_name_part(&text,name,&reason);
  }
  if (status != TURBODB_STATUS_OK) return insert_error(c,status,reason);
  return text.len ? insert_error(c,TURBODB_STATUS_UNSUPPORTED,
      "unsupported identifier qualification or quoting") : TURBODB_STATUS_OK;
}
static turbodb_status_t insert_validate(insert_context *c, orm_sql_type type, const turbodb_value_t *value) {
  orm_sql_predicate validator; turbodb_value_t ignored;
  turbodb_status_t status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL, type, NULL, &validator, c->error);
  return status == TURBODB_STATUS_OK ? orm_tidesdb_sql_predicate_eval(&validator, value, NULL,
      c->store->budget, &ignored, c->error) : status;
}
static turbodb_status_t insert_warning(insert_context *c, size_t column,
    orm_sql_assignment_adjustment adjustment) {
  if (adjustment==ORM_SQL_ASSIGNMENT_EXACT) return TURBODB_STATUS_OK;
  const vstr name=c->schema.columns[column].name;
  const int width=name.len>(size_t)INT_MAX?INT_MAX:(int)name.len;
  const uint32_t code=adjustment==ORM_SQL_ASSIGNMENT_OUT_OF_RANGE?1264u:
      adjustment==ORM_SQL_ASSIGNMENT_NULL_TO_NOT_NULL?1048u:
      adjustment==ORM_SQL_ASSIGNMENT_INVALID?1366u:1265u;
  char message[ORM_SQL_DIAGNOSTIC_MESSAGE_CAPACITY];
  if (adjustment==ORM_SQL_ASSIGNMENT_OUT_OF_RANGE)
    (void)snprintf(message,sizeof(message),
        "Out of range value for column '%.*s' at row %zu",width,name.data,
        c->candidate_row);
  else if (adjustment==ORM_SQL_ASSIGNMENT_NULL_TO_NOT_NULL)
    (void)snprintf(message,sizeof(message),
        "Column '%.*s' cannot be null at row %zu",width,name.data,
        c->candidate_row);
  else if (adjustment==ORM_SQL_ASSIGNMENT_INVALID)
    (void)snprintf(message,sizeof(message),
        "Incorrect numeric value for column '%.*s' at row %zu",width,name.data,
        c->candidate_row);
  else
    (void)snprintf(message,sizeof(message),
        "Data truncated for column '%.*s' at row %zu",width,name.data,
        c->candidate_row);
  return orm_sql_diagnostics_add(c->diagnostics,code,message,c->error);
}
static turbodb_status_t insert_assign(insert_context *c, size_t column,
    const turbodb_value_t *input, bool permissive, turbodb_value_t *out) {
  orm_sql_assignment_adjustment adjustment=ORM_SQL_ASSIGNMENT_EXACT;
  turbodb_status_t status=orm_tidesdb_sql_assignment_convert(
      c->schema.columns[column].type,input,permissive,c->store->budget,out,
      &adjustment,c->error);
  if (status==TURBODB_STATUS_OK) status=insert_warning(c,column,adjustment);
  return status;
}
static turbodb_status_t insert_conflict_warning(insert_context *c) {
  char message[ORM_SQL_DIAGNOSTIC_MESSAGE_CAPACITY];
  (void)snprintf(message,sizeof(message),"Duplicate key ignored at row %zu",
      c->candidate_row);
  return orm_sql_diagnostics_add(c->diagnostics,1062u,message,c->error);
}
static turbodb_status_t insert_parameters(insert_context *c) {
  const size_t markers = c->parameter_count;
  turbodb_status_t status = orm_sql_bind_parameter_offsets(c->document, markers, c->store->budget,
      &c->offsets, &c->offset_bytes, c->error);
  if (status == TURBODB_STATUS_OK) status = insert_vector(c, &c->inputs, markers, sizeof(orm_sql_expr_input),
      _Alignof(orm_sql_expr_input), &c->input_bytes);
  if (status == TURBODB_STATUS_OK && !c->binding_only) status = insert_vector(c, &c->arguments, markers, sizeof(turbodb_value_t),
      _Alignof(turbodb_value_t), &c->argument_bytes);
  if (status == TURBODB_STATUS_OK) status = insert_vector(c, &c->parameter_types, markers,
      sizeof(orm_sql_type), _Alignof(orm_sql_type), &c->parameter_type_bytes);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < markers; ++i) {
    if (c->binding_only) {
      *(orm_sql_type *)vec_at(&c->parameter_types, i) = c->binding_types[i];
      orm_sql_predicate validator;
      status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL, c->binding_types[i], NULL, &validator, c->error);
      continue;
    }
    const turbodb_value_t *value = &c->parameters[i];
    *(orm_sql_type *)vec_at(&c->parameter_types, i) =
        (orm_sql_type){value->kind, value->kind == TURBODB_VALUE_NULL};
    status = insert_validate(c, (orm_sql_type){value->kind, value->kind == TURBODB_VALUE_NULL}, value);
  }
  return status;
}
static turbodb_status_t insert_target_column(insert_context *c, sqlparser_id id,
    size_t position, size_t *target) {
  const size_t count = c->schema.count;
  size_t slot = 0; vstr name = {0};
  turbodb_status_t status = insert_name(c, id, &name);
  if (status == TURBODB_STATUS_OK)
    status = insert_charge(c, ORM_SQL_BUDGET_EXECUTION_STEPS, count + position);
  if (status != TURBODB_STATUS_OK) return status;
  for (slot = 0; slot < count; ++slot)
    if (name.len == c->schema.columns[slot].name.len &&
        !memcmp(name.data, c->schema.columns[slot].name.data, name.len)) break;
  if (slot == count)
    return insert_error(c, TURBODB_STATUS_SQL_ERROR, "unknown target column");
  for (size_t i = 0; i < position; ++i)
    if (*(const size_t *)vec_at_const(&c->columns, i) == slot)
      return insert_error(c, TURBODB_STATUS_SQL_ERROR, "duplicate target column");
  *(size_t *)vec_at(&c->columns, position) = slot;
  if (target) *target = slot;
  return TURBODB_STATUS_OK;
}
static turbodb_status_t insert_default_value(insert_context *c, size_t column,
    turbodb_value_t *out) {
  const orm_sql_schema_column *target = &c->schema.columns[column];
  const orm_sql_column_default *column_default = &c->schema.defaults[column];
  if (column_default->specified) {
    *out = column_default->value;
    return TURBODB_STATUS_OK;
  }
  if (!target->type.nullable && c->permissive) {
    const turbodb_value_t missing=turbodb_null();
    return insert_assign(c,column,&missing,true,out);
  }
  if (!target->type.nullable)
    return insert_error(c,TURBODB_STATUS_SQL_ERROR,
        "target column has no default value");
  *out = turbodb_null();
  return TURBODB_STATUS_OK;
}
static turbodb_status_t insert_default_check(insert_context *c, size_t column) {
  const orm_sql_schema_column *target = &c->schema.columns[column];
  const orm_sql_column_default *column_default = &c->schema.defaults[column];
  return !column_default->specified && !target->type.nullable && !c->permissive ?
      insert_error(c, TURBODB_STATUS_SQL_ERROR, "target column has no default value") : TURBODB_STATUS_OK;
}
static turbodb_status_t insert_missing_defaults(insert_context *c,
    turbodb_value_t *row) {
  for (size_t column = 0; column < c->schema.count; ++column) {
    bool targeted = false;
    for (size_t i = 0; i < c->target_count; ++i) {
      turbodb_status_t status = insert_charge(c,ORM_SQL_BUDGET_EXECUTION_STEPS,1);
      if (status != TURBODB_STATUS_OK) return status;
      if (*(const size_t *)vec_at_const(&c->columns,i) == column) {
        targeted = true;
        break;
      }
    }
    if (!targeted) {
      if (!row) {
        const turbodb_status_t status = insert_default_check(c, column);
        if (status != TURBODB_STATUS_OK) return status;
        continue;
      }
      turbodb_value_t value = turbodb_null();
      const turbodb_status_t status = insert_default_value(c,column,&value);
      if (status != TURBODB_STATUS_OK) return status;
      row[column] = value;
    }
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t insert_columns(insert_context *c, const sqlparser_node *statement) {
  const size_t schema_count = c->schema.count;
  const size_t count = statement->as.insert.columns_specified ?
      statement->as.insert.columns.count : schema_count;
  if (count > schema_count)
    return insert_error(c,TURBODB_STATUS_SQL_ERROR,"too many target columns");
  c->target_count = count;
  turbodb_status_t status = insert_vector(c, &c->columns, count, sizeof(size_t),
      _Alignof(size_t), &c->column_bytes);
  if (!statement->as.insert.columns_specified) {
    for (size_t i = 0; status == TURBODB_STATUS_OK && i < count; ++i) {
      status = insert_charge(c,ORM_SQL_BUDGET_EXECUTION_STEPS,1);
      if (status == TURBODB_STATUS_OK) *(size_t *)vec_at(&c->columns,i) = i;
    }
  } else {
    sqlparser_id id = statement->as.insert.columns.first;
    for (size_t i = 0; status == TURBODB_STATUS_OK && i < count; ++i) {
      status = insert_target_column(c, id, i, NULL);
      if (status == TURBODB_STATUS_OK)
        id = sqlparser_get_node(c->document, id)->next;
    }
  }
  return status == TURBODB_STATUS_OK ? insert_missing_defaults(c,NULL) : status;
}
/* AST IDs need not follow source order. Spans locate parameter occurrences;
 * compiled input slots remain sorted by ID as required by expr.h. */
static turbodb_status_t insert_inputs(insert_context *c, const sqlparser_node *root, size_t *count) {
  const size_t nodes = sqlparser_node_count(c->document);
  turbodb_status_t status = insert_charge(c, ORM_SQL_BUDGET_EXECUTION_STEPS, nodes);
  if (status != TURBODB_STATUS_OK) return status;
  *count = 0;
  for (size_t i = 1; i <= nodes; ++i) {
    const sqlparser_node *node = sqlparser_get_node(c->document, (sqlparser_id)i);
    if (node->kind != SQLPARSER_PARAMETER || node->span.offset < root->span.offset ||
        node->span.offset - root->span.offset >= root->span.length) continue;
    size_t first = 0, last = c->parameter_count;
    while (first < last) {
      status = insert_charge(c, ORM_SQL_BUDGET_EXECUTION_STEPS, 1);
      if (status != TURBODB_STATUS_OK) return status;
      const size_t mid = first + (last - first) / 2;
      if (*(const uint64_t *)vec_at_const(&c->offsets, mid) < node->span.offset) first = mid + 1;
      else last = mid;
    }
    if (first == c->parameter_count || *count == c->parameter_count ||
        *(const uint64_t *)vec_at_const(&c->offsets, first) != node->span.offset)
      return insert_error(c, TURBODB_STATUS_INTERNAL_ERROR, "parameter occurrence is not indexed");
    *(orm_sql_expr_input *)vec_at(&c->inputs, *count) =
        (orm_sql_expr_input){(sqlparser_id)i, *(const orm_sql_type *)vec_at_const(&c->parameter_types, first)};
    if (!c->binding_only) *(turbodb_value_t *)vec_at(&c->arguments, *count) = c->parameters[first];
    ++*count;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t insert_value(insert_context *c, sqlparser_id id, size_t cell, size_t column) {
  const sqlparser_node *node = sqlparser_get_node(c->document, id);
  if (!node) return insert_error(c, TURBODB_STATUS_INVALID_ARGUMENT, "missing value expression");
  c->offset = node->span.offset;
  if (node->kind == SQLPARSER_DEFAULT_VALUE) {
    if (c->binding_only) return insert_default_check(c, column);
    turbodb_value_t value = turbodb_null();
    const turbodb_status_t status = insert_default_value(c,column,&value);
    if (status == TURBODB_STATUS_OK) *(turbodb_value_t *)vec_at(&c->values,cell) = value;
    return status;
  }
  size_t inputs = 0;
  turbodb_status_t status = insert_inputs(c, node, &inputs); orm_sql_expr program = {0}; turbodb_value_t value = turbodb_null();
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_expr_compile_value(c->document, id,
      vec_data_const(&c->inputs), inputs, c->max_depth, c->store->budget, &program, c->error);
  if (status == TURBODB_STATUS_OK && !orm_tidesdb_sql_assignment_compatible(
      c->schema.columns[column].type,program.result))
    status = insert_error(c, TURBODB_STATUS_TYPE_ERROR,
        "expression cannot convert to target column");
  if (status == TURBODB_STATUS_OK && !c->binding_only) status = orm_tidesdb_sql_expr_eval_evaluation(&program,
      vec_data_const(&c->arguments),inputs,c->evaluation,&value,c->error);
  turbodb_value_t converted=turbodb_null();
  if (status == TURBODB_STATUS_OK && !c->binding_only)
    status = insert_assign(c,column,&value,c->permissive,&converted);
  /* Catalog numeric types ensure no program-owned byte view escapes destroy. */
  if (status == TURBODB_STATUS_OK && !c->binding_only) *(turbodb_value_t *)vec_at(&c->values, cell) = converted;
  const turbodb_status_t destroyed = orm_tidesdb_sql_expr_destroy(&program, status == TURBODB_STATUS_OK ? c->error : NULL);
  if (destroyed != TURBODB_STATUS_OK) c->store->failed = true;
  return status == TURBODB_STATUS_OK ? destroyed : status;
}
static turbodb_status_t insert_rows(insert_context *c, const sqlparser_node *statement) {
  const size_t rows = statement->as.insert.rows.count, columns = c->schema.count;
  const size_t targets = c->target_count;
  if (rows > SIZE_MAX / columns) return insert_error(c, TURBODB_STATUS_LIMIT_EXCEEDED, "row dimensions overflow");
  turbodb_status_t status = c->binding_only ? TURBODB_STATUS_OK :
      insert_charge(c, ORM_SQL_BUDGET_MATERIALIZED_ROWS, rows);
  if (status != TURBODB_STATUS_OK) return status;
  if (!c->binding_only) {
    c->row_slots = rows;
    status = insert_vector(c, &c->values, rows * columns, sizeof(turbodb_value_t), _Alignof(turbodb_value_t), &c->value_bytes);
  }
  sqlparser_id id = statement->as.insert.rows.first;
  for (size_t r = 0; status == TURBODB_STATUS_OK && r < rows; ++r) {
    c->candidate_row=r+1;
    status = insert_missing_defaults(c,
        c->binding_only ? NULL : (turbodb_value_t *)vec_at(&c->values,r * columns));
    if (status != TURBODB_STATUS_OK) break;
    const sqlparser_node *row = sqlparser_get_node(c->document, id);
    if (!row || row->kind != SQLPARSER_ROW) return insert_error(c, TURBODB_STATUS_INVALID_ARGUMENT, "expected a VALUES row");
    c->offset = row->span.offset;
    if (row->as.row.values.count != targets)
      return insert_error(c, TURBODB_STATUS_SQL_ERROR, "column/value count mismatch");
    sqlparser_id value = row->as.row.values.first;
    for (size_t i = 0; status == TURBODB_STATUS_OK && i < targets; ++i) {
      const size_t column = *(const size_t *)vec_at_const(&c->columns, i);
      status = insert_value(c, value, r * columns + column, column);
      if (status == TURBODB_STATUS_OK) value = sqlparser_get_node(c->document, value)->next;
    }
    id = row->next;
  }
  return status;
}
static turbodb_status_t insert_set(insert_context *c, const sqlparser_node *statement) {
  const size_t count = statement->as.insert.assignments.count;
  if (count > c->schema.count)
    return insert_error(c,TURBODB_STATUS_SQL_ERROR,"too many SET assignments");
  c->target_count = count;
  turbodb_status_t status = insert_vector(c, &c->columns, count, sizeof(size_t),
      _Alignof(size_t), &c->column_bytes);
  c->candidate_row=1;
  if (status == TURBODB_STATUS_OK && !c->binding_only)
    status = insert_charge(c, ORM_SQL_BUDGET_MATERIALIZED_ROWS, 1);
  if (status == TURBODB_STATUS_OK && !c->binding_only) c->row_slots = 1;
  if (status == TURBODB_STATUS_OK && !c->binding_only)
    status = insert_vector(c, &c->values, c->schema.count, sizeof(turbodb_value_t),
        _Alignof(turbodb_value_t), &c->value_bytes);
  sqlparser_id id = statement->as.insert.assignments.first;
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < count; ++i) {
    const sqlparser_node *assignment = sqlparser_get_node(c->document, id);
    if (!assignment || assignment->kind != SQLPARSER_ASSIGNMENT ||
        assignment->as.assignment.scope != SQLPARSER_SCOPE_DEFAULT)
      return insert_error(c, TURBODB_STATUS_UNSUPPORTED,
          "expected an INSERT SET column assignment");
    c->offset = assignment->span.offset;
    status = insert_target_column(c,assignment->as.assignment.name,i,NULL);
    id = assignment->next;
  }
  if (status == TURBODB_STATUS_OK)
    status = insert_missing_defaults(c,c->binding_only ? NULL : vec_data(&c->values));
  id = statement->as.insert.assignments.first;
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < count; ++i) {
    const sqlparser_node *assignment = sqlparser_get_node(c->document,id);
    const size_t target = *(const size_t *)vec_at_const(&c->columns,i);
    status = insert_value(c,assignment->as.assignment.value,target,target);
    id = assignment->next;
  }
  return status;
}
static turbodb_status_t insert_query(insert_context *c,
    const sqlparser_node *statement) {
  const size_t columns = c->schema.count, targets = c->target_count;
  turbodb_status_t status = TURBODB_STATUS_OK;
  if (!c->binding_only) {
    c->query_rows.budget = c->store->budget;
    status = insert_vector(c,&c->values,columns,
        sizeof(turbodb_value_t),_Alignof(turbodb_value_t),&c->value_bytes);
  }
  const orm_sql_query_scope scope = {.document=c->document,
      .root=statement->as.insert.query,
      .parameter_types=vec_data_const(&c->parameter_types),
      .parameter_count=c->parameter_count,.max_depth=c->max_depth,
      .budget=c->store->budget,.anonymous_output=true,
      .max_iterations=c->max_iterations,.evaluation=c->evaluation};
  orm_sql_query query = {0};
  if (status == TURBODB_STATUS_OK)
    status = c->binding_only ? orm_sql_runtime_query_bind(&scope,c->store,&query,c->error) :
        orm_sql_runtime_query_open(&scope,c->store,c->parameters,&query,c->error);
  if (status == TURBODB_STATUS_OK && query.columns != targets)
    status = insert_error(c,TURBODB_STATUS_SQL_ERROR,
        "INSERT SELECT column count differs from target");
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < targets; ++i) {
    orm_sql_schema_column output = {0};
    status = orm_tidesdb_sql_runtime_column(&query,i,&output,c->error);
    const size_t target = *(const size_t *)vec_at_const(&c->columns,i);
    if (status == TURBODB_STATUS_OK && !orm_tidesdb_sql_assignment_compatible(
        c->schema.columns[target].type,output.type))
      status = insert_error(c,TURBODB_STATUS_TYPE_ERROR,
          "INSERT SELECT output cannot convert to target column");
  }
  while (status == TURBODB_STATUS_OK && !c->binding_only) {
    orm_sql_scan_row row = {0};
    status = orm_tidesdb_sql_runtime_next(&query,&row,c->error);
    if (status != TURBODB_STATUS_OK) break;
    if (row.state == ORM_SQL_SCAN_DONE) break;
    c->candidate_row=vec_size(&c->query_rows.snapshots)+1;
    if (row.state != ORM_SQL_SCAN_ROW || row.count != targets) {
      status = insert_error(c,TURBODB_STATUS_INTERNAL_ERROR,
          "INSERT SELECT returned an invalid row");
      break;
    }
    status=insert_missing_defaults(c,vec_data(&c->values));
    for (size_t i = 0; status == TURBODB_STATUS_OK && i < targets; ++i) {
      const size_t target = *(const size_t *)vec_at_const(&c->columns,i);
      const turbodb_value_t value = row.values[i]; turbodb_value_t converted=turbodb_null();
      status = insert_assign(c,target,&value,c->permissive,&converted);
      if (status == TURBODB_STATUS_OK)
        *(turbodb_value_t *)vec_at(&c->values,target) = converted;
    }
    turbodb_value_t *copy = NULL;
    if (status == TURBODB_STATUS_OK)
      status = orm_sql_rows_append(&c->query_rows,
          vec_data_const(&c->values),columns,0,&copy,c->error);
  }
  const turbodb_status_t released = orm_tidesdb_sql_runtime_close(&query,
      status == TURBODB_STATUS_OK ? c->error : NULL);
  if (released != TURBODB_STATUS_OK) c->store->failed = true;
  return status == TURBODB_STATUS_OK ? released : status;
}
static turbodb_status_t insert_alias_validate(insert_context *c,
    const sqlparser_node *statement) {
  const sqlparser_id row=statement->as.insert.row_alias;
  const sqlparser_list aliases=statement->as.insert.column_aliases;
  if (!row) return aliases.count ? insert_error(c,TURBODB_STATUS_INVALID_ARGUMENT,
      "column aliases require an INSERT row alias") : TURBODB_STATUS_OK;
  vstr alias={0}; turbodb_status_t status=insert_name(c,row,&alias);
  if (status == TURBODB_STATUS_OK && insert_name_equal(alias,c->table))
    status=insert_error(c,TURBODB_STATUS_SQL_ERROR,
        "INSERT row alias must differ from target table");
  if (status == TURBODB_STATUS_OK && aliases.count && aliases.count != c->target_count)
    status=insert_error(c,TURBODB_STATUS_SQL_ERROR,
        "INSERT column alias count differs from target columns");
  sqlparser_id id=aliases.first;
  for (size_t i=0; status == TURBODB_STATUS_OK && i<aliases.count; ++i) {
    vstr name={0}; status=insert_name(c,id,&name);
    sqlparser_id prior=aliases.first;
    for (size_t j=0; status == TURBODB_STATUS_OK && j<i; ++j) {
      vstr other={0}; status=insert_name(c,prior,&other);
      if (status == TURBODB_STATUS_OK && insert_name_equal(name,other))
        status=insert_error(c,TURBODB_STATUS_SQL_ERROR,
            "duplicate INSERT column alias");
      prior=sqlparser_get_node(c->document,prior)->next;
    }
    if (status == TURBODB_STATUS_OK) id=sqlparser_get_node(c->document,id)->next;
  }
  return status;
}
static turbodb_status_t insert_alias_target(insert_context *c,
    const sqlparser_node *statement, vstr name, size_t *column, bool *found) {
  *found=false;
  const sqlparser_list aliases=statement->as.insert.column_aliases;
  if (!aliases.count) {
    for (size_t i=0; i<c->schema.count; ++i) {
      turbodb_status_t status=insert_charge(c,ORM_SQL_BUDGET_EXECUTION_STEPS,1);
      if (status != TURBODB_STATUS_OK) return status;
      if (insert_name_equal(name,c->schema.columns[i].name)) {
        *column=i; *found=true; return TURBODB_STATUS_OK;
      }
    }
    return TURBODB_STATUS_OK;
  }
  sqlparser_id id=aliases.first;
  for (size_t i=0; i<aliases.count; ++i) {
    turbodb_status_t status=insert_charge(c,ORM_SQL_BUDGET_EXECUTION_STEPS,1);
    vstr alias={0};
    if (status == TURBODB_STATUS_OK) status=insert_name(c,id,&alias);
    if (status != TURBODB_STATUS_OK) return status;
    if (insert_name_equal(name,alias)) {
      *column=*(const size_t *)vec_at_const(&c->columns,i);
      *found=true; return TURBODB_STATUS_OK;
    }
    id=sqlparser_get_node(c->document,id)->next;
  }
  return TURBODB_STATUS_OK;
}
static bool insert_name_is(insert_context *c, sqlparser_id id, const char *expected) {
  const sqlparser_node *name = sqlparser_get_node(c->document, id);
  if (!name || name->kind != SQLPARSER_NAME || name->span.length != strlen(expected)) return false;
  const char *text = sqlparser_text(c->document, name->span);
  for (size_t i = 0; i < name->span.length; ++i) {
    char value = text[i];
    if (value >= 'a' && value <= 'z') value = (char)(value - 'a' + 'A');
    if (value != expected[i]) return false;
  }
  return true;
}
static turbodb_status_t insert_duplicate_substitutions(insert_context *c,
    const sqlparser_node *statement, const orm_sql_binding_scope *scope) {
  const size_t nodes = sqlparser_node_count(c->document);
  const sqlparser_node *first = sqlparser_get_node(c->document,
      statement->as.insert.duplicate_assignments.first);
  const sqlparser_node *last = sqlparser_get_node(c->document,
      statement->as.insert.duplicate_assignments.last);
  if (!first || !last) return insert_error(c, TURBODB_STATUS_INVALID_ARGUMENT,
      "invalid duplicate-key assignment list");
  const size_t begin = first->span.offset, end = last->span.offset + last->span.length;
  for (size_t i = 1; i <= nodes; ++i) {
    const sqlparser_node *call = sqlparser_get_node(c->document, (sqlparser_id)i);
    if (call->kind != SQLPARSER_CALL || call->span.offset < begin ||
        call->span.offset >= end || !insert_name_is(c, call->as.call.name, "VALUES")) continue;
    c->offset = call->span.offset;
    const sqlparser_node *argument = sqlparser_get_node(c->document,
        call->as.call.arguments.first);
    if (call->as.call.distinct || call->as.call.arguments.count != 1 ||
        !argument || argument->kind != SQLPARSER_NAME || argument->as.name.parts != 1 ||
        argument->next)
      return insert_error(c, TURBODB_STATUS_SQL_ERROR,
          "VALUES() requires one unqualified target column");
    size_t column = 0;
    turbodb_status_t status = orm_sql_bind_column(scope,
        call->as.call.arguments.first, &column, c->error);
    if (status != TURBODB_STATUS_OK) return status;
    *(size_t *)vec_at(&c->substitutions, i - 1) = c->schema.count + column + 1;
  }
  if (!statement->as.insert.row_alias) return TURBODB_STATUS_OK;
  vstr row_alias={0}; turbodb_status_t status=insert_name(c,
      statement->as.insert.row_alias,&row_alias);
  for (size_t i=1; status == TURBODB_STATUS_OK && i<=nodes; ++i) {
    const sqlparser_node *node=sqlparser_get_node(c->document,(sqlparser_id)i);
    if (node->kind != SQLPARSER_NAME || node->span.offset < begin ||
        node->span.offset >= end) continue;
    vstr name={0}, qualifier={0};
    status=insert_reference(c,(sqlparser_id)i,&name,&qualifier);
    if (status != TURBODB_STATUS_OK) break;
    bool candidate=false; size_t column=0;
    if (qualifier.len) {
      if (!insert_name_equal(qualifier,row_alias)) continue;
      status=insert_alias_target(c,statement,name,&column,&candidate);
      if (status == TURBODB_STATUS_OK && !candidate)
        status=insert_error(c,TURBODB_STATUS_SQL_ERROR,
            "unknown INSERT row-alias column");
    } else if (statement->as.insert.column_aliases.count) {
      bool current=false;
      for (size_t j=0; j<c->schema.count; ++j)
        current=current || insert_name_equal(name,c->schema.columns[j].name);
      if (!current)
        status=insert_alias_target(c,statement,name,&column,&candidate);
    }
    if (status == TURBODB_STATUS_OK && candidate)
      *(size_t *)vec_at(&c->substitutions,i-1)=c->schema.count+column+1;
  }
  if (status != TURBODB_STATUS_OK) return status;
  return TURBODB_STATUS_OK;
}
static turbodb_status_t insert_conflict_buffers(insert_context *c, bool with_current) {
  const size_t rows = with_current ? 2 : 1;
  if (c->row_slots > SIZE_MAX - rows)
    return insert_error(c, TURBODB_STATUS_LIMIT_EXCEEDED,
        "INSERT conflict materialized row count overflow");
  orm_sql_budget_amount amount = {0};
  amount.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS] = rows;
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve(c->store->budget,
      &amount, c->error);
  if (status == TURBODB_STATUS_OK) c->row_slots += rows;
  if (status == TURBODB_STATUS_OK) status = insert_vector(c, &c->existing,
      c->schema.count, sizeof(turbodb_value_t), _Alignof(turbodb_value_t),
      &c->existing_bytes);
  if (status == TURBODB_STATUS_OK && with_current)
    status = insert_vector(c, &c->current, c->schema.count,
        sizeof(turbodb_value_t), _Alignof(turbodb_value_t), &c->current_bytes);
  return status;
}
static turbodb_status_t insert_duplicate_prepare(insert_context *c,
    const sqlparser_node *statement, vstr table) {
  const size_t columns = c->schema.count;
  const size_t nodes = sqlparser_node_count(c->document);
  const size_t count = statement->as.insert.duplicate_assignments.count;
  if (!count || columns > SIZE_MAX / 2 || c->parameter_count > SIZE_MAX - 2 * columns)
    return insert_error(c, TURBODB_STATUS_LIMIT_EXCEEDED,
        "duplicate-key binding capacity overflow");
  turbodb_status_t status = insert_charge(c, ORM_SQL_BUDGET_AST_NODES, nodes);
  if (status == TURBODB_STATUS_OK && !c->binding_only) status = insert_conflict_buffers(c, true);
  if (status == TURBODB_STATUS_OK) status = insert_vector(c, &c->augmented_columns,
      2 * columns, sizeof(orm_sql_schema_column), _Alignof(orm_sql_schema_column),
      &c->augmented_column_bytes);
  if (status == TURBODB_STATUS_OK) status = insert_vector(c, &c->substitutions,
      nodes, sizeof(size_t), _Alignof(size_t), &c->substitution_bytes);
  if (status == TURBODB_STATUS_OK) status = insert_vector(c, &c->assignments,
      count, sizeof(insert_assignment), _Alignof(insert_assignment),
      &c->assignment_bytes);
  if (status != TURBODB_STATUS_OK) return status;
  orm_sql_schema_column *augmented = vec_data(&c->augmented_columns);
  for (size_t i = 0; i < columns; ++i) {
    augmented[i] = c->schema.columns[i];
    augmented[columns + i] = (orm_sql_schema_column){.type = c->schema.columns[i].type};
  }
  const orm_sql_binding_scope target_scope = {.document = c->document,
      .schema = &c->schema, .qualifier = table, .budget = c->store->budget};
  status = insert_duplicate_substitutions(c, statement, &target_scope);
  const orm_sql_table_schema augmented_schema = {c->schema.name, augmented, 2 * columns};
  const orm_sql_binding_scope expression_scope = {.document = c->document,
      .schema = &augmented_schema, .qualifier = table,
      .parameter_types = vec_data_const(&c->parameter_types),
      .parameter_offsets = vec_data_const(&c->offsets),
      .parameter_count = c->parameter_count, .budget = c->store->budget,
      .substitutions = vec_data_const(&c->substitutions),
      .substitution_count = nodes};
  sqlparser_id id = statement->as.insert.duplicate_assignments.first;
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < count; ++i) {
    const sqlparser_node *node = sqlparser_get_node(c->document, id);
    if (!node || node->kind != SQLPARSER_ASSIGNMENT ||
        node->as.assignment.scope != SQLPARSER_SCOPE_DEFAULT)
      return insert_error(c, TURBODB_STATUS_UNSUPPORTED,
          "expected a duplicate-key column assignment");
    c->offset = node->span.offset;
    insert_assignment *assignment = vec_at(&c->assignments, i);
    status = orm_sql_bind_column(&target_scope, node->as.assignment.name,
        &assignment->column, c->error);
    const sqlparser_node *value = sqlparser_get_node(c->document,
        node->as.assignment.value);
    if (status == TURBODB_STATUS_OK && value &&
        value->kind == SQLPARSER_DEFAULT_VALUE) {
      c->offset = value->span.offset;
      status = insert_default_check(c, assignment->column);
      assignment->default_value=status==TURBODB_STATUS_OK;
    } else if (status == TURBODB_STATUS_OK) {
      status = orm_sql_bind_expression(&expression_scope,
          node->as.assignment.value, c->max_depth,
          (orm_sql_expression_target){&assignment->program, &assignment->slots,
            &assignment->slot_bytes}, false, c->error);
    }
    if (status == TURBODB_STATUS_OK && !assignment->default_value &&
        !orm_tidesdb_sql_assignment_compatible(
          c->schema.columns[assignment->column].type,
          assignment->program.result))
      status = insert_error(c, TURBODB_STATUS_TYPE_ERROR,
          "duplicate-key assignment cannot convert to target column");
    if (status == TURBODB_STATUS_OK && !assignment->default_value && !c->binding_only)
      status = orm_tidesdb_sql_expr_run_open_evaluation(
        &assignment->program,c->evaluation,&assignment->run,c->error);
    if (status == TURBODB_STATUS_OK && !assignment->default_value && !c->binding_only)
      status = insert_vector(c, &assignment->arguments,
        vec_size(&assignment->slots), sizeof(turbodb_value_t), _Alignof(turbodb_value_t),
        &assignment->argument_bytes);
    id = node->next;
  }
  return status;
}
static turbodb_status_t insert_duplicate_eval(insert_context *c,
    insert_assignment *assignment, const turbodb_value_t *candidate,
    turbodb_value_t *current, turbodb_value_t *out) {
  if (assignment->default_value)
    return insert_default_value(c,assignment->column,out);
  const size_t columns = c->schema.count;
  for (size_t i = 0; i < vec_size(&assignment->slots); ++i) {
    const size_t slot = *(const size_t *)vec_at_const(&assignment->slots, i);
    const turbodb_value_t value = slot < columns ? current[slot] :
        slot < 2 * columns ? candidate[slot - columns] :
        c->parameters[slot - 2 * columns];
    *(turbodb_value_t *)vec_at(&assignment->arguments, i) = value;
  }
  return orm_tidesdb_sql_expr_run_eval(&assignment->run,
      vec_data_const(&assignment->arguments), vec_size(&assignment->arguments),
      out, c->error);
}
static turbodb_status_t insert_value_equal(insert_context *c, size_t column,
    const turbodb_value_t *left, const turbodb_value_t *right, bool *equal) {
  orm_sql_predicate predicate; turbodb_value_t value = turbodb_null();
  turbodb_status_t status = orm_tidesdb_sql_predicate_bind(ORM_SQL_NULL_SAFE_EQUAL,
      c->schema.columns[column].type, &c->schema.columns[column].type,
      &predicate, c->error);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_predicate_eval(&predicate,
      left, right, c->store->budget, &value, c->error);
  if (status == TURBODB_STATUS_OK) *equal = value.data.boolean_value;
  return status;
}
static const turbodb_value_t *insert_candidate(const insert_context *c,
    size_t row) {
  if (c->query_rows.budget) return orm_sql_rows_at(&c->query_rows,row);
  return (const turbodb_value_t *)vec_data_const(&c->values) + row * c->schema.count;
}
static turbodb_status_t insert_replace_execute(insert_context *c, vstr table,
    size_t rows, size_t *affected) {
  const size_t columns = c->schema.count;
  turbodb_value_t *existing = vec_data(&c->existing);
  for (size_t r = 0; r < rows; ++r) {
    const turbodb_value_t *candidate = insert_candidate(c,r);
    if (!candidate) return insert_error(c,TURBODB_STATUS_INTERNAL_ERROR,
        "REPLACE candidate row is unavailable");
    for (;;) {
      bool found = false;
      turbodb_status_t status = orm_sql_relation_insert_conflict(c->store, table,
          candidate, columns, existing, &found, c->error);
      if (status != TURBODB_STATUS_OK) return status;
      if (!found) break;
      if (*affected == SIZE_MAX) return insert_error(c,
          TURBODB_STATUS_LIMIT_EXCEEDED, "affected row count overflow");
      const turbodb_value_t key = existing[c->definition.primary_key];
      status = orm_tidesdb_sql_relation_delete_rows(c->store, table,
          &key, 1, c->error);
      if (status != TURBODB_STATUS_OK) return status;
      ++*affected;
    }
    if (*affected == SIZE_MAX) return insert_error(c,
        TURBODB_STATUS_LIMIT_EXCEEDED, "affected row count overflow");
    const turbodb_status_t status = orm_tidesdb_sql_relation_insert_rows(c->store,
        table, candidate, 1, columns, c->error);
    if (status != TURBODB_STATUS_OK) return status;
    ++*affected;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t insert_duplicate_execute(insert_context *c, vstr table,
    size_t rows, bool ignore, size_t *affected) {
  const size_t columns = c->schema.count;
  for (size_t r = 0; r < rows; ++r) {
    c->candidate_row=r+1;
    const turbodb_value_t *candidate = insert_candidate(c,r);
    if (!candidate) return insert_error(c,TURBODB_STATUS_INTERNAL_ERROR,
        "INSERT candidate row is unavailable");
    turbodb_value_t *existing = vec_data(&c->existing), *current = vec_data(&c->current);
    bool found = false;
    turbodb_status_t status = orm_sql_relation_insert_conflict(c->store, table,
        candidate, columns, existing, &found, c->error);
    if (status != TURBODB_STATUS_OK) return status;
    if (!found) {
      if (*affected == SIZE_MAX) return insert_error(c, TURBODB_STATUS_LIMIT_EXCEEDED,
          "affected row count overflow");
      status = orm_tidesdb_sql_relation_insert_rows(c->store, table,
          candidate, 1, columns, c->error);
      if (status == TURBODB_STATUS_CONSTRAINT && ignore) {
        tdsql_error_init(c->error);
        status=insert_conflict_warning(c);
        if (status != TURBODB_STATUS_OK) return status;
        continue;
      }
      if (status != TURBODB_STATUS_OK) return status;
      ++*affected; continue;
    }
    memcpy(current, existing, columns * sizeof(*current));
    for (size_t i = 0; i < vec_size(&c->assignments); ++i) {
      insert_assignment *assignment = vec_at(&c->assignments, i);
      turbodb_value_t value = turbodb_null(), converted=turbodb_null();
      status = insert_duplicate_eval(c, assignment, candidate, current, &value);
      if (status == TURBODB_STATUS_OK) status = insert_assign(c,
          assignment->column,&value,ignore,&converted);
      if (status != TURBODB_STATUS_OK) return status;
      current[assignment->column] = converted;
    }
    bool changed = false;
    for (size_t i = 0; status == TURBODB_STATUS_OK && i < columns; ++i) {
      bool equal = false;
      status = insert_value_equal(c, i, &existing[i], &current[i], &equal);
      changed = changed || !equal;
    }
    if (status != TURBODB_STATUS_OK) return status;
    if (!changed) {
      if (c->client_found_rows) {
        if (*affected == SIZE_MAX) return insert_error(c,
            TURBODB_STATUS_LIMIT_EXCEEDED, "affected row count overflow");
        ++*affected;
      }
      continue;
    }
    if (*affected > SIZE_MAX - 2) return insert_error(c,
        TURBODB_STATUS_LIMIT_EXCEEDED, "affected row count overflow");
    bool same_primary = false;
    status = insert_value_equal(c, c->definition.primary_key,
        &existing[c->definition.primary_key], &current[c->definition.primary_key],
        &same_primary);
    if (status == TURBODB_STATUS_OK) status = same_primary ?
        orm_tidesdb_sql_relation_update_rows(c->store, table, current, 1,
          columns, c->error) :
        orm_tidesdb_sql_relation_move_rows(c->store, table,
          &existing[c->definition.primary_key], current, 1, columns, c->error);
    if (status == TURBODB_STATUS_CONSTRAINT && ignore) {
      tdsql_error_init(c->error);
      status=insert_conflict_warning(c);
      if (status != TURBODB_STATUS_OK) return status;
      continue;
    }
    if (status != TURBODB_STATUS_OK) return status;
    *affected += 2;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t insert_ignore_execute(insert_context *c, vstr table,
    size_t rows, size_t *affected) {
  const size_t columns = c->schema.count;
  turbodb_value_t *existing = vec_data(&c->existing);
  for (size_t r = 0; r < rows; ++r) {
    c->candidate_row=r+1;
    const turbodb_value_t *candidate = insert_candidate(c,r);
    if (!candidate) return insert_error(c,TURBODB_STATUS_INTERNAL_ERROR,
        "INSERT candidate row is unavailable");
    bool found = false;
    turbodb_status_t status = orm_sql_relation_insert_conflict(c->store, table,
        candidate, columns, existing, &found, c->error);
    if (status != TURBODB_STATUS_OK) return status;
    if (found) {
      status=insert_conflict_warning(c);
      if (status != TURBODB_STATUS_OK) return status;
      continue;
    }
    if (*affected == SIZE_MAX)
      return insert_error(c, TURBODB_STATUS_LIMIT_EXCEEDED,
          "affected row count overflow");
    status = orm_tidesdb_sql_relation_insert_rows(c->store, table,
        candidate, 1, columns, c->error);
    if (status == TURBODB_STATUS_CONSTRAINT) {
      tdsql_error_init(c->error);
      status=insert_conflict_warning(c);
      if (status != TURBODB_STATUS_OK) return status;
      continue;
    }
    if (status != TURBODB_STATUS_OK) return status;
    ++*affected;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t insert_query_execute(insert_context *c, vstr table,
    size_t rows, size_t *affected) {
  for (size_t r = 0; r < rows; ++r) {
    const turbodb_value_t *candidate = insert_candidate(c,r);
    if (!candidate) return insert_error(c,TURBODB_STATUS_INTERNAL_ERROR,
        "INSERT candidate row is unavailable");
    if (*affected == SIZE_MAX)
      return insert_error(c,TURBODB_STATUS_LIMIT_EXCEEDED,
          "affected row count overflow");
    const turbodb_status_t status = orm_tidesdb_sql_relation_insert_rows(c->store,
        table,candidate,1,c->schema.count,c->error);
    if (status != TURBODB_STATUS_OK) return status;
    ++*affected;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t insert_assignment_close(insert_context *c,
    insert_assignment *assignment, turbodb_status_t status) {
  if (assignment->default_value) return status;
  turbodb_status_t released = orm_tidesdb_sql_expr_run_close(&assignment->run,
      status == TURBODB_STATUS_OK ? c->error : NULL);
  if (released != TURBODB_STATUS_OK) c->store->failed = true;
  if (status == TURBODB_STATUS_OK) status = released;
  released = orm_tidesdb_sql_expr_destroy(&assignment->program,
      status == TURBODB_STATUS_OK ? c->error : NULL);
  if (released != TURBODB_STATUS_OK) c->store->failed = true;
  if (status == TURBODB_STATUS_OK) status = released;
  vec_t *vectors[] = {&assignment->slots, &assignment->arguments};
  const size_t bytes[] = {assignment->slot_bytes, assignment->argument_bytes};
  for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
    released = orm_sql_work_release(vectors[i], bytes[i], c->store->budget,
        status == TURBODB_STATUS_OK ? c->error : NULL);
    if (released != TURBODB_STATUS_OK) c->store->failed = true;
    if (status == TURBODB_STATUS_OK) status = released;
  }
  return status;
}
static turbodb_status_t insert_close(insert_context *c, turbodb_status_t status) {
  for (size_t i = 0; i < vec_size(&c->assignments); ++i)
    status = insert_assignment_close(c, vec_at(&c->assignments, i), status);
  vec_t *vectors[] = {&c->columns, &c->offsets, &c->inputs, &c->arguments,
    &c->values, &c->parameter_types, &c->augmented_columns, &c->substitutions,
    &c->assignments, &c->existing, &c->current};
  const size_t bytes[] = {c->column_bytes, c->offset_bytes, c->input_bytes,
    c->argument_bytes, c->value_bytes, c->parameter_type_bytes,
    c->augmented_column_bytes, c->substitution_bytes, c->assignment_bytes,
    c->existing_bytes, c->current_bytes};
  for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
    const turbodb_status_t released = orm_sql_work_release(vectors[i], bytes[i], c->store->budget, status == TURBODB_STATUS_OK ? c->error : NULL);
    if (released != TURBODB_STATUS_OK) c->store->failed = true;
    if (status == TURBODB_STATUS_OK) status = released;
  }
  const turbodb_status_t rows_closed = orm_sql_rows_close(&c->query_rows,
      status == TURBODB_STATUS_OK ? c->error : NULL);
  if (rows_closed != TURBODB_STATUS_OK) c->store->failed = true;
  if (status == TURBODB_STATUS_OK) status = rows_closed;
  if (c->row_slots) {
    const turbodb_status_t released = orm_tidesdb_sql_budget_release(c->store->budget, ORM_SQL_BUDGET_MATERIALIZED_ROWS,
        c->row_slots, status == TURBODB_STATUS_OK ? c->error : NULL);
    if (released != TURBODB_STATUS_OK) c->store->failed = true;
    if (status == TURBODB_STATUS_OK) status = released;
  }
  const turbodb_status_t destroyed = orm_tidesdb_sql_catalog_destroy(&c->definition, status == TURBODB_STATUS_OK ? c->error : NULL);
  if (destroyed != TURBODB_STATUS_OK) c->store->failed = true;
  return status == TURBODB_STATUS_OK ? destroyed : status;
}
turbodb_status_t orm_tidesdb_sql_insert_execute(const sqlparser_document *document,
    orm_sql_catalog_store *store, const turbodb_value_t *parameters, size_t parameter_count,
    size_t max_depth, uint64_t max_iterations, bool client_found_rows,
    orm_sql_diagnostics *diagnostics, size_t *affected,
    turbodb_error_t *error) {
  return orm_sql_insert_execute_evaluation(document,store,parameters,parameter_count,
      max_depth,max_iterations,client_found_rows,
      (orm_sql_evaluation){.diagnostics=diagnostics},affected,error);
}
static turbodb_status_t insert_process(insert_context c, size_t *affected) {
  const sqlparser_document *document = c.document;
  orm_sql_catalog_store *store = c.store;
  turbodb_error_t *error = c.error;
  const orm_sql_evaluation evaluation = c.evaluation;
  if (!document || (!affected && !c.binding_only) || !c.max_depth ||
      (c.parameter_count && (c.binding_only ? !c.binding_types : !c.parameters)))
    return insert_error(&c, TURBODB_STATUS_INVALID_ARGUMENT, "invalid execution arguments");
  turbodb_status_t status = c.binding_only ? orm_sql_store_ready(store, error) :
      orm_sql_store_writable(store, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (sqlparser_get_dialect(document) != SQLPARSER_MYSQL || sqlparser_statements(document).count != 1)
    return insert_error(&c, TURBODB_STATUS_UNSUPPORTED, "one MySQL INSERT statement required");
  const sqlparser_node *statement = sqlparser_get_node(document, sqlparser_statements(document).first);
  if (!statement || statement->kind != SQLPARSER_INSERT)
    return insert_error(&c, TURBODB_STATUS_UNSUPPORTED, "expected INSERT statement");
  c.offset = statement->span.offset;
  const bool ignore = statement->as.insert.conflict == SQLPARSER_CONFLICT_IGNORE;
  c.permissive=ignore;
  c.evaluation=evaluation;
  c.evaluation.mode=ignore ? ORM_SQL_EVALUATION_IGNORE_WRITE : ORM_SQL_EVALUATION_WRITE;
  const bool replace = statement->as.insert.replace;
  const bool set_form = statement->as.insert.assignments.count != 0;
  const bool values_form = statement->as.insert.rows.count != 0;
  const bool query_form = statement->as.insert.query != 0;
  if ((statement->as.insert.conflict != SQLPARSER_CONFLICT_DEFAULT && !ignore) ||
      statement->as.insert.default_values || statement->as.insert.low_priority ||
      (size_t)set_form + (size_t)values_form + (size_t)query_form != 1 ||
      (set_form && (statement->as.insert.columns.count ||
          statement->as.insert.columns_specified)))
    return insert_error(&c, TURBODB_STATUS_UNSUPPORTED, "unsupported INSERT form or modifier");
  size_t rows = set_form ? 1 : statement->as.insert.rows.count;
  vstr table = {0}; uint64_t id = 0, version = 0; bool found = false;
  status = insert_name(&c, statement->as.insert.table, &table);
  if (status == TURBODB_STATUS_OK) c.table=table;
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_catalog_lookup(store, table, &c.definition, &id, &version, &found, error);
  if (status == TURBODB_STATUS_OK && !found) status = insert_error(&c, TURBODB_STATUS_SQL_ERROR, "table does not exist");
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_catalog_schema(&c.definition, &c.schema, error);
  if (status == TURBODB_STATUS_OK && (values_form || query_form))
    status = insert_columns(&c, statement);
  if (status == TURBODB_STATUS_OK) status = insert_parameters(&c);
  if (status == TURBODB_STATUS_OK) status = set_form ? insert_set(&c,statement) :
      query_form ? insert_query(&c,statement) : insert_rows(&c,statement);
  if (status == TURBODB_STATUS_OK) status=insert_alias_validate(&c,statement);
  if (status == TURBODB_STATUS_OK && query_form)
    rows = vec_size(&c.query_rows.snapshots);
  const bool duplicate = statement->as.insert.duplicate_assignments.count != 0;
  if (status == TURBODB_STATUS_OK && replace && (ignore || duplicate))
    status = insert_error(&c, TURBODB_STATUS_UNSUPPORTED,
        "REPLACE does not support INSERT conflict modifiers");
  if (status == TURBODB_STATUS_OK && duplicate)
    status = insert_duplicate_prepare(&c, statement, table);
  else if (status == TURBODB_STATUS_OK && !c.binding_only && (replace || ignore))
    status = insert_conflict_buffers(&c, false);
  bool command = false; size_t changed = 0;
  if (status == TURBODB_STATUS_OK && !c.binding_only && (replace || duplicate || ignore || query_form)) {
    status = orm_sql_store_command_begin(store, error);
    command = status == TURBODB_STATUS_OK;
  }
  if (status == TURBODB_STATUS_OK && !c.binding_only) {
    if (replace)
      status = insert_replace_execute(&c, table, rows, &changed);
    else if (duplicate)
      status = insert_duplicate_execute(&c, table, rows, ignore, &changed);
    else if (ignore)
      status = insert_ignore_execute(&c, table, rows, &changed);
    else if (query_form)
      status = insert_query_execute(&c,table,rows,&changed);
    else
      status = orm_tidesdb_sql_relation_insert_rows(store, table,
          vec_data_const(&c.values), rows, c.schema.count, error);
  }
  status = insert_close(&c, status);
  if (command) status = orm_sql_store_command_finish(store, status, error);
  if (status == TURBODB_STATUS_OK && !c.binding_only)
    *affected = replace || duplicate || ignore || query_form ? changed : rows;
  return status;
}
turbodb_status_t orm_sql_insert_execute_evaluation(const sqlparser_document *document,
    orm_sql_catalog_store *store, const turbodb_value_t *parameters, size_t parameter_count,
    size_t max_depth, uint64_t max_iterations, bool client_found_rows,
    orm_sql_evaluation evaluation, size_t *affected, turbodb_error_t *error) {
  return insert_process((insert_context){.document=document,.store=store,.parameters=parameters,
      .parameter_count=parameter_count,.max_depth=max_depth,.max_iterations=max_iterations,
      .client_found_rows=client_found_rows,.evaluation=evaluation,
      .diagnostics=evaluation.diagnostics,.error=error},affected);
}
turbodb_status_t orm_sql_insert_bind(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *store, turbodb_error_t *error) {
  if (!scope || !store || scope->budget != store->budget) {
    tdsql_error_set(error,TURBODB_STATUS_INVALID_ARGUMENT,"INSERT binding requires the scope's Catalog budget");
    return TURBODB_STATUS_INVALID_ARGUMENT;
  }
  turbodb_status_t status = orm_sql_store_ready(store, error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_bind_write_scope(scope, error);
  if (status != TURBODB_STATUS_OK) return status;
  return insert_process((insert_context){.document=scope->document,.store=store,
      .binding_types=scope->parameter_types,.parameter_count=scope->parameter_count,
      .max_depth=scope->max_depth,.max_iterations=scope->max_iterations,
      .evaluation=scope->evaluation,.diagnostics=scope->evaluation.diagnostics,
      .binding_only=true,.error=error},NULL);
}

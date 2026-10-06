#include "change.h"
#include "binding.h"
#include "dependencies.h"
#include "relation.h"
#include "name.h"
#include "store_internal.h"
#include "work.h"
#include "error.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct change_expression {
  orm_sql_expr program;
  orm_sql_expr_run run;
  vec_t slots, query_slots, arguments;
  size_t slot_bytes, query_slot_bytes, argument_bytes, column;
  turbodb_value_t default_result;
  bool default_value;
} change_expression;
typedef struct change_order {
  change_expression expression;
  size_t column, key;
  bool descending;
} change_order;
typedef struct change_sorted_row {
  const turbodb_value_t *row, *keys;
  const change_order *orders;
  size_t order_count;
} change_sorted_row;
typedef struct change_context {
  const sqlparser_document *document;
  orm_sql_catalog_store *store;
  const turbodb_value_t *parameters;
  const orm_sql_type *binding_types;
  size_t parameter_count, max_depth, offset;
  uint64_t max_iterations;
  bool deleting, updates_key, permissive, binding_only;
  vstr table, qualifier;
  orm_sql_table_definition definition;
  orm_sql_table_schema schema;
  vec_t types, offsets, expressions, row, values;
  size_t type_bytes, offset_bytes, expression_bytes, row_bytes, value_bytes;
  vec_t orders, snapshots, sorted, keys, old_keys, row_numbers;
  size_t order_bytes, snapshot_bytes, sorted_bytes, key_bytes, key_count;
  size_t matched, changed, selected_slots, old_key_bytes, row_number_bytes;
  size_t candidate_row;
  uint64_t limit;
  orm_sql_diagnostics *diagnostics;
  orm_sql_dependencies dependencies;
  orm_sql_expr_query_sources query_sources;
  turbodb_error_t *error;
} change_context;
static turbodb_status_t change_error(change_context *c, turbodb_status_t status, const char *reason) {
  char message[TURBODB_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message), "TidesDB SQL %s at byte %zu: %s",
      c->deleting ? "DELETE" : "UPDATE", c->offset, reason);
  tdsql_error_set(c->error, status, message); return status;
}
static turbodb_status_t change_charge(change_context *c, orm_sql_budget_resource resource, size_t count) {
  orm_sql_budget_amount amount = {0}; amount.value[resource] = count;
  return orm_tidesdb_sql_budget_reserve(c->store->budget, &amount, c->error);
}
static turbodb_status_t change_vector(change_context *c, vec_t *v, size_t count, size_t width, size_t align, size_t *bytes) {
  return orm_sql_work_zero(v, count, width, align, c->store->budget, bytes, c->error);
}
static turbodb_status_t change_validate(change_context *c, orm_sql_type type, const turbodb_value_t *value) {
  orm_sql_predicate predicate; turbodb_value_t ignored;
  turbodb_status_t status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL, type, NULL, &predicate, c->error);
  return status == TURBODB_STATUS_OK ? orm_tidesdb_sql_predicate_eval(&predicate, value, NULL,
      c->store->budget, &ignored, c->error) : status;
}
static turbodb_status_t change_warning(change_context *c, size_t column,
    orm_sql_assignment_adjustment adjustment) {
  if (adjustment == ORM_SQL_ASSIGNMENT_EXACT) return TURBODB_STATUS_OK;
  const vstr name = c->schema.columns[column].name;
  const int width = name.len > (size_t)INT_MAX ? INT_MAX : (int)name.len;
  const uint32_t code = adjustment == ORM_SQL_ASSIGNMENT_OUT_OF_RANGE ? 1264u :
      adjustment == ORM_SQL_ASSIGNMENT_NULL_TO_NOT_NULL ? 1048u :
      adjustment == ORM_SQL_ASSIGNMENT_INVALID ? 1366u : 1265u;
  char message[ORM_SQL_DIAGNOSTIC_MESSAGE_CAPACITY];
  if (adjustment == ORM_SQL_ASSIGNMENT_OUT_OF_RANGE)
    (void)snprintf(message, sizeof(message),
        "Out of range value for column '%.*s' at row %zu", width, name.data,
        c->candidate_row);
  else if (adjustment == ORM_SQL_ASSIGNMENT_NULL_TO_NOT_NULL)
    (void)snprintf(message, sizeof(message),
        "Column '%.*s' cannot be null at row %zu", width, name.data,
        c->candidate_row);
  else if (adjustment == ORM_SQL_ASSIGNMENT_INVALID)
    (void)snprintf(message, sizeof(message),
        "Incorrect numeric value for column '%.*s' at row %zu", width,
        name.data, c->candidate_row);
  else
    (void)snprintf(message, sizeof(message),
        "Data truncated for column '%.*s' at row %zu", width, name.data,
        c->candidate_row);
  return orm_sql_diagnostics_add(c->diagnostics, code, message, c->error);
}
static turbodb_status_t change_assign(change_context *c, size_t column,
    const turbodb_value_t *input, turbodb_value_t *out) {
  orm_sql_assignment_adjustment adjustment = ORM_SQL_ASSIGNMENT_EXACT;
  turbodb_status_t status = orm_tidesdb_sql_assignment_convert(
      c->schema.columns[column].type, input, c->permissive, c->store->budget,
      out, &adjustment, c->error);
  if (status == TURBODB_STATUS_OK)
    status = change_warning(c, column, adjustment);
  return status;
}
static turbodb_status_t change_conflict_warning(change_context *c,
    size_t row_number) {
  char message[ORM_SQL_DIAGNOSTIC_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message),
      "Duplicate key ignored at row %zu", row_number);
  return orm_sql_diagnostics_add(c->diagnostics, 1062u, message, c->error);
}
static turbodb_status_t change_parameters(change_context *c) {
  const size_t markers = c->parameter_count;
  turbodb_status_t status = orm_sql_bind_parameter_offsets(c->document, markers, c->store->budget,
      &c->offsets, &c->offset_bytes, c->error);
  if (status == TURBODB_STATUS_OK) status = change_vector(c, &c->types, markers, sizeof(orm_sql_type), _Alignof(orm_sql_type), &c->type_bytes);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < markers; ++i) {
    const orm_sql_type type = c->binding_only ? c->binding_types[i] :
        (orm_sql_type){c->parameters[i].kind, c->parameters[i].kind == TURBODB_VALUE_NULL};
    *(orm_sql_type *)vec_at(&c->types, i) = type;
    if (c->binding_only) {
      orm_sql_predicate validator;
      status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL, type, NULL, &validator, c->error);
    } else status = change_validate(c, type, &c->parameters[i]);
  }
  return status;
}
static turbodb_status_t change_table(change_context *c, sqlparser_id id) {
  const sqlparser_node *node = sqlparser_get_node(c->document, id);
  if (!node || node->kind != SQLPARSER_NAME || node->as.name.parts != 1)
    return change_error(c, TURBODB_STATUS_UNSUPPORTED, "one unqualified table required");
  c->offset = node->span.offset;
  vstr text = {sqlparser_text(c->document, node->span), node->span.length}; const char *reason = NULL;
  turbodb_status_t status = orm_sql_name_part(&text, &c->table, &reason);
  if (status != TURBODB_STATUS_OK) return change_error(c, status, reason);
  if (text.len) return change_error(c, TURBODB_STATUS_UNSUPPORTED, "unsupported table name");
  uint64_t id_value = 0, version = 0; bool found = false;
  status = orm_tidesdb_sql_catalog_lookup(c->store, c->table, &c->definition, &id_value, &version, &found, c->error);
  if (status == TURBODB_STATUS_OK && !found) return change_error(c, TURBODB_STATUS_SQL_ERROR, "table does not exist");
  return status == TURBODB_STATUS_OK ? orm_tidesdb_sql_catalog_schema(&c->definition, &c->schema, c->error) : status;
}
static turbodb_status_t change_alias(change_context *c, sqlparser_id id) {
  c->qualifier = c->table;
  if (!id) return TURBODB_STATUS_OK;
  const sqlparser_node *node = sqlparser_get_node(c->document, id);
  if (node) c->offset = node->span.offset;
  const char *reason = NULL;
  const turbodb_status_t status = orm_sql_name_node(c->document, id,
      &c->qualifier, &reason);
  return status == TURBODB_STATUS_OK ? status : change_error(c, status, reason);
}
static turbodb_status_t change_limit(change_context *c, sqlparser_id id) {
  if (!id) return TURBODB_STATUS_OK;
  const sqlparser_node *node = sqlparser_get_node(c->document, id);
  if (!node || node->kind != SQLPARSER_LIMIT)
    return change_error(c, TURBODB_STATUS_UNSUPPORTED, "expected LIMIT row_count");
  c->offset = node->span.offset;
  if (node->as.limit.offset)
    return change_error(c, TURBODB_STATUS_UNSUPPORTED, "write LIMIT does not accept an offset");
  node = sqlparser_get_node(c->document, node->as.limit.count);
  if (!node) return change_error(c, TURBODB_STATUS_UNSUPPORTED, "missing LIMIT count");
  c->offset = node->span.offset;
  turbodb_value_t value = turbodb_null();
  if (node->kind == SQLPARSER_NUMBER) {
    const vstr text = {sqlparser_text(c->document, node->span), node->span.length};
    turbodb_error_t cause; tdsql_error_init(&cause);
    const turbodb_status_t status = orm_tidesdb_sql_integer_literal(text, false, &value, &cause);
    if (status != TURBODB_STATUS_OK) return change_error(c, status, cause.message);
  } else if (node->kind == SQLPARSER_PARAMETER) {
    bool found = false;
    for (size_t i = 0; i < c->parameter_count; ++i) {
      const turbodb_status_t status = change_charge(c, ORM_SQL_BUDGET_EXECUTION_STEPS, 1);
      if (status != TURBODB_STATUS_OK) return status;
      if (*(const uint64_t *)vec_at_const(&c->offsets, i) == node->span.offset) {
        if (c->binding_only) {
          const turbodb_value_kind_t kind = c->binding_types[i].kind;
          return kind == TURBODB_VALUE_INT64 || kind == TURBODB_VALUE_UINT64 ?
              TURBODB_STATUS_OK : change_error(c, TURBODB_STATUS_TYPE_ERROR,
                  "LIMIT requires an integer parameter type");
        }
        value = c->parameters[i]; found = true; break;
      }
    }
    if (!found) return change_error(c, TURBODB_STATUS_SQL_ERROR, "unbound LIMIT parameter");
  } else return change_error(c, TURBODB_STATUS_UNSUPPORTED, "LIMIT requires an integer literal or parameter");
  if (value.kind != TURBODB_VALUE_UINT64 && (value.kind != TURBODB_VALUE_INT64 || value.data.int64_value < 0))
    return change_error(c, TURBODB_STATUS_TYPE_ERROR, "LIMIT requires a nonnegative integer");
  c->limit = value.kind == TURBODB_VALUE_UINT64 ? value.data.uint64_value : (uint64_t)value.data.int64_value;
  return TURBODB_STATUS_OK;
}
static turbodb_status_t change_expression_prepare(change_context *c, change_expression *expression) {
  if (c->binding_only) return TURBODB_STATUS_OK;
  turbodb_status_t status = TURBODB_STATUS_OK;
  if (expression->program.budget) {
    status = orm_tidesdb_sql_expr_run_open_mapped(&expression->program,
            vec_data_const(&expression->query_slots),
            vec_size(&expression->query_slots), &c->query_sources,
            &expression->run, c->error);
  }
  if (status == TURBODB_STATUS_OK) status = change_vector(c, &expression->arguments, vec_size(&expression->slots),
      sizeof(turbodb_value_t), _Alignof(turbodb_value_t), &expression->argument_bytes);
  return status;
}
static turbodb_status_t change_default(change_context *c, size_t column,
    turbodb_value_t *out) {
  const orm_sql_schema_column *target = &c->schema.columns[column];
  const orm_sql_column_default *column_default =
      c->schema.defaults ? &c->schema.defaults[column] : NULL;
  if (column_default && column_default->specified) {
    *out = column_default->value;
    return TURBODB_STATUS_OK;
  }
  if (!target->type.nullable)
    return change_error(c,TURBODB_STATUS_SQL_ERROR,
        "target column has no default value");
  *out = turbodb_null();
  return TURBODB_STATUS_OK;
}
static turbodb_status_t change_compile(change_context *c, const sqlparser_node *statement) {
  const size_t assignments = c->deleting ? 0 : statement->as.update.assignments.count;
  if (assignments == SIZE_MAX || c->parameter_count > SIZE_MAX - c->schema.count)
    return change_error(c, TURBODB_STATUS_LIMIT_EXCEEDED, "binding capacity overflow");
  turbodb_status_t status = change_charge(c, ORM_SQL_BUDGET_AST_NODES, sqlparser_node_count(c->document));
  if (status == TURBODB_STATUS_OK) status = change_vector(c, &c->expressions, assignments + 1,
      sizeof(change_expression), _Alignof(change_expression), &c->expression_bytes);
  const orm_sql_binding_scope scope = {.document = c->document, .schema = &c->schema, .qualifier = c->qualifier,
      .parameter_types = vec_data_const(&c->types), .parameter_offsets = vec_data_const(&c->offsets),
      .parameter_count = c->parameter_count, .budget = c->store->budget,
      .queries = vec_data_const(&c->dependencies.bindings),
      .query_count = c->dependencies.query_count};
  sqlparser_id assignment = c->deleting ? 0 : statement->as.update.assignments.first;
  for (size_t i = 0; status == TURBODB_STATUS_OK && i <= assignments; ++i) {
    change_expression *expression = vec_at(&c->expressions, i);
    sqlparser_id root = c->deleting ? statement->as.delete_stmt.where : statement->as.update.where;
    if (i) {
      const sqlparser_node *node = sqlparser_get_node(c->document, assignment);
      if (!node || node->kind != SQLPARSER_ASSIGNMENT || node->as.assignment.scope != SQLPARSER_SCOPE_DEFAULT)
        return change_error(c, TURBODB_STATUS_UNSUPPORTED, "expected a column assignment");
      c->offset = node->span.offset; root = node->as.assignment.value; assignment = node->next;
      status = orm_sql_bind_column(&scope, node->as.assignment.name, &expression->column, c->error);
      if (status == TURBODB_STATUS_OK && expression->column == c->definition.primary_key)
        c->updates_key = true;
      const sqlparser_node *value = sqlparser_get_node(c->document,root);
      if (status == TURBODB_STATUS_OK && value &&
          value->kind == SQLPARSER_DEFAULT_VALUE) {
        c->offset = value->span.offset;
        status = change_default(c,expression->column,
            &expression->default_result);
        expression->default_value = status == TURBODB_STATUS_OK;
      }
    }
    if (status == TURBODB_STATUS_OK && !expression->default_value)
      status = orm_sql_bind_expression(&scope, root, c->max_depth,
        (orm_sql_expression_target){&expression->program, &expression->slots,
          &expression->slot_bytes, &expression->query_slots,
          &expression->query_slot_bytes}, i == 0, c->error);
    if (status == TURBODB_STATUS_OK && i && !expression->default_value &&
        !orm_tidesdb_sql_assignment_compatible(
          c->schema.columns[expression->column].type,
          expression->program.result))
      status = change_error(c, TURBODB_STATUS_TYPE_ERROR, "assignment kind differs from target column");
    if (status == TURBODB_STATUS_OK && !expression->default_value)
      status = change_expression_prepare(c, expression);
  }
  if (status == TURBODB_STATUS_OK && !c->deleting && !c->binding_only)
    status = change_vector(c, &c->row, c->schema.count, sizeof(turbodb_value_t), _Alignof(turbodb_value_t), &c->row_bytes);
  return status;
}
static turbodb_status_t change_bind_order(change_context *c, sqlparser_list orders) {
  turbodb_status_t status = change_charge(c, ORM_SQL_BUDGET_PLAN_NODES, orders.count);
  if (status == TURBODB_STATUS_OK) status = change_vector(c, &c->orders, orders.count,
      sizeof(change_order), _Alignof(change_order), &c->order_bytes);
  const orm_sql_binding_scope scope = {.document = c->document, .schema = &c->schema,
      .qualifier = c->qualifier, .budget = c->store->budget,
      .parameter_types = vec_data_const(&c->types), .parameter_offsets = vec_data_const(&c->offsets),
      .parameter_count = c->parameter_count,
      .queries = vec_data_const(&c->dependencies.bindings),
      .query_count = c->dependencies.query_count};
  sqlparser_id id = orders.first;
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < orders.count; ++i) {
    const sqlparser_node *node = sqlparser_get_node(c->document, id);
    if (!node || node->kind != SQLPARSER_ORDER)
      return change_error(c, TURBODB_STATUS_UNSUPPORTED, "expected ORDER BY item");
    c->offset = node->span.offset;
    change_order *order = vec_at(&c->orders, i); order->descending = node->as.order.descending;
    const sqlparser_node *root = sqlparser_get_node(c->document, node->as.order.expression);
    if (!root) return change_error(c, TURBODB_STATUS_UNSUPPORTED, "missing ORDER BY expression");
    const sqlparser_node *operand = root->kind == SQLPARSER_UNARY ?
        sqlparser_get_node(c->document, root->as.unary.operand) : NULL;
    if (root->kind == SQLPARSER_NUMBER || (operand && operand->kind == SQLPARSER_NUMBER &&
        (root->as.unary.op == SQLPARSER_OP_POSITIVE || root->as.unary.op == SQLPARSER_OP_NEGATE)))
      return change_error(c, TURBODB_STATUS_UNSUPPORTED, "numeric ORDER BY positions are not supported");
    if (root->kind == SQLPARSER_NAME) {
      status = orm_sql_bind_column(&scope, node->as.order.expression, &order->column, c->error);
      if (status == TURBODB_STATUS_OK) {
        const turbodb_value_kind_t kind = c->schema.columns[order->column].type.kind;
        if (kind != TURBODB_VALUE_INT64 && kind != TURBODB_VALUE_UINT64 && kind != TURBODB_VALUE_DOUBLE)
          status = change_error(c, TURBODB_STATUS_UNSUPPORTED, "ORDER BY requires a numeric column");
      }
    } else {
      change_expression *expression = &order->expression;
      order->key = c->key_count++;
      status = orm_sql_bind_expression(&scope, node->as.order.expression, c->max_depth,
          (orm_sql_expression_target){&expression->program, &expression->slots,
            &expression->slot_bytes, &expression->query_slots,
            &expression->query_slot_bytes}, false, c->error);
      if (status == TURBODB_STATUS_OK) {
        const turbodb_value_kind_t kind = expression->program.result.kind;
        if (kind != TURBODB_VALUE_INT64 && kind != TURBODB_VALUE_UINT64 && kind != TURBODB_VALUE_DOUBLE &&
            kind != TURBODB_VALUE_BOOLEAN && kind != TURBODB_VALUE_NULL)
          status = change_error(c, TURBODB_STATUS_UNSUPPORTED, "ORDER BY requires numeric boolean or NULL results");
      }
      if (status == TURBODB_STATUS_OK) status = change_expression_prepare(c, expression);
    }
    id = node->next;
  }
  return status;
}
/* Binding and validated scalar keys guarantee identical non-NULL kinds and finite
 * doubles. NULL sorts first ascending. No subtraction of integer endpoints. */
static int change_compare_row(const void *left, const void *right) {
  const change_sorted_row *a = left, *b = right;
  for (size_t i = 0; i < a->order_count; ++i) {
    const change_order *order = &a->orders[i];
    const bool computed = order->expression.program.budget != NULL;
    const int compared = orm_sql_value_order(computed ? &a->keys[order->key] : &a->row[order->column],
        computed ? &b->keys[order->key] : &b->row[order->column]);
    if (compared) return order->descending ? -compared : compared;
  }
  return 0;
}
static bool change_sorted_copy(void *to, const void *from) { *(change_sorted_row *)to = *(const change_sorted_row *)from; return true; }
static void change_sorted_move(void *to, void *from) { *(change_sorted_row *)to = *(change_sorted_row *)from; }
/* Records only borrow the immutable snapshot; destruction releases no owner. */
static void change_sorted_destroy(void *value) { (void)value; }
static const cmeta_type_traits change_sorted_traits = {
    CMETA_TRAIT_COMPARE | CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY |
        CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY,
    NULL, NULL, change_compare_row, change_sorted_copy, change_sorted_move, change_sorted_destroy};
static const cmeta_type_desc change_sorted_type = {"sql_change_sorted_row", sizeof(change_sorted_row),
    _Alignof(change_sorted_row), CMETA_T_OBJECT, NULL, &change_sorted_traits};
static turbodb_status_t change_eval(change_context *c, change_expression *expression,
    const turbodb_value_t *row, turbodb_value_t *out) {
  for (size_t i = 0; i < vec_size(&expression->slots); ++i) {
    const size_t slot = *(const size_t *)vec_at_const(&expression->slots, i);
    *(turbodb_value_t *)vec_at(&expression->arguments, i) = slot < c->schema.count ? row[slot] : c->parameters[slot - c->schema.count];
  }
  return orm_tidesdb_sql_expr_run_eval_row(&expression->run,vec_data_const(&expression->arguments),
      vec_size(&expression->arguments),row,c->schema.count,out,c->error);
}
static turbodb_status_t change_match(change_context *c, const turbodb_value_t *row, bool *matched) {
  change_expression *filter = vec_at(&c->expressions, 0);
  if (!filter->program.budget) { *matched = true; return TURBODB_STATUS_OK; }
  turbodb_value_t value;
  const turbodb_status_t status = change_eval(c, filter, row, &value);
  if (status == TURBODB_STATUS_OK) *matched = value.kind == TURBODB_VALUE_BOOLEAN && value.data.boolean_value;
  return status;
}
static turbodb_status_t change_row(change_context *c, const turbodb_value_t *original) {
  if (c->deleting) { *(turbodb_value_t *)vec_at(&c->values, c->changed++) = original[c->definition.primary_key]; return TURBODB_STATUS_OK; }
  if (c->candidate_row == SIZE_MAX)
    return change_error(c, TURBODB_STATUS_LIMIT_EXCEEDED,
        "selected row count overflow");
  ++c->candidate_row;
  turbodb_value_t *row = vec_data(&c->row); memcpy(row, original, c->schema.count * sizeof(*row));
  for (size_t i = 1; i < vec_size(&c->expressions); ++i) {
    change_expression *assignment = vec_at(&c->expressions, i);
    turbodb_value_t value, converted = turbodb_null();
    turbodb_status_t status = TURBODB_STATUS_OK;
    if (assignment->default_value) value = assignment->default_result;
    else status = change_eval(c, assignment, row, &value);
    if (status == TURBODB_STATUS_OK)
      status = change_assign(c, assignment->column, &value, &converted);
    if (status != TURBODB_STATUS_OK) return status;
    row[assignment->column] = converted;
  }
  bool changed = false;
  for (size_t i = 0; i < c->schema.count; ++i) {
    orm_sql_predicate equality; turbodb_value_t equal = turbodb_null();
    turbodb_status_t status = orm_tidesdb_sql_predicate_bind(ORM_SQL_NULL_SAFE_EQUAL, c->schema.columns[i].type,
        &c->schema.columns[i].type, &equality, c->error);
    if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_predicate_eval(&equality, &original[i], &row[i], c->store->budget, &equal, c->error);
    if (status != TURBODB_STATUS_OK) return status;
    changed = changed || !equal.data.boolean_value;
  }
  if (changed) {
    if (c->updates_key) *(turbodb_value_t *)vec_at(&c->old_keys, c->changed) = original[c->definition.primary_key];
    if (c->permissive)
      *(size_t *)vec_at(&c->row_numbers, c->changed) = c->candidate_row;
    memcpy((turbodb_value_t *)vec_data(&c->values) + c->changed * c->schema.count, row, c->schema.count * sizeof(*row));
    ++c->changed;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t change_capture(change_context *c, const turbodb_value_t *row, size_t index) {
  turbodb_value_t *copy = (turbodb_value_t *)vec_data(&c->snapshots) + index * c->schema.count;
  turbodb_value_t *keys = c->key_count ? (turbodb_value_t *)vec_data(&c->keys) + index * c->key_count : NULL;
  memcpy(copy, row, c->schema.count * sizeof(*copy));
  for (size_t i = 0; i < vec_size(&c->orders); ++i) {
    change_order *order = vec_at(&c->orders, i);
    if (!order->expression.program.budget) continue;
    turbodb_status_t status = change_eval(c, &order->expression, row, &keys[order->key]);
    if (status == TURBODB_STATUS_OK) status = change_validate(c, order->expression.program.result, &keys[order->key]);
    if (status != TURBODB_STATUS_OK) return status;
  }
  *(change_sorted_row *)vec_at(&c->sorted, index) = (change_sorted_row){
      copy, keys, vec_data_const(&c->orders), vec_size(&c->orders)};
  return TURBODB_STATUS_OK;
}
/* Count pass reserves a slot before accepting each match. Same owner snapshot
 * and immutable inputs make the materialization pass deterministic. */
static turbodb_status_t change_scan(change_context *c, bool materialize) {
  orm_sql_relation_source source = {0}; size_t matched = 0;
  const bool ordered = vec_size(&c->orders) != 0;
  turbodb_status_t status = orm_tidesdb_sql_relation_open(c->store, c->table, &source, c->error);
  while (status == TURBODB_STATUS_OK && (ordered || matched < c->limit)) {
    const turbodb_value_t *row = NULL; bool match = false;
    status = source.source.next(source.source.context, &row, c->error);
    if (status != TURBODB_STATUS_OK || !row) break;
    status = change_match(c, row, &match);
    if (status != TURBODB_STATUS_OK) break;
    if (!match) continue;
    if (!materialize) {
      if (c->matched == SIZE_MAX) { status = change_error(c, TURBODB_STATUS_LIMIT_EXCEEDED, "matched row count overflow"); break; }
      status = change_charge(c, ORM_SQL_BUDGET_MATERIALIZED_ROWS, 1);
      if (status == TURBODB_STATUS_OK) ++c->matched;
    } else {
      if (matched == c->matched) { status = change_error(c, TURBODB_STATUS_INTERNAL_ERROR, "snapshot match count changed"); break; }
      status = ordered ? change_capture(c, row, matched) : change_row(c, row);
    }
    if (status == TURBODB_STATUS_OK) ++matched;
  }
  if (status == TURBODB_STATUS_OK && materialize && matched != c->matched)
    status = change_error(c, TURBODB_STATUS_INTERNAL_ERROR, "snapshot match count changed");
  const turbodb_status_t closed = orm_tidesdb_sql_relation_close(&source, status == TURBODB_STATUS_OK ? c->error : NULL);
  if (closed != TURBODB_STATUS_OK || status == TURBODB_STATUS_INTERNAL_ERROR) c->store->failed = true;
  return status == TURBODB_STATUS_OK ? closed : status;
}
static turbodb_status_t change_materialize(change_context *c) {
  const bool ordered = vec_size(&c->orders) != 0;
  const size_t selected = c->matched < c->limit ? c->matched : (size_t)c->limit;
  const size_t columns = c->deleting ? 1 : c->schema.count;
  if (selected > SIZE_MAX / sizeof(turbodb_value_t) / columns ||
      (ordered && c->matched > SIZE_MAX / sizeof(turbodb_value_t) / c->schema.count) ||
      (c->key_count && c->matched > SIZE_MAX / sizeof(turbodb_value_t) / c->key_count))
    return change_error(c, TURBODB_STATUS_LIMIT_EXCEEDED, "materialized row capacity overflow");
  turbodb_status_t status = TURBODB_STATUS_OK;
  if (ordered) {
    status = change_charge(c, ORM_SQL_BUDGET_MATERIALIZED_ROWS, selected);
    if (status == TURBODB_STATUS_OK) c->selected_slots = selected;
    if (status == TURBODB_STATUS_OK) status = change_vector(c, &c->snapshots, c->matched * c->schema.count,
        sizeof(turbodb_value_t), _Alignof(turbodb_value_t), &c->snapshot_bytes);
    if (status == TURBODB_STATUS_OK) status = change_vector(c, &c->keys, c->matched * c->key_count,
        sizeof(turbodb_value_t), _Alignof(turbodb_value_t), &c->key_bytes);
    if (status == TURBODB_STATUS_OK) status = change_vector(c, &c->sorted, c->matched,
        sizeof(change_sorted_row), _Alignof(change_sorted_row), &c->sorted_bytes);
  }
  if (status == TURBODB_STATUS_OK) status = change_vector(c, &c->values, selected * columns,
      sizeof(turbodb_value_t), _Alignof(turbodb_value_t), &c->value_bytes);
  if (status == TURBODB_STATUS_OK && c->updates_key) status = change_vector(c, &c->old_keys, selected,
      sizeof(turbodb_value_t), _Alignof(turbodb_value_t), &c->old_key_bytes);
  if (status == TURBODB_STATUS_OK && c->permissive)
    status = change_vector(c, &c->row_numbers, selected, sizeof(size_t),
        _Alignof(size_t), &c->row_number_bytes);
  if (status == TURBODB_STATUS_OK) status = change_scan(c, true);
  if (status == TURBODB_STATUS_OK && ordered) status = orm_sql_work_sort(vec_data(&c->sorted), c->matched,
      &change_sorted_type, vec_size(&c->orders), c->store->budget, c->error);
  for (size_t i = 0; status == TURBODB_STATUS_OK && ordered && i < selected; ++i)
    status = change_row(c, ((const change_sorted_row *)vec_at_const(&c->sorted, i))->row);
  return status;
}
static turbodb_status_t change_write_ignore(change_context *c, size_t *written) {
  turbodb_status_t status = orm_sql_store_command_begin(c->store, c->error);
  const bool command = status == TURBODB_STATUS_OK;
  const size_t columns = c->schema.count;
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < c->changed; ++i) {
    const turbodb_value_t *row = (const turbodb_value_t *)vec_data_const(&c->values) +
        i * columns;
    status = c->updates_key ? orm_tidesdb_sql_relation_move_rows(c->store,
        c->table, (const turbodb_value_t *)vec_data_const(&c->old_keys) + i, row,
        1, columns, c->error) : orm_tidesdb_sql_relation_update_rows(c->store,
        c->table, row, 1, columns, c->error);
    if (status == TURBODB_STATUS_CONSTRAINT) {
      tdsql_error_init(c->error);
      status = change_conflict_warning(c,
          *(const size_t *)vec_at_const(&c->row_numbers, i));
      continue;
    }
    if (status == TURBODB_STATUS_OK) ++*written;
  }
  if (command)
    status = orm_sql_store_command_finish(c->store, status, c->error);
  return status;
}
static turbodb_status_t change_expression_close(change_context *c, change_expression *expression, turbodb_status_t status) {
  turbodb_status_t released = orm_tidesdb_sql_expr_run_close(&expression->run, status == TURBODB_STATUS_OK ? c->error : NULL);
  if (released != TURBODB_STATUS_OK) c->store->failed = true;
  if (status == TURBODB_STATUS_OK) status = released;
  released = orm_tidesdb_sql_expr_destroy(&expression->program, status == TURBODB_STATUS_OK ? c->error : NULL);
  if (released != TURBODB_STATUS_OK) c->store->failed = true;
  if (status == TURBODB_STATUS_OK) status = released;
  vec_t *vectors[] = {&expression->slots, &expression->query_slots,
      &expression->arguments};
  const size_t bytes[] = {expression->slot_bytes, expression->query_slot_bytes,
      expression->argument_bytes};
  for (size_t j = 0; j < sizeof(vectors) / sizeof(vectors[0]); ++j) {
    released = orm_sql_work_release(vectors[j], bytes[j], c->store->budget, status == TURBODB_STATUS_OK ? c->error : NULL);
    if (released != TURBODB_STATUS_OK) c->store->failed = true;
    if (status == TURBODB_STATUS_OK) status = released;
  }
  return status;
}
static turbodb_status_t change_readers_close(change_context *c, turbodb_status_t status) {
  for (size_t i = 0; i < vec_size(&c->expressions); ++i) {
    const turbodb_status_t released = orm_tidesdb_sql_expr_run_close(
        &((change_expression *)vec_at(&c->expressions, i))->run,
        status == TURBODB_STATUS_OK ? c->error : NULL);
    if (released != TURBODB_STATUS_OK) c->store->failed = true;
    if (status == TURBODB_STATUS_OK) status = released;
  }
  for (size_t i = 0; i < vec_size(&c->orders); ++i) {
    const turbodb_status_t released = orm_tidesdb_sql_expr_run_close(
        &((change_order *)vec_at(&c->orders, i))->expression.run,
        status == TURBODB_STATUS_OK ? c->error : NULL);
    if (released != TURBODB_STATUS_OK) c->store->failed = true;
    if (status == TURBODB_STATUS_OK) status = released;
  }
  const turbodb_status_t released = orm_sql_dependencies_close(&c->dependencies,
      status == TURBODB_STATUS_OK ? c->error : NULL);
  if (released != TURBODB_STATUS_OK) c->store->failed = true;
  return status == TURBODB_STATUS_OK ? released : status;
}
static turbodb_status_t change_close(change_context *c, turbodb_status_t status) {
  for (size_t i = 0; i < vec_size(&c->expressions); ++i)
    status = change_expression_close(c, vec_at(&c->expressions, i), status);
  for (size_t i = 0; i < vec_size(&c->orders); ++i)
    status = change_expression_close(c, &((change_order *)vec_at(&c->orders, i))->expression, status);
  {
    const turbodb_status_t released = orm_sql_dependencies_close(&c->dependencies,
        status == TURBODB_STATUS_OK ? c->error : NULL);
    if (released != TURBODB_STATUS_OK) c->store->failed = true;
    if (status == TURBODB_STATUS_OK) status = released;
  }
  vec_t *vectors[] = {&c->types, &c->offsets, &c->expressions, &c->row,
      &c->values, &c->sorted, &c->snapshots, &c->orders, &c->keys,
      &c->old_keys, &c->row_numbers};
  const size_t bytes[] = {c->type_bytes, c->offset_bytes, c->expression_bytes, c->row_bytes, c->value_bytes,
      c->sorted_bytes, c->snapshot_bytes, c->order_bytes, c->key_bytes,
      c->old_key_bytes, c->row_number_bytes};
  for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
    const turbodb_status_t released = orm_sql_work_release(vectors[i], bytes[i], c->store->budget, status == TURBODB_STATUS_OK ? c->error : NULL);
    if (released != TURBODB_STATUS_OK) c->store->failed = true;
    if (status == TURBODB_STATUS_OK) status = released;
  }
  const size_t slots[] = {c->matched, c->selected_slots};
  for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); ++i) if (slots[i]) {
    const turbodb_status_t released = orm_tidesdb_sql_budget_release(c->store->budget, ORM_SQL_BUDGET_MATERIALIZED_ROWS,
        slots[i], status == TURBODB_STATUS_OK ? c->error : NULL);
    if (released != TURBODB_STATUS_OK) c->store->failed = true;
    if (status == TURBODB_STATUS_OK) status = released;
  }
  const turbodb_status_t destroyed = orm_tidesdb_sql_catalog_destroy(&c->definition, status == TURBODB_STATUS_OK ? c->error : NULL);
  if (destroyed != TURBODB_STATUS_OK) c->store->failed = true;
  return status == TURBODB_STATUS_OK ? destroyed : status;
}
turbodb_status_t orm_tidesdb_sql_change_execute(const sqlparser_document *document,
    orm_sql_catalog_store *store, const turbodb_value_t *parameters, size_t parameter_count,
    size_t max_depth, uint64_t max_iterations, bool client_found_rows,
    orm_sql_diagnostics *diagnostics,
    size_t *affected,
    turbodb_error_t *error) {
  return orm_sql_change_execute_evaluation(document,store,parameters,parameter_count,
      max_depth,max_iterations,client_found_rows,
      (orm_sql_evaluation){.diagnostics=diagnostics},affected,error);
}
static turbodb_status_t change_process(change_context c, orm_sql_evaluation evaluation,
    bool client_found_rows, size_t *affected) {
  const sqlparser_document *document = c.document;
  orm_sql_catalog_store *store = c.store;
  const turbodb_value_t *parameters = c.parameters;
  const size_t parameter_count = c.parameter_count, max_depth = c.max_depth;
  const uint64_t max_iterations = c.max_iterations;
  turbodb_error_t *error = c.error;
  if (!document || (!affected && !c.binding_only) || !max_depth ||
      (parameter_count && (c.binding_only ? !c.binding_types : !parameters)))
    return change_error(&c, TURBODB_STATUS_INVALID_ARGUMENT, "invalid execution arguments");
  turbodb_status_t status = c.binding_only ? orm_sql_store_ready(store, error) :
      orm_sql_store_writable(store, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (sqlparser_get_dialect(document) != SQLPARSER_MYSQL || sqlparser_statements(document).count != 1)
    return change_error(&c, TURBODB_STATUS_UNSUPPORTED, "one MySQL UPDATE or DELETE required");
  const sqlparser_id root = sqlparser_statements(document).first;
  const sqlparser_node *statement = sqlparser_get_node(document, root);
  if (statement && statement->kind == SQLPARSER_WITH)
    statement = sqlparser_get_node(document, statement->as.with.body);
  if (!statement || (statement->kind != SQLPARSER_UPDATE && statement->kind != SQLPARSER_DELETE))
    return change_error(&c, TURBODB_STATUS_UNSUPPORTED, "expected UPDATE or DELETE");
  c.deleting = statement->kind == SQLPARSER_DELETE; c.offset = statement->span.offset;
  c.permissive = !c.deleting &&
      statement->as.update.conflict == SQLPARSER_CONFLICT_IGNORE;
  if (c.deleting ? (statement->as.delete_stmt.low_priority || statement->as.delete_stmt.indexed_by || statement->as.delete_stmt.not_indexed ||
          statement->as.delete_stmt.targets.count || statement->as.delete_stmt.from) :
      (statement->as.update.low_priority ||
          (statement->as.update.conflict != SQLPARSER_CONFLICT_DEFAULT &&
            !c.permissive) || statement->as.update.indexed_by ||
          statement->as.update.not_indexed || !statement->as.update.assignments.count))
    return change_error(&c, TURBODB_STATUS_UNSUPPORTED, "unsupported write clause or modifier");
  const sqlparser_id table = c.deleting ? statement->as.delete_stmt.table :
      statement->as.update.table;
  const sqlparser_id alias = c.deleting ? statement->as.delete_stmt.alias :
      statement->as.update.alias;
  status = change_table(&c, table);
  if (status == TURBODB_STATUS_OK) status = change_alias(&c, alias);
  if (status == TURBODB_STATUS_OK) status = change_parameters(&c);
  if (status == TURBODB_STATUS_OK) {
    orm_sql_query_scope scope = {document, root, vec_data_const(&c.types),
        parameter_count, max_depth, store->budget};
    scope.max_iterations = max_iterations;
    scope.dependency_schema = &c.schema;
    scope.dependency_qualifier = c.qualifier;
    scope.evaluation=evaluation;
    scope.evaluation.mode=c.deleting ? ORM_SQL_EVALUATION_QUERY :
        c.permissive ? ORM_SQL_EVALUATION_IGNORE_WRITE : ORM_SQL_EVALUATION_WRITE;
    status = c.binding_only ? orm_sql_dependencies_bind(&scope,store,&c.dependencies,error) :
        max_iterations ? orm_sql_dependencies_open_recursive(&scope, store,
        parameters, false, max_iterations, &c.dependencies, error) :
        orm_sql_dependencies_open(&scope, store, parameters, false,
            &c.dependencies, error);
    c.query_sources = (orm_sql_expr_query_sources){
        vec_data_const(&c.dependencies.sources), c.dependencies.query_count,scope.evaluation};
  }
  if (status == TURBODB_STATUS_OK) status = change_limit(&c, c.deleting ? statement->as.delete_stmt.limit : statement->as.update.limit);
  if (status == TURBODB_STATUS_OK) status = change_compile(&c, statement);
  if (status == TURBODB_STATUS_OK) status = change_bind_order(&c, c.deleting ? statement->as.delete_stmt.order_by : statement->as.update.order_by);
  if (status == TURBODB_STATUS_OK && !c.binding_only && c.limit) status = change_scan(&c, false);
  if (status == TURBODB_STATUS_OK && c.matched) status = change_materialize(&c);
  status = change_readers_close(&c, status);
  size_t written = c.changed;
  if (status == TURBODB_STATUS_OK && c.changed) {
    if (c.permissive) {
      written = 0;
      status = change_write_ignore(&c, &written);
    } else status = c.deleting ? orm_tidesdb_sql_relation_delete_rows(store,
        c.table, vec_data_const(&c.values), c.changed, error) :
        c.updates_key ? orm_tidesdb_sql_relation_move_rows(store, c.table,
            vec_data_const(&c.old_keys), vec_data_const(&c.values), c.changed,
            c.schema.count, error) : orm_tidesdb_sql_relation_update_rows(store,
            c.table, vec_data_const(&c.values), c.changed, c.schema.count, error);
  }
  const size_t selected = c.matched < c.limit ? c.matched : (size_t)c.limit;
  const size_t reported = !c.deleting && client_found_rows ? selected : written;
  status = change_close(&c, status);
  if (status == TURBODB_STATUS_OK && !c.binding_only) *affected = reported;
  return status;
}
turbodb_status_t orm_sql_change_execute_evaluation(const sqlparser_document *document,
    orm_sql_catalog_store *store, const turbodb_value_t *parameters, size_t parameter_count,
    size_t max_depth, uint64_t max_iterations, bool client_found_rows,
    orm_sql_evaluation evaluation, size_t *affected, turbodb_error_t *error) {
  return change_process((change_context){.document=document,.store=store,.parameters=parameters,
      .parameter_count=parameter_count,.max_depth=max_depth,.max_iterations=max_iterations,
      .limit=UINT64_MAX,.diagnostics=evaluation.diagnostics,.error=error},
      evaluation,client_found_rows,affected);
}
turbodb_status_t orm_sql_change_bind(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *store, turbodb_error_t *error) {
  if (!scope || !store || scope->budget != store->budget) {
    tdsql_error_set(error,TURBODB_STATUS_INVALID_ARGUMENT,"write binding requires the scope's Catalog budget");
    return TURBODB_STATUS_INVALID_ARGUMENT;
  }
  turbodb_status_t status = orm_sql_store_ready(store, error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_bind_write_scope(scope, error);
  if (status != TURBODB_STATUS_OK) return status;
  return change_process((change_context){.document=scope->document,.store=store,
      .binding_types=scope->parameter_types,.parameter_count=scope->parameter_count,
      .max_depth=scope->max_depth,.max_iterations=scope->max_iterations,.limit=UINT64_MAX,
      .diagnostics=scope->evaluation.diagnostics,.binding_only=true,.error=error},
      scope->evaluation,false,NULL);
}

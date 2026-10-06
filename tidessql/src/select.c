#include "select.h"
#include "binding.h"
#include "work.h"
#include "name.h"
#include "error.h"
#include <stdio.h>
#include <string.h>

typedef struct select_computation {
  sqlparser_id root; /* Binding only; never dereferenced after bind. */
  orm_sql_expr program;
  vec_t slots, query_slots;
  size_t slot_bytes, query_slot_bytes;
} select_computation;
typedef orm_sql_expression_target select_expression_target;
static select_expression_target select_target(select_computation *computation) {
  return (select_expression_target){&computation->program,&computation->slots,&computation->slot_bytes,
      &computation->query_slots,&computation->query_slot_bytes};
}
static orm_sql_scan_expression select_scan_expression(select_computation *computation) {
  return (orm_sql_scan_expression){&computation->program,vec_data_const(&computation->slots),vec_size(&computation->slots),
      vec_data_const(&computation->query_slots),vec_size(&computation->query_slots)};
}
typedef struct select_group_run {
  orm_sql_scan input;
  orm_sql_row_source source;
  orm_sql_aggregate aggregate;
  vec_t items, expressions, types, projection;
  size_t item_bytes, expression_bytes, type_bytes, projection_bytes;
} select_group_run;
typedef struct select_window_clause {
  sqlparser_id node;
  sqlparser_id frame; /* Binding only. */
  sqlparser_list partition_by, order_by; /* Resolved AST lists, binding only. */
  orm_sql_window_kind kind;
  orm_sql_select_bound argument;
  size_t first_key, partition_count, order_count, value_count, canonical, frame_slot;
} select_window_clause;
typedef struct select_window_run {
  orm_sql_scan input;
  orm_sql_row_source source;
  vec_t stages;
  size_t stage_bytes;
} select_window_run;

typedef struct select_named_window {
  sqlparser_id node, frame;
  vstr name;
  size_t parent;
  sqlparser_list partition_by, order_by;
  bool resolved;
} select_named_window;

typedef struct select_frame_boundary {
  sqlparser_boundary_kind kind;
  turbodb_value_t literal;
  size_t parameter, offset;
  bool is_parameter;
} select_frame_boundary;
enum { SELECT_FRAME_BOUNDARIES = 2 };
typedef struct select_window_frame {
  sqlparser_frame_unit unit;
  select_frame_boundary boundaries[SELECT_FRAME_BOUNDARIES];
} select_window_frame;
typedef struct select_binder {
  const sqlparser_document *document;
  const orm_sql_table_schema *schema;
  orm_sql_select plan;
  vstr qualifier;
  bool composed;
  bool scalar_output, anonymous_output;
  bool no_from, hidden_input;
  const orm_sql_table_schema *outer_schema;
  vstr outer_qualifier;
  size_t parameter_marker_count;
  const bool *parameter_resolved;
  bool correlated;
  size_t offset;
  turbodb_error_t *error;
  const size_t *substitutions;
  size_t substitution_count;
  const orm_sql_expr_query_binding *queries;
  size_t query_count;
  vec_t nested_nodes;
  size_t nested_node_bytes;
  vec_t group_capture;
  size_t group_capture_bytes;
  vec_t named_windows;
  size_t named_window_bytes;
} select_binder;

static turbodb_status_t select_error(turbodb_error_t *error, turbodb_status_t status,
    size_t offset, const char *reason) {
  char message[TURBODB_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message), "TidesDB SQL SELECT at byte %zu: %s", offset, reason);
  tdsql_error_set(error, status, message);
  return status;
}
static turbodb_status_t bind_error(select_binder *b, turbodb_status_t status, const char *reason) {
  return select_error(b->error, status, b->offset, reason);
}
static turbodb_status_t bind_charge(select_binder *b, orm_sql_budget_resource resource, size_t count) {
  orm_sql_budget_amount amount = {0}; amount.value[resource] = count;
  return orm_tidesdb_sql_budget_reserve(b->plan.budget, &amount, b->error);
}
static turbodb_status_t select_vector(select_binder *b, vec_t *v, size_t count,
    size_t size, size_t align, size_t *bytes) {
  return orm_sql_work_zero(v, count, size, align, b->plan.budget, bytes, b->error);
}

static turbodb_status_t bind_parameters(select_binder *b, const orm_sql_type *types, size_t count) {
  const size_t outer=b->outer_schema?b->outer_schema->count:0;
  if(count>SIZE_MAX-outer || count+outer>b->plan.budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES])
    return bind_error(b,TURBODB_STATUS_LIMIT_EXCEEDED,"SELECT parameter and correlation width exceeds plan capacity");
  b->parameter_marker_count=count;
  turbodb_status_t status = orm_sql_bind_parameter_offsets(b->document, count, b->plan.budget,
      &b->plan.parameter_offsets, &b->plan.parameter_offset_bytes, b->error);
  if (status == TURBODB_STATUS_OK)
    status = select_vector(b, &b->plan.parameter_types, count+outer, sizeof(orm_sql_type),
        _Alignof(orm_sql_type), &b->plan.parameter_type_bytes);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < count+outer; ++i) {
    const orm_sql_type type=i<count?types[i]:b->outer_schema->columns[i-count].type;
    if(i<count&&b->parameter_resolved&&!b->parameter_resolved[i]) continue;
    orm_sql_predicate validator;
    status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL, type, NULL, &validator, b->error);
    if (status == TURBODB_STATUS_OK) *(orm_sql_type *)vec_at(&b->plan.parameter_types, i) = type;
  }
  return status;
}
static turbodb_status_t parameter_slot(select_binder *b, const sqlparser_node *node, size_t *slot) {
  size_t first = 0, last = vec_size(&b->plan.parameter_offsets);
  while (first < last) {
    const turbodb_status_t status = bind_charge(b, ORM_SQL_BUDGET_EXECUTION_STEPS, 1);
    if (status != TURBODB_STATUS_OK) return status;
    const size_t mid = first + (last - first) / 2;
    if (*(const uint64_t *)vec_at_const(&b->plan.parameter_offsets, mid) < node->span.offset) first = mid + 1;
    else last = mid;
  }
  if (first == vec_size(&b->plan.parameter_offsets) ||
      *(const uint64_t *)vec_at_const(&b->plan.parameter_offsets, first) != node->span.offset)
    return bind_error(b, TURBODB_STATUS_INTERNAL_ERROR, "parameter occurrence is not indexed");
  if(b->parameter_resolved&&!b->parameter_resolved[first])
    return bind_error(b,TURBODB_STATUS_INVALID_STATE,"parameter type is unresolved");
  *slot = first;
  return TURBODB_STATUS_OK;
}
static bool name_equal(vstr a, vstr b) {
  return a.len == b.len && (!a.len || !memcmp(a.data, b.data, a.len));
}
static turbodb_status_t plain_name(select_binder *b, vstr name) {
  const char *reason = NULL;
  const turbodb_status_t status = orm_sql_name_validate(name, &reason);
  return status == TURBODB_STATUS_OK ? status : bind_error(b, status, reason);
}
static turbodb_status_t name_part(select_binder *b, vstr *text, vstr *out) {
  const char *reason = NULL;
  const turbodb_status_t status = orm_sql_name_part(text, out, &reason);
  return status == TURBODB_STATUS_OK ? status : bind_error(b, status, reason);
}
static turbodb_status_t node_name(select_binder *b, sqlparser_id id, bool qualified,
    vstr *out, bool *star, vstr *qualifier) {
  const sqlparser_node *node = sqlparser_get_node(b->document, id);
  if (!node) return bind_error(b, TURBODB_STATUS_INVALID_ARGUMENT, "missing name AST");
  b->offset = node->span.offset;
  if (node->kind != SQLPARSER_NAME && !(star && node->kind == SQLPARSER_STAR))
    return bind_error(b, TURBODB_STATUS_UNSUPPORTED, "expected a column or identifier");
  vstr text = {sqlparser_text(b->document, node->span), node->span.length};
  if (star) *star = false;
  if (qualifier) *qualifier = (vstr){0};
  if (star && node->kind == SQLPARSER_STAR && text.len == 1 && text.data[0] == '*') {
    *star = true; *out = (vstr){0}; return TURBODB_STATUS_OK;
  }
  turbodb_status_t status = name_part(b, &text, out);
  if (status != TURBODB_STATUS_OK) return status;
  if (text.len && text.data[0] == '.' && qualified) {
    bool known = !b->composed && name_equal(*out,b->qualifier);
    for (size_t i = 0; b->composed && !known && i < b->schema->count; ++i) {
      status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,1);
      if (status != TURBODB_STATUS_OK) return status;
      known = name_equal(*out,b->schema->columns[i].qualifier);
    }
    if(!known&&b->outer_schema) {
      known=name_equal(*out,b->outer_qualifier);
      for(size_t i=0;!known&&i<b->outer_schema->count;++i) {
        status=bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,1);
        if(status!=TURBODB_STATUS_OK) return status;
        known=name_equal(*out,b->outer_schema->columns[i].qualifier);
      }
    }
    if (!known) return bind_error(b, TURBODB_STATUS_SQL_ERROR, "unknown table qualifier");
    if (qualifier) *qualifier = *out;
    ++text.data; --text.len; orm_sql_name_space(&text);
    if (star && node->kind == SQLPARSER_STAR && text.len == 1 && text.data[0] == '*') {
      *star = true; *out = (vstr){0}; return TURBODB_STATUS_OK;
    }
    status = name_part(b, &text, out);
    if (status != TURBODB_STATUS_OK) return status;
  }
  if (text.len || node->kind == SQLPARSER_STAR)
    return bind_error(b, TURBODB_STATUS_UNSUPPORTED, "unsupported identifier qualification or quoting");
  return TURBODB_STATUS_OK;
}
static turbodb_status_t find_column(select_binder *b, sqlparser_id node, size_t *out) {
  const orm_sql_binding_scope scope = {.document=b->document,.schema=b->schema,.qualifier=b->qualifier,.budget=b->plan.budget,
      .parameter_types=vec_data_const(&b->plan.parameter_types),.parameter_count=vec_size(&b->plan.parameter_types),
      .parameter_resolved=b->parameter_resolved,
      .parameter_marker_count=b->parameter_marker_count,.outer_schema=b->outer_schema,.outer_qualifier=b->outer_qualifier,
      .correlated=&b->correlated,.hidden_input=b->hidden_input};
  return orm_sql_bind_column(&scope,node,out,b->error);
}
static turbodb_status_t bind_schema(select_binder *b, bool anonymous, bool labels) {
  turbodb_status_t status = b->composed ? TURBODB_STATUS_OK : plain_name(b, b->schema->name);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < b->schema->count; ++i) {
    const orm_sql_schema_column *column = &b->schema->columns[i];
    status = anonymous && !column->name.len ? TURBODB_STATUS_OK : labels && column->name.len && column->name.data ?
        (column->name.len <= ORM_SQL_SELECT_NAME_BYTES ? TURBODB_STATUS_OK :
          bind_error(b,TURBODB_STATUS_LIMIT_EXCEEDED,"output label exceeds name capacity")) : plain_name(b, column->name);
    if (status == TURBODB_STATUS_OK && b->composed) status = plain_name(b,column->qualifier);
    orm_sql_predicate validator;
    if (status == TURBODB_STATUS_OK)
      status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL, column->type, NULL, &validator, b->error);
    for (size_t j = 0; status == TURBODB_STATUS_OK && j < i; ++j) {
      status = bind_charge(b, ORM_SQL_BUDGET_EXECUTION_STEPS, 1);
      if (status == TURBODB_STATUS_OK && !(anonymous && b->anonymous_output) && name_equal(column->name, b->schema->columns[j].name) &&
          (!b->composed || name_equal(column->qualifier,b->schema->columns[j].qualifier)))
        status = bind_error(b, TURBODB_STATUS_SQL_ERROR, "duplicate schema column");
    }
  }
  if (status == TURBODB_STATUS_OK)
    status = select_vector(b, &b->plan.types, b->schema->count, sizeof(orm_sql_type),
        _Alignof(orm_sql_type), &b->plan.type_bytes);
  if (status == TURBODB_STATUS_OK)
    for (size_t i = 0; i < b->schema->count; ++i)
      *(orm_sql_type *)vec_at(&b->plan.types, i) = b->schema->columns[i].type;
  return status;
}
static turbodb_status_t bind_table(select_binder *b, sqlparser_id id) {
  const sqlparser_node *table = sqlparser_get_node(b->document, id);
  if (!table || table->kind != SQLPARSER_TABLE || table->as.table.query ||
      table->as.table.arguments.count || table->as.table.indexed_by || table->as.table.group ||
      table->as.table.table_function || table->as.table.not_indexed)
    return bind_error(b, TURBODB_STATUS_UNSUPPORTED, "SELECT requires one plain table");
  vstr name;
  turbodb_status_t status = node_name(b, table->as.table.name, false, &name, NULL, NULL);
  if (status != TURBODB_STATUS_OK) return status;
  if (!name_equal(name, b->schema->name))
    return bind_error(b, TURBODB_STATUS_SQL_ERROR, "unknown table in declared schema");
  b->qualifier = name;
  if (table->as.table.alias)
    status = node_name(b, table->as.table.alias, false, &b->qualifier, NULL, NULL);
  return status;
}
static turbodb_status_t append_column(select_binder *b, size_t output, size_t slot, vstr name, orm_sql_type type) {
  if (name.len > ORM_SQL_SELECT_NAME_BYTES)
    return bind_error(b,TURBODB_STATUS_LIMIT_EXCEEDED,"output label exceeds name capacity");
  turbodb_status_t status = bind_charge(b, ORM_SQL_BUDGET_PLAN_NODES, 1);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < output; ++i) {
    const orm_sql_select_column *previous = vec_at_const(&b->plan.columns, i);
    status = bind_charge(b, ORM_SQL_BUDGET_EXECUTION_STEPS, 1);
    if (status == TURBODB_STATUS_OK && !b->anonymous_output && name_equal(name, (vstr){previous->name, strlen(previous->name)}))
      status = bind_error(b, TURBODB_STATUS_SQL_ERROR, "duplicate output column name");
  }
  if (status != TURBODB_STATUS_OK) return status;
  orm_sql_select_column *column = vec_at(&b->plan.columns, output);
  memcpy(column->name, name.data, name.len); column->name[name.len] = '\0';
  column->type = type;
  *(size_t *)vec_at(&b->plan.projection, output) = slot;
  return TURBODB_STATUS_OK;
}
static turbodb_status_t select_star_slot(select_binder *b, vstr qualifier, size_t ordinal, size_t *out) {
  if (!b->composed) { *out = ordinal; return TURBODB_STATUS_OK; }
  size_t qualified = 0;
  for (size_t i = 0; i < b->schema->count; ++i) {
    const turbodb_status_t status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,1);
    if (status != TURBODB_STATUS_OK) return status;
    const orm_sql_schema_column *column = &b->schema->columns[i];
    if (qualifier.len) {
      if (name_equal(qualifier,column->qualifier) && qualified++ == ordinal) { *out = i; return TURBODB_STATUS_OK; }
    } else if (!column->qualified_only && (column->star_order ? column->star_order : i+1) == ordinal+1) {
      *out = i; return TURBODB_STATUS_OK;
    }
  }
  return bind_error(b,TURBODB_STATUS_INVALID_STATE,"invalid FROM star column order");
}
static turbodb_status_t bind_projection(select_binder *b, sqlparser_list list) {
  if (b->scalar_output && list.count != 1)
    return bind_error(b,TURBODB_STATUS_SQL_ERROR,"scalar subquery requires exactly one output column");
  size_t count = 0;
  bool has_computations = false;
  /* Two bounded passes size star expansion before reserving any output. */
  for (unsigned pass = 0; pass < 2; ++pass) {
    size_t output = 0;
    for (sqlparser_id id = list.first; id;) {
      const sqlparser_node *item = sqlparser_get_node(b->document, id);
      const sqlparser_node *expression = sqlparser_get_node(b->document, item->as.projection.expression);
      if (!expression) return bind_error(b, TURBODB_STATUS_INVALID_ARGUMENT, "missing projection expression");
      bool computed = expression->kind != SQLPARSER_NAME && expression->kind != SQLPARSER_STAR;
      vstr name = vstr_from_cstr(""), qualifier = {0}; bool star = false;
      b->offset = expression->span.offset;
      if (computed && !item->as.projection.alias && !b->scalar_output && !b->anonymous_output && !b->no_from)
        return bind_error(b, TURBODB_STATUS_UNSUPPORTED, "computed projection requires an explicit alias");
      turbodb_status_t status = computed ? TURBODB_STATUS_OK :
          node_name(b, item->as.projection.expression, true, &name, &star, &qualifier);
      if (status != TURBODB_STATUS_OK) return status;
      if (star && b->no_from) return bind_error(b,TURBODB_STATUS_SQL_ERROR,"star projection requires FROM");
      if (computed && !item->as.projection.alias && b->no_from && !b->scalar_output && !b->anonymous_output)
        name = (vstr){sqlparser_text(b->document,expression->span),expression->span.length};
      size_t slot = 0;
      if (star && item->as.projection.alias)
        return bind_error(b, TURBODB_STATUS_UNSUPPORTED, "star cannot have an alias");
      if (!star) {
        if (!computed) {
          status = find_column(b, item->as.projection.expression, &slot);
          if(status==TURBODB_STATUS_OK&&slot>=b->schema->count) { computed=true; slot=0; }
        }
        if (status == TURBODB_STATUS_OK && item->as.projection.alias)
          status = node_name(b, item->as.projection.alias, false, &name, NULL, NULL);
        if (status != TURBODB_STATUS_OK) return status;
      }
      size_t width = star ? b->schema->count : 1;
      if (star && b->composed) {
        width = 0;
        for (size_t i = 0; i < b->schema->count; ++i) {
          status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,1);
          if (status != TURBODB_STATUS_OK) return status;
          if (qualifier.len ? name_equal(qualifier,b->schema->columns[i].qualifier) : !b->schema->columns[i].qualified_only) ++width;
        }
      }
      if (width > SIZE_MAX - output || output + width >
          b->plan.budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES])
        return bind_error(b, TURBODB_STATUS_LIMIT_EXCEEDED, "projection exceeds plan capacity");
      has_computations = has_computations || computed;
      if (pass) {
        for (size_t i = 0; i < width; ++i) {
          size_t selected = slot;
          if (star) status = select_star_slot(b,qualifier,i,&selected);
          if (status == TURBODB_STATUS_OK) status = append_column(b, output + i, selected,
              star ? b->schema->columns[selected].name : name,
              computed ? (orm_sql_type){TURBODB_VALUE_NULL, true} : b->schema->columns[selected].type);
          if (status != TURBODB_STATUS_OK) return status;
        }
        if (computed)
          ((select_computation *)vec_at(&b->plan.computations, output))->root = item->as.projection.expression;
      }
      output += width; id = item->next;
    }
    if (!pass) {
      count = output;
      if (!count) return bind_error(b, TURBODB_STATUS_SQL_ERROR, "SELECT has no output columns");
      turbodb_status_t status = select_vector(b, &b->plan.columns, count, sizeof(orm_sql_select_column),
          _Alignof(orm_sql_select_column), &b->plan.column_bytes);
      if (status == TURBODB_STATUS_OK)
        status = select_vector(b, &b->plan.projection, count, sizeof(size_t),
            _Alignof(size_t), &b->plan.projection_bytes);
      if (status == TURBODB_STATUS_OK && has_computations)
        status = select_vector(b, &b->plan.computations, count, sizeof(select_computation),
            _Alignof(select_computation), &b->plan.computation_bytes);
      if (status == TURBODB_STATUS_OK && has_computations)
        status = select_vector(b, &b->plan.expressions, count, sizeof(orm_sql_scan_expression),
            _Alignof(orm_sql_scan_expression), &b->plan.expression_bytes);
      if (status != TURBODB_STATUS_OK) return status;
    }
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t bind_integer(select_binder *b, sqlparser_id id, orm_sql_select_bound *out) {
  const sqlparser_node *node = sqlparser_get_node(b->document, id);
  if (!node) return bind_error(b, TURBODB_STATUS_INVALID_ARGUMENT, "missing LIMIT argument");
  b->offset = node->span.offset;
  if (node->kind == SQLPARSER_PARAMETER) {
    turbodb_status_t status = parameter_slot(b, node, &out->parameter);
    if (status != TURBODB_STATUS_OK) return status;
    const orm_sql_type *type = vec_at_const(&b->plan.parameter_types, out->parameter);
    if (type->kind != TURBODB_VALUE_INT64 && type->kind != TURBODB_VALUE_UINT64)
      return bind_error(b, TURBODB_STATUS_TYPE_ERROR, "LIMIT parameter type must be I64 or U64");
    out->is_parameter = true;
    return TURBODB_STATUS_OK;
  }
  if (node->kind != SQLPARSER_NUMBER)
    return bind_error(b, TURBODB_STATUS_UNSUPPORTED, "LIMIT requires nonnegative integer literals or parameters");
  turbodb_value_t value;
  turbodb_error_t cause; tdsql_error_init(&cause);
  turbodb_status_t status = orm_tidesdb_sql_integer_literal(
      (vstr){sqlparser_text(b->document, node->span), node->span.length}, false, &value, &cause);
  if (status != TURBODB_STATUS_OK) return bind_error(b, status, cause.message);
  out->literal = value.kind == TURBODB_VALUE_UINT64 ? value.data.uint64_value : (uint64_t)value.data.int64_value;
  return TURBODB_STATUS_OK;
}
static turbodb_status_t bind_limit(select_binder *b, sqlparser_id id) {
  b->plan.limit.literal = UINT64_MAX;
  if (!id) return TURBODB_STATUS_OK;
  const sqlparser_node *node = sqlparser_get_node(b->document, id);
  turbodb_status_t status = bind_integer(b, node->as.limit.count, &b->plan.limit);
  if (status == TURBODB_STATUS_OK && node->as.limit.offset)
    status = bind_integer(b, node->as.limit.offset, &b->plan.offset);
  return status;
}

static turbodb_status_t bind_expression(select_binder *b, sqlparser_id root, size_t max_depth,
    select_expression_target target, bool predicate) {
  const orm_sql_binding_scope scope = {.document = b->document, .schema = b->schema, .qualifier = b->qualifier,
      .parameter_types = vec_data_const(&b->plan.parameter_types), .parameter_offsets = vec_data_const(&b->plan.parameter_offsets),
      .parameter_resolved=b->parameter_resolved,
      .parameter_count = vec_size(&b->plan.parameter_types), .budget = b->plan.budget,
      .parameter_marker_count=b->parameter_marker_count,.outer_schema=b->outer_schema,.outer_qualifier=b->outer_qualifier,
      .correlated=&b->correlated,
      .substitutions=b->substitutions,.substitution_count=b->substitution_count,
      .queries=b->queries,.query_count=b->query_count,.hidden_input=b->hidden_input,
      .capture_slots=b->substitutions?vec_data_const(&b->group_capture):NULL,
      .capture_count=b->substitutions?vec_size(&b->group_capture):0};
  return orm_sql_bind_expression(&scope, root, max_depth,
      target, predicate, b->error);
}

static turbodb_status_t bind_computations(select_binder *b, size_t max_depth) {
  for (size_t i = 0; i < vec_size(&b->plan.computations); ++i) {
    select_computation *computation = vec_at(&b->plan.computations, i);
    if (!computation->root) continue;
    const select_expression_target target = select_target(computation);
    const turbodb_status_t status = bind_expression(b, computation->root, max_depth, target, false);
    if (status != TURBODB_STATUS_OK) return status;
    ((orm_sql_select_column *)vec_at(&b->plan.columns, i))->type = computation->program.result;
    *(orm_sql_scan_expression *)vec_at(&b->plan.expressions, i) = select_scan_expression(computation);
    computation->root = 0;
  }
  return TURBODB_STATUS_OK;
}

static bool select_inside(const sqlparser_node *node, const sqlparser_node *root) {
  return root && node->span.offset >= root->span.offset &&
      node->span.offset-root->span.offset <= root->span.length &&
      node->span.length <= root->span.length-(node->span.offset-root->span.offset);
}
static bool select_nested(const select_binder *b, size_t index) {
  return vec_size(&b->nested_nodes) && *(const unsigned char *)vec_at_const(&b->nested_nodes,index);
}
/* Query bodies form opaque scopes, including compound tails outside their leaf
 * SELECT spans. This scratch mask is independent of dependency admission. */
static turbodb_status_t bind_query_scope(select_binder *b, const sqlparser_node *statement) {
  const size_t nodes = sqlparser_node_count(b->document);
  turbodb_status_t status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,nodes);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < nodes; ++i) {
    const sqlparser_node *query = sqlparser_get_node(b->document,(sqlparser_id)(i+1));
    if (query == statement || select_nested(b,i) || !select_inside(query,statement) ||
        (query->kind != SQLPARSER_SELECT && query->kind != SQLPARSER_UNION && query->kind != SQLPARSER_QUERY_GROUP)) continue;
    if (!vec_size(&b->nested_nodes)) status = select_vector(b,&b->nested_nodes,nodes,
        sizeof(unsigned char),_Alignof(unsigned char),&b->nested_node_bytes);
    if (status == TURBODB_STATUS_OK) status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,nodes);
    for (size_t j = 0; status == TURBODB_STATUS_OK && j < nodes; ++j)
      if (select_inside(sqlparser_get_node(b->document,(sqlparser_id)(j+1)),query))
        *(unsigned char *)vec_at(&b->nested_nodes,j) = 1;
  }
  return status;
}
static bool select_window_name_same(vstr left, vstr right) {
  if (left.len != right.len) return false;
  for (size_t i = 0; i < left.len; ++i) {
    const char a = left.data[i], c = right.data[i];
    if ((a >= 'a' && a <= 'z' ? a-('a'-'A') : a) !=
        (c >= 'a' && c <= 'z' ? c-('a'-'A') : c)) return false;
  }
  return true;
}
static turbodb_status_t bind_window_parent(select_binder *b, sqlparser_id id, size_t *out) {
  vstr name = {0}; const char *reason = NULL;
  turbodb_status_t status = orm_sql_name_node(b->document,id,&name,&reason);
  if (status != TURBODB_STATUS_OK) return bind_error(b,status,reason);
  const size_t count = vec_size(&b->named_windows);
  status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,count);
  if (status != TURBODB_STATUS_OK) return status;
  for (size_t i = 0; i < count; ++i) {
    const select_named_window *window = vec_at_const(&b->named_windows,i);
    if (select_window_name_same(window->name,name)) { *out=i; return TURBODB_STATUS_OK; }
  }
  return bind_error(b,TURBODB_STATUS_SQL_ERROR,"unknown named window in this query block");
}
static turbodb_status_t bind_window_inherit(select_binder *b, const select_named_window *parent,
    sqlparser_list *partitions, sqlparser_list *orders) {
  if (parent->frame)
    return bind_error(b,TURBODB_STATUS_SQL_ERROR,"cannot inherit a window with an explicit frame");
  if (partitions->count)
    return bind_error(b,TURBODB_STATUS_SQL_ERROR,"inherited windows cannot add PARTITION BY");
  if (orders->count && parent->order_by.count)
    return bind_error(b,TURBODB_STATUS_SQL_ERROR,"inherited windows cannot redefine ORDER BY");
  *partitions = parent->partition_by;
  if (!orders->count) *orders = parent->order_by;
  return TURBODB_STATUS_OK;
}
/* Fixed binding scratch. Resolve parent edges once, then scan ready nodes;
 * O(D^2) work is step-budgeted, and no inheritance depth consumes C stack. */
static turbodb_status_t bind_named_windows(select_binder *b, sqlparser_list definitions) {
  const size_t count = definitions.count;
  if (!count) return TURBODB_STATUS_OK;
  turbodb_status_t status = bind_charge(b,ORM_SQL_BUDGET_PLAN_NODES,count);
  if (status == TURBODB_STATUS_OK) status = select_vector(b,&b->named_windows,count,
      sizeof(select_named_window),_Alignof(select_named_window),&b->named_window_bytes);
  size_t index = 0;
  for (sqlparser_id id = definitions.first; status == TURBODB_STATUS_OK && id;) {
    const sqlparser_node *node = sqlparser_get_node(b->document,id);
    b->offset = node->span.offset;
    select_named_window *window = vec_at(&b->named_windows,index);
    window->node=id; window->parent=SIZE_MAX; window->frame=node->as.window_definition.frame;
    window->partition_by=node->as.window_definition.partition_by;
    window->order_by=node->as.window_definition.order_by;
    const char *reason = NULL;
    status = orm_sql_name_node(b->document,node->as.window_definition.name,&window->name,&reason);
    if (status != TURBODB_STATUS_OK) return bind_error(b,status,reason);
    status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,index+1);
    for (size_t i = 0; status == TURBODB_STATUS_OK && i < index; ++i)
      if (select_window_name_same(((const select_named_window *)vec_at_const(&b->named_windows,i))->name,window->name))
        return bind_error(b,TURBODB_STATUS_SQL_ERROR,"duplicate named window in this query block");
    id=node->next; ++index;
  }
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < count; ++i) {
    select_named_window *window = vec_at(&b->named_windows,i);
    const sqlparser_node *node = sqlparser_get_node(b->document,window->node);
    b->offset=node->span.offset;
    if (node->as.window_definition.base)
      status = bind_window_parent(b,node->as.window_definition.base,&window->parent);
  }
  size_t remaining = count;
  while (status == TURBODB_STATUS_OK && remaining) {
    const size_t before = remaining;
    status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,count);
    for (size_t i = 0; status == TURBODB_STATUS_OK && i < count; ++i) {
      select_named_window *window = vec_at(&b->named_windows,i);
      if (window->resolved) continue;
      if (window->parent != SIZE_MAX) {
        const select_named_window *parent = vec_at_const(&b->named_windows,window->parent);
        if (!parent->resolved) continue;
        b->offset=sqlparser_get_node(b->document,window->node)->span.offset;
        status = bind_window_inherit(b,parent,&window->partition_by,&window->order_by);
        if (status != TURBODB_STATUS_OK) break;
      }
      window->resolved=true; --remaining;
    }
    if (status == TURBODB_STATUS_OK && before == remaining)
      return bind_error(b,TURBODB_STATUS_SQL_ERROR,"cycle in named window inheritance");
  }
  return status;
}
static turbodb_status_t bind_window_admission(select_binder *b, const sqlparser_node *statement) {
  const size_t nodes = sqlparser_node_count(b->document);
  size_t count = 0;
  turbodb_status_t status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,nodes);
  if (status != TURBODB_STATUS_OK) return status;
  for (size_t i = 1; i <= nodes; ++i) {
    const sqlparser_node *node = sqlparser_get_node(b->document,(sqlparser_id)i);
    if (!select_nested(b,i-1) && select_inside(node,statement) && node->kind == SQLPARSER_WINDOW) ++count;
  }
  if (!count) return TURBODB_STATUS_OK;
  status = bind_charge(b,ORM_SQL_BUDGET_PLAN_NODES,count);
  if (status == TURBODB_STATUS_OK) status = select_vector(b,&b->plan.windows,count,sizeof(select_window_clause),
      _Alignof(select_window_clause),&b->plan.window_bytes);
  size_t index = 0;
  for (size_t i = 1; status == TURBODB_STATUS_OK && i <= nodes; ++i) {
    const sqlparser_node *node = sqlparser_get_node(b->document,(sqlparser_id)i);
    if (select_nested(b,i-1) || !select_inside(node,statement) || node->kind != SQLPARSER_WINDOW) continue;
    b->offset = node->span.offset;
    bool allowed = false;
    const sqlparser_list lists[] = {statement->as.select.columns,statement->as.select.order_by};
    for (size_t list = 0; list < sizeof(lists)/sizeof(lists[0]); ++list)
      for (sqlparser_id id = lists[list].first; id;) {
        const sqlparser_node *item = sqlparser_get_node(b->document,id); id = item->next;
        status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,1);
        if (status != TURBODB_STATUS_OK) return status;
        allowed = allowed || select_inside(node,sqlparser_get_node(b->document,
            list ? item->as.order.expression : item->as.projection.expression));
      }
    if (!allowed) return bind_error(b,TURBODB_STATUS_SQL_ERROR,"window functions are allowed only in SELECT and ORDER BY");
    select_window_clause *window = vec_at(&b->plan.windows,index++);
    window->node = (sqlparser_id)i;
    window->frame_slot=SIZE_MAX;
    window->frame=node->as.window.frame;
    window->partition_by=node->as.window.partition_by; window->order_by=node->as.window.order_by;
    const sqlparser_id reference = node->as.window.name ? node->as.window.name : node->as.window.base;
    if (reference) {
      size_t parent;
      status = bind_window_parent(b,reference,&parent);
      if (status == TURBODB_STATUS_OK) {
        const select_named_window *definition = vec_at_const(&b->named_windows,parent);
        if (node->as.window.name) {
          window->partition_by=definition->partition_by; window->order_by=definition->order_by;
          window->frame=definition->frame;
        } else status=bind_window_inherit(b,definition,&window->partition_by,&window->order_by);
      }
      if (status != TURBODB_STATUS_OK) return status;
    }
    const sqlparser_node *call = sqlparser_get_node(b->document,node->as.window.call);
    if (!orm_sql_window_kind_at(b->document,call,&window->kind))
      return bind_error(b,TURBODB_STATUS_UNSUPPORTED,"unsupported SQL window function");
    const bool offset = orm_sql_window_offset_kind(window->kind);
    const bool aggregate_window = orm_sql_window_aggregate_kind(window->kind);
    const bool frame_value = orm_sql_window_frame_kind(window->kind), nth = window->kind == ORM_SQL_NTH_VALUE;
    const size_t arguments = call->as.call.arguments.count;
    const size_t arity = nth ? 2u : (frame_value || window->kind == ORM_SQL_NTILE) ? 1u : 0u;
    if (call->as.call.distinct && aggregate_window)
      return bind_error(b,TURBODB_STATUS_UNSUPPORTED,"window aggregate DISTINCT is not supported");
    if (call->as.call.distinct || (offset ? (!arguments || arguments > 3) : arguments != arity))
      return bind_error(b,TURBODB_STATUS_SQL_ERROR,"invalid SQL window function arguments");
    window->argument.literal = offset ? 1 : 0;
    window->value_count = offset ? (arguments == 3 ? 2 : 1) : frame_value ? 1 : 0;
    if (window->kind == ORM_SQL_WINDOW_COUNT_VALUE) {
      const sqlparser_node *argument=sqlparser_get_node(b->document,call->as.call.arguments.first);
      if (argument->kind == SQLPARSER_STAR && argument->span.length == 1) {
        window->kind=ORM_SQL_WINDOW_COUNT_ALL; window->value_count=0;
      }
    }
    if (window->kind == ORM_SQL_NTILE || nth || (offset && arguments > 1)) {
      const sqlparser_id argument_id = offset || nth ? sqlparser_get_node(b->document,call->as.call.arguments.first)->next : call->as.call.arguments.first;
      const sqlparser_node *argument = sqlparser_get_node(b->document,argument_id);
      if (argument->kind != SQLPARSER_NUMBER && argument->kind != SQLPARSER_PARAMETER)
        return bind_error(b,TURBODB_STATUS_SQL_ERROR,nth ? "NTH_VALUE requires an integer literal or parameter position" :
            offset ? "LAG/LEAD requires an integer literal or parameter offset" : "NTILE requires an integer literal or parameter");
      if (argument->kind == SQLPARSER_PARAMETER) {
        size_t slot; status = parameter_slot(b,argument,&slot);
        if (status != TURBODB_STATUS_OK) return status;
        const orm_sql_type *type = vec_at_const(&b->plan.parameter_types,slot);
        if (type->kind != TURBODB_VALUE_INT64 && type->kind != TURBODB_VALUE_UINT64)
          return bind_error(b,TURBODB_STATUS_TYPE_ERROR,nth ? "NTH_VALUE position parameter type must be I64 or U64" :
              offset ? "LAG/LEAD offset parameter type must be I64 or U64" : "NTILE parameter type must be I64 or U64");
      }
      status = bind_integer(b,argument_id,&window->argument);
      if (status != TURBODB_STATUS_OK) return status;
      if (!window->argument.is_parameter && ((!offset && !window->argument.literal) ||
          window->argument.literal > (nth ? ORM_SQL_WINDOW_MAX_NTH : ORM_SQL_WINDOW_MAX_OFFSET)))
        return bind_error(b,TURBODB_STATUS_SQL_ERROR,nth ? "NTH_VALUE position must be 1..INT64_MAX" :
            offset ? "LAG/LEAD offset must be 0..2^63" : "NTILE bucket count must be 1..2^63");
    }
    status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,nodes);
    if (status != TURBODB_STATUS_OK) return status;
    for (size_t j = 1; j <= nodes; ++j) {
      const sqlparser_node *inner = sqlparser_get_node(b->document,(sqlparser_id)j);
      if (select_nested(b,j-1) || !select_inside(inner,statement)) continue;
      if (j != i && inner->kind == SQLPARSER_WINDOW && select_inside(inner,node))
        return bind_error(b,TURBODB_STATUS_SQL_ERROR,"nested window functions are not allowed");
      orm_sql_aggregate_kind aggregate;
      if (select_inside(node,inner) && orm_sql_bind_aggregate_kind(b->document,inner,&aggregate))
        return bind_error(b,TURBODB_STATUS_SQL_ERROR,"aggregate arguments cannot contain window functions");
    }
  }
  return status;
}
static bool select_frame_numeric(turbodb_value_kind_t kind, sqlparser_frame_unit unit) {
  return kind == TURBODB_VALUE_INT64 || kind == TURBODB_VALUE_UINT64 ||
      (unit == SQLPARSER_FRAME_RANGE && kind == TURBODB_VALUE_DOUBLE);
}
static turbodb_status_t bind_frame_boundary(select_binder *b, sqlparser_id id,
    sqlparser_frame_unit unit, select_frame_boundary *out) {
  out->kind=SQLPARSER_BOUND_CURRENT_ROW;
  if (!id) return TURBODB_STATUS_OK;
  const sqlparser_node *node=sqlparser_get_node(b->document,id);
  b->offset=node->span.offset; out->offset=b->offset; out->kind=node->as.boundary.kind;
  if (node->as.boundary.unit)
    return bind_error(b,unit == SQLPARSER_FRAME_ROWS ? TURBODB_STATUS_SQL_ERROR : TURBODB_STATUS_UNSUPPORTED,
        unit == SQLPARSER_FRAME_ROWS ? "ROWS frame cannot use INTERVAL" : "temporal RANGE frames require temporal types");
  if (!node->as.boundary.value) return TURBODB_STATUS_OK;
  const sqlparser_node *value=sqlparser_get_node(b->document,node->as.boundary.value);
  turbodb_status_t status=bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,value->span.length);
  if (status != TURBODB_STATUS_OK) return status;
  if (value->kind == SQLPARSER_PARAMETER) {
    status=parameter_slot(b,value,&out->parameter);
    if (status != TURBODB_STATUS_OK) return status;
    if (!select_frame_numeric(((const orm_sql_type *)vec_at_const(&b->plan.parameter_types,out->parameter))->kind,unit))
      return bind_error(b,TURBODB_STATUS_TYPE_ERROR,"frame offset parameter requires a numeric type; ROWS requires an integer");
    out->is_parameter=true;
  } else {
    status=orm_tidesdb_sql_number_literal((vstr){sqlparser_text(b->document,value->span),value->span.length},false,&out->literal,b->error);
    if (status != TURBODB_STATUS_OK) return status;
    if (!select_frame_numeric(out->literal.kind,unit))
      return bind_error(b,TURBODB_STATUS_SQL_ERROR,"ROWS frame offset literal must be an integer");
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t bind_window_frames(select_binder *b, const sqlparser_node *statement) {
  const size_t nodes=sqlparser_node_count(b->document);
  size_t count=0;
  turbodb_status_t status=bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,nodes);
  if (status != TURBODB_STATUS_OK) return status;
  for (size_t i=1;i<=nodes;++i) {
    const sqlparser_node *node=sqlparser_get_node(b->document,(sqlparser_id)i);
    if (!select_nested(b,i-1) && select_inside(node,statement) && node->kind == SQLPARSER_WINDOW_FRAME) ++count;
  }
  if (!count) return TURBODB_STATUS_OK;
  status=bind_charge(b,ORM_SQL_BUDGET_PLAN_NODES,count);
  if (status == TURBODB_STATUS_OK) status=select_vector(b,&b->plan.window_frames,count,sizeof(select_window_frame),
      _Alignof(select_window_frame),&b->plan.window_frame_bytes);
  size_t index=0;
  for (size_t i=1;status == TURBODB_STATUS_OK && i<=nodes;++i) {
    const sqlparser_node *node=sqlparser_get_node(b->document,(sqlparser_id)i);
    if (select_nested(b,i-1) || !select_inside(node,statement) || node->kind != SQLPARSER_WINDOW_FRAME) continue;
    const size_t slot=index++;
    select_window_frame *frame=vec_at(&b->plan.window_frames,slot); frame->unit=node->as.frame.unit;
    status=bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,vec_size(&b->plan.windows));
    if (status != TURBODB_STATUS_OK) return status;
    for (size_t j=0;j<vec_size(&b->plan.windows);++j) {
      select_window_clause *window=vec_at(&b->plan.windows,j);
      if (window->frame == i) window->frame_slot=slot;
    }
    status=bind_frame_boundary(b,node->as.frame.start,frame->unit,&frame->boundaries[0]);
    if (status == TURBODB_STATUS_OK) status=bind_frame_boundary(b,node->as.frame.end,frame->unit,&frame->boundaries[1]);
    if (status != TURBODB_STATUS_OK) return status;
    b->offset=node->span.offset;
    const sqlparser_boundary_kind start=frame->boundaries[0].kind, end=frame->boundaries[1].kind;
    if (start == SQLPARSER_BOUND_UNBOUNDED_FOLLOWING || end == SQLPARSER_BOUND_UNBOUNDED_PRECEDING || start > end)
      return bind_error(b,TURBODB_STATUS_SQL_ERROR,"illegal window frame boundary order");
  }
  return status;
}
static bool bind_range_offset(select_binder *b, sqlparser_id frame) {
  const sqlparser_node *node=sqlparser_get_node(b->document,frame);
  if (!node || node->as.frame.unit != SQLPARSER_FRAME_RANGE) return false;
  const sqlparser_node *start=sqlparser_get_node(b->document,node->as.frame.start);
  const sqlparser_node *end=sqlparser_get_node(b->document,node->as.frame.end);
  return start->as.boundary.value || (end && end->as.boundary.value);
}
static turbodb_status_t bind_frame_order(select_binder *b, sqlparser_id frame, size_t orders) {
  if (bind_range_offset(b,frame) && orders != 1)
    return bind_error(b,TURBODB_STATUS_SQL_ERROR,"RANGE frame with offset requires one numeric ORDER BY expression");
  return TURBODB_STATUS_OK;
}
static turbodb_status_t select_computation_destroy(select_computation *computation,
    orm_tidesdb_sql_budget *budget, turbodb_error_t *error) {
  turbodb_status_t status = orm_tidesdb_sql_expr_destroy(&computation->program,error);
  const turbodb_status_t released = orm_sql_work_release(&computation->slots,computation->slot_bytes,budget,
      status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  const turbodb_status_t queries_released = orm_sql_work_release(&computation->query_slots,computation->query_slot_bytes,budget,
      status == TURBODB_STATUS_OK ? error : NULL);
  return status == TURBODB_STATUS_OK ? queries_released : status;
}
static turbodb_status_t bind_contains_window(select_binder *b, sqlparser_id root, bool *out) {
  *out = false;
  turbodb_status_t status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,vec_size(&b->plan.windows));
  if (status != TURBODB_STATUS_OK) return status;
  for (size_t i = 0; i < vec_size(&b->plan.windows); ++i) {
    const select_window_clause *window = vec_at_const(&b->plan.windows,i);
    if (select_inside(sqlparser_get_node(b->document,window->node),sqlparser_get_node(b->document,root))) { *out=true; break; }
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t bind_group_aggregate_kind(select_binder *b, const sqlparser_node *node,
    orm_sql_aggregate_kind *kind, bool *out) {
  *out=false;
  if (!orm_sql_bind_aggregate_kind(b->document,node,kind)) return TURBODB_STATUS_OK;
  turbodb_status_t status=bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,vec_size(&b->plan.windows));
  if (status != TURBODB_STATUS_OK) return status;
  for (size_t i=0;i<vec_size(&b->plan.windows);++i) {
    const select_window_clause *window=vec_at_const(&b->plan.windows,i);
    const sqlparser_node *wrapper=sqlparser_get_node(b->document,window->node);
    if (sqlparser_get_node(b->document,wrapper->as.window.call) == node) return TURBODB_STATUS_OK;
  }
  *out=true; return TURBODB_STATUS_OK;
}
static turbodb_status_t bind_contains_aggregate(select_binder *b, sqlparser_id root, bool *out) {
  const size_t nodes = sqlparser_node_count(b->document);
  turbodb_status_t status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,nodes);
  if (status != TURBODB_STATUS_OK) return status;
  *out = false;
  for (size_t i = 1; i <= nodes; ++i) {
    const sqlparser_node *node = sqlparser_get_node(b->document,(sqlparser_id)i); orm_sql_aggregate_kind kind;
    if (select_nested(b,i-1) || !select_inside(node,sqlparser_get_node(b->document,root))) continue;
    bool aggregate;
    status=bind_group_aggregate_kind(b,node,&kind,&aggregate);
    if (status != TURBODB_STATUS_OK) return status;
    if (aggregate) { *out = true; break; }
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t bind_same_roots(select_binder *b,sqlparser_id left,sqlparser_id right,size_t max_depth,bool *same) {
  select_computation compared[2]={{0},{0}};
  turbodb_status_t status=bind_expression(b,left,max_depth,select_target(&compared[0]),false);
  if(status==TURBODB_STATUS_OK) status=bind_expression(b,right,max_depth,select_target(&compared[1]),false);
  if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_expr_same(&compared[0].program,vec_data_const(&compared[0].slots),
      &compared[1].program,vec_data_const(&compared[1].slots),same,b->error);
  for(size_t i=0;i<2;++i) {
    const turbodb_status_t released=select_computation_destroy(&compared[i],b->plan.budget,status==TURBODB_STATUS_OK?b->error:NULL);
    if(status==TURBODB_STATUS_OK) status=released;
  }
  return status;
}
static turbodb_status_t bind_group_reference(select_binder *b, const sqlparser_node *statement,
    sqlparser_id key,size_t max_depth,sqlparser_id *out) {
  const sqlparser_node *node = sqlparser_get_node(b->document,key);
  *out = key;
  if (node->kind == SQLPARSER_NAME) {
    vstr name; turbodb_status_t status = node_name(b,key,true,&name,NULL,NULL);
    if (status != TURBODB_STATUS_OK) return status;
    for (size_t i = 0; !b->hidden_input && i < b->schema->count; ++i) {
      status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,1);
      if (status != TURBODB_STATUS_OK) return status;
      if (name_equal(name,b->schema->columns[i].name)) return TURBODB_STATUS_OK;
    }
    if (node->as.name.parts != 1) return bind_error(b,TURBODB_STATUS_SQL_ERROR,"unknown GROUP BY column");
    sqlparser_id matched=0;
    for (sqlparser_id id = statement->as.select.columns.first; id;) {
      const sqlparser_node *item = sqlparser_get_node(b->document,id); id = item->next;
      status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,1);
      if (status != TURBODB_STATUS_OK) return status;
      if (!item->as.projection.alias) continue;
      vstr alias; status = node_name(b,item->as.projection.alias,false,&alias,NULL,NULL);
      if (status != TURBODB_STATUS_OK) return status;
      if (name_equal(name,alias)) {
        if(matched) {
          bool same=false;
          status=bind_same_roots(b,matched,item->as.projection.expression,max_depth,&same);
          if(status!=TURBODB_STATUS_OK) return status;
          if(!same) return bind_error(b,TURBODB_STATUS_SQL_ERROR,"ambiguous GROUP BY output alias");
        } else matched=item->as.projection.expression;
      }
    }
    if(matched) { *out=matched; return TURBODB_STATUS_OK; }
    return bind_error(b,TURBODB_STATUS_SQL_ERROR,"unknown GROUP BY column or alias");
  }
  bool ordinal = node->kind == SQLPARSER_NUMBER;
  const char *text = sqlparser_text(b->document,node->span);
  for (size_t i = 0; ordinal && i < node->span.length; ++i) ordinal = text[i] >= '0' && text[i] <= '9';
  if (!ordinal) return TURBODB_STATUS_OK;
  orm_sql_select_bound position = {0};
  turbodb_status_t status = bind_integer(b,key,&position);
  if (status != TURBODB_STATUS_OK) return status;
  if (!position.literal || position.literal > statement->as.select.columns.count)
    return bind_error(b,TURBODB_STATUS_SQL_ERROR,"GROUP BY position is outside the select list");
  sqlparser_id id = statement->as.select.columns.first;
  for (uint64_t i = 1; i < position.literal; ++i) {
    status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,1);
    if (status != TURBODB_STATUS_OK) return status;
    id = sqlparser_get_node(b->document,id)->next;
  }
  *out = sqlparser_get_node(b->document,id)->as.projection.expression;
  return TURBODB_STATUS_OK;
}
static turbodb_status_t bind_group_keys(select_binder *b, const sqlparser_node *statement,
    size_t max_depth, vec_t *schema_columns) {
  sqlparser_id key = statement->as.select.group_by.first;
  for (size_t i = 0; i < statement->as.select.group_by.count; ++i) {
    select_computation *computation = vec_at(&b->plan.key_computations,i);
    turbodb_status_t status = bind_group_reference(b,statement,key,max_depth,&computation->root);
    bool aggregate = false;
    if (status == TURBODB_STATUS_OK) status = bind_contains_aggregate(b,computation->root,&aggregate);
    if (status != TURBODB_STATUS_OK) return status;
    if (aggregate) return bind_error(b,TURBODB_STATUS_SQL_ERROR,"aggregate functions are not allowed in GROUP BY");
    bool window = false;
    status = bind_contains_window(b,computation->root,&window);
    if (status != TURBODB_STATUS_OK) return status;
    if (window) return bind_error(b,TURBODB_STATUS_SQL_ERROR,"window functions are not allowed in GROUP BY");
    status = bind_expression(b,computation->root,max_depth,
        select_target(computation),false);
    if (status != TURBODB_STATUS_OK) return status;
    const orm_sql_type type = computation->program.result;
    if (type.kind == TURBODB_VALUE_TEXT || type.kind == TURBODB_VALUE_BLOB)
      return bind_error(b,TURBODB_STATUS_UNSUPPORTED,"GROUP BY requires numeric, BOOL or NULL keys");
    orm_sql_schema_column *column = vec_at(schema_columns,i); column->type = type;
    if (sqlparser_get_node(b->document,computation->root)->kind == SQLPARSER_NAME) {
      size_t source_slot;
      status = find_column(b,computation->root,&source_slot);
      if (status != TURBODB_STATUS_OK) return status;
      const orm_sql_schema_column *source_column;
      if(source_slot<b->schema->count) {
        if(*(const size_t *)vec_at_const(&b->group_capture,source_slot)==SIZE_MAX)
          *(size_t *)vec_at(&b->group_capture,source_slot)=i;
        source_column=&b->schema->columns[source_slot];
        column->qualifier=source_column->qualifier;
      } else {
        const size_t outer=source_slot-b->schema->count-b->parameter_marker_count;
        source_column=&b->outer_schema->columns[outer];
        column->qualifier=source_column->qualifier.len?source_column->qualifier:b->outer_qualifier;
      }
      column->name=source_column->name;
      column->qualified_only=source_column->qualified_only;
      /* Repeated keys retain their physical slots, but introduce only one
       * source-name binding; otherwise GROUP BY a.id,a.id becomes ambiguous. */
      for (size_t j = 0; j < i; ++j) {
        status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,1);
        if (status != TURBODB_STATUS_OK) return status;
        const orm_sql_schema_column *previous = vec_at_const(schema_columns,j);
        if (name_equal(column->name,previous->name) && name_equal(column->qualifier,previous->qualifier)) {
          column->name = (vstr){0}; column->qualifier = (vstr){0}; break;
        }
      }
    }
    const size_t slot = b->schema->count+i;
    *(size_t *)vec_at(&b->plan.group_keys,i) = slot;
    *(orm_sql_type *)vec_at(&b->plan.pre_types,slot) = type;
    *(orm_sql_scan_expression *)vec_at(&b->plan.pre_expressions,slot) =
        select_scan_expression(computation);
    key = sqlparser_get_node(b->document,key)->next;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t bind_structural_name(select_binder *b, const sqlparser_node *node, bool *inside);
/* Matching happens in the original row scope. Output aliases are resolved
 * later in HAVING, so expressions using only those aliases are not candidates. */
static turbodb_status_t bind_key_scope(select_binder *b, const sqlparser_node *root, bool *out) {
  *out = true;
  const size_t nodes = sqlparser_node_count(b->document);
  turbodb_status_t status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,nodes);
  for (size_t i = 1; status == TURBODB_STATUS_OK && *out && i <= nodes; ++i) {
    const sqlparser_node *node = sqlparser_get_node(b->document,(sqlparser_id)i);
    if (node->kind != SQLPARSER_NAME || select_nested(b,i-1) || !select_inside(node,root)) continue;
    bool structural;
    status = bind_structural_name(b,node,&structural);
    if (status != TURBODB_STATUS_OK || structural) continue;
    vstr name;
    status = node_name(b,(sqlparser_id)i,true,&name,NULL,NULL);
    if (status != TURBODB_STATUS_OK) return status;
    const orm_sql_table_schema *schemas[] = {b->schema,b->outer_schema};
    bool found = false;
    for (size_t scope = 0; status == TURBODB_STATUS_OK && !found && scope < sizeof(schemas)/sizeof(schemas[0]); ++scope) {
      if (!schemas[scope]) continue;
      for (size_t column = 0; status == TURBODB_STATUS_OK && !found && column < schemas[scope]->count; ++column) {
        status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,1);
        found = name_equal(name,schemas[scope]->columns[column].name);
      }
    }
    *out = found;
  }
  return status;
}
static turbodb_status_t bind_key_match(select_binder *b, sqlparser_id root, size_t max_depth, vec_t *substitutions) {
  const sqlparser_node *node = sqlparser_get_node(b->document,root);
  if (node->kind == SQLPARSER_NAME || node->kind == SQLPARSER_STAR) return TURBODB_STATUS_OK;
  bool window = false;
  turbodb_status_t window_status = bind_contains_window(b,root,&window);
  if (window_status != TURBODB_STATUS_OK || window) return window_status;
  const size_t keys = vec_size(&b->plan.key_computations);
  turbodb_status_t status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,keys);
  if (status != TURBODB_STATUS_OK) return status;
  bool candidate = false;
  for (size_t i = 0; i < keys; ++i) {
    const select_computation *key = vec_at_const(&b->plan.key_computations,i);
    if (node->kind == sqlparser_get_node(b->document,key->root)->kind) { candidate = true; break; }
  }
  if (!candidate) return TURBODB_STATUS_OK;
  status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,vec_size(&b->plan.windows));
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < vec_size(&b->plan.windows); ++i) {
    const select_window_clause *clause = vec_at_const(&b->plan.windows,i);
    if (sqlparser_get_node(b->document,clause->node)->as.window.call == root) return TURBODB_STATUS_OK;
  }
  bool in_scope = false;
  if (status == TURBODB_STATUS_OK) status = bind_key_scope(b,node,&in_scope);
  if (status != TURBODB_STATUS_OK || !in_scope) return status;
  bool aggregate = false; status = bind_contains_aggregate(b,root,&aggregate);
  if (status != TURBODB_STATUS_OK || aggregate) return status;
  select_computation comparison = {0};
  status = bind_expression(b,root,max_depth,
      select_target(&comparison),false);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < keys; ++i) {
    const select_computation *key = vec_at_const(&b->plan.key_computations,i);
    if (node->kind != sqlparser_get_node(b->document,key->root)->kind) continue;
    bool same = false;
    status = orm_tidesdb_sql_expr_same(&comparison.program,vec_data_const(&comparison.slots),
        &key->program,vec_data_const(&key->slots),&same,b->error);
    if (status == TURBODB_STATUS_OK && same) { *(size_t *)vec_at(substitutions,root-1) = i+1; break; }
  }
  const turbodb_status_t released = select_computation_destroy(&comparison,b->plan.budget,status == TURBODB_STATUS_OK ? b->error : NULL);
  return status == TURBODB_STATUS_OK ? released : status;
}
static turbodb_status_t bind_key_tree(select_binder *b, sqlparser_id root,
    size_t max_depth, vec_t *substitutions) {
  if (!root) return TURBODB_STATUS_OK;
  turbodb_status_t status = bind_key_match(b,root,max_depth,substitutions);
  if (status != TURBODB_STATUS_OK || *(const size_t *)vec_at_const(substitutions,root-1)) return status;
  const sqlparser_node *parent = sqlparser_get_node(b->document,root);
  const size_t nodes = sqlparser_node_count(b->document);
  status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,nodes);
  for (size_t i = 1; status == TURBODB_STATUS_OK && i <= nodes; ++i) {
    const sqlparser_node *node = sqlparser_get_node(b->document,(sqlparser_id)i);
    if (i == root || select_nested(b,i-1) || !select_inside(node,parent)) continue;
    status = bind_key_match(b,(sqlparser_id)i,max_depth,substitutions);
  }
  return status;
}
static turbodb_status_t bind_key_outputs(select_binder *b, const sqlparser_node *statement,
    size_t max_depth, vec_t *substitutions) {
  if (!vec_size(&b->plan.key_computations)) return TURBODB_STATUS_OK;
  const sqlparser_list lists[] = {statement->as.select.columns,statement->as.select.order_by};
  for (size_t i = 0; i < sizeof(lists)/sizeof(lists[0]); ++i) {
    for (sqlparser_id id = lists[i].first; id;) {
      const sqlparser_node *item = sqlparser_get_node(b->document,id); id = item->next;
      const sqlparser_id root = i ? item->as.order.expression : item->as.projection.expression;
      const turbodb_status_t status = bind_key_tree(b,root,max_depth,substitutions);
      if (status != TURBODB_STATUS_OK) return status;
    }
  }
  for (size_t i = 0; i < vec_size(&b->plan.windows); ++i) {
    const select_window_clause *window = vec_at_const(&b->plan.windows,i);
    const sqlparser_node *node = sqlparser_get_node(b->document,window->node);
    const sqlparser_list window_lists[] = {window->partition_by,window->order_by};
    if (window->value_count) {
      const sqlparser_node *call = sqlparser_get_node(b->document,node->as.window.call);
      sqlparser_id argument = call->as.call.arguments.first;
      turbodb_status_t status = bind_key_tree(b,argument,max_depth,substitutions);
      if (status == TURBODB_STATUS_OK && window->value_count == 2) {
        argument = sqlparser_get_node(b->document,sqlparser_get_node(b->document,argument)->next)->next;
        status = bind_key_tree(b,argument,max_depth,substitutions);
      }
      if (status != TURBODB_STATUS_OK) return status;
    }
    for (size_t list = 0; list < sizeof(window_lists)/sizeof(window_lists[0]); ++list)
      for (sqlparser_id id = window_lists[list].first; id;) {
        const sqlparser_id current = id;
        const sqlparser_node *entry = sqlparser_get_node(b->document,id); id = entry->next;
        const turbodb_status_t status = bind_key_tree(b,list ? entry->as.order.expression : current,max_depth,substitutions);
        if (status != TURBODB_STATUS_OK) return status;
      }
  }
  for (size_t i = 0; i < vec_size(&b->named_windows); ++i) {
    const select_named_window *window = vec_at_const(&b->named_windows,i);
    const sqlparser_node *node = sqlparser_get_node(b->document,window->node);
    const sqlparser_list keys[] = {node->as.window_definition.partition_by,node->as.window_definition.order_by};
    for (size_t list = 0; list < sizeof(keys)/sizeof(keys[0]); ++list)
      for (sqlparser_id id = keys[list].first; id;) {
        const sqlparser_id current = id;
        const sqlparser_node *entry = sqlparser_get_node(b->document,id); id=entry->next;
        const turbodb_status_t status = bind_key_tree(b,list ? entry->as.order.expression : current,max_depth,substitutions);
        if (status != TURBODB_STATUS_OK) return status;
      }
  }
  return bind_key_tree(b,statement->as.select.having,max_depth,substitutions);
}
/* Binding scratch never escapes: group schema borrows names only during bind,
 * and substitutions turn validated aggregate CALLs into typed group inputs. */
static turbodb_status_t bind_groups(select_binder *b, const sqlparser_node *statement, size_t max_depth,
    vec_t *schema_columns, size_t *schema_bytes, vec_t *substitutions, size_t *substitution_bytes) {
  const size_t nodes = sqlparser_node_count(b->document), keys = statement->as.select.group_by.count;
  size_t calls = 0, arguments = 0;
  turbodb_status_t status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,nodes);
  if (status != TURBODB_STATUS_OK) return status;
  for (size_t i = 1; i <= nodes; ++i) {
    orm_sql_aggregate_kind kind;
    const sqlparser_node *node = sqlparser_get_node(b->document,(sqlparser_id)i);
    if (select_nested(b,i-1) || !select_inside(node,statement)) continue;
    bool aggregate;
    status=bind_group_aggregate_kind(b,node,&kind,&aggregate);
    if (status != TURBODB_STATUS_OK) return status;
    if (aggregate) {
      ++calls;
      const size_t count=node->as.call.arguments.count?node->as.call.arguments.count:1;
      if(count>SIZE_MAX-arguments) return bind_error(b,TURBODB_STATUS_LIMIT_EXCEEDED,"aggregate argument capacity exceeded");
      arguments+=count;
    }
  }
  if (!keys && !calls) {
    if (statement->as.select.having) return bind_error(b,TURBODB_STATUS_UNSUPPORTED,"HAVING requires grouped or aggregate SELECT");
    return TURBODB_STATUS_OK;
  }
  b->plan.grouped = true;
  const size_t columns = b->schema->count;
  if (calls > SIZE_MAX-keys || arguments>SIZE_MAX-keys || keys+arguments > SIZE_MAX-columns || keys+calls >
      b->plan.budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES] || columns+keys+arguments >
      b->plan.budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES])
    return bind_error(b,TURBODB_STATUS_LIMIT_EXCEEDED,"aggregate plan capacity exceeded");
  status = select_vector(b,schema_columns,keys+calls,sizeof(orm_sql_schema_column),_Alignof(orm_sql_schema_column),schema_bytes);
  if(status==TURBODB_STATUS_OK) status=select_vector(b,&b->group_capture,columns,sizeof(size_t),
      _Alignof(size_t),&b->group_capture_bytes);
  if(status==TURBODB_STATUS_OK) status=bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,columns);
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<columns;++i)
    *(size_t *)vec_at(&b->group_capture,i)=SIZE_MAX;
  if (status == TURBODB_STATUS_OK) status = select_vector(b,substitutions,nodes,sizeof(size_t),_Alignof(size_t),substitution_bytes);
  if (status == TURBODB_STATUS_OK) status = select_vector(b,&b->plan.group_keys,keys,sizeof(size_t),_Alignof(size_t),&b->plan.group_key_bytes);
  if (status == TURBODB_STATUS_OK) status = select_vector(b,&b->plan.aggregate_items,calls,sizeof(orm_sql_aggregate_item),
      _Alignof(orm_sql_aggregate_item),&b->plan.aggregate_item_bytes);
  if (status == TURBODB_STATUS_OK) status = select_vector(b,&b->plan.group_computations,arguments,sizeof(select_computation),
      _Alignof(select_computation),&b->plan.group_computation_bytes);
  if (status == TURBODB_STATUS_OK) status = select_vector(b,&b->plan.key_computations,keys,sizeof(select_computation),
      _Alignof(select_computation),&b->plan.key_computation_bytes);
  if (status == TURBODB_STATUS_OK) status = select_vector(b,&b->plan.pre_projection,columns+keys+arguments,sizeof(size_t),
      _Alignof(size_t),&b->plan.pre_projection_bytes);
  if (status == TURBODB_STATUS_OK) status = select_vector(b,&b->plan.pre_expressions,columns+keys+arguments,sizeof(orm_sql_scan_expression),
      _Alignof(orm_sql_scan_expression),&b->plan.pre_expression_bytes);
  if (status == TURBODB_STATUS_OK) status = select_vector(b,&b->plan.pre_types,columns+keys+arguments,sizeof(orm_sql_type),
      _Alignof(orm_sql_type),&b->plan.pre_type_bytes);
  if (status != TURBODB_STATUS_OK) return status;
  status = bind_group_keys(b,statement,max_depth,schema_columns);
  if (status != TURBODB_STATUS_OK) return status;
  for (size_t i = 0; i < columns; ++i) {
    *(size_t *)vec_at(&b->plan.pre_projection,i) = i;
    *(orm_sql_type *)vec_at(&b->plan.pre_types,i) = b->schema->columns[i].type;
  }
  size_t index = 0, first_argument = 0;
  for (size_t i = 1; i <= nodes; ++i) {
    const sqlparser_node *node = sqlparser_get_node(b->document,(sqlparser_id)i); orm_sql_aggregate_kind kind;
    if (select_nested(b,i-1) || !select_inside(node,statement)) continue;
    bool aggregate;
    status=bind_group_aggregate_kind(b,node,&kind,&aggregate);
    if (status != TURBODB_STATUS_OK) return status;
    if (!aggregate) continue;
    b->offset = node->span.offset;
    if (select_inside(node,sqlparser_get_node(b->document,statement->as.select.where)))
      return bind_error(b,TURBODB_STATUS_SQL_ERROR,"aggregate functions are not allowed in WHERE");
    const bool distinct=node->as.call.distinct && kind!=ORM_SQL_MIN && kind!=ORM_SQL_MAX;
    if (distinct && kind!=ORM_SQL_COUNT_VALUE && kind!=ORM_SQL_SUM && kind!=ORM_SQL_AVG)
      return bind_error(b,TURBODB_STATUS_UNSUPPORTED,"aggregate DISTINCT is not supported for this function");
    const size_t count=node->as.call.arguments.count;
    if (!count || (count!=1 && !(distinct && kind==ORM_SQL_COUNT_VALUE)))
      return bind_error(b,TURBODB_STATUS_SQL_ERROR,"aggregate requires exactly one argument");
    sqlparser_id argument_id=node->as.call.arguments.first;
    orm_sql_type argument_type = b->schema->columns[0].type;
    for(size_t argument_index=0;argument_index<count;++argument_index) {
      const sqlparser_node *argument=sqlparser_get_node(b->document,argument_id);
      select_computation *computation=vec_at(&b->plan.group_computations,first_argument+argument_index);
      const size_t slot=columns+keys+first_argument+argument_index;
      orm_sql_type type=b->schema->columns[0].type;
      if(kind==ORM_SQL_COUNT_VALUE && argument && argument->kind==SQLPARSER_STAR && argument->span.length==1 && !distinct && count==1)
        kind=ORM_SQL_COUNT_ALL;
      else {
        if(argument && argument->kind==SQLPARSER_STAR && distinct)
          return bind_error(b,TURBODB_STATUS_SQL_ERROR,"DISTINCT aggregate requires scalar arguments");
        status=bind_expression(b,argument_id,max_depth,select_target(computation),false);
        if(status!=TURBODB_STATUS_OK) return status;
        type=computation->program.result;
        if(distinct && (type.kind==TURBODB_VALUE_TEXT || type.kind==TURBODB_VALUE_BLOB))
          return bind_error(b,TURBODB_STATUS_UNSUPPORTED,"aggregate DISTINCT requires numeric, BOOL or NULL arguments");
        *(orm_sql_scan_expression *)vec_at(&b->plan.pre_expressions,slot)=select_scan_expression(computation);
      }
      if(!argument_index) argument_type=type;
      *(orm_sql_type *)vec_at(&b->plan.pre_types,slot)=type;
      argument_id=argument->next;
    }
    *(orm_sql_aggregate_item *)vec_at(&b->plan.aggregate_items,index) =
        (orm_sql_aggregate_item){kind,columns+keys+first_argument,count,distinct};
    orm_sql_schema_column *result = vec_at(schema_columns,keys+index);
    status = orm_tidesdb_sql_aggregate_type(kind,&argument_type,&result->type,b->error);
    if (status != TURBODB_STATUS_OK) return status;
    size_t canonical = index;
    for (size_t j = 0; j < index; ++j) {
      status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,1);
      if (status != TURBODB_STATUS_OK) return status;
      const orm_sql_aggregate_item *previous_item=vec_at_const(&b->plan.aggregate_items,j);
      if(previous_item->kind!=kind || previous_item->distinct!=distinct || orm_sql_aggregate_arguments(previous_item)!=count) continue;
      bool same = kind == ORM_SQL_COUNT_ALL;
      if(!same) {
        same=true;
        for(size_t argument_index=0;same && argument_index<count;++argument_index) {
          const select_computation *previous=vec_at_const(&b->plan.group_computations,previous_item->slot-columns-keys+argument_index);
          const select_computation *computation=vec_at_const(&b->plan.group_computations,first_argument+argument_index);
          status=orm_tidesdb_sql_expr_same(&previous->program,vec_data_const(&previous->slots),
              &computation->program,vec_data_const(&computation->slots),&same,b->error);
          if(status!=TURBODB_STATUS_OK) break;
        }
      }
      if (status != TURBODB_STATUS_OK) return status;
      if (same) { canonical = j; break; }
    }
    *(size_t *)vec_at(substitutions,i-1) = keys+canonical+1;
    first_argument+=count;
    ++index;
  }
  return TURBODB_STATUS_OK;
}
static select_window_frame select_frame_specification(const orm_sql_select *program, const select_window_clause *window) {
  if (window->frame_slot != SIZE_MAX) return *(const select_window_frame *)vec_at_const(&program->window_frames,window->frame_slot);
  select_window_frame frame={.unit=SQLPARSER_FRAME_RANGE};
  frame.boundaries[0].kind=SQLPARSER_BOUND_UNBOUNDED_PRECEDING;
  frame.boundaries[1].kind=window->order_count ? SQLPARSER_BOUND_CURRENT_ROW : SQLPARSER_BOUND_UNBOUNDED_FOLLOWING;
  return frame;
}
static bool select_frame_same(const orm_sql_select *program, const select_window_clause *left, const select_window_clause *right) {
  const select_window_frame a=select_frame_specification(program,left), b=select_frame_specification(program,right);
  if (a.unit != b.unit) return false;
  for (size_t i=0;i<SELECT_FRAME_BOUNDARIES;++i) {
    const select_frame_boundary *x=&a.boundaries[i], *y=&b.boundaries[i];
    if (x->kind != y->kind) return false;
    if (x->kind != SQLPARSER_BOUND_PRECEDING && x->kind != SQLPARSER_BOUND_FOLLOWING) continue;
    if (x->is_parameter != y->is_parameter || (x->is_parameter ? x->parameter != y->parameter :
        x->literal.kind != y->literal.kind || orm_sql_value_order(&x->literal,&y->literal))) return false;
  }
  return true;
}
static turbodb_status_t bind_window_equivalent(select_binder *b, select_window_clause *current, size_t previous, bool *same) {
  const turbodb_status_t compared = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,1);
  if (compared != TURBODB_STATUS_OK) return compared;
  const select_window_clause *other = vec_at_const(&b->plan.windows,previous);
  *same = current->kind == other->kind && current->partition_count == other->partition_count &&
      current->order_count == other->order_count && current->value_count == other->value_count &&
      current->argument.is_parameter == other->argument.is_parameter &&
      (current->argument.is_parameter ? current->argument.parameter == other->argument.parameter :
        current->argument.literal == other->argument.literal);
  if (*same && orm_sql_window_frame_kind(current->kind)) {
    const turbodb_status_t status=bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,SELECT_FRAME_BOUNDARIES);
    if (status != TURBODB_STATUS_OK) return status;
    *same=select_frame_same(&b->plan,current,other);
  }
  for (size_t i = 0; *same && i < current->partition_count+current->order_count+current->value_count; ++i) {
    turbodb_status_t status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,1);
    if (status != TURBODB_STATUS_OK) return status;
    const orm_sql_scan_order *left = vec_at_const(&b->plan.window_orders,current->first_key+i);
    const orm_sql_scan_order *right = vec_at_const(&b->plan.window_orders,other->first_key+i);
    if (left->descending != right->descending) { *same=false; break; }
    const select_computation *a = vec_at_const(&b->plan.window_computations,current->first_key+i);
    const select_computation *c = vec_at_const(&b->plan.window_computations,other->first_key+i);
    status = orm_tidesdb_sql_expr_same(&a->program,vec_data_const(&a->slots),&c->program,vec_data_const(&c->slots),same,b->error);
    if (status != TURBODB_STATUS_OK) return status;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t bind_named_window_keys(select_binder *b, size_t max_depth) {
  for (size_t i = 0; i < vec_size(&b->named_windows); ++i) {
    const select_named_window *window = vec_at_const(&b->named_windows,i);
    const turbodb_status_t frame_status=bind_frame_order(b,window->frame,window->order_by.count);
    if (frame_status != TURBODB_STATUS_OK) return frame_status;
    const sqlparser_list lists[] = {window->partition_by,window->order_by};
    for (size_t list = 0; list < sizeof(lists)/sizeof(lists[0]); ++list)
      for (sqlparser_id id = lists[list].first; id;) {
        const sqlparser_id current = id;
        const sqlparser_node *item = sqlparser_get_node(b->document,id); id=item->next;
        b->offset=item->span.offset;
        select_computation computation = {0};
        turbodb_status_t status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,1);
        if (status == TURBODB_STATUS_OK) status = bind_expression(b,
            list ? item->as.order.expression : current,max_depth,select_target(&computation),false);
        if (status == TURBODB_STATUS_OK && (computation.program.result.kind == TURBODB_VALUE_TEXT ||
            computation.program.result.kind == TURBODB_VALUE_BLOB))
          status = bind_error(b,TURBODB_STATUS_UNSUPPORTED,"window keys require numeric, BOOL or NULL values");
        if (status == TURBODB_STATUS_OK && list && bind_range_offset(b,window->frame) &&
            computation.program.result.kind == TURBODB_VALUE_NULL)
          status=bind_error(b,TURBODB_STATUS_SQL_ERROR,"RANGE frame offset requires numeric ORDER BY type");
        const turbodb_status_t released = select_computation_destroy(&computation,b->plan.budget,
            status == TURBODB_STATUS_OK ? b->error : NULL);
        if (status != TURBODB_STATUS_OK || released != TURBODB_STATUS_OK) return status == TURBODB_STATUS_OK ? released : status;
      }
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t bind_window_computation(select_binder *b, sqlparser_id root, size_t key,
    size_t columns, size_t max_depth, vec_t *schema_columns) {
  select_computation *computation = vec_at(&b->plan.window_computations,key);
  turbodb_status_t status = bind_expression(b,root,max_depth,select_target(computation),false);
  if (status != TURBODB_STATUS_OK) return status;
  const orm_sql_type type = computation->program.result;
  *(orm_sql_scan_expression *)vec_at(&b->plan.window_expressions,columns+key) = select_scan_expression(computation);
  *(orm_sql_type *)vec_at(&b->plan.window_types,columns+key) = type;
  ((orm_sql_schema_column *)vec_at(schema_columns,columns+key))->type = type;
  *(size_t *)vec_at(&b->plan.window_keys,key) = columns+key;
  return TURBODB_STATUS_OK;
}
static turbodb_status_t bind_windows(select_binder *b, size_t max_depth, vec_t *schema_columns,
    size_t *schema_bytes, vec_t *substitutions, size_t *substitution_bytes) {
  const size_t count = vec_size(&b->plan.windows), columns = b->schema->count;
  if (!count) return TURBODB_STATUS_OK;
  size_t keys = 0;
  turbodb_status_t status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,count);
  if (status != TURBODB_STATUS_OK) return status;
  for (size_t i = 0; i < count; ++i) {
    select_window_clause *window = vec_at(&b->plan.windows,i);
    const size_t partitions = window->partition_by.count, orders = window->order_by.count;
    status=bind_frame_order(b,window->frame,orders);
    if (status != TURBODB_STATUS_OK) return status;
    if (partitions > SIZE_MAX-orders || window->value_count > SIZE_MAX-partitions-orders ||
        partitions+orders+window->value_count > SIZE_MAX-keys)
      return bind_error(b,TURBODB_STATUS_LIMIT_EXCEEDED,"window key width overflow");
    window->first_key = keys; window->partition_count = partitions; window->order_count = orders;
    keys += partitions+orders+window->value_count;
  }
  const size_t parameters = vec_size(&b->plan.parameter_types);
  const uint64_t capacity = b->plan.budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES];
  if (keys > SIZE_MAX-columns || count > SIZE_MAX-columns-keys || parameters > SIZE_MAX-columns-keys-count ||
      columns+keys+count+parameters > capacity)
    return bind_error(b,TURBODB_STATUS_LIMIT_EXCEEDED,"window columns exceed plan capacity");
  const size_t width = columns+keys;
  b->plan.window_base_columns = columns;
  status = bind_charge(b,ORM_SQL_BUDGET_PLAN_NODES,keys);
  if (status == TURBODB_STATUS_OK) status = select_vector(b,&b->plan.window_computations,keys,sizeof(select_computation),
      _Alignof(select_computation),&b->plan.window_computation_bytes);
  if (status == TURBODB_STATUS_OK) status = select_vector(b,&b->plan.window_projection,width,sizeof(size_t),
      _Alignof(size_t),&b->plan.window_projection_bytes);
  if (status == TURBODB_STATUS_OK) status = select_vector(b,&b->plan.window_expressions,width,sizeof(orm_sql_scan_expression),
      _Alignof(orm_sql_scan_expression),&b->plan.window_expression_bytes);
  if (status == TURBODB_STATUS_OK) status = select_vector(b,&b->plan.window_types,width,sizeof(orm_sql_type),
      _Alignof(orm_sql_type),&b->plan.window_type_bytes);
  if (status == TURBODB_STATUS_OK) status = select_vector(b,&b->plan.window_orders,keys,sizeof(orm_sql_scan_order),
      _Alignof(orm_sql_scan_order),&b->plan.window_order_bytes);
  if (status == TURBODB_STATUS_OK) status = select_vector(b,&b->plan.window_keys,keys,sizeof(size_t),
      _Alignof(size_t),&b->plan.window_key_bytes);
  if (status == TURBODB_STATUS_OK) status = select_vector(b,schema_columns,width+count,sizeof(orm_sql_schema_column),
      _Alignof(orm_sql_schema_column),schema_bytes);
  if (status == TURBODB_STATUS_OK && !substitutions->initialized) status = select_vector(b,substitutions,
      sqlparser_node_count(b->document),sizeof(size_t),_Alignof(size_t),substitution_bytes);
  if (status == TURBODB_STATUS_OK) status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,columns);
  if (status != TURBODB_STATUS_OK) return status;
  for (size_t i = 0; i < columns; ++i) {
    *(orm_sql_schema_column *)vec_at(schema_columns,i) = b->schema->columns[i];
    *(size_t *)vec_at(&b->plan.window_projection,i) = i;
    *(orm_sql_type *)vec_at(&b->plan.window_types,i) = b->schema->columns[i].type;
  }
  for (size_t i = 0; i < count; ++i) {
    select_window_clause *window = vec_at(&b->plan.windows,i);
    const sqlparser_node *node = sqlparser_get_node(b->document,window->node);
    const sqlparser_list lists[] = {window->partition_by,window->order_by};
    size_t key = window->first_key;
    for (size_t list = 0; list < sizeof(lists)/sizeof(lists[0]); ++list)
      for (sqlparser_id id = lists[list].first; id;) {
        const sqlparser_id current = id;
        const sqlparser_node *item = sqlparser_get_node(b->document,id); id = item->next;
        status = bind_window_computation(b,list ? item->as.order.expression : current,key,columns,max_depth,schema_columns);
        if (status != TURBODB_STATUS_OK) return status;
        const orm_sql_type type = *(const orm_sql_type *)vec_at_const(&b->plan.window_types,columns+key);
        if (type.kind == TURBODB_VALUE_TEXT || type.kind == TURBODB_VALUE_BLOB)
          return bind_error(b,TURBODB_STATUS_UNSUPPORTED,"window keys require numeric, BOOL or NULL values");
        if (list && bind_range_offset(b,window->frame) && type.kind == TURBODB_VALUE_NULL)
          return bind_error(b,TURBODB_STATUS_SQL_ERROR,"RANGE frame offset requires numeric ORDER BY type");
        *(orm_sql_scan_order *)vec_at(&b->plan.window_orders,key) =
            (orm_sql_scan_order){.slot=columns+key,.descending=list && item->as.order.descending};
        ++key;
      }
    orm_sql_type result_type = {
        window->kind == ORM_SQL_PERCENT_RANK || window->kind == ORM_SQL_CUME_DIST ? TURBODB_VALUE_DOUBLE : TURBODB_VALUE_INT64,false};
    if (window->value_count) {
      const sqlparser_node *call = sqlparser_get_node(b->document,node->as.window.call);
      sqlparser_id argument = call->as.call.arguments.first;
      const size_t value_key = key;
      status = bind_window_computation(b,argument,key++,columns,max_depth,schema_columns);
      if (status == TURBODB_STATUS_OK && window->value_count == 2) {
        argument = sqlparser_get_node(b->document,sqlparser_get_node(b->document,argument)->next)->next;
        status = bind_window_computation(b,argument,key++,columns,max_depth,schema_columns);
      }
      if (status == TURBODB_STATUS_OK && orm_sql_window_aggregate_kind(window->kind))
        status=orm_tidesdb_sql_aggregate_type(orm_sql_window_reduction_kind(window->kind),
            vec_at_const(&b->plan.window_types,columns+value_key),&result_type,b->error);
      else if (status == TURBODB_STATUS_OK) status = orm_sql_window_offset_type(
          *(const orm_sql_type *)vec_at_const(&b->plan.window_types,columns+value_key),
          window->value_count == 2 ? *(const orm_sql_type *)vec_at_const(&b->plan.window_types,columns+value_key+1) :
            (orm_sql_type){TURBODB_VALUE_NULL,true},&result_type,b->error);
      if (status != TURBODB_STATUS_OK) return status;
    }
    window->canonical = i;
    for (size_t j = 0; j < i; ++j) {
      bool same;
      status = bind_window_equivalent(b,window,j,&same);
      if (status != TURBODB_STATUS_OK) return status;
      if (same) { window->canonical = ((const select_window_clause *)vec_at_const(&b->plan.windows,j))->canonical; break; }
    }
    ((orm_sql_schema_column *)vec_at(schema_columns,width+i))->type = result_type;
  }
  /* Publish replacements only after all keys bind in their pre-window scope. */
  if (!b->plan.grouped && b->query_count) {
    status = select_vector(b,&b->group_capture,columns,sizeof(size_t),_Alignof(size_t),&b->group_capture_bytes);
    if (status == TURBODB_STATUS_OK) status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,columns);
    if (status != TURBODB_STATUS_OK) return status;
    for (size_t i = 0; i < columns; ++i) *(size_t *)vec_at(&b->group_capture,i) = i;
  }
  for (size_t i = 0; i < count; ++i) {
    const select_window_clause *window = vec_at_const(&b->plan.windows,i);
    *(size_t *)vec_at(substitutions,window->node-1) = width+window->canonical+1;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t bind_structural_name(select_binder *b, const sqlparser_node *node, bool *inside) {
  *inside = false;
  const size_t nodes = sqlparser_node_count(b->document);
  turbodb_status_t status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,nodes);
  if (status != TURBODB_STATUS_OK) return status;
  for (size_t i = 0; i < nodes; ++i) {
    const sqlparser_node *parent = sqlparser_get_node(b->document,(sqlparser_id)(i+1));
    /* Prefix NAME nodes are construction artifacts; replaced key/call children
     * belong to the original row scope rather than the output alias scope. */
    if ((parent->kind == SQLPARSER_NAME && parent != node && select_inside(node,parent)) ||
        (parent->kind == SQLPARSER_CALL && sqlparser_get_node(b->document,parent->as.call.name) == node) ||
        (parent->kind == SQLPARSER_TYPE && sqlparser_get_node(b->document,parent->as.type.name) == node) ||
        (b->substitutions && b->substitutions[i] && parent != node && select_inside(node,parent))) {
      *inside = true; break;
    }
  }
  return TURBODB_STATUS_OK;
}
/* Duplicate hidden output names are legal only while their caller supplies
 * names. A name reference must still identify the same bound expression. */
static turbodb_status_t bind_output_alias(select_binder *b,vstr name,size_t *out) {
  const size_t count=vec_size(&b->plan.columns);
  size_t matched=count;
  for(size_t i=0;i<count;++i) {
    turbodb_status_t status=bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,1);
    if(status!=TURBODB_STATUS_OK) return status;
    const orm_sql_select_column *column=vec_at_const(&b->plan.columns,i);
    if(!name_equal(name,vstr_from_cstr(column->name))) continue;
    if(matched<count) {
      const select_computation *left=vec_at_const(&b->plan.computations,matched);
      const select_computation *right=vec_at_const(&b->plan.computations,i);
      const bool left_computed=left&&left->program.budget,right_computed=right&&right->program.budget;
      bool same=!left_computed&&!right_computed&&*(const size_t *)vec_at_const(&b->plan.projection,matched)==
          *(const size_t *)vec_at_const(&b->plan.projection,i);
      if(left_computed&&right_computed) status=orm_tidesdb_sql_expr_same(&left->program,vec_data_const(&left->slots),
          &right->program,vec_data_const(&right->slots),&same,b->error);
      if(status!=TURBODB_STATUS_OK) return status;
      if(!same) return bind_error(b,TURBODB_STATUS_SQL_ERROR,"ambiguous output alias");
    } else matched=i;
  }
  *out=matched; return TURBODB_STATUS_OK;
}
static turbodb_status_t bind_having_aliases(select_binder *b, const sqlparser_node *statement, vec_t *substitutions) {
  const sqlparser_node *root = sqlparser_get_node(b->document,statement->as.select.having);
  if (!root) return TURBODB_STATUS_OK;
  const size_t nodes = sqlparser_node_count(b->document);
  turbodb_status_t status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,nodes);
  for (size_t i = 1; status == TURBODB_STATUS_OK && i <= nodes; ++i) {
    const sqlparser_node *node = sqlparser_get_node(b->document,(sqlparser_id)i);
    if (select_nested(b,i-1) || node->kind != SQLPARSER_NAME || node->as.name.parts != 1 || !select_inside(node,root)) continue;
    bool inside;
    status = bind_structural_name(b,node,&inside); if (status != TURBODB_STATUS_OK) break;
    if (inside) continue;
    vstr name; status = node_name(b,(sqlparser_id)i,false,&name,NULL,NULL);
    if (status != TURBODB_STATUS_OK) break;
    bool grouped_name = false;
    for (size_t k = 0; k < vec_size(&b->plan.group_keys); ++k) {
      status = bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,1); if (status != TURBODB_STATUS_OK) return status;
      if (name_equal(name,b->schema->columns[k].name)) { grouped_name=true; break; }
    }
    if (grouped_name) continue;
    size_t output;
    status=bind_output_alias(b,name,&output); if(status!=TURBODB_STATUS_OK) return status;
    if(output<vec_size(&b->plan.columns)) {
      sqlparser_id projection=statement->as.select.columns.first;
      for(size_t j=0;j<output;++j) {
        status=bind_charge(b,ORM_SQL_BUDGET_EXECUTION_STEPS,1); if(status!=TURBODB_STATUS_OK) return status;
        projection=sqlparser_get_node(b->document,projection)->next;
      }
      const sqlparser_node *item=sqlparser_get_node(b->document,projection);
      const size_t j=output;
      const orm_sql_scan_expression *expression = vec_at_const(&b->plan.expressions,j);
      const size_t slot = expression && expression->program ?
          *(const size_t *)vec_at_const(substitutions,item->as.projection.expression-1) :
          *(const size_t *)vec_at_const(&b->plan.projection,j)+1;
      if (!slot) return bind_error(b,TURBODB_STATUS_UNSUPPORTED,"HAVING aliases require a direct group column or aggregate");
      if (vec_size(&b->plan.windows) && slot > b->plan.window_base_columns)
        return bind_error(b,TURBODB_STATUS_SQL_ERROR,"window results are not allowed in HAVING");
      *(size_t *)vec_at(substitutions,i-1) = slot;
    }
  }
  return status;
}


static turbodb_status_t bind_order_expression_scope(select_binder *b, const sqlparser_node *root) {
  /* Until expression inputs can reference computed outputs, reject alias
   * references rather than accidentally binding a same-named source column. */
  const size_t nodes = sqlparser_node_count(b->document);
  turbodb_status_t status = bind_charge(b, ORM_SQL_BUDGET_EXECUTION_STEPS, nodes);
  for (size_t i = 1; status == TURBODB_STATUS_OK && i <= nodes; ++i) {
    const sqlparser_node *node = sqlparser_get_node(b->document, (sqlparser_id)i);
    if (select_nested(b,i-1) || node->kind != SQLPARSER_NAME || node->span.offset < root->span.offset ||
        node->span.offset - root->span.offset > root->span.length ||
        node->span.length > root->span.length - (node->span.offset - root->span.offset)) continue;
    bool inside;
    status = bind_structural_name(b,node,&inside); if (status != TURBODB_STATUS_OK) return status;
    if (inside) continue;
    const char *text = sqlparser_text(b->document, node->span);
    if (memchr(text, '.', node->span.length)) continue;
    vstr name;
    status = node_name(b, (sqlparser_id)i, false, &name, NULL, NULL);
    for (size_t j = 0; status == TURBODB_STATUS_OK && j < vec_size(&b->plan.columns); ++j) {
      status = bind_charge(b, ORM_SQL_BUDGET_EXECUTION_STEPS, 1);
      if (status != TURBODB_STATUS_OK) return status;
      const orm_sql_select_column *column = vec_at_const(&b->plan.columns, j);
      if (!name_equal(name, vstr_from_cstr(column->name))) continue;
      const orm_sql_scan_expression *expression = vec_at_const(&b->plan.expressions, j);
      const size_t slot = *(const size_t *)vec_at_const(&b->plan.projection, j);
      if ((expression && expression->program) || !name_equal(name, b->schema->columns[slot].name))
        return bind_error(b, TURBODB_STATUS_UNSUPPORTED, "ORDER BY output aliases must be standalone keys");
    }
  }
  return status;
}
static turbodb_status_t bind_order(select_binder *b, sqlparser_list list, size_t max_depth) {
  turbodb_status_t status = bind_charge(b, ORM_SQL_BUDGET_PLAN_NODES, list.count);
  if (status == TURBODB_STATUS_OK) status = select_vector(b, &b->plan.orders, list.count,
      sizeof(orm_sql_scan_order), _Alignof(orm_sql_scan_order), &b->plan.order_bytes);
  if (status == TURBODB_STATUS_OK) status = select_vector(b, &b->plan.order_computations, list.count,
      sizeof(select_computation), _Alignof(select_computation), &b->plan.order_computation_bytes);
  sqlparser_id id = list.first;
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < list.count; ++i) {
    const sqlparser_node *node = sqlparser_get_node(b->document, id);
    const sqlparser_node *root = node ? sqlparser_get_node(b->document, node->as.order.expression) : NULL;
    if (!root) return bind_error(b, TURBODB_STATUS_INVALID_ARGUMENT, "missing ORDER BY expression");
    const sqlparser_node *operand = root->kind == SQLPARSER_UNARY ?
        sqlparser_get_node(b->document, root->as.unary.operand) : NULL;
    if (operand && operand->kind == SQLPARSER_NUMBER &&
        (root->as.unary.op == SQLPARSER_OP_NEGATE || root->as.unary.op == SQLPARSER_OP_POSITIVE))
      return bind_error(b, TURBODB_STATUS_UNSUPPORTED, "signed numeric ORDER BY keys are not supported");
    b->offset = root->span.offset;
    orm_sql_scan_order *order = vec_at(&b->plan.orders, i);
    order->descending = node->as.order.descending;
    size_t output = vec_size(&b->plan.columns);
    const char *text = sqlparser_text(b->document, root->span);
    bool ordinal = root->kind == SQLPARSER_NUMBER;
    for (size_t j = 0; ordinal && j < root->span.length; ++j) ordinal = text[j] >= '0' && text[j] <= '9';
    if (ordinal) {
      turbodb_value_t value;
      status = orm_tidesdb_sql_integer_literal((vstr){text,root->span.length}, false, &value, b->error);
      if (status != TURBODB_STATUS_OK) return status;
      const uint64_t index = value.kind == TURBODB_VALUE_UINT64 ? value.data.uint64_value : (uint64_t)value.data.int64_value;
      if (!index || index > output) return bind_error(b, TURBODB_STATUS_SQL_ERROR, "ORDER BY position outside select list");
      output = (size_t)index - 1;
    } else if (root->kind == SQLPARSER_NAME && !memchr(text, '.', root->span.length)) {
      vstr name;
      status = node_name(b, node->as.order.expression, false, &name, NULL, NULL);
      if(status==TURBODB_STATUS_OK) status=bind_output_alias(b,name,&output);
    }
    if (status != TURBODB_STATUS_OK) return status;
    orm_sql_type type;
    if (output < vec_size(&b->plan.columns)) {
      order->slot = *(const size_t *)vec_at_const(&b->plan.projection, output);
      const orm_sql_scan_expression *expression = vec_at_const(&b->plan.expressions, output);
      if (expression) order->expression = *expression;
      type = ((const orm_sql_select_column *)vec_at_const(&b->plan.columns, output))->type;
    } else {
      select_computation *computation = vec_at(&b->plan.order_computations, i);
      status = root->kind == SQLPARSER_NAME ? TURBODB_STATUS_OK : bind_order_expression_scope(b, root);
      if (status == TURBODB_STATUS_OK) status = bind_expression(b, node->as.order.expression, max_depth,
          select_target(computation), false);
      if (status != TURBODB_STATUS_OK) return status;
      order->expression = select_scan_expression(computation);
      type = computation->program.result;
    }
    if (type.kind == TURBODB_VALUE_TEXT || type.kind == TURBODB_VALUE_BLOB)
      return bind_error(b, TURBODB_STATUS_UNSUPPORTED, "ORDER BY requires numeric, BOOL or NULL keys");
    id = node->next;
  }
  return status;
}

static turbodb_status_t bind_distinct(select_binder *b) {
  if (!b->plan.distinct) return TURBODB_STATUS_OK;
  const size_t outputs = vec_size(&b->plan.columns);
  for (size_t i = 0; i < outputs; ++i) {
    const orm_sql_select_column *column = vec_at_const(&b->plan.columns,i);
    if (column->type.kind == TURBODB_VALUE_TEXT || column->type.kind == TURBODB_VALUE_BLOB)
      return bind_error(b, TURBODB_STATUS_UNSUPPORTED, "DISTINCT requires numeric, BOOL or NULL outputs");
  }
  for (size_t i = 0; i < vec_size(&b->plan.orders); ++i) {
    const orm_sql_scan_order *order = vec_at_const(&b->plan.orders,i);
    bool selected = false;
    for (size_t j = 0; !selected && j < outputs; ++j) {
      turbodb_status_t status = bind_charge(b, ORM_SQL_BUDGET_EXECUTION_STEPS, 1);
      if (status != TURBODB_STATUS_OK) return status;
      const orm_sql_scan_expression *output = vec_at_const(&b->plan.expressions,j);
      const bool computed = output && output->program;
      if (computed && order->expression.program) {
        status = orm_tidesdb_sql_expr_same(output->program,output->slots,
            order->expression.program,order->expression.slots,&selected,b->error);
        if (status != TURBODB_STATUS_OK) return status;
      } else if (!computed && !order->expression.program)
        selected = *(const size_t *)vec_at_const(&b->plan.projection,j) == order->slot;
    }
    if (selected) continue;
    const size_t count = order->expression.program ? order->expression.count : 1;
    for (size_t k = 0; k < count; ++k) {
      const size_t slot = order->expression.program ? order->expression.slots[k] : order->slot;
      if (slot >= b->schema->count) continue; /* Parameter positions do not vary between source rows. */
      selected = false;
      for (size_t j = 0; !selected && j < outputs; ++j) {
        const turbodb_status_t status = bind_charge(b, ORM_SQL_BUDGET_EXECUTION_STEPS, 1);
        if (status != TURBODB_STATUS_OK) return status;
        const orm_sql_scan_expression *output = vec_at_const(&b->plan.expressions,j);
        selected = (!output || !output->program) &&
            *(const size_t *)vec_at_const(&b->plan.projection,j) == slot;
      }
      if (!selected) return bind_error(b, TURBODB_STATUS_SQL_ERROR,
          "DISTINCT ORDER BY references a source column outside the select list");
    }
  }
  return TURBODB_STATUS_OK;
}

turbodb_status_t orm_tidesdb_sql_select_bind(const sqlparser_document *document,
    const orm_sql_table_schema *schema, size_t max_depth, orm_tidesdb_sql_budget *budget,
    orm_sql_select *out, turbodb_error_t *error) {
  return orm_tidesdb_sql_select_bind_parameters(document, schema, NULL, 0, max_depth, budget, out, error);
}
static turbodb_status_t select_bind_parameters(const sqlparser_document *document,
    const orm_sql_table_schema *schema, const orm_sql_type *parameter_types, size_t parameter_count,
    size_t max_depth, orm_tidesdb_sql_budget *budget, orm_sql_select *out, turbodb_error_t *error, bool explain, const orm_sql_from *from, sqlparser_id root,
    const orm_sql_query_scope *scope) {
  if (!document || !schema || !schema->columns || !schema->count || !max_depth || !budget || !out || out->budget ||
      (parameter_count && !parameter_types))
    return select_error(error, TURBODB_STATUS_INVALID_ARGUMENT, 0, "invalid SELECT binding arguments");
  if (sqlparser_get_dialect(document) != SQLPARSER_MYSQL || sqlparser_statements(document).count != 1)
    return select_error(error, TURBODB_STATUS_UNSUPPORTED, 0, "expected exactly one MySQL SELECT");
  const size_t outer_count=scope&&scope->outer_schema?scope->outer_schema->count:0;
  if (schema->count > SIZE_MAX / sizeof(orm_sql_schema_column) ||
      parameter_count > SIZE_MAX - outer_count || parameter_count+outer_count>SIZE_MAX-schema->count ||
      schema->count + parameter_count+outer_count > budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES])
    return select_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, 0, "schema exceeds plan capacity");
  const sqlparser_node *statement = sqlparser_get_node(document, root ? root : sqlparser_statements(document).first);
  if (explain) {
    if (!statement || statement->kind != SQLPARSER_EXPLAIN || statement->as.explain.query_plan ||
        (statement->as.explain.format != SQLPARSER_EXPLAIN_DEFAULT && statement->as.explain.format != SQLPARSER_EXPLAIN_TRADITIONAL))
      return select_error(error,TURBODB_STATUS_UNSUPPORTED,0,"EXPLAIN requires default or TRADITIONAL format");
    statement = sqlparser_get_node(document,statement->as.explain.statement);
  }
  if (!statement || statement->kind != SQLPARSER_SELECT || statement->as.select.calc_found_rows)
    return select_error(error, TURBODB_STATUS_UNSUPPORTED, statement ? statement->span.offset : 0,
        "unsupported SELECT statement or modifier");
  if (!statement->as.select.from && (!scope || !scope->unit_input))
    return select_error(error,TURBODB_STATUS_UNSUPPORTED,0,"SELECT without FROM requires the runtime unit source");
  if (from && (from->budget != budget || from->root != statement->as.select.from))
    return select_error(error,TURBODB_STATUS_INVALID_ARGUMENT,0,"FROM plan must bind this SELECT and budget");
  select_binder b = {.document = document, .schema = schema, .plan = {.budget = budget}, .error = error,.composed=from!=NULL,
      .queries=scope ? scope->queries : NULL,.query_count=scope ? scope->query_count : 0,
      .scalar_output=scope && scope->scalar_output,.anonymous_output=scope && scope->anonymous_output,
      .no_from=!statement->as.select.from,.hidden_input=!statement->as.select.from,
      .outer_schema=scope?scope->outer_schema:NULL,.outer_qualifier=scope?scope->outer_qualifier:(vstr){0}};
  b.parameter_resolved=scope?scope->parameter_resolved:NULL;
  b.plan.distinct = statement->as.select.distinct;
  vec_t group_schema = {0}, substitutions = {0}; size_t group_schema_bytes = 0, substitution_bytes = 0;
  vec_t window_schema = {0}; size_t window_schema_bytes = 0;
  orm_sql_table_schema grouped_schema = {0};
  orm_sql_table_schema windowed_schema = {0};
  turbodb_status_t status = bind_charge(&b, ORM_SQL_BUDGET_AST_NODES, sqlparser_node_count(document));
  if (status == TURBODB_STATUS_OK)
    status = orm_tidesdb_sql_budget_reserve_capacity(budget, 1, sizeof(b), 0, &b.plan.metadata_bytes, error);
  if (status == TURBODB_STATUS_OK) status = bind_schema(&b,false,false);
  if (status == TURBODB_STATUS_OK && !from && !b.no_from) status = bind_table(&b, statement->as.select.from);
  if (status == TURBODB_STATUS_OK) status = bind_parameters(&b, parameter_types, parameter_count);
  if (status == TURBODB_STATUS_OK) status = bind_query_scope(&b,statement);
  if (status == TURBODB_STATUS_OK) status = bind_named_windows(&b,statement->as.select.windows);
  if (status == TURBODB_STATUS_OK) status = bind_window_admission(&b,statement);
  if (status == TURBODB_STATUS_OK) status = bind_window_frames(&b,statement);
  if (status == TURBODB_STATUS_OK) status = bind_groups(&b,statement,max_depth,&group_schema,&group_schema_bytes,
      &substitutions,&substitution_bytes);
  if (status == TURBODB_STATUS_OK) status = bind_key_outputs(&b,statement,max_depth,&substitutions);
  if (status == TURBODB_STATUS_OK) status = bind_expression(&b,statement->as.select.where,max_depth,
      b.plan.grouped ? (select_expression_target){&b.plan.pre_filter,&b.plan.pre_slots,&b.plan.pre_slot_bytes,&b.plan.pre_query_slots,&b.plan.pre_query_slot_bytes} :
      (select_expression_target){&b.plan.filter,&b.plan.slots,&b.plan.slot_bytes,&b.plan.query_slots,&b.plan.query_slot_bytes},true);
  if (status == TURBODB_STATUS_OK && b.plan.grouped) {
    grouped_schema = (orm_sql_table_schema){schema->name,vec_data_const(&group_schema),vec_size(&group_schema)};
    b.schema = &grouped_schema; b.substitutions = vec_data_const(&substitutions); b.substitution_count = vec_size(&substitutions);
    b.hidden_input = false;
    for (sqlparser_id id = statement->as.select.columns.first; id;) {
      const sqlparser_node *item = sqlparser_get_node(document,id);
      if (sqlparser_get_node(document,item->as.projection.expression)->kind == SQLPARSER_STAR) {
        status = bind_error(&b,TURBODB_STATUS_UNSUPPORTED,"grouped star projection is not supported"); break;
      }
      id = item->next;
    }
  }
  if (status == TURBODB_STATUS_OK) status = bind_named_window_keys(&b,max_depth);
  if (status == TURBODB_STATUS_OK) status = bind_projection(&b, statement->as.select.columns);
  const orm_sql_table_schema *base_schema = b.schema;
  if (status == TURBODB_STATUS_OK) status = bind_windows(&b,max_depth,&window_schema,&window_schema_bytes,
      &substitutions,&substitution_bytes);
  if (status == TURBODB_STATUS_OK && vec_size(&b.plan.windows)) {
    windowed_schema = (orm_sql_table_schema){schema->name,vec_data_const(&window_schema),vec_size(&window_schema)};
    b.schema = &windowed_schema; b.substitutions = vec_data_const(&substitutions); b.substitution_count = vec_size(&substitutions);
  }
  if (status == TURBODB_STATUS_OK) status = bind_computations(&b, max_depth);
  if (status == TURBODB_STATUS_OK) status = bind_order(&b, statement->as.select.order_by, max_depth);
  if (status == TURBODB_STATUS_OK) status = bind_distinct(&b);
  if (status == TURBODB_STATUS_OK) status = bind_limit(&b, statement->as.select.limit);
  if (status == TURBODB_STATUS_OK && b.plan.grouped) status = bind_having_aliases(&b,statement,&substitutions);
  b.schema = base_schema;
  if (status == TURBODB_STATUS_OK && b.plan.grouped) status = bind_expression(&b,statement->as.select.having,max_depth,
      (select_expression_target){&b.plan.filter,&b.plan.slots,&b.plan.slot_bytes,&b.plan.query_slots,&b.plan.query_slot_bytes},true);
  vec_t *scratch[] = {&group_schema,&substitutions,&b.nested_nodes,&b.group_capture,&window_schema,&b.named_windows};
  const size_t scratch_bytes[] = {group_schema_bytes,substitution_bytes,b.nested_node_bytes,b.group_capture_bytes,window_schema_bytes,b.named_window_bytes};
  for (size_t i = 0; i < sizeof(scratch)/sizeof(scratch[0]); ++i) {
    const turbodb_status_t released = orm_sql_work_release(scratch[i],scratch_bytes[i],budget,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  if (status != TURBODB_STATUS_OK) {
    const turbodb_status_t cleanup = orm_tidesdb_sql_select_destroy(&b.plan, NULL);
    return cleanup == TURBODB_STATUS_OK ? status : cleanup;
  }
  b.plan.correlated=b.correlated||(from&&from->correlated);
  *out = b.plan;
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_tidesdb_sql_select_bind_parameters(const sqlparser_document *document,
    const orm_sql_table_schema *schema, const orm_sql_type *parameter_types, size_t count,
    size_t max_depth, orm_tidesdb_sql_budget *budget, orm_sql_select *out, turbodb_error_t *error) {
  return select_bind_parameters(document,schema,parameter_types,count,max_depth,budget,out,error,false,NULL,0,NULL);
}
turbodb_status_t orm_tidesdb_sql_select_bind_explain(const sqlparser_document *document,
    const orm_sql_table_schema *schema, const orm_sql_type *parameter_types, size_t count,
    size_t max_depth, orm_tidesdb_sql_budget *budget, orm_sql_select *out, turbodb_error_t *error) {
  return select_bind_parameters(document,schema,parameter_types,count,max_depth,budget,out,error,true,NULL,0,NULL);
}
turbodb_status_t orm_tidesdb_sql_select_bind_from(const sqlparser_document *document,
    const orm_sql_from *from, size_t max_depth, orm_sql_select *out, turbodb_error_t *error) {
  const orm_sql_from_node *root = orm_tidesdb_sql_from_at(from,0);
  if (!root) return select_error(error,TURBODB_STATUS_INVALID_ARGUMENT,0,"SELECT requires a bound FROM plan");
  const sqlparser_node *statement = document ? sqlparser_get_node(document,sqlparser_statements(document).first) : NULL;
  return select_bind_parameters(document,&root->schema,vec_data_const(&from->parameter_types),
      vec_size(&from->parameter_types),max_depth,from->budget,out,error,statement && statement->kind == SQLPARSER_EXPLAIN,from,0,NULL);
}
turbodb_status_t orm_tidesdb_sql_select_bind_at(const orm_sql_query_scope *scope,
    const orm_sql_table_schema *schema, const orm_sql_from *from, orm_sql_select *out, turbodb_error_t *error) {
  if (!scope || !scope->root) return select_error(error,TURBODB_STATUS_INVALID_ARGUMENT,0,"SELECT query block required");
  return select_bind_parameters(scope->document,schema,scope->parameter_types,scope->parameter_count,
      scope->max_depth,scope->budget,out,error,false,from,scope->root,scope);
}
turbodb_status_t orm_tidesdb_sql_select_bind_tail(const orm_sql_query_scope *scope,
    const orm_sql_table_schema *schema, sqlparser_list orders, sqlparser_id limit,
    orm_sql_select *out, turbodb_error_t *error) {
  if (!scope || !scope->document || !scope->max_depth || !scope->budget || !schema || !schema->columns ||
      !schema->count || !out || out->budget || (scope->parameter_count && !scope->parameter_types))
    return select_error(error,TURBODB_STATUS_INVALID_ARGUMENT,0,"invalid query tail binding inputs");
  if (sqlparser_get_dialect(scope->document) != SQLPARSER_MYSQL || sqlparser_statements(scope->document).count != 1)
    return select_error(error,TURBODB_STATUS_UNSUPPORTED,0,"query tail requires one MySQL statement");
  const size_t outer_count=scope->outer_schema?scope->outer_schema->count:0;
  if (schema->count > scope->budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES] ||
      scope->parameter_count > SIZE_MAX-outer_count ||
      scope->parameter_count+outer_count>SIZE_MAX-schema->count ||
      schema->count+scope->parameter_count+outer_count >
      scope->budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES])
    return select_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,0,"query tail width exceeds plan capacity");
  select_binder b = {.document=scope->document,.schema=schema,.plan={.budget=scope->budget},.error=error,
      .queries=scope->queries,.query_count=scope->query_count,.anonymous_output=scope->anonymous_output,
      .outer_schema=scope->outer_schema,.outer_qualifier=scope->outer_qualifier,
      .parameter_resolved=scope->parameter_resolved};
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve_capacity(scope->budget,1,sizeof(b),0,&b.plan.metadata_bytes,error);
  if (status == TURBODB_STATUS_OK) status = bind_schema(&b,scope->scalar_output || scope->anonymous_output,true);
  if (status == TURBODB_STATUS_OK) status = bind_parameters(&b,scope->parameter_types,scope->parameter_count);
  if (status == TURBODB_STATUS_OK) status = bind_query_scope(&b,sqlparser_get_node(scope->document,scope->root));
  if (status == TURBODB_STATUS_OK) status = select_vector(&b,&b.plan.columns,schema->count,sizeof(orm_sql_select_column),
      _Alignof(orm_sql_select_column),&b.plan.column_bytes);
  if (status == TURBODB_STATUS_OK) status = select_vector(&b,&b.plan.projection,schema->count,sizeof(size_t),_Alignof(size_t),&b.plan.projection_bytes);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < schema->count; ++i)
    status = append_column(&b,i,i,schema->columns[i].name,schema->columns[i].type);
  if (status == TURBODB_STATUS_OK) status = bind_order(&b,orders,scope->max_depth);
  if (status == TURBODB_STATUS_OK) status = bind_limit(&b,limit);
  const turbodb_status_t scope_released = orm_sql_work_release(&b.nested_nodes,b.nested_node_bytes,scope->budget,
      status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = scope_released;
  if (status != TURBODB_STATUS_OK) {
    const turbodb_status_t released = orm_tidesdb_sql_select_destroy(&b.plan,NULL);
    return released == TURBODB_STATUS_OK ? status : released;
  }
  b.plan.correlated=b.correlated;
  *out = b.plan; return TURBODB_STATUS_OK;
}
const orm_sql_select_column *orm_tidesdb_sql_select_column_at(const orm_sql_select *program, size_t ordinal) {
  return program && program->budget && ordinal < vec_size(&program->columns) ?
      vec_at_const(&program->columns, ordinal) : NULL;
}
turbodb_status_t orm_tidesdb_sql_select_open(orm_sql_select *program, const turbodb_value_t *rows,
    size_t row_count, orm_sql_select_run *out, turbodb_error_t *error) {
  return orm_tidesdb_sql_select_open_parameters(program, rows, row_count, NULL, 0, out, error);
}
static turbodb_status_t select_bound_value(const orm_sql_select *program, const orm_sql_select_bound *bound,
    const turbodb_value_t *parameters, uint64_t *out, const char *operation, turbodb_error_t *error) {
  if (!bound->is_parameter) { *out = bound->literal; return TURBODB_STATUS_OK; }
  const turbodb_value_t *value = &parameters[bound->parameter];
  const orm_sql_type *type = vec_at_const(&program->parameter_types, bound->parameter);
  const size_t offset = (size_t)*(const uint64_t *)vec_at_const(&program->parameter_offsets, bound->parameter);
  orm_sql_predicate validator; turbodb_value_t ignored;
  turbodb_error_t cause; tdsql_error_init(&cause);
  turbodb_status_t status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL, *type, NULL, &validator, &cause);
  if (status == TURBODB_STATUS_OK)
    status = orm_tidesdb_sql_predicate_eval(&validator, value, NULL, program->budget, &ignored, &cause);
  if (status != TURBODB_STATUS_OK) return select_error(error, status, offset, cause.message);
  if (value->kind == TURBODB_VALUE_NULL || (value->kind == TURBODB_VALUE_INT64 && value->data.int64_value < 0)) {
    char message[TURBODB_ERROR_MESSAGE_CAPACITY];
    (void)snprintf(message,sizeof(message),"%s parameter must be non-NULL and nonnegative",operation);
    return select_error(error, TURBODB_STATUS_TYPE_ERROR, offset, message);
  }
  *out = value->kind == TURBODB_VALUE_UINT64 ? value->data.uint64_value : (uint64_t)value->data.int64_value;
  return TURBODB_STATUS_OK;
}
static turbodb_status_t select_window_argument(const orm_sql_select *program, const select_window_clause *window,
    const turbodb_value_t *parameters, uint64_t *out, turbodb_error_t *error) {
  const bool offset = orm_sql_window_offset_kind(window->kind);
  const bool nth = window->kind == ORM_SQL_NTH_VALUE;
  if (window->kind != ORM_SQL_NTILE && !offset && !nth) { *out=0; return TURBODB_STATUS_OK; }
  uint64_t value = 0;
  turbodb_status_t status = select_bound_value(program,&window->argument,parameters,&value,nth ? "NTH_VALUE" : offset ? "LAG/LEAD" : "NTILE",error);
  if (status != TURBODB_STATUS_OK) return status;
  if ((!offset && !value) || value > (nth ? ORM_SQL_WINDOW_MAX_NTH : ORM_SQL_WINDOW_MAX_OFFSET))
    return select_error(error,TURBODB_STATUS_SQL_ERROR,0,nth ? "NTH_VALUE position must be 1..INT64_MAX" :
        offset ? "LAG/LEAD offset must be 0..2^63" : "NTILE bucket count must be 1..2^63");
  *out = value; return TURBODB_STATUS_OK;
}
static turbodb_status_t select_validate_frames(const orm_sql_select *program, const turbodb_value_t *parameters, turbodb_error_t *error) {
  for (size_t i=0;i<vec_size(&program->window_frames);++i) {
    const select_window_frame *frame=vec_at_const(&program->window_frames,i);
    for (size_t j=0;j<SELECT_FRAME_BOUNDARIES;++j) {
      const select_frame_boundary *bound=&frame->boundaries[j];
      if (bound->kind != SQLPARSER_BOUND_PRECEDING && bound->kind != SQLPARSER_BOUND_FOLLOWING) continue;
      orm_sql_budget_amount steps={0}; steps.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=1;
      turbodb_status_t status=orm_tidesdb_sql_budget_reserve(program->budget,&steps,error);
      if (status != TURBODB_STATUS_OK) return status;
      const turbodb_value_t *value=bound->is_parameter ? &parameters[bound->parameter] : &bound->literal;
      if (value->kind == TURBODB_VALUE_NULL || (value->kind == TURBODB_VALUE_INT64 && value->data.int64_value < 0) ||
          (value->kind == TURBODB_VALUE_DOUBLE && value->data.double_value < 0))
        return select_error(error,TURBODB_STATUS_TYPE_ERROR,bound->offset,"frame offset must be non-NULL and nonnegative");
    }
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t select_validate_windows(const orm_sql_select *program, const turbodb_value_t *parameters, turbodb_error_t *error) {
  for (size_t i = 0; i < vec_size(&program->windows); ++i) {
    uint64_t buckets;
    turbodb_status_t status = select_window_argument(program,vec_at_const(&program->windows,i),parameters,&buckets,error);
    if (status != TURBODB_STATUS_OK) return status;
  }
  return select_validate_frames(program,parameters,error);
}
turbodb_status_t orm_tidesdb_sql_select_validate_parameters(const orm_sql_select *program,
    const turbodb_value_t *parameters, size_t count, uint64_t *offset, uint64_t *limit, turbodb_error_t *error) {
  if (!program || !program->budget || !offset || !limit || count != vec_size(&program->parameter_types) || (count && !parameters))
    return select_error(error,TURBODB_STATUS_INVALID_ARGUMENT,0,"invalid SELECT parameter validation arguments");
  for (size_t i = 0; i < count; ++i) {
    orm_sql_predicate validator; turbodb_value_t ignored;
    turbodb_status_t status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL,
        *(const orm_sql_type *)vec_at_const(&program->parameter_types,i),NULL,&validator,error);
    if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_predicate_eval(&validator,&parameters[i],NULL,program->budget,&ignored,error);
    if (status != TURBODB_STATUS_OK) return status;
  }
  uint64_t page_offset = 0, page_limit = 0;
  turbodb_status_t status = select_bound_value(program,&program->offset,parameters,&page_offset,"LIMIT",error);
  if (status == TURBODB_STATUS_OK) status = select_bound_value(program,&program->limit,parameters,&page_limit,"LIMIT",error);
  if (status == TURBODB_STATUS_OK) status = select_validate_windows(program,parameters,error);
  if (status == TURBODB_STATUS_OK) { *offset = page_offset; *limit = page_limit; }
  return status;
}
static turbodb_status_t select_group_pull(void *context, const turbodb_value_t **out, turbodb_error_t *error) {
  select_group_run *run = context; orm_sql_scan_row row;
  const turbodb_status_t status = orm_tidesdb_sql_scan_next(&run->input,&row,error);
  if (status == TURBODB_STATUS_OK) *out = row.state == ORM_SQL_SCAN_ROW ? row.values : NULL;
  return status;
}
static turbodb_status_t select_group_close(orm_sql_select_run *run, orm_tidesdb_sql_budget *budget, turbodb_error_t *error) {
  turbodb_status_t status = TURBODB_STATUS_OK;
  if (vec_size(&run->group_run)) {
    select_group_run *group = vec_at(&run->group_run,0);
    status = orm_tidesdb_sql_aggregate_close(&group->aggregate,error);
    const turbodb_status_t released = orm_tidesdb_sql_scan_close(&group->input,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
    vec_t *vectors[] = {&group->items,&group->expressions,&group->types,&group->projection};
    const size_t bytes[] = {group->item_bytes,group->expression_bytes,group->type_bytes,group->projection_bytes};
    for (size_t i = 0; i < sizeof(vectors)/sizeof(vectors[0]); ++i) {
      const turbodb_status_t cleanup = orm_sql_work_release(vectors[i],bytes[i],budget,status == TURBODB_STATUS_OK ? error : NULL);
      if (status == TURBODB_STATUS_OK) status = cleanup;
    }
  }
  const turbodb_status_t released = orm_sql_work_release(&run->group_run,run->group_run_bytes,budget,
      status == TURBODB_STATUS_OK ? error : NULL);
  run->group_run_bytes = 0;
  return status == TURBODB_STATUS_OK ? released : status;
}
/* Preserve output slots for HAVING while dropping only unused aggregate work.
 * The bound plan remains immutable; derived input specs belong to this run. */
static turbodb_status_t select_group_cardinality(orm_sql_select *program, select_group_run *group,
    orm_sql_scan_spec *input, orm_sql_aggregate_spec *aggregate, turbodb_error_t *error) {
  const size_t count = vec_size(&program->aggregate_items), columns = vec_size(&program->pre_types);
  if (!count) return TURBODB_STATUS_OK;
  turbodb_status_t status = orm_sql_work_zero(&group->items,count,sizeof(orm_sql_aggregate_item),
      _Alignof(orm_sql_aggregate_item),program->budget,&group->item_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&group->expressions,columns,sizeof(orm_sql_scan_expression),
      _Alignof(orm_sql_scan_expression),program->budget,&group->expression_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&group->types,columns,sizeof(orm_sql_type),
      _Alignof(orm_sql_type),program->budget,&group->type_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&group->projection,columns,sizeof(size_t),
      _Alignof(size_t),program->budget,&group->projection_bytes,error);
  if (status != TURBODB_STATUS_OK) return status;
  orm_sql_budget_amount steps = {0}; steps.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = columns;
  status = orm_tidesdb_sql_budget_reserve(program->budget,&steps,error);
  if (status != TURBODB_STATUS_OK) return status;
  memcpy(vec_data(&group->expressions),vec_data_const(&program->pre_expressions),columns*sizeof(orm_sql_scan_expression));
  memcpy(vec_data(&group->types),vec_data_const(&program->pre_types),columns*sizeof(orm_sql_type));
  memcpy(vec_data(&group->projection),vec_data_const(&program->pre_projection),columns*sizeof(size_t));
  for (size_t i = 0; i < count; ++i) {
    steps.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = vec_size(&program->slots)+1;
    status = orm_tidesdb_sql_budget_reserve(program->budget,&steps,error);
    if (status != TURBODB_STATUS_OK) return status;
    const size_t output = aggregate->key_count+i;
    bool used = false;
    for (size_t j = 0; j < vec_size(&program->slots); ++j)
      if (*(const size_t *)vec_at_const(&program->slots,j) == output) { used = true; break; }
    orm_sql_aggregate_item *item = vec_at(&group->items,i);
    *item = *(const orm_sql_aggregate_item *)vec_at_const(&program->aggregate_items,i);
    if (used) continue;
    /* Even an unused aggregate keeps implicit grouping on empty input. */
    const size_t slot = item->slot, arguments=orm_sql_aggregate_arguments(item);
    *item = (orm_sql_aggregate_item){ORM_SQL_COUNT_ALL,0};
    for(size_t argument=0;argument<arguments;++argument) {
      *(orm_sql_scan_expression *)vec_at(&group->expressions,slot+argument) = (orm_sql_scan_expression){0};
      *(size_t *)vec_at(&group->projection,slot+argument) = 0;
      *(orm_sql_type *)vec_at(&group->types,slot+argument) = *(const orm_sql_type *)vec_at_const(&program->types,0);
    }
  }
  input->expressions = vec_data_const(&group->expressions);
  input->projection = vec_data_const(&group->projection);
  aggregate->items = vec_data_const(&group->items);
  return TURBODB_STATUS_OK;
}
static turbodb_status_t select_window_pull(void *context, const turbodb_value_t **out, turbodb_error_t *error) {
  select_window_run *stage = context; orm_sql_scan_row row;
  const turbodb_status_t status = orm_tidesdb_sql_scan_next(&stage->input,&row,error);
  if (status == TURBODB_STATUS_OK) *out = row.state == ORM_SQL_SCAN_ROW ? row.values : NULL;
  return status;
}
static turbodb_status_t select_window_close(orm_sql_select_run *run, orm_tidesdb_sql_budget *budget, turbodb_error_t *error) {
  if (vec_size(&run->window_run)) {
    select_window_run *stage = vec_at(&run->window_run,0);
    for (size_t i = vec_size(&stage->stages); i > 0; --i) {
      const turbodb_status_t status = orm_tidesdb_sql_window_close(vec_at(&stage->stages,i-1),error);
      if (status != TURBODB_STATUS_OK) return status;
    }
    turbodb_status_t status = orm_tidesdb_sql_scan_close(&stage->input,error);
    if (status != TURBODB_STATUS_OK) return status;
    status = orm_sql_work_release(&stage->stages,stage->stage_bytes,budget,error);
    if (status != TURBODB_STATUS_OK) return status;
    stage->stage_bytes = 0;
  }
  const turbodb_status_t status = orm_sql_work_release(&run->window_run,run->window_run_bytes,budget,error);
  run->window_run_bytes = 0; return status;
}
static orm_sql_window_frame select_runtime_frame(const orm_sql_select *program, const select_window_clause *window,
    const turbodb_value_t *parameters) {
  static const orm_sql_frame_boundary_kind kinds[]={ORM_SQL_BOUND_UNBOUNDED_PRECEDING,ORM_SQL_BOUND_PRECEDING,
      ORM_SQL_BOUND_CURRENT_ROW,ORM_SQL_BOUND_FOLLOWING,ORM_SQL_BOUND_UNBOUNDED_FOLLOWING};
  const select_window_frame source=select_frame_specification(program,window);
  orm_sql_window_frame frame={.unit=source.unit == SQLPARSER_FRAME_ROWS ? ORM_SQL_FRAME_ROWS : ORM_SQL_FRAME_RANGE};
  for (size_t i=0;i<SELECT_FRAME_BOUNDARIES;++i) {
    const select_frame_boundary *bound=&source.boundaries[i];
    frame.boundaries[i].kind=kinds[bound->kind];
    frame.boundaries[i].distance=bound->is_parameter ? parameters[bound->parameter] : bound->literal;
  }
  return frame;
}
static turbodb_status_t select_window_open(orm_sql_select *program, const orm_sql_memory_source *memory,
    orm_sql_row_source *pull, const orm_sql_scan_spec *final, orm_sql_select_run *out, turbodb_error_t *error) {
  turbodb_status_t status = orm_sql_work_zero(&out->window_run,1,sizeof(select_window_run),_Alignof(select_window_run),
      program->budget,&out->window_run_bytes,error);
  if (status != TURBODB_STATUS_OK) return status;
  select_window_run *stage = vec_at(&out->window_run,0);
  const size_t count = vec_size(&program->windows);
  status = orm_sql_work_zero(&stage->stages,count,sizeof(orm_sql_window),_Alignof(orm_sql_window),
      program->budget,&stage->stage_bytes,error);
  const orm_sql_scan_spec input = {.filter=program->filter.budget ? &program->filter : NULL,
      .filter_slots=vec_data_const(&program->slots),.filter_count=vec_size(&program->slots),
      .projection=vec_data_const(&program->window_projection),.projection_count=vec_size(&program->window_projection),
      .limit=UINT64_MAX,.parameters=final->parameters,.parameter_types=final->parameter_types,.parameter_count=final->parameter_count,
      .expressions=vec_data_const(&program->window_expressions),.queries=final->queries,.query_count=final->query_count,
      .filter_query_slots=vec_data_const(&program->query_slots),.filter_query_count=vec_size(&program->query_slots),
      .evaluation=final->evaluation};
  if (status == TURBODB_STATUS_OK) status = pull ? orm_tidesdb_sql_scan_open_source(pull,&input,program->budget,&stage->input,error) :
      orm_tidesdb_sql_scan_open(memory,&input,program->budget,&stage->input,error);
  stage->source = (orm_sql_row_source){program->budget,vec_data_const(&program->window_types),
      vec_size(&program->window_types),stage,select_window_pull,false};
  orm_sql_row_source *source = &stage->source;
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < count; ++i) {
    const select_window_clause *window = vec_at_const(&program->windows,i);
    uint64_t buckets = 0;
    status = select_window_argument(program,window,final->parameters,&buckets,error);
    const size_t *keys = window->partition_count ? vec_at_const(&program->window_keys,window->first_key) : NULL;
    const orm_sql_scan_order *orders = window->order_count ?
        vec_at_const(&program->window_orders,window->first_key+window->partition_count) : NULL;
    const bool offset = orm_sql_window_offset_kind(window->kind);
    const size_t value_slot = program->window_base_columns+window->first_key+window->partition_count+window->order_count;
    const orm_sql_window_spec spec = {.kind=window->kind,.partitions=keys,.partition_count=window->partition_count,
        .orders=orders,.order_count=window->order_count,.buckets=window->kind == ORM_SQL_NTILE ? buckets : 0,
        .offset=offset || window->kind == ORM_SQL_NTH_VALUE ? buckets : 0,
        .value_slot=orm_sql_window_value_kind(window->kind) ? value_slot : 0,.default_slot=window->value_count == 2 ? value_slot+1 : 0,
        .has_default=window->value_count == 2,.frame=select_runtime_frame(program,window,final->parameters)};
    orm_sql_window *operator = vec_at(&stage->stages,i);
    if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_window_open(source,&spec,operator,error);
    if (status == TURBODB_STATUS_OK) source = orm_tidesdb_sql_window_source(operator);
  }
  orm_sql_scan_spec output = *final;
  output.filter = NULL; output.filter_slots = NULL; output.filter_count = 0;
  output.filter_query_slots = NULL; output.filter_query_count = 0;
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_scan_open_source(source,&output,program->budget,&out->scan,error);
  return status;
}
static turbodb_status_t select_group_open(orm_sql_select *program, const orm_sql_memory_source *memory,
    orm_sql_row_source *pull, const orm_sql_scan_spec *final, bool prune, orm_sql_select_run *out, turbodb_error_t *error) {
  turbodb_status_t status = orm_sql_work_zero(&out->group_run,1,sizeof(select_group_run),_Alignof(select_group_run),
      program->budget,&out->group_run_bytes,error);
  if (status != TURBODB_STATUS_OK) return status;
  select_group_run *group = vec_at(&out->group_run,0);
  orm_sql_scan_spec input = {.filter=program->pre_filter.budget ? &program->pre_filter : NULL,
      .filter_slots=vec_data_const(&program->pre_slots),.filter_count=vec_size(&program->pre_slots),
      .projection=vec_data_const(&program->pre_projection),.projection_count=vec_size(&program->pre_projection),
      .limit=UINT64_MAX,.parameters=final->parameters,.parameter_types=final->parameter_types,.parameter_count=final->parameter_count,
      .expressions=vec_data_const(&program->pre_expressions),.queries=final->queries,.query_count=final->query_count,
      .filter_query_slots=vec_data_const(&program->pre_query_slots),.filter_query_count=vec_size(&program->pre_query_slots),
      .evaluation=final->evaluation};
  orm_sql_aggregate_spec spec = {vec_data_const(&program->group_keys),vec_size(&program->group_keys),
      vec_data_const(&program->aggregate_items),vec_size(&program->aggregate_items)};
  if (prune) status = select_group_cardinality(program,group,&input,&spec,error);
  if (status == TURBODB_STATUS_OK) status = pull ? orm_tidesdb_sql_scan_open_source(pull,&input,program->budget,&group->input,error) :
      orm_tidesdb_sql_scan_open(memory,&input,program->budget,&group->input,error);
  group->source = (orm_sql_row_source){program->budget,
      group->types.initialized ? vec_data_const(&group->types) : vec_data_const(&program->pre_types),
      vec_size(&program->pre_types),group,select_group_pull,false};
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_aggregate_open(&group->source,&spec,&group->aggregate,error);
  if (status == TURBODB_STATUS_OK) status = vec_size(&program->windows) && !prune ?
      select_window_open(program,NULL,orm_tidesdb_sql_aggregate_source(&group->aggregate),final,out,error) :
      orm_tidesdb_sql_scan_open_source(orm_tidesdb_sql_aggregate_source(&group->aggregate),final,program->budget,&out->scan,error);
  return status;
}
static turbodb_status_t select_open(orm_sql_select *program, const turbodb_value_t *rows,
    size_t row_count, const turbodb_value_t *parameters, size_t parameter_count,
    orm_sql_select_run *out, turbodb_error_t *error, orm_sql_row_source *pull,
    const orm_sql_expr_query_sources *queries, orm_sql_query_demand demand) {
  if (!program || !program->budget || !out || out->program || out->scan.budget || out->group_run.initialized || out->window_run.initialized ||
      parameter_count != vec_size(&program->parameter_types) || (parameter_count && !parameters))
    return select_error(error, TURBODB_STATUS_INVALID_ARGUMENT, 0, "invalid SELECT run arguments");
  if (program->active_runs == SIZE_MAX)
    return select_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, 0, "SELECT active run count overflow");
  if (pull) {
    if (!pull->types || pull->columns != vec_size(&program->types))
      return select_error(error, TURBODB_STATUS_INVALID_ARGUMENT, 0, "SELECT source shape differs from schema");
    for (size_t i = 0; i < pull->columns; ++i) {
      const orm_sql_type *type = vec_at_const(&program->types, i);
      if (type->kind != pull->types[i].kind || type->nullable != pull->types[i].nullable)
        return select_error(error, TURBODB_STATUS_TYPE_ERROR, 0, "SELECT source types differ from schema");
    }
  }
  const orm_sql_memory_source source = {rows, row_count, vec_size(&program->types), vec_data_const(&program->types)};
  uint64_t offset = 0, limit = 0;
  turbodb_status_t status = select_bound_value(program, &program->offset, parameters, &offset, "LIMIT", error);
  if (status == TURBODB_STATUS_OK) status = select_bound_value(program, &program->limit, parameters, &limit, "LIMIT", error);
  if (status == TURBODB_STATUS_OK) status = select_validate_windows(program,parameters,error);
  if (status != TURBODB_STATUS_OK) return status;
  orm_sql_scan_spec spec = {program->filter.budget ? &program->filter : NULL,
      vec_data_const(&program->slots), vec_size(&program->slots), vec_data_const(&program->projection),
      vec_size(&program->projection), offset, limit, parameters,
      vec_data_const(&program->parameter_types), parameter_count, vec_data_const(&program->expressions),
      vec_data_const(&program->orders), vec_size(&program->orders), program->distinct,
      queries ? queries->items : NULL, queries ? queries->count : 0,
      vec_data_const(&program->query_slots),vec_size(&program->query_slots),
      queries ? queries->evaluation : (orm_sql_evaluation){0}};
  const bool cardinality = demand != ORM_SQL_QUERY_VALUES;
  if (demand == ORM_SQL_QUERY_EXISTENCE && !offset) spec.distinct = false;
  if (demand == ORM_SQL_QUERY_EXISTENCE && spec.limit > 1) spec.limit = 1;
  const size_t witness = 0;
  if (cardinality) {
    spec.orders = NULL; spec.order_count = 0;
    if (!spec.distinct) {
      spec.projection = &witness; spec.projection_count = 1; spec.expressions = NULL;
    }
  }
  size_t bytes = 0;
  status = orm_tidesdb_sql_budget_reserve_capacity(program->budget, 1,
      sizeof(*out)-sizeof(out->scan), 0, &bytes, error);
  if (status != TURBODB_STATUS_OK) return status;
  status = program->grouped ? select_group_open(program,&source,pull,&spec,cardinality && !spec.distinct,out,error) :
      vec_size(&program->windows) && (!cardinality || spec.distinct) ? select_window_open(program,&source,pull,&spec,out,error) :
      pull ? orm_tidesdb_sql_scan_open_source(pull, &spec, program->budget, &out->scan, error) :
      orm_tidesdb_sql_scan_open(&source, &spec, program->budget, &out->scan, error);
  if (status != TURBODB_STATUS_OK) {
    const turbodb_status_t window_cleanup = select_window_close(out,program->budget,NULL);
    const turbodb_status_t cleanup = window_cleanup == TURBODB_STATUS_OK ? select_group_close(out,program->budget,NULL) : window_cleanup;
    const turbodb_status_t released = orm_tidesdb_sql_budget_release(program->budget, ORM_SQL_BUDGET_WORK_BYTES, bytes, NULL);
    return cleanup != TURBODB_STATUS_OK ? cleanup : released == TURBODB_STATUS_OK ? status : released;
  }
  out->program = program; ++program->active_runs;
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_tidesdb_sql_select_open_parameters(orm_sql_select *program, const turbodb_value_t *rows,
    size_t row_count, const turbodb_value_t *parameters, size_t parameter_count,
    orm_sql_select_run *out, turbodb_error_t *error) {
  return select_open(program, rows, row_count, parameters, parameter_count, out, error, NULL, NULL, ORM_SQL_QUERY_VALUES);
}
turbodb_status_t orm_tidesdb_sql_select_open_source(orm_sql_select *program, orm_sql_row_source *source,
    const turbodb_value_t *parameters, size_t parameter_count, orm_sql_select_run *out, turbodb_error_t *error) {
  if (!source) return select_error(error, TURBODB_STATUS_INVALID_ARGUMENT, 0, "missing SELECT source");
  return select_open(program, NULL, 0, parameters, parameter_count, out, error, source, NULL, ORM_SQL_QUERY_VALUES);
}

turbodb_status_t orm_tidesdb_sql_select_open_source_queries(orm_sql_select *program, orm_sql_row_source *source,
    const turbodb_value_t *parameters, size_t parameter_count, const orm_sql_expr_query_sources *queries,
    orm_sql_select_run *out, turbodb_error_t *error) {
  if (!source) return select_error(error,TURBODB_STATUS_INVALID_ARGUMENT,0,"missing SELECT source");
  return select_open(program,NULL,0,parameters,parameter_count,out,error,source,queries,ORM_SQL_QUERY_VALUES);
}

turbodb_status_t orm_sql_select_open_cardinality(orm_sql_select *program, orm_sql_row_source *source,
    const turbodb_value_t *parameters, size_t parameter_count, const orm_sql_expr_query_sources *queries,
    orm_sql_select_run *out, turbodb_error_t *error) {
  if (!source) return select_error(error,TURBODB_STATUS_INVALID_ARGUMENT,0,"missing SELECT source");
  return select_open(program,NULL,0,parameters,parameter_count,out,error,source,queries,ORM_SQL_QUERY_CARDINALITY);
}

turbodb_status_t orm_sql_select_open_demand(orm_sql_select *program, orm_sql_row_source *source,
    const turbodb_value_t *parameters, size_t parameter_count, const orm_sql_expr_query_sources *queries,
    orm_sql_query_demand demand, orm_sql_select_run *out, turbodb_error_t *error) {
  if (!source || demand < ORM_SQL_QUERY_VALUES || demand > ORM_SQL_QUERY_EXISTENCE)
    return select_error(error,TURBODB_STATUS_INVALID_ARGUMENT,0,"invalid SELECT demand");
  return select_open(program,NULL,0,parameters,parameter_count,out,error,source,queries,demand);
}

turbodb_status_t orm_tidesdb_sql_select_close(orm_sql_select_run *run, turbodb_error_t *error) {
  if (!run || !run->program) return TURBODB_STATUS_OK;
  if (run->scan.evaluating)
    return select_error(error,TURBODB_STATUS_BUSY,0,"SELECT is evaluating");
  orm_sql_select *program = run->program;
  turbodb_status_t status = orm_tidesdb_sql_scan_close(&run->scan, error);
  if (status != TURBODB_STATUS_OK) return status;
  status = select_window_close(run,program->budget,error);
  if (status != TURBODB_STATUS_OK) return status;
  const turbodb_status_t groups_released = select_group_close(run,program->budget,status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = groups_released;
  const turbodb_status_t released = orm_tidesdb_sql_budget_release(program->budget,
      ORM_SQL_BUDGET_WORK_BYTES, sizeof(*run)-sizeof(run->scan), status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  --program->active_runs; *run = (orm_sql_select_run){0};
  return status;
}
turbodb_status_t orm_tidesdb_sql_select_destroy(orm_sql_select *program, turbodb_error_t *error) {
  if (!program || !program->budget) return TURBODB_STATUS_OK;
  if (program->active_runs || program->filter.active_runs || program->pre_filter.active_runs)
    return select_error(error, TURBODB_STATUS_BUSY, 0, "SELECT plan has active runs");
  vec_t *computations[] = {&program->computations,&program->order_computations,&program->group_computations,&program->key_computations,&program->window_computations};
  for (size_t k = 0; k < sizeof(computations)/sizeof(computations[0]); ++k)
    for (size_t i = 0; i < vec_size(computations[k]); ++i)
      if (((const select_computation *)vec_at_const(computations[k],i))->program.active_runs)
        return select_error(error, TURBODB_STATUS_BUSY, 0, "SELECT projection has active runs");
  turbodb_status_t status = orm_tidesdb_sql_expr_destroy(&program->filter, error);
  const turbodb_status_t pre_released = orm_tidesdb_sql_expr_destroy(&program->pre_filter,status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = pre_released;
  for (size_t k = 0; k < sizeof(computations)/sizeof(computations[0]); ++k) {
    for (size_t i = 0; i < vec_size(computations[k]); ++i) {
      select_computation *computation = vec_at(computations[k],i);
      const turbodb_status_t released = select_computation_destroy(computation,program->budget,status == TURBODB_STATUS_OK ? error : NULL);
      if (status == TURBODB_STATUS_OK) status = released;
    }
  }
  vec_t *vectors[] = {&program->types, &program->columns, &program->projection, &program->slots,
                     &program->parameter_types, &program->parameter_offsets, &program->computations, &program->expressions, &program->orders, &program->order_computations,
                     &program->pre_slots,&program->group_keys,&program->aggregate_items,&program->group_computations,&program->pre_projection,&program->pre_expressions,&program->pre_types,
                     &program->key_computations,&program->query_slots,&program->pre_query_slots,
                     &program->windows,&program->window_computations,&program->window_projection,&program->window_expressions,
                     &program->window_types,&program->window_orders,&program->window_keys,&program->window_frames};
  const size_t bytes[] = {program->type_bytes, program->column_bytes, program->projection_bytes, program->slot_bytes,
                         program->parameter_type_bytes, program->parameter_offset_bytes,
                         program->computation_bytes, program->expression_bytes, program->order_bytes, program->order_computation_bytes,
                         program->pre_slot_bytes,program->group_key_bytes,program->aggregate_item_bytes,program->group_computation_bytes,
                         program->pre_projection_bytes,program->pre_expression_bytes,program->pre_type_bytes,program->key_computation_bytes,program->query_slot_bytes,program->pre_query_slot_bytes,
                         program->window_bytes,program->window_computation_bytes,program->window_projection_bytes,program->window_expression_bytes,
                         program->window_type_bytes,program->window_order_bytes,program->window_key_bytes,program->window_frame_bytes};
  for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
    const turbodb_status_t released = orm_sql_work_release(vectors[i], bytes[i], program->budget,
        status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  if (program->metadata_bytes) {
    const turbodb_status_t released = orm_tidesdb_sql_budget_release(program->budget, ORM_SQL_BUDGET_WORK_BYTES,
        program->metadata_bytes, status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  *program = (orm_sql_select){0};
  return status;
}

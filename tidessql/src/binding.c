#include "binding.h"
#include "name.h"
#include "work.h"
#include "error.h"
#include <stdio.h>
#include <string.h>

bool orm_sql_bind_aggregate_kind(const sqlparser_document *document, const sqlparser_node *node,
    orm_sql_aggregate_kind *kind) {
  if (!node || node->kind != SQLPARSER_CALL) return false;
  const sqlparser_node *name = sqlparser_get_node(document,node->as.call.name);
  if (!name || name->kind != SQLPARSER_NAME) return false;
  const char *text = sqlparser_text(document,name->span);
  const char *names[] = {"COUNT","MIN","MAX","SUM","AVG","VAR_POP","VAR_SAMP","STDDEV_POP","STDDEV_SAMP",
      "VARIANCE","STD","STDDEV","BIT_AND","BIT_OR","BIT_XOR"};
  const orm_sql_aggregate_kind kinds[] = {ORM_SQL_COUNT_VALUE,ORM_SQL_MIN,ORM_SQL_MAX,ORM_SQL_SUM,ORM_SQL_AVG,
      ORM_SQL_VAR_POP,ORM_SQL_VAR_SAMP,ORM_SQL_STDDEV_POP,ORM_SQL_STDDEV_SAMP,
      ORM_SQL_VAR_POP,ORM_SQL_STDDEV_POP,ORM_SQL_STDDEV_POP,ORM_SQL_BIT_AND,ORM_SQL_BIT_OR,ORM_SQL_BIT_XOR};
  for (size_t i = 0; i < sizeof(names)/sizeof(names[0]); ++i) {
    if (name->span.length != strlen(names[i])) continue;
    bool same = true;
    for (size_t j = 0; same && j < name->span.length; ++j) {
      const char c = text[j] >= 'a' && text[j] <= 'z' ? text[j]-('a'-'A') : text[j];
      same = c == names[i][j];
    }
    if (same) { *kind=kinds[i]; return true; }
  }
  return false;
}
typedef struct expression_binding { bool seen, input, query; size_t slot; } expression_binding;
typedef struct binding_context {
  const sqlparser_document *document;
  const orm_sql_table_schema *schema;
  vstr qualifier;
  const orm_sql_type *parameter_types;
  const uint64_t *parameter_offsets;
  const bool *parameter_resolved;
  size_t parameter_count, parameter_marker_count, offset;
  const orm_sql_table_schema *outer_schema;
  vstr outer_qualifier;
  bool *correlated;
  const size_t *capture_slots;
  size_t capture_count;
  orm_tidesdb_sql_budget *budget;
  turbodb_error_t *error;
  const size_t *substitutions;
  size_t substitution_count;
  const orm_sql_expr_query_binding *queries;
  size_t query_count;
  bool hidden_input;
  bool ascii_insensitive_names;
} binding_context;
turbodb_status_t orm_sql_bind_statement_scope(const orm_sql_query_scope *scope,
    turbodb_error_t *error) {
  if (!scope || !scope->document || !scope->budget || !scope->max_depth ||
      !scope->root || (scope->parameter_count && !scope->parameter_types)) {
    tdsql_error_set(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid type-only binding scope");
    return TURBODB_STATUS_INVALID_ARGUMENT;
  }
  if (sqlparser_get_dialect(scope->document) != SQLPARSER_MYSQL ||
      sqlparser_statements(scope->document).count != 1) {
    tdsql_error_set(error,TURBODB_STATUS_UNSUPPORTED,"type-only binding requires one complete MySQL statement");
    return TURBODB_STATUS_UNSUPPORTED;
  }
  const size_t nodes = sqlparser_node_count(scope->document);
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = nodes;
  return orm_tidesdb_sql_budget_reserve(scope->budget, &amount, error);
}
turbodb_status_t orm_sql_bind_write_scope(const orm_sql_query_scope *scope,
    turbodb_error_t *error) {
  if (scope && scope->document && scope->root &&
      scope->root != sqlparser_statements(scope->document).first) {
    tdsql_error_set(error,TURBODB_STATUS_UNSUPPORTED,"write binding requires the complete statement root");
    return TURBODB_STATUS_UNSUPPORTED;
  }
  return orm_sql_bind_statement_scope(scope,error);
}
turbodb_status_t orm_sql_bind_parameter_offsets(const sqlparser_document *document,
    size_t count, orm_tidesdb_sql_budget *budget, vec_t *offsets, size_t *bytes, turbodb_error_t *error) {
  const size_t nodes = sqlparser_node_count(document); size_t markers = 0;
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = nodes;
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve(budget, &amount, error);
  if (status != TURBODB_STATUS_OK) return status;
  for (size_t i = 1; i <= nodes; ++i)
    if (sqlparser_get_node(document, (sqlparser_id)i)->kind == SQLPARSER_PARAMETER) ++markers;
  if (markers != count) {
    tdsql_error_set(error, TURBODB_STATUS_SQL_ERROR, "SQL binding parameter count does not match markers");
    return TURBODB_STATUS_SQL_ERROR;
  }
  status = orm_sql_work_zero(offsets, count, sizeof(uint64_t), _Alignof(uint64_t), budget, bytes, error);
  if (status != TURBODB_STATUS_OK) return status;
  size_t index = 0;
  for (size_t i = 1; i <= nodes; ++i) {
    const sqlparser_node *node = sqlparser_get_node(document, (sqlparser_id)i);
    if (node->kind == SQLPARSER_PARAMETER) *(uint64_t *)vec_at(offsets, index++) = node->span.offset;
  }
  return orm_sql_work_sort_u64(vec_data(offsets), count, budget, error);
}
static turbodb_status_t binding_error(binding_context *b, turbodb_status_t status, const char *reason) {
  char message[TURBODB_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message), "TidesDB SQL expression at byte %zu: %s", b->offset, reason);
  tdsql_error_set(b->error, status, message); return status;
}
static turbodb_status_t binding_charge(binding_context *b, orm_sql_budget_resource resource, size_t count) {
  orm_sql_budget_amount amount = {0}; amount.value[resource] = count;
  return orm_tidesdb_sql_budget_reserve(b->budget, &amount, b->error);
}
static turbodb_status_t binding_vector(binding_context *b, vec_t *v, size_t count,
    size_t width, size_t align, size_t *bytes) {
  return orm_sql_work_zero(v, count, width, align, b->budget, bytes, b->error);
}
static turbodb_status_t binding_name(binding_context *b, sqlparser_id id, vstr *out, vstr *qualifier) {
  const sqlparser_node *node = sqlparser_get_node(b->document, id);
  if (!node || node->kind != SQLPARSER_NAME) return binding_error(b, TURBODB_STATUS_UNSUPPORTED, "expected a column name");
  b->offset = node->span.offset;
  vstr text = {sqlparser_text(b->document, node->span), node->span.length}; const char *reason = NULL;
  turbodb_status_t status = orm_sql_name_part(&text, out, &reason);
  if (status != TURBODB_STATUS_OK) return binding_error(b, status, reason);
  *qualifier = (vstr){0};
  if (text.len && text.data[0] == '.') {
    *qualifier = *out;
    ++text.data; --text.len;
    status = orm_sql_name_part(&text, out, &reason);
    if (status != TURBODB_STATUS_OK) return binding_error(b, status, reason);
  }
  return text.len ? binding_error(b, TURBODB_STATUS_UNSUPPORTED, "unsupported identifier qualification or quoting") : TURBODB_STATUS_OK;
}
static bool binding_name_equal(vstr left, vstr right) {
  return left.len == right.len && (!left.len || !memcmp(left.data,right.data,left.len));
}
static bool binding_column_name_equal(binding_context *b,vstr left,vstr right) {
  if (!b->ascii_insensitive_names) return binding_name_equal(left,right);
  if (left.len!=right.len) return false;
  for(size_t i=0;i<left.len;++i) {
    const char a=left.data[i],c=right.data[i];
    if ((a>='A'&&a<='Z'?a+('a'-'A'):a)!=(c>='A'&&c<='Z'?c+('a'-'A'):c)) return false;
  }
  return true;
}
static turbodb_status_t binding_column_in(binding_context *b,const orm_sql_table_schema *schema,
    vstr default_qualifier,vstr name,vstr qualifier,size_t *found,bool *known_qualifier) {
  *found=schema->count; *known_qualifier=!qualifier.len;
  size_t depth=SIZE_MAX;
  for (size_t i = 0; i < schema->count; ++i) {
    const turbodb_status_t status = binding_charge(b, ORM_SQL_BUDGET_EXECUTION_STEPS, 1);
    if (status != TURBODB_STATUS_OK) return status;
    const orm_sql_schema_column *column = &schema->columns[i];
    if (!qualifier.len && column->qualified_only) continue;
    if(column->lexical_depth>depth) continue;
    const vstr relation = column->qualifier.len ? column->qualifier : default_qualifier;
    if (qualifier.len && !binding_name_equal(qualifier,relation)) continue;
    if(qualifier.len&&column->lexical_depth<depth) {
      depth=column->lexical_depth; *found=schema->count;
    }
    *known_qualifier = true;
    if (binding_column_name_equal(b,name,column->name)) {
      if(column->lexical_depth<depth) {
        depth=column->lexical_depth; *found=schema->count;
      }
      if (*found != schema->count) return binding_error(b,TURBODB_STATUS_SQL_ERROR,"ambiguous column in declared schema");
      *found = i;
    }
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t binding_column(binding_context *b, vstr name, vstr qualifier, size_t *slot) {
  size_t found=b->schema->count; bool known=false;
  turbodb_status_t status=TURBODB_STATUS_OK;
  if(!b->hidden_input) status=binding_column_in(b,b->schema,b->qualifier,name,qualifier,&found,&known);
  if(status!=TURBODB_STATUS_OK) return status;
  if(found!=b->schema->count) { *slot=found; return TURBODB_STATUS_OK; }
  /* A matching local qualifier shadows an outer relation even when its column
   * name does not match. */
  if(qualifier.len&&known) return binding_error(b,TURBODB_STATUS_SQL_ERROR,"unknown column in declared schema");
  if(b->outer_schema) {
    bool outer_known=false; size_t outer=b->outer_schema->count;
    status=binding_column_in(b,b->outer_schema,b->outer_qualifier,name,qualifier,&outer,&outer_known);
    if(status!=TURBODB_STATUS_OK) return status;
    if(outer!=b->outer_schema->count) {
      if(b->schema->count>SIZE_MAX-b->parameter_marker_count ||
          b->schema->count+b->parameter_marker_count>SIZE_MAX-outer)
        return binding_error(b,TURBODB_STATUS_LIMIT_EXCEEDED,"correlated column slot overflow");
      *slot=b->schema->count+b->parameter_marker_count+outer;
      if(b->correlated) *b->correlated=true;
      if(b->outer_schema->columns[outer].capture_used)
        *b->outer_schema->columns[outer].capture_used=true;
      return TURBODB_STATUS_OK;
    }
    if(qualifier.len&&!outer_known) return binding_error(b,TURBODB_STATUS_SQL_ERROR,"unknown table qualifier");
    if(qualifier.len&&outer_known) return binding_error(b,TURBODB_STATUS_SQL_ERROR,"unknown column in declared schema");
  }
  if(b->hidden_input) return binding_error(b,TURBODB_STATUS_SQL_ERROR,"column reference requires FROM");
  if(qualifier.len&&!known) return binding_error(b,TURBODB_STATUS_SQL_ERROR,"unknown table qualifier");
  return binding_error(b, TURBODB_STATUS_SQL_ERROR, "unknown column in declared schema");
}
static turbodb_status_t binding_parameter(binding_context *b, const sqlparser_node *node, size_t *slot) {
  size_t first = 0, last = b->parameter_marker_count;
  while (first < last) {
    const turbodb_status_t status = binding_charge(b, ORM_SQL_BUDGET_EXECUTION_STEPS, 1);
    if (status != TURBODB_STATUS_OK) return status;
    const size_t mid = first + (last - first) / 2;
    if (b->parameter_offsets[mid] < node->span.offset) first = mid + 1; else last = mid;
  }
  if (first == b->parameter_marker_count || b->parameter_offsets[first] != node->span.offset)
    return binding_error(b, TURBODB_STATUS_INTERNAL_ERROR, "parameter occurrence is not indexed");
  if (b->parameter_resolved && !b->parameter_resolved[first])
    return binding_error(b,TURBODB_STATUS_INVALID_STATE,"parameter type is unresolved");
  *slot = first; return TURBODB_STATUS_OK;
}
static turbodb_status_t binding_query(binding_context *b, sqlparser_id id, size_t *slot) {
  size_t first = 0, last = b->query_count;
  while (first < last) {
    const turbodb_status_t status = binding_charge(b, ORM_SQL_BUDGET_EXECUTION_STEPS, 1);
    if (status != TURBODB_STATUS_OK) return status;
    const size_t mid = first + (last - first) / 2;
    if (b->queries[mid].node < id) first = mid + 1; else last = mid;
  }
  *slot = first < b->query_count && b->queries[first].node == id ? first : b->query_count;
  return TURBODB_STATUS_OK;
}
/* Visit predicate children and explicit IN/CASE lists, never projection aliases. The
 * indexed marks let us compact occurrences in node-ID order for expr_compile;
 * no assumption about parser allocation order or recursion on the C stack. */
static turbodb_status_t collect_inputs(binding_context *b, sqlparser_id root,
    expression_binding *marks, sqlparser_id *stack, size_t capacity, size_t *count, size_t *query_count) {
  size_t pending = 1; stack[0] = root;
  while (pending) {
    const sqlparser_id id = stack[--pending];
    const sqlparser_node *node = sqlparser_get_node(b->document, id);
    if (!node || id > capacity || marks[id - 1].seen)
      return binding_error(b, TURBODB_STATUS_INVALID_ARGUMENT, "invalid predicate tree");
    marks[id - 1].seen = true; b->offset = node->span.offset;
    turbodb_status_t status = binding_charge(b, ORM_SQL_BUDGET_EXECUTION_STEPS, 1);
    if (status != TURBODB_STATUS_OK) return status;
    size_t query_slot = b->query_count;
    status = binding_query(b, id, &query_slot);
    if (status != TURBODB_STATUS_OK) return status;
    if (b->substitutions && b->substitutions[id-1]) {
      const size_t slot = b->substitutions[id-1]-1;
      if (slot >= b->schema->count)
        return binding_error(b,TURBODB_STATUS_INVALID_ARGUMENT,"invalid group expression substitution");
      marks[id-1].slot = slot; marks[id-1].input = true; ++*count;
    } else if (query_slot != b->query_count) {
      const orm_sql_expr_query_binding *query=&b->queries[query_slot];
      if(query->bind_capture) {
        status=query->bind_capture(query->capture_context,b->capture_slots,
            b->capture_count,b->schema->count,b->error);
        if(status!=TURBODB_STATUS_OK) return status;
      }
      marks[id-1].slot = query_slot; marks[id-1].query = true; ++*query_count;
      if (node->kind == SQLPARSER_IN) {
        if (pending == capacity)
          return binding_error(b, TURBODB_STATUS_LIMIT_EXCEEDED, "predicate traversal capacity exceeded");
        stack[pending++] = node->as.in.value;
      }
    } else if (node->kind == SQLPARSER_SUBQUERY ||
        (node->kind == SQLPARSER_UNARY && node->as.unary.op == SQLPARSER_OP_EXISTS)) {
      return binding_error(b, TURBODB_STATUS_UNSUPPORTED, "subquery requires a bound dependency");
    } else if (node->kind == SQLPARSER_PARAMETER) {
      status = binding_parameter(b, node, &marks[id - 1].slot);
      if (status != TURBODB_STATUS_OK) return status;
      marks[id - 1].slot += b->schema->count;
      marks[id - 1].input = true; ++*count;
    } else if (node->kind == SQLPARSER_NAME) {
      vstr name, qualifier;
      status = binding_name(b, id, &name, &qualifier);
      if (status == TURBODB_STATUS_OK) status = binding_column(b, name, qualifier, &marks[id - 1].slot);
      if (status != TURBODB_STATUS_OK) return status;
      marks[id - 1].input = true; ++*count;
    } else if (node->kind == SQLPARSER_CALL) {
      orm_sql_expr_function function;
      status = orm_tidesdb_sql_expr_resolve_call(b->document, node, &function, b->error);
      if (status != TURBODB_STATUS_OK) return status;
      if (node->as.call.arguments.count > capacity - pending)
        return binding_error(b, TURBODB_STATUS_LIMIT_EXCEEDED, "predicate traversal capacity exceeded");
      sqlparser_id item = node->as.call.arguments.first;
      for (size_t i = 0; i < node->as.call.arguments.count; ++i) {
        const sqlparser_node *argument = sqlparser_get_node(b->document, item);
        if (!argument) return binding_error(b, TURBODB_STATUS_INVALID_ARGUMENT, "invalid function argument list");
        stack[pending++] = item;
        item = argument->next;
      }
      if (item) return binding_error(b, TURBODB_STATUS_INVALID_ARGUMENT, "invalid function argument list");
    } else if (node->kind == SQLPARSER_CASE) {
      const size_t extra = (node->as.case_expr.operand != 0) + (node->as.case_expr.otherwise != 0);
      if (!node->as.case_expr.branches.count)
        return binding_error(b, TURBODB_STATUS_INVALID_ARGUMENT, "CASE requires WHEN branches");
      if (extra > capacity - pending || node->as.case_expr.branches.count > capacity - pending - extra)
        return binding_error(b, TURBODB_STATUS_LIMIT_EXCEEDED, "predicate traversal capacity exceeded");
      sqlparser_id item = node->as.case_expr.branches.first;
      for (size_t i = 0; i < node->as.case_expr.branches.count; ++i) {
        const sqlparser_node *entry = sqlparser_get_node(b->document, item);
        if (!entry || entry->kind != SQLPARSER_WHEN)
          return binding_error(b, TURBODB_STATUS_INVALID_ARGUMENT, "invalid CASE branch");
        stack[pending++] = item;
        item = entry->next;
      }
      if (item) return binding_error(b, TURBODB_STATUS_INVALID_ARGUMENT, "invalid CASE branch list");
      if (node->as.case_expr.otherwise) stack[pending++] = node->as.case_expr.otherwise;
      if (node->as.case_expr.operand) stack[pending++] = node->as.case_expr.operand;
    } else if (node->kind == SQLPARSER_WHEN) {
      enum { WHEN_CHILDREN = 2 };
      if (WHEN_CHILDREN > capacity - pending)
        return binding_error(b, TURBODB_STATUS_LIMIT_EXCEEDED, "predicate traversal capacity exceeded");
      stack[pending++] = node->as.when.result;
      stack[pending++] = node->as.when.condition;
    } else if (node->kind == SQLPARSER_IN) {
      if (node->as.in.query || node->as.in.table || !node->as.in.items.count)
        return binding_error(b, TURBODB_STATUS_UNSUPPORTED, "IN requires a nonempty scalar list");
      if (node->as.in.items.count >= capacity - pending)
        return binding_error(b, TURBODB_STATUS_LIMIT_EXCEEDED, "predicate traversal capacity exceeded");
      sqlparser_id item = node->as.in.items.first;
      for (size_t i = 0; i < node->as.in.items.count; ++i) {
        const sqlparser_node *entry = sqlparser_get_node(b->document, item);
        if (!entry) return binding_error(b, TURBODB_STATUS_INVALID_ARGUMENT, "invalid IN list");
        stack[pending++] = item;
        item = entry->next;
      }
      if (item) return binding_error(b, TURBODB_STATUS_INVALID_ARGUMENT, "invalid IN list");
      stack[pending++] = node->as.in.value;
    } else if (node->kind == SQLPARSER_BETWEEN) {
      enum { BETWEEN_CHILDREN = 3 };
      if (BETWEEN_CHILDREN > capacity - pending)
        return binding_error(b, TURBODB_STATUS_LIMIT_EXCEEDED, "predicate traversal capacity exceeded");
      stack[pending++] = node->as.between.upper;
      stack[pending++] = node->as.between.lower;
      stack[pending++] = node->as.between.value;
    } else if (node->kind == SQLPARSER_CAST) {
      orm_sql_cast_target target;
      status=orm_tidesdb_sql_expr_resolve_cast(b->document,node,b->budget,&target,b->error);
      if (status!=TURBODB_STATUS_OK) return status;
      if (pending==capacity)
        return binding_error(b,TURBODB_STATUS_LIMIT_EXCEEDED,"predicate traversal capacity exceeded");
      stack[pending++]=node->as.cast.expression;
    } else if (node->kind == SQLPARSER_BINARY || node->kind == SQLPARSER_UNARY) {
      const bool binary = node->kind == SQLPARSER_BINARY;
      const size_t children = binary ? 2 : 1;
      if (binary && node->as.binary.escape) {
        const sqlparser_node *escape = sqlparser_get_node(b->document, node->as.binary.escape);
        if ((node->as.binary.op != SQLPARSER_OP_LIKE && node->as.binary.op != SQLPARSER_OP_NOT_LIKE) ||
            !escape || escape->kind != SQLPARSER_STRING)
          return binding_error(b, TURBODB_STATUS_UNSUPPORTED, "LIKE ESCAPE requires a string literal");
        /* The compiler validates/owns the literal; it is not a runtime input. */
      }
      if (children > capacity - pending)
        return binding_error(b, TURBODB_STATUS_LIMIT_EXCEEDED, "predicate traversal capacity exceeded");
      if (binary) stack[pending++] = node->as.binary.right;
      stack[pending++] = binary ? node->as.binary.left : node->as.unary.operand;
    } else if (node->kind != SQLPARSER_NULL && node->kind != SQLPARSER_BOOLEAN &&
               node->kind != SQLPARSER_NUMBER && node->kind != SQLPARSER_STRING &&
               node->kind != SQLPARSER_VARIABLE) {
      return binding_error(b, TURBODB_STATUS_UNSUPPORTED, "unsupported predicate AST");
    }
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t binding_expression(binding_context *b, sqlparser_id root, size_t max_depth,
    orm_sql_expression_target target, bool predicate) {
  if (!root) return TURBODB_STATUS_OK;
  const size_t nodes = sqlparser_node_count(b->document);
  if ((b->substitutions && b->substitution_count != nodes) || (!b->substitutions && b->substitution_count))
    return binding_error(b,TURBODB_STATUS_INVALID_ARGUMENT,"invalid substitution table size");
  if (b->query_count && !b->queries)
    return binding_error(b,TURBODB_STATUS_INVALID_ARGUMENT,"missing query dependency table");
  if (b->query_count > nodes || b->query_count > b->budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES])
    return binding_error(b,TURBODB_STATUS_LIMIT_EXCEEDED,"query dependency table exceeds bounds");
  turbodb_status_t checked = binding_charge(b, ORM_SQL_BUDGET_EXECUTION_STEPS, b->query_count);
  if (checked != TURBODB_STATUS_OK) return checked;
  for (size_t i = 0; i < b->query_count; ++i)
    if (!b->queries[i].node || b->queries[i].node > nodes || (i && b->queries[i-1].node >= b->queries[i].node))
      return binding_error(b,TURBODB_STATUS_INVALID_ARGUMENT,"query dependencies must have distinct ordered node ids");
  vec_t marks = {0}, stack = {0}, inputs = {0}, queries = {0};
  size_t mark_bytes = 0, stack_bytes = 0, input_bytes = 0, query_bytes = 0, count = 0, query_count = 0;
  turbodb_status_t status = binding_vector(b, &marks, nodes, sizeof(expression_binding), _Alignof(expression_binding), &mark_bytes);
  if (status == TURBODB_STATUS_OK)
    status = binding_vector(b, &stack, nodes, sizeof(sqlparser_id), _Alignof(sqlparser_id), &stack_bytes);
  if (status == TURBODB_STATUS_OK)
    status = collect_inputs(b, root, vec_data(&marks), vec_data(&stack), nodes, &count, &query_count);
  if (status == TURBODB_STATUS_OK && query_count && (!target.query_slots || !target.query_slot_bytes))
    status = binding_error(b,TURBODB_STATUS_INVALID_ARGUMENT,"missing query mapping output");
  if (status == TURBODB_STATUS_OK)
    status = binding_vector(b, &inputs, count, sizeof(orm_sql_expr_input), _Alignof(orm_sql_expr_input), &input_bytes);
  if (status == TURBODB_STATUS_OK)
    status = binding_vector(b, target.slots, count, sizeof(size_t), _Alignof(size_t), target.slot_bytes);
  if (status == TURBODB_STATUS_OK && query_count)
    status = binding_vector(b, &queries, query_count, sizeof(orm_sql_expr_query_binding), _Alignof(orm_sql_expr_query_binding), &query_bytes);
  if (status == TURBODB_STATUS_OK && query_count)
    status = binding_vector(b, target.query_slots, query_count, sizeof(size_t), _Alignof(size_t), target.query_slot_bytes);
  if (status == TURBODB_STATUS_OK) {
    size_t slot = 0, query = 0;
    for (size_t i = 0; i < nodes; ++i) {
      const expression_binding *mark = vec_at_const(&marks, i);
      if (mark->query) {
        *(orm_sql_expr_query_binding *)vec_at(&queries, query) = b->queries[mark->slot];
        *(size_t *)vec_at(target.query_slots, query++) = mark->slot;
      }
      if (!mark->input) continue;
      const orm_sql_type type = mark->slot < b->schema->count ? b->schema->columns[mark->slot].type :
          b->parameter_types[mark->slot - b->schema->count];
      const sqlparser_kind kind = sqlparser_get_node(b->document,(sqlparser_id)(i+1))->kind;
      *(orm_sql_expr_input *)vec_at(&inputs, slot) = (orm_sql_expr_input){(sqlparser_id)(i + 1), type,
          kind != SQLPARSER_NAME && kind != SQLPARSER_PARAMETER};
      *(size_t *)vec_at(target.slots, slot++) = mark->slot;
    }
    const orm_sql_expr_bindings bindings = {vec_data_const(&inputs), count, vec_data_const(&queries), query_count};
    status = orm_tidesdb_sql_expr_compile_queries(b->document, root, &bindings, predicate,
        max_depth, b->budget, target.program, b->error);
  }
  vec_t *vectors[] = {&marks, &stack, &inputs, &queries};
  const size_t bytes[] = {mark_bytes, stack_bytes, input_bytes, query_bytes};
  for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
    const turbodb_status_t released = orm_sql_work_release(vectors[i], bytes[i], b->budget,
        status == TURBODB_STATUS_OK ? b->error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  return status;
}

static binding_context binding_scope(const orm_sql_binding_scope *scope, turbodb_error_t *error) {
  return (binding_context){.document = scope->document, .schema = scope->schema, .qualifier = scope->qualifier,
      .parameter_types = scope->parameter_types, .parameter_offsets = scope->parameter_offsets,
      .parameter_resolved=scope->parameter_resolved,
      .parameter_count = scope->parameter_count,
      .parameter_marker_count=scope->outer_schema ? scope->parameter_marker_count : scope->parameter_count,
      .outer_schema=scope->outer_schema,.outer_qualifier=scope->outer_qualifier,.correlated=scope->correlated,
      .capture_slots=scope->capture_slots,.capture_count=scope->capture_count,
      .budget = scope->budget, .error = error,
      .substitutions=scope->substitutions,.substitution_count=scope->substitution_count,
      .queries=scope->queries,.query_count=scope->query_count,.hidden_input=scope->hidden_input,
      .ascii_insensitive_names=scope->ascii_insensitive_names};
}
turbodb_status_t orm_sql_bind_expression(const orm_sql_binding_scope *scope, sqlparser_id root,
    size_t max_depth, orm_sql_expression_target target, bool predicate, turbodb_error_t *error) {
  binding_context b = binding_scope(scope, error);
  return binding_expression(&b, root, max_depth, target, predicate);
}
turbodb_status_t orm_sql_bind_column(const orm_sql_binding_scope *scope,
    sqlparser_id name, size_t *slot, turbodb_error_t *error) {
  binding_context b = binding_scope(scope, error); vstr text = {0}, qualifier = {0};
  const turbodb_status_t status = binding_name(&b, name, &text, &qualifier);
  return status == TURBODB_STATUS_OK ? binding_column(&b, text, qualifier, slot) : status;
}

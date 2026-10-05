#include "sql.h"
#include "value.h"
#include <sqlparser/sqlparser.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { SQL_IDENTIFIER_BYTES = 63u };

typedef struct sql_lowering {
  const orm_query_plan *raw;
  const orm_limits *limits;
  orm_query_plan *plan;
  orm_error_t *error;
  sqlparser_document *document;
  size_t offset, parameter;
  tstr literal;
} sql_lowering;

static orm_status_t sql_error(sql_lowering *p, orm_status_t status, const char *reason) {
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message), "TidesDB SQL at byte %zu: %s", p->offset, reason);
  orm_error_set(p->error, status, message);
  return status;
}

static const sqlparser_node *node(sql_lowering *p, sqlparser_id id) {
  const sqlparser_node *n = sqlparser_get_node(p->document, id);
  if (n) p->offset = n->span.offset;
  return n;
}

static vstr node_text(sql_lowering *p, const sqlparser_node *n) {
  return (vstr){sqlparser_text(p->document, n->span), n->span.length};
}

static bool identifier_start(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static orm_status_t identifier(sql_lowering *p, sqlparser_id id, vstr *out) {
  const sqlparser_node *n = node(p, id);
  if (!n || n->kind != SQLPARSER_NAME || n->as.name.parts != 1)
    return sql_error(p, ORM_STATUS_UNSUPPORTED, "expected an unqualified identifier");
  vstr text = node_text(p, n);
  if (text.len >= 2 && text.data[0] == '`') {
    ++text.data;
    text.len -= 2;
  }
  if (text.len > SQL_IDENTIFIER_BYTES)
    return sql_error(p, ORM_STATUS_LIMIT_EXCEEDED, "identifier is too long");
  if (!text.len || !identifier_start((unsigned char)text.data[0]))
    return sql_error(p, ORM_STATUS_UNSUPPORTED, "expected an ASCII identifier");
  for (size_t i = 1; i < text.len; ++i)
    if (!identifier_start((unsigned char)text.data[i]) &&
        !(text.data[i] >= '0' && text.data[i] <= '9'))
      return sql_error(p, ORM_STATUS_UNSUPPORTED, "expected an ASCII identifier");
  *out = text;
  return ORM_STATUS_OK;
}

static orm_status_t table(sql_lowering *p, sqlparser_id id, orm_query_kind kind) {
  vstr name;
  orm_status_t status = identifier(p, id, &name);
  if (status != ORM_STATUS_OK) return status;
  return orm_plan_init(p->plan, kind, name, p->limits, p->error);
}

static orm_value_t bound_value(const orm_owned_value *value) {
  switch (value->kind) {
    case ORM_VALUE_INT64: return orm_i64(value->data.int64_value);
    case ORM_VALUE_UINT64: return orm_u64(value->data.uint64_value);
    case ORM_VALUE_DOUBLE: return orm_f64(value->data.double_value);
    case ORM_VALUE_BOOLEAN: return orm_bool(value->data.boolean_value);
    case ORM_VALUE_TEXT: return orm_text_v(tstr_to_v(value->bytes));
    case ORM_VALUE_BLOB: return orm_blob(value->bytes, tstr_len(value->bytes));
    default: return orm_null();
  }
}

static orm_status_t integer(sql_lowering *p, const sqlparser_node *n,
    bool negative, orm_value_t *out) {
  orm_error_t error;
  orm_error_init(&error);
  const orm_status_t status = orm_tidesdb_sql_integer_literal(node_text(p, n), negative, out, &error);
  return status == ORM_STATUS_OK ? status : sql_error(p, status, error.message);
}

static orm_status_t string_value(sql_lowering *p, const sqlparser_node *n, orm_value_t *out) {
  const vstr text = node_text(p, n);
  const char quote = text.data[0];
  size_t size = 0;
  /* NO_BACKSLASH_ESCAPES is fixed before parsing. Only doubled delimiters
   * decode; spans borrow the document until the plan copies this value. */
  for (size_t i = 1; i + 1 < text.len; ++i) {
    if (text.data[i] == quote) ++i;
    ++size;
  }
  if (p->plan->parameter_bytes > p->limits->max_parameter_bytes ||
      size > p->limits->max_parameter_bytes - p->plan->parameter_bytes)
    return sql_error(p, ORM_STATUS_LIMIT_EXCEEDED, "string literal exceeds parameter budget");
  p->literal = tstr_new_len(NULL, size);
  if (!p->literal) return sql_error(p, ORM_STATUS_OUT_OF_MEMORY, "copy string literal");
  size = 0;
  for (size_t i = 1; i + 1 < text.len; ++i) {
    p->literal[size++] = text.data[i];
    if (text.data[i] == quote) ++i;
  }
  *out = orm_text_v(tstr_to_v(p->literal));
  return ORM_STATUS_OK;
}

static orm_status_t value(sql_lowering *p, sqlparser_id id, orm_value_t *out) {
  const sqlparser_node *n = node(p, id);
  tstr_freep(&p->literal);
  if (!n) return sql_error(p, ORM_STATUS_INTERNAL_ERROR, "missing AST value");
  switch (n->kind) {
    case SQLPARSER_PARAMETER: {
      const orm_owned_value *bound = vec_at_const(&p->raw->raw_parameters, p->parameter);
      if (!bound) return sql_error(p, ORM_STATUS_INVALID_ARGUMENT, "missing positional parameter");
      if (bound->kind < ORM_VALUE_NULL || bound->kind > ORM_VALUE_BLOB)
        return sql_error(p, ORM_STATUS_INVALID_ARGUMENT, "invalid parameter kind");
      *out = bound_value(bound);
      if (out->kind == ORM_VALUE_DOUBLE && !isfinite(out->data.double_value))
        return sql_error(p, ORM_STATUS_INVALID_ARGUMENT, "non-finite numeric parameter");
      ++p->parameter;
      return ORM_STATUS_OK;
    }
    case SQLPARSER_NUMBER: return integer(p, n, false, out);
    case SQLPARSER_UNARY:
      if (n->as.unary.op == SQLPARSER_OP_NEGATE) {
        const sqlparser_node *operand = node(p, n->as.unary.operand);
        if (operand && operand->kind == SQLPARSER_NUMBER) return integer(p, operand, true, out);
      }
      break;
    case SQLPARSER_STRING: return string_value(p, n, out);
    case SQLPARSER_NULL: *out = orm_null(); return ORM_STATUS_OK;
    case SQLPARSER_BOOLEAN: {
      const vstr text = node_text(p, n);
      *out = orm_bool(text.data[0] == 't' || text.data[0] == 'T');
      return ORM_STATUS_OK;
    }
    default: break;
  }
  return sql_error(p, ORM_STATUS_UNSUPPORTED, "expected ?, integer, string, boolean or NULL");
}

static orm_status_t column(sql_lowering *p, sqlparser_id id) {
  vstr name;
  orm_status_t status = identifier(p, id, &name);
  if (status != ORM_STATUS_OK) return status;
  for (size_t i = 0; i < vec_size(&p->plan->columns); ++i) {
    const tstr *existing = vec_at_const(&p->plan->columns, i);
    if (tstr_eq_v(*existing, name)) return sql_error(p, ORM_STATUS_SQL_ERROR, "duplicate column");
  }
  return orm_plan_add_column(p->plan, name, p->limits, p->error);
}

static orm_status_t assignment(sql_lowering *p, sqlparser_id name_id, sqlparser_id value_id) {
  vstr name;
  orm_value_t input;
  orm_status_t status = identifier(p, name_id, &name);
  if (status != ORM_STATUS_OK) return status;
  for (size_t i = 0; i < vec_size(&p->plan->assignments); ++i) {
    const orm_assignment *existing = vec_at_const(&p->plan->assignments, i);
    if (tstr_eq_v(existing->column, name))
      return sql_error(p, ORM_STATUS_SQL_ERROR, "duplicate assignment");
  }
  status = value(p, value_id, &input);
  if (status == ORM_STATUS_OK)
    status = orm_plan_add_assignment(p->plan, name, input, p->limits, p->error);
  return status;
}

static orm_status_t predicate(sql_lowering *p, const sqlparser_node *n) {
  sqlparser_id name_id, value_id = SQLPARSER_NONE;
  orm_compare_t comparison;
  orm_value_t input = orm_null();
  if (n->kind == SQLPARSER_UNARY &&
      (n->as.unary.op == SQLPARSER_OP_IS_NULL || n->as.unary.op == SQLPARSER_OP_IS_NOT_NULL)) {
    name_id = n->as.unary.operand;
    comparison = n->as.unary.op == SQLPARSER_OP_IS_NULL ? ORM_COMPARE_EQUAL : ORM_COMPARE_NOT_EQUAL;
  } else if (n->kind == SQLPARSER_BINARY && !n->as.binary.escape) {
    name_id = n->as.binary.left;
    value_id = n->as.binary.right;
    switch (n->as.binary.op) {
      case SQLPARSER_OP_EQ: comparison = ORM_COMPARE_EQUAL; break;
      case SQLPARSER_OP_NE: comparison = ORM_COMPARE_NOT_EQUAL; break;
      case SQLPARSER_OP_LT: comparison = ORM_COMPARE_LESS; break;
      case SQLPARSER_OP_LE: comparison = ORM_COMPARE_LESS_EQUAL; break;
      case SQLPARSER_OP_GT: comparison = ORM_COMPARE_GREATER; break;
      case SQLPARSER_OP_GE: comparison = ORM_COMPARE_GREATER_EQUAL; break;
      default: return sql_error(p, ORM_STATUS_UNSUPPORTED, "unsupported WHERE operator");
    }
  } else return sql_error(p, ORM_STATUS_UNSUPPORTED, "unsupported WHERE expression");
  vstr name;
  orm_status_t status = identifier(p, name_id, &name);
  if (status == ORM_STATUS_OK && value_id) {
    status = value(p, value_id, &input);
    if (status == ORM_STATUS_OK && input.kind == ORM_VALUE_NULL)
      status = sql_error(p, ORM_STATUS_UNSUPPORTED, "NULL comparisons require IS NULL or IS NOT NULL");
  }
  if (status == ORM_STATUS_OK)
    status = orm_plan_add_predicate(p->plan, name, comparison, input, p->limits, p->error);
  return status;
}

static orm_status_t work_status(sql_lowering *p, stl_status status) {
  if (status == STL_OK) return ORM_STATUS_OK;
  return sql_error(p, status == STL_CAPACITY_EXCEEDED ? ORM_STATUS_LIMIT_EXCEEDED :
      status == STL_OUT_OF_MEMORY ? ORM_STATUS_OUT_OF_MEMORY : ORM_STATUS_INTERNAL_ERROR,
      "WHERE traversal storage");
}

static orm_status_t predicates(sql_lowering *p, sqlparser_id where) {
  if (!where) return ORM_STATUS_OK;
  vec_t pending = {0};
  const size_t nodes = sqlparser_node_count(p->document);
  const size_t capacity = p->limits->max_predicates < nodes ? p->limits->max_predicates : nodes;
  if (!capacity) return sql_error(p, ORM_STATUS_LIMIT_EXCEEDED, "predicate limit exceeded");
  orm_status_t status = work_status(p, vec_init_bytes(&pending, sizeof(sqlparser_id),
      _Alignof(sqlparser_id), capacity));
  if (status == ORM_STATUS_OK) status = work_status(p, vec_push(&pending, &where));
  /* Left-first O(nodes) traversal preserves positional binding order. The
   * work stack cannot exceed the predicate budget; no C recursion is used. */
  while (status == ORM_STATUS_OK && vec_size(&pending)) {
    sqlparser_id id;
    status = work_status(p, vec_pop(&pending, &id));
    if (status != ORM_STATUS_OK) break;
    const sqlparser_node *n = node(p, id);
    if (n->kind == SQLPARSER_BINARY && n->as.binary.op == SQLPARSER_OP_AND) {
      status = work_status(p, vec_push(&pending, &n->as.binary.right));
      if (status == ORM_STATUS_OK) status = work_status(p, vec_push(&pending, &n->as.binary.left));
    } else status = predicate(p, n);
  }
  vec_destroy(&pending);
  return status;
}

static orm_status_t pagination_value(sql_lowering *p, sqlparser_id id, uint64_t *out) {
  orm_value_t input;
  orm_status_t status = value(p, id, &input);
  if (status != ORM_STATUS_OK) return status;
  if (input.kind == ORM_VALUE_UINT64) *out = input.data.uint64_value;
  else if (input.kind == ORM_VALUE_INT64 && input.data.int64_value >= 0)
    *out = (uint64_t)input.data.int64_value;
  else return sql_error(p, ORM_STATUS_INVALID_ARGUMENT, "pagination requires a nonnegative integer");
  return ORM_STATUS_OK;
}

static orm_status_t pagination(sql_lowering *p, sqlparser_id id) {
  if (!id) return ORM_STATUS_OK;
  const sqlparser_node *n = node(p, id);
  const sqlparser_id count = n->as.limit.count, offset = n->as.limit.offset;
  const sqlparser_node *count_node = node(p, count);
  const sqlparser_node *offset_node = node(p, offset);
  const bool offset_first = offset_node && offset_node->span.offset < count_node->span.offset;
  orm_status_t status = ORM_STATUS_OK;
  if (offset_first) status = pagination_value(p, offset, &p->plan->offset);
  if (status == ORM_STATUS_OK) status = pagination_value(p, count, &p->plan->limit);
  if (status == ORM_STATUS_OK && offset && !offset_first)
    status = pagination_value(p, offset, &p->plan->offset);
  if (status == ORM_STATUS_OK && p->plan->limit > p->limits->max_result_rows)
    status = sql_error(p, ORM_STATUS_LIMIT_EXCEEDED, "LIMIT exceeds max_result_rows");
  p->plan->has_limit = true;
  p->plan->has_offset = offset != SQLPARSER_NONE;
  return status;
}

static orm_status_t select_statement(sql_lowering *p, const sqlparser_node *s) {
  if (s->as.select.distinct || s->as.select.calc_found_rows || s->as.select.group_by.count ||
      s->as.select.having || s->as.select.order_by.count || s->as.select.windows.count)
    return sql_error(p, ORM_STATUS_UNSUPPORTED, "unsupported SELECT clause or modifier");
  const sqlparser_node *from = node(p, s->as.select.from);
  if (!from || from->kind != SQLPARSER_TABLE || !from->as.table.name ||
      from->as.table.query || from->as.table.alias || from->as.table.arguments.count ||
      from->as.table.indexed_by || from->as.table.group || from->as.table.table_function || from->as.table.not_indexed ||
      from->as.table.lateral)
    return sql_error(p, ORM_STATUS_UNSUPPORTED, "SELECT requires one table without an alias");
  orm_status_t status = table(p, from->as.table.name, ORM_QUERY_SELECT);
  for (sqlparser_id id = s->as.select.columns.first; status == ORM_STATUS_OK && id;) {
    const sqlparser_node *projection = node(p, id);
    if (projection->as.projection.alias)
      return sql_error(p, ORM_STATUS_UNSUPPORTED, "column aliases are unsupported");
    status = column(p, projection->as.projection.expression);
    id = projection->next;
  }
  if (status == ORM_STATUS_OK) status = predicates(p, s->as.select.where);
  if (status == ORM_STATUS_OK) status = pagination(p, s->as.select.limit);
  return status;
}

static orm_status_t insert_statement(sql_lowering *p, const sqlparser_node *s) {
  if (s->as.insert.replace || s->as.insert.conflict != SQLPARSER_CONFLICT_DEFAULT ||
      s->as.insert.default_values || s->as.insert.low_priority || s->as.insert.query ||
      s->as.insert.assignments.count || s->as.insert.rows.count != 1 || !s->as.insert.columns.count)
    return sql_error(p, ORM_STATUS_UNSUPPORTED, "INSERT requires explicit columns and one VALUES row");
  const sqlparser_node *row = node(p, s->as.insert.rows.first);
  if (row->as.row.values.count != s->as.insert.columns.count)
    return sql_error(p, ORM_STATUS_SQL_ERROR, "INSERT column/value count mismatch");
  /* The existing plan builder admits column declarations in SELECT state.
   * Finish that list before switching the private plan to INSERT assignments. */
  orm_status_t status = table(p, s->as.insert.table, ORM_QUERY_SELECT);
  for (sqlparser_id id = s->as.insert.columns.first; status == ORM_STATUS_OK && id;) {
    status = column(p, id);
    id = node(p, id)->next;
  }
  p->plan->kind = ORM_QUERY_INSERT;
  sqlparser_id value_id = row->as.row.values.first;
  for (sqlparser_id id = s->as.insert.columns.first; status == ORM_STATUS_OK && id;) {
    status = assignment(p, id, value_id);
    id = node(p, id)->next;
    value_id = node(p, value_id)->next;
  }
  return status;
}

static orm_status_t update_statement(sql_lowering *p, const sqlparser_node *s) {
  if (s->as.update.alias || s->as.update.limit || s->as.update.order_by.count || s->as.update.low_priority ||
      s->as.update.conflict != SQLPARSER_CONFLICT_DEFAULT || s->as.update.indexed_by || s->as.update.not_indexed)
    return sql_error(p, ORM_STATUS_UNSUPPORTED, "unsupported UPDATE clause or modifier");
  orm_status_t status = table(p, s->as.update.table, ORM_QUERY_UPDATE);
  for (sqlparser_id id = s->as.update.assignments.first; status == ORM_STATUS_OK && id;) {
    const sqlparser_node *a = node(p, id);
    status = assignment(p, a->as.assignment.name, a->as.assignment.value);
    id = a->next;
  }
  if (status == ORM_STATUS_OK) status = predicates(p, s->as.update.where);
  return status;
}

static orm_status_t delete_statement(sql_lowering *p, const sqlparser_node *s) {
  if (s->as.delete_stmt.alias || s->as.delete_stmt.limit || s->as.delete_stmt.order_by.count || s->as.delete_stmt.low_priority ||
      s->as.delete_stmt.targets.count || s->as.delete_stmt.from ||
      s->as.delete_stmt.indexed_by || s->as.delete_stmt.not_indexed)
    return sql_error(p, ORM_STATUS_UNSUPPORTED, "unsupported DELETE clause or modifier");
  orm_status_t status = table(p, s->as.delete_stmt.table, ORM_QUERY_DELETE);
  if (status == ORM_STATUS_OK) status = predicates(p, s->as.delete_stmt.where);
  return status;
}

static orm_status_t statement(sql_lowering *p, const sqlparser_node *s) {
  switch (s->kind) {
    case SQLPARSER_SELECT: return select_statement(p, s);
    case SQLPARSER_INSERT: return insert_statement(p, s);
    case SQLPARSER_UPDATE: return update_statement(p, s);
    case SQLPARSER_DELETE: return delete_statement(p, s);
    default: return sql_error(p, ORM_STATUS_UNSUPPORTED, "only SELECT, INSERT, UPDATE and DELETE are executable");
  }
}

static orm_status_t parse_status(sql_lowering *p, const sqlparser_error *error) {
  orm_status_t status;
  switch (error->status) {
    case SQLPARSER_OK: return ORM_STATUS_OK;
    case SQLPARSER_INVALID_ARGUMENT: status = ORM_STATUS_INVALID_ARGUMENT; break;
    case SQLPARSER_SYNTAX_ERROR: status = ORM_STATUS_SQL_ERROR; break;
    case SQLPARSER_LIMIT_EXCEEDED: status = ORM_STATUS_LIMIT_EXCEEDED; break;
    case SQLPARSER_OUT_OF_MEMORY: status = ORM_STATUS_OUT_OF_MEMORY; break;
    default: status = ORM_STATUS_INTERNAL_ERROR; break;
  }
  p->offset = error->offset;
  return sql_error(p, status, error->message);
}

orm_status_t orm_tidesdb_sql_parse(const orm_query_plan *raw,
    const orm_limits *limits, orm_query_plan *out, orm_error_t *error) {
  if (!out || !raw || raw == out || !limits || raw->kind != ORM_QUERY_RAW || !raw->raw_sql) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT, "invalid TidesDB SQL input");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  memset(out, 0, sizeof(*out));
  sql_lowering lowering = {.raw = raw, .limits = limits, .plan = out, .error = error};
  const size_t size = tstr_len(raw->raw_sql);
  sqlparser_limits budget = sqlparser_default_limits();
  if (size > limits->max_query_bytes || vec_size(&raw->raw_parameters) > limits->max_parameters)
    return sql_error(&lowering, ORM_STATUS_LIMIT_EXCEEDED, "query/parameter budget exceeded");
  if (!size) return sql_error(&lowering, ORM_STATUS_SQL_ERROR, "empty SQL input");
  if (limits->max_query_bytes < budget.max_input_bytes) budget.max_input_bytes = limits->max_query_bytes;
  budget.max_statements = 1;
  const sqlparser_options options = {SQLPARSER_MYSQL, true};
  sqlparser_error diagnostic;
  sqlparser_status parsed = sqlparser_parse_with_options(raw->raw_sql, size, &options,
      &budget, &lowering.document, &diagnostic);
  if (parsed != SQLPARSER_OK) return parse_status(&lowering, &diagnostic);
  sqlparser_list statements = sqlparser_statements(lowering.document);
  orm_status_t status = statements.count == 1
      ? statement(&lowering, node(&lowering, statements.first))
      : sql_error(&lowering, ORM_STATUS_SQL_ERROR, "expected one SQL statement");
  if (status == ORM_STATUS_OK && lowering.parameter != vec_size(&raw->raw_parameters))
    status = sql_error(&lowering, ORM_STATUS_INVALID_ARGUMENT, "unused positional parameter");
  if (status != ORM_STATUS_OK) {
    /* Plan helpers report their own reason. Add the SQL boundary context once. */
    if (error && strncmp(error->message, "TidesDB SQL", sizeof("TidesDB SQL") - 1) != 0) {
      const orm_error_t cause = *error;
      sql_error(&lowering, status, cause.message);
    }
    orm_plan_destroy(out);
  }
  tstr_freep(&lowering.literal);
  sqlparser_document_destroy(lowering.document);
  return status;
}

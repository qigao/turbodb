#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sqlp_error(sqlp_context *ctx, sqlparser_status status, const char *message) {
  if (ctx->error.status != SQLPARSER_OK) return;
  ctx->error.status = status;
  ctx->error.offset = ctx->token.offset;
  (void)snprintf(ctx->error.message, sizeof(ctx->error.message), "%s", message);
}

sqlparser_id sqlp_add(sqlp_context *ctx, sqlparser_node node) {
  if (ctx->error.status != SQLPARSER_OK) return SQLPARSER_NONE;
  stl_status status = vec_push(&ctx->document->nodes, &node);
  if (status != STL_OK) {
    sqlp_error(ctx, status == STL_CAPACITY_EXCEEDED ? SQLPARSER_LIMIT_EXCEEDED
                                                 : SQLPARSER_OUT_OF_MEMORY,
               "cannot allocate SQL AST node within the node budget");
    return SQLPARSER_NONE;
  }
  return (sqlparser_id)vec_size(&ctx->document->nodes);
}

sqlparser_id sqlp_atom(sqlp_context *ctx, sqlparser_kind kind, sqlparser_span span) {
  sqlparser_node node = {.kind = kind, .span = span};
  if (kind == SQLPARSER_NAME) node.as.name.parts = 1;
  return sqlp_add(ctx, node);
}

sqlparser_id sqlp_qualified_name(sqlp_context *ctx, sqlparser_id first, sqlparser_span last) {
  const sqlparser_node *n = sqlparser_get_node(ctx->document, first);
  if (!n || ctx->error.status != SQLPARSER_OK) return SQLPARSER_NONE;
  sqlparser_node name = {.kind=SQLPARSER_NAME, .span=sqlp_cover(n->span,last), .as.name={n->as.name.parts+1}};
  return sqlp_add(ctx, name);
}

sqlp_constraints sqlp_declare_constraint(sqlp_context *ctx, sqlp_constraints list,
    sqlparser_span start, sqlparser_span name) {
  if (!list.declarations.count) list.start = start;
  list.active_name = sqlp_atom(ctx, SQLPARSER_NAME, name);
  list.declarations = sqlp_append(ctx, list.declarations, list.active_name);
  return list;
}

sqlp_constraints sqlp_append_constraint(sqlp_context *ctx, sqlp_constraints list, sqlparser_id id) {
  if (ctx->error.status != SQLPARSER_OK || !id) return list;
  sqlparser_node *n = vec_at(&ctx->document->nodes, id-1u);
  n->as.constraint.declarations = list.declarations;
  /* COLLATE already uses name for its collation, independently of declarations. */
  if (n->as.constraint.kind != SQLPARSER_COLUMN_COLLATION)
    n->as.constraint.name = list.active_name;
  if (list.declarations.count) n->span = sqlp_cover(list.start, n->span);
  list.nodes = sqlp_append(ctx, list.nodes, id);
  list.declarations = (sqlparser_list){0};
  return list;
}

sqlp_constraints sqlp_end_constraints(sqlp_context *ctx, sqlp_constraints list) {
  if (list.declarations.count) {
    sqlparser_node trailing = {.kind=SQLPARSER_CONSTRAINT,
      .span=sqlp_cover(list.start, sqlp_span(ctx, list.declarations.last)),
      .as.constraint={.kind=SQLPARSER_CONSTRAINT_DECLARATION,
        .name=list.active_name,.declarations=list.declarations}};
    list.nodes = sqlp_append(ctx, list.nodes, sqlp_add(ctx, trailing));
  }
  list.declarations = (sqlparser_list){0};
  list.active_name = SQLPARSER_NONE;
  return list;
}

sqlp_constraints sqlp_table_constraints(sqlp_context *ctx, sqlparser_list columns) {
  sqlp_constraints result = {.nodes=columns};
  if (ctx->error.status != SQLPARSER_OK) return result;
  const sqlparser_node *column = sqlparser_get_node(ctx->document, columns.last);
  /* SQLite's first comma introduces conslist, unlike tconscomma which clears
   * the name. Derive that inherited name from the last column in O(constraints),
   * without a parser-global pending name that could leak across statements. */
  for (sqlparser_id id=column->as.column.constraints.first; id;) {
    const sqlparser_node *constraint = sqlparser_get_node(ctx->document, id);
    if (constraint->as.constraint.declarations.count)
      result.active_name = constraint->as.constraint.declarations.last;
    id = constraint->next;
  }
  return result;
}

sqlparser_list sqlp_append(sqlp_context *ctx, sqlparser_list list, sqlparser_id id) {
  if (ctx->error.status != SQLPARSER_OK || id == SQLPARSER_NONE) return list;
  if (list.last != SQLPARSER_NONE) {
    sqlparser_node *last = vec_at(&ctx->document->nodes, list.last - 1u);
    last->next = id;
  } else {
    list.first = id;
  }
  list.last = id;
  ++list.count;
  return list;
}

sqlparser_span sqlp_span(sqlp_context *ctx, sqlparser_id id) {
  const sqlparser_node *node = sqlparser_get_node(ctx->document, id);
  return node ? node->span : ctx->token;
}

sqlp_key_columns sqlp_append_key(sqlp_context *ctx, sqlp_key_columns list, sqlparser_id term) {
  if (ctx->error.status != SQLPARSER_OK) return list;
  const sqlparser_node *order = sqlparser_get_node(ctx->document, term);
  const sqlparser_node *expression = sqlparser_get_node(ctx->document, order->as.order.expression);
  sqlparser_id name = expression->kind == SQLPARSER_COLLATE
      ? expression->as.collate.expression : order->as.order.expression;
  list.names = sqlp_append(ctx, list.names, name);
  list.terms = sqlp_append(ctx, list.terms, term);
  return list;
}

sqlparser_span sqlp_cover(sqlparser_span first, sqlparser_span last) {
  return (sqlparser_span){first.offset, last.offset + last.length - first.offset};
}

sqlparser_id sqlp_finish(sqlp_context *ctx, sqlparser_id id, sqlparser_span start) {
  if (id != SQLPARSER_NONE && ctx->error.status == SQLPARSER_OK) {
    sqlparser_node *node = vec_at(&ctx->document->nodes, id - 1u);
    node->span = (sqlparser_span){start.offset, ctx->token.offset - start.offset};
  }
  return id;
}

static void account_statement(sqlp_context *ctx) {
  if (ctx->error.status != SQLPARSER_OK) return;
  if (ctx->statement_count >= ctx->limits.max_statements) {
    sqlp_error(ctx, SQLPARSER_LIMIT_EXCEEDED, "SQL statement limit exceeded");
    return;
  }
  ++ctx->statement_count;
}

void sqlp_publish(sqlp_context *ctx, sqlparser_id id) {
  account_statement(ctx);
  ctx->document->statements = sqlp_append(ctx, ctx->document->statements, id);
}

void sqlp_trigger_step(sqlp_context *ctx, sqlparser_id id) {
  if (ctx->error.status != SQLPARSER_OK) return;
  const sqlparser_node *step = sqlparser_get_node(ctx->document, id);
  if (!step) return;
  sqlparser_id table = SQLPARSER_NONE;
  if (step->kind == SQLPARSER_INSERT) {
    table = step->as.insert.table;
    if (step->as.insert.default_values)
      sqlp_error(ctx, SQLPARSER_SYNTAX_ERROR, "DEFAULT VALUES is not allowed in SQLite triggers");
  } else if (step->kind == SQLPARSER_UPDATE) {
    table = step->as.update.table;
    if (step->as.update.indexed_by || step->as.update.not_indexed)
      sqlp_error(ctx, SQLPARSER_SYNTAX_ERROR, "index hints are not allowed on SQLite trigger writes");
    if (step->as.update.order_by.count || step->as.update.limit)
      sqlp_error(ctx, SQLPARSER_SYNTAX_ERROR, "ORDER BY and LIMIT are not allowed on SQLite trigger writes");
  } else if (step->kind == SQLPARSER_DELETE) {
    table = step->as.delete_stmt.table;
    if (step->as.delete_stmt.indexed_by || step->as.delete_stmt.not_indexed)
      sqlp_error(ctx, SQLPARSER_SYNTAX_ERROR, "index hints are not allowed on SQLite trigger writes");
    if (step->as.delete_stmt.order_by.count || step->as.delete_stmt.limit)
      sqlp_error(ctx, SQLPARSER_SYNTAX_ERROR, "ORDER BY and LIMIT are not allowed on SQLite trigger writes");
  }
  const sqlparser_node *target = sqlparser_get_node(ctx->document, table);
  if (target && target->as.name.parts != 1)
    sqlp_error(ctx, SQLPARSER_SYNTAX_ERROR, "SQLite trigger write targets must be unqualified");
  account_statement(ctx);
}

void *sqlp_stack_realloc(void *pointer, size_t bytes, sqlp_context *ctx) {
  void *result = realloc(pointer, bytes);
  if (result == NULL) sqlp_error(ctx, SQLPARSER_OUT_OF_MEMORY, "cannot grow parser stack");
  return result;
}
void sqlp_stack_free(void *pointer, sqlp_context *ctx) {
  (void)ctx;
  free(pointer);
}

sqlparser_id sqlp_query_tail(sqlp_context *ctx, sqlparser_id id,
                           sqlparser_list order, sqlparser_id limit) {
  if (id == SQLPARSER_NONE || ctx->error.status != SQLPARSER_OK) return id;
  sqlparser_node *node = vec_at(&ctx->document->nodes, id - 1u);
  if (node->kind == SQLPARSER_SELECT) {
    node->as.select.order_by = order;
    node->as.select.limit = limit;
  } else if (node->kind == SQLPARSER_QUERY_GROUP) {
    node->as.query_group.order_by = order;
    node->as.query_group.limit = limit;
  } else if (node->kind == SQLPARSER_UNION) {
    if (ctx->dialect == SQLPARSER_SQLITE) {
      const sqlparser_node *right = sqlparser_get_node(ctx->document, node->as.compound.right);
      if (right && right->kind == SQLPARSER_VALUES && (order.count || limit))
        sqlp_error(ctx, SQLPARSER_SYNTAX_ERROR, "SQLite VALUES cannot have ORDER BY or LIMIT");
    }
    node->as.compound.order_by = order;
    node->as.compound.limit = limit;
  } else if (node->kind == SQLPARSER_VALUES && (order.count || limit)) {
    sqlp_error(ctx, SQLPARSER_SYNTAX_ERROR, "SQLite VALUES cannot have ORDER BY or LIMIT");
  }
  node->span.length = ctx->token.offset - node->span.offset;
  return id;
}

static bool span_keyword(const sqlparser_document *document, sqlparser_span span, const char *keyword) {
  if (strlen(keyword) != span.length) return false;
  const char *text = sqlparser_text(document, span);
  for (size_t i = 0; i < span.length; ++i) {
    unsigned char c = (unsigned char)text[i];
    if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
    if (c != (unsigned char)keyword[i]) return false;
  }
  return true;
}

void sqlp_check_window_call(sqlp_context *ctx, sqlparser_id call) {
  /* MySQL's OVER grammar is restricted to built-in window/aggregate calls.
   * A generic function or a qualified/quoted routine name cannot acquire OVER. */
  static const char *const names[] = {
    "avg", "bit_and", "bit_or", "bit_xor", "count", "json_arrayagg", "json_objectagg",
    "max", "min", "std", "stddev", "stddev_pop", "stddev_samp", "sum", "var_pop",
    "var_samp", "variance", "cume_dist", "dense_rank", "first_value", "lag",
    "last_value", "lead", "nth_value", "ntile", "percent_rank", "rank", "row_number"
  };
  if (ctx->error.status != SQLPARSER_OK) return;
  const sqlparser_node *node = sqlparser_get_node(ctx->document, call);
  sqlparser_span name = sqlp_span(ctx, node->as.call.name);
  for (size_t i = 0; i < sizeof(names)/sizeof(names[0]); ++i)
    if (span_keyword(ctx->document, name, names[i])) return;
  sqlp_error(ctx, SQLPARSER_SYNTAX_ERROR, "OVER requires a supported MySQL window or aggregate function");
  ctx->error.offset = name.offset;
}

sqlparser_scope sqlp_variable_scope(sqlp_context *ctx, sqlparser_span span) {
  static const struct { const char *text; sqlparser_scope scope; } scopes[] = {
    {"@@global", SQLPARSER_SCOPE_GLOBAL}, {"@@session", SQLPARSER_SCOPE_SESSION},
    {"@@local", SQLPARSER_SCOPE_LOCAL}
  };
  for (size_t i = 0; i < sizeof(scopes) / sizeof(scopes[0]); ++i) {
    if (span_keyword(ctx->document, span, scopes[i].text)) return scopes[i].scope;
  }
  sqlp_error(ctx, SQLPARSER_SYNTAX_ERROR, "expected @@global, @@session or @@local scope");
  ctx->error.offset = span.offset;
  return SQLPARSER_SCOPE_DEFAULT;
}

sqlparser_list sqlparser_statements(const sqlparser_document *document) {
  return document ? document->statements : (sqlparser_list){0};
}
sqlparser_dialect sqlparser_get_dialect(const sqlparser_document *document) {
  return document ? document->dialect : SQLPARSER_DIALECT_UNKNOWN;
}
size_t sqlparser_node_count(const sqlparser_document *document) {
  return document ? vec_size(&document->nodes) : 0;
}
const sqlparser_node *sqlparser_get_node(const sqlparser_document *document, sqlparser_id id) {
  return document && id ? vec_at_const(&document->nodes, id - 1u) : NULL;
}
const char *sqlparser_text(const sqlparser_document *document, sqlparser_span span) {
  if (document == NULL || span.offset > document->length ||
      span.length > document->length - span.offset) return NULL;
  return document->sql + span.offset;
}
void sqlparser_document_destroy(sqlparser_document *document) {
  if (document == NULL) return;
  vec_destroy(&document->nodes);
  tstr_free(document->sql);
  free(document);
}

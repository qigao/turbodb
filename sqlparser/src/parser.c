#include "internal.h"
#include "sqlparser_grammar_gen.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
  SQLP_DEFAULT_BYTES = 1024 * 1024,
  SQLP_DEFAULT_NODES = 65536,
  SQLP_DEFAULT_STATEMENTS = 1024,
  SQLP_DEFAULT_STACK = 4096,
  SQLP_MAX_STACK = 1024 * 1024
};

sqlparser_limits sqlparser_default_limits(void) {
  return (sqlparser_limits){SQLP_DEFAULT_BYTES, SQLP_DEFAULT_NODES,
                           SQLP_DEFAULT_STATEMENTS, SQLP_DEFAULT_STACK};
}

bool sqlparser_sqlite_update_delete_limit_enabled(void) {
  return SQLPARSER_SQLITE_ENABLE_UPDATE_DELETE_LIMIT != 0;
}

static bool valid_limits(sqlparser_limits limits) {
  return limits.max_input_bytes > 0 && limits.max_input_bytes < SIZE_MAX &&
      limits.max_nodes > 0 && limits.max_nodes <= UINT32_MAX &&
      limits.max_nodes <= SIZE_MAX / sizeof(sqlparser_node) &&
      limits.max_statements > 0 && limits.max_stack_entries >= 2 &&
      limits.max_stack_entries <= SQLP_MAX_STACK;
}

sqlparser_status sqlparser_parse(const char *sql, size_t length,
    const sqlparser_limits *limits, sqlparser_document **out, sqlparser_error *error) {
  return sqlparser_parse_dialect(sql, length, SQLPARSER_MYSQL, limits, out, error);
}

static void feed(void *engine, sqlp_context *ctx, int token) {
  if (ctx->dialect == SQLPARSER_SQLITE) SqliteParser(engine, token, ctx->token);
  else SqlParser(engine, token, ctx->token);
}

sqlparser_status sqlparser_parse_dialect(const char *sql, size_t length,
    sqlparser_dialect dialect, const sqlparser_limits *limits,
    sqlparser_document **out, sqlparser_error *error) {
  const sqlparser_options options = {dialect, false};
  return sqlparser_parse_with_options(sql, length, &options, limits, out, error);
}

sqlparser_status sqlparser_parse_with_options(const char *sql, size_t length,
    const sqlparser_options *options, const sqlparser_limits *limits,
    sqlparser_document **out, sqlparser_error *error) {
  const sqlparser_options config = options ? *options : (sqlparser_options){SQLPARSER_MYSQL, false};
  const sqlparser_dialect dialect = config.dialect;
  sqlp_context ctx = {0};
  ctx.dialect = dialect;
  ctx.limits = limits ? *limits : sqlparser_default_limits();
  void *engine = NULL;
  if (out == NULL || *out != NULL || (sql == NULL && length != 0) ||
      !valid_limits(ctx.limits) || (dialect != SQLPARSER_MYSQL && dialect != SQLPARSER_SQLITE) ||
      (dialect != SQLPARSER_MYSQL && config.mysql_no_backslash_escapes)) {
    sqlp_error(&ctx, SQLPARSER_INVALID_ARGUMENT, "invalid SQL parser arguments or limits");
    goto done;
  }
  if (length > ctx.limits.max_input_bytes || length > PTRDIFF_MAX) {
    sqlp_error(&ctx, SQLPARSER_LIMIT_EXCEEDED, "SQL input byte limit exceeded");
    goto done;
  }
  const char *nul = length ? memchr(sql, 0, length) : NULL;
  if (nul != NULL) {
    ctx.token.offset = (size_t)(nul - sql);
    sqlp_error(&ctx, SQLPARSER_SYNTAX_ERROR, "embedded NUL in SQL input");
    goto done;
  }
  ctx.document = calloc(1, sizeof(*ctx.document));
  if (ctx.document == NULL) {
    sqlp_error(&ctx, SQLPARSER_OUT_OF_MEMORY, "cannot allocate SQL document");
    goto done;
  }
  ctx.document->length = length;
  ctx.document->options = config;
  ctx.document->sql = tstr_new_len(sql, length);
  if (ctx.document->sql == NULL ||
      vec_init_bytes(&ctx.document->nodes, sizeof(sqlparser_node),
                     _Alignof(sqlparser_node), ctx.limits.max_nodes) != STL_OK) {
    sqlp_error(&ctx, SQLPARSER_OUT_OF_MEMORY, "cannot allocate SQL document storage");
    goto done;
  }
  engine = dialect == SQLPARSER_SQLITE ? SqliteParserAlloc(malloc, &ctx) : SqlParserAlloc(malloc, &ctx);
  if (engine == NULL) {
    sqlp_error(&ctx, SQLPARSER_OUT_OF_MEMORY, "cannot allocate SQL parser");
    goto done;
  }
  sqlp_lexer lexer = {ctx.document->sql, ctx.document->sql, ctx.document->sql + length, dialect};
  lexer.mysql_no_backslash_escapes = config.mysql_no_backslash_escapes;
  int last = SQLTK_SEMI;
  while (ctx.error.status == SQLPARSER_OK) {
    int token = sqlp_lex(&lexer, &ctx.token);
    if (token < 0) {
      sqlp_error(&ctx, SQLPARSER_SYNTAX_ERROR, "invalid or unterminated SQL token");
      break;
    }
    if (token == 0) {
      if (last != SQLTK_SEMI) feed(engine, &ctx, SQLTK_SEMI);
      if (ctx.error.status == SQLPARSER_OK) feed(engine, &ctx, 0);
      break;
    }
    feed(engine, &ctx, token);
    last = token;
  }
  if (ctx.error.status == SQLPARSER_OK && !ctx.accepted)
    sqlp_error(&ctx, SQLPARSER_SYNTAX_ERROR, "incomplete SQL statement");
done:
  if (engine) {
    if (dialect == SQLPARSER_SQLITE) SqliteParserFree(engine, free);
    else SqlParserFree(engine, free);
  }
  if (ctx.error.status == SQLPARSER_OK) *out = ctx.document;
  else sqlparser_document_destroy(ctx.document);
  if (error) *error = ctx.error;
  return ctx.error.status;
}

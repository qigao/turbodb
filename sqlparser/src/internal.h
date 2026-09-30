#ifndef SQLPARSER_INTERNAL_H
#define SQLPARSER_INTERNAL_H

#include <sqlparser/sqlparser.h>
#include <cstl/vec.h>
#include <tstr.h>

struct sqlparser_document {
  sqlparser_dialect dialect;
  tstr sql;
  size_t length;
  vec_t nodes;
  sqlparser_list statements;
};
typedef struct sqlp_context {
  sqlparser_document *document;
  sqlparser_limits limits;
  sqlparser_error error;
  sqlparser_span token;
  sqlparser_dialect dialect;
  bool accepted;
  size_t statement_count;
} sqlp_context;
typedef struct sqlp_lexer {
  const char *base, *cursor, *end;
  sqlparser_dialect dialect;
  /* Local token context, retained across whitespace/comments only. */
  bool mysql_after_name, mysql_qualified;
} sqlp_lexer;
typedef struct sqlp_show_filter { sqlparser_id pattern, where; } sqlp_show_filter;
typedef struct sqlp_show_options { bool full, extended; } sqlp_show_options;
typedef struct sqlp_type_flags { bool is_unsigned, zerofill; } sqlp_type_flags;
typedef struct sqlp_select_options { bool distinct, all, calc_found_rows; } sqlp_select_options;
typedef struct sqlp_transaction_options {
  sqlparser_transaction_access access;
  sqlparser_isolation isolation;
  bool consistent_snapshot;
} sqlp_transaction_options;
typedef struct sqlp_transaction_head { sqlparser_span span; sqlparser_transaction_kind kind; } sqlp_transaction_head;
typedef struct sqlp_join_condition { sqlparser_id on; sqlparser_list using_columns; } sqlp_join_condition;
typedef struct sqlp_insert_head { sqlparser_span span; bool replace; sqlparser_conflict conflict; bool low_priority; } sqlp_insert_head;
typedef struct sqlp_write_head { sqlparser_span span; bool low_priority; } sqlp_write_head;
typedef struct sqlp_index_hint { sqlparser_id name; bool not_indexed; } sqlp_index_hint;
typedef struct sqlp_trigger_event { sqlparser_trigger_event kind; sqlparser_list columns; } sqlp_trigger_event;
typedef struct sqlp_key_columns { sqlparser_list names, terms; } sqlp_key_columns;
/* Local to one column or table-constraint list; all storage belongs to the AST. */
typedef struct sqlp_constraints {
  sqlparser_list nodes, declarations;
  sqlparser_id active_name;
  sqlparser_span start;
} sqlp_constraints;

void sqlp_error(sqlp_context *ctx, sqlparser_status status, const char *message);
sqlparser_id sqlp_add(sqlp_context *ctx, sqlparser_node node);
sqlparser_id sqlp_atom(sqlp_context *ctx, sqlparser_kind kind, sqlparser_span span);
sqlparser_list sqlp_append(sqlp_context *ctx, sqlparser_list list, sqlparser_id id);
sqlp_key_columns sqlp_append_key(sqlp_context *ctx, sqlp_key_columns list, sqlparser_id term);
sqlparser_span sqlp_span(sqlp_context *ctx, sqlparser_id id);
sqlparser_span sqlp_cover(sqlparser_span first, sqlparser_span last);
sqlparser_id sqlp_finish(sqlp_context *ctx, sqlparser_id id, sqlparser_span start);
void sqlp_publish(sqlp_context *ctx, sqlparser_id id);
void sqlp_trigger_step(sqlp_context *ctx, sqlparser_id id);
sqlparser_id sqlp_qualified_name(sqlp_context *ctx, sqlparser_id first, sqlparser_span last);
sqlp_constraints sqlp_declare_constraint(sqlp_context *ctx, sqlp_constraints list,
    sqlparser_span start, sqlparser_span name);
sqlp_constraints sqlp_append_constraint(sqlp_context *ctx, sqlp_constraints list, sqlparser_id id);
sqlp_constraints sqlp_end_constraints(sqlp_context *ctx, sqlp_constraints list);
sqlp_constraints sqlp_table_constraints(sqlp_context *ctx, sqlparser_list columns);
void *sqlp_stack_realloc(void *pointer, size_t bytes, sqlp_context *ctx);
void sqlp_stack_free(void *pointer, sqlp_context *ctx);
sqlparser_id sqlp_query_tail(sqlp_context *ctx, sqlparser_id id, sqlparser_list order, sqlparser_id limit);
int sqlp_lex(sqlp_lexer *lexer, sqlparser_span *token);
sqlparser_scope sqlp_variable_scope(sqlp_context *ctx, sqlparser_span span);
void sqlp_check_window_call(sqlp_context *ctx, sqlparser_id call);

void *SqlParserAlloc(void *(*allocate)(size_t), sqlp_context *ctx);
void SqlParser(void *parser, int token, sqlparser_span span);
void SqlParserFree(void *parser, void (*deallocate)(void *));
void *SqliteParserAlloc(void *(*allocate)(size_t), sqlp_context *ctx);
void SqliteParser(void *parser, int token, sqlparser_span span);
void SqliteParserFree(void *parser, void (*deallocate)(void *));
#endif

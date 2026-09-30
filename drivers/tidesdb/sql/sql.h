#ifndef ORM_TIDESDB_SQL_H
#define ORM_TIDESDB_SQL_H

#include "orm_internal.h"

enum {
  TDB_SQL_END = 0, TDB_SQL_INVALID = 256, TDB_SQL_WORD,
  TDB_SQL_INTEGER, TDB_SQL_STRING, TDB_SQL_LE, TDB_SQL_GE, TDB_SQL_NE
};

typedef struct orm_tidesdb_sql_token {
  int kind;
  vstr text;
} orm_tidesdb_sql_token;

/* Input is a bounded, NUL-terminated tstr. Tokens borrow it until parsing ends. */
orm_tidesdb_sql_token orm_tidesdb_sql_next(const char **cursor, const char *end);

/* Single-threaded conversion with no database side effects. The destination
 * must be empty; success owns every identifier/value until orm_plan_destroy.
 * Failure destroys the partial plan and leaves a zeroed destination. */
orm_status_t orm_tidesdb_sql_parse(const orm_query_plan *raw,
    const orm_limits *limits, orm_query_plan *out, orm_error_t *error);

#endif

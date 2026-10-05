#ifndef ORM_TIDESDB_SQL_H
#define ORM_TIDESDB_SQL_H

#include "orm_internal.h"

/* MySQL syntax with NO_BACKSLASH_ESCAPES, lowered to the executable CRUD subset.
 * The temporary AST never escapes this single-threaded, side-effect-free call.
 * The destination
 * must be empty; success owns every identifier/value until orm_plan_destroy.
 * Failure destroys the partial plan and leaves a zeroed destination. */
orm_status_t orm_tidesdb_sql_parse(const orm_query_plan *raw,
    const orm_limits *limits, orm_query_plan *out, orm_error_t *error);

#endif

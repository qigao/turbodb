#ifndef TURBODB_ORM_MYSQL_DIALECT_H
#define TURBODB_ORM_MYSQL_DIALECT_H

#include "orm_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct orm_mysql_rendered_query {
  tstr text;
  const orm_owned_value **parameters;
  size_t parameter_count;
} orm_mysql_rendered_query;

orm_status_t orm_mysql_render_plan(
    const orm_query_plan *plan, const orm_limits *limits,
    orm_mysql_rendered_query *out, orm_error_t *error);

void orm_mysql_rendered_query_destroy(
    orm_mysql_rendered_query *query);

#ifdef __cplusplus
}
#endif

#endif

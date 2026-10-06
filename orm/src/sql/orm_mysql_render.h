#ifndef ORM_MYSQL_RENDER_H
#define ORM_MYSQL_RENDER_H

#include "orm_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Private synchronous renderer shared by SQL drivers. Text and parameter
 * array are owned; values borrow the frozen plan until destroy. Query bytes
 * and parameter count obey limits. Failure leaves an empty output. */
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

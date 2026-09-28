#ifndef TURBODB_ORM_MYSQL_DIALECT_H
#define TURBODB_ORM_MYSQL_DIALECT_H

#include "orm_internal.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mysql_dialect_query_t {
  tstr sql;
  vec_t parameters; /* const orm_owned_value * */
} mysql_dialect_query_t;

orm_status_t mysql_dialect_render(
    const orm_query_plan *plan,
    const orm_limits *limits,
    mysql_dialect_query_t *out,
    orm_error_t *error);

void mysql_dialect_query_destroy(mysql_dialect_query_t *query);

#ifdef __cplusplus
}
#endif

#endif

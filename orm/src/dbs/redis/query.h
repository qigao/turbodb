#ifndef ORM_REDIS_QUERY_H
#define ORM_REDIS_QUERY_H

#include "orm_internal.h"

typedef struct orm_redis_query {
  vec_t arguments;
  vec_t output_columns;
} orm_redis_query;

orm_status_t orm_redis_query_build(const orm_query_plan *plan,
                                   const orm_limits *limits,
                                   vstr index_prefix,
                                   orm_redis_query *out,
                                   orm_error_t *error);
void orm_redis_query_destroy(orm_redis_query *query);
orm_status_t orm_redis_value_text(const orm_owned_value *value, tstr *out,
                                  orm_error_t *error);

#endif

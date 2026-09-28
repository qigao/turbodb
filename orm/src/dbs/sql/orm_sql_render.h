#ifndef ORM_SQL_RENDER_H
#define ORM_SQL_RENDER_H

#include "orm_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum orm_sql_dialect {
  ORM_SQL_SQLITE = 0,
  ORM_SQL_POSTGRES = 1
} orm_sql_dialect;

/* text owns the SQL bytes. parameters contains borrowed orm_owned_value*. */
typedef struct orm_sql_query {
  tstr text;
  vec_t parameters;
} orm_sql_query;

ORM_C_API orm_status_t orm_sql_render(const orm_query_plan *plan,
                                      const orm_limits *limits,
                                      orm_sql_dialect dialect,
                                      orm_sql_query *out_query,
                                      orm_error_t *error);
ORM_C_API void orm_sql_query_destroy(orm_sql_query *query);

#ifdef __cplusplus
}
#endif

#endif

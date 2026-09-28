#ifndef ORM_MYSQL_SQL_H
#define ORM_MYSQL_SQL_H

#include "orm_sql_render.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Driver-local MySQL projection.
 *
 * The shared SQL renderer remains database-generic for its existing
 * SQLite/PostgreSQL consumers. MySQL reuses its stable ?N spelling, then
 * lowers placeholders to libmysql positional '?' inside the Driver module.
 */
orm_status_t orm_mysql_sql_render(
    const orm_query_plan *plan,
    const orm_limits *limits,
    orm_sql_query *out_query,
    orm_error_t *error);

#ifdef __cplusplus
}
#endif

#endif

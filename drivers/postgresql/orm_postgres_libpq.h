#ifndef ORM_POSTGRES_LIBPQ_H
#define ORM_POSTGRES_LIBPQ_H

#include "orm_postgres_cursor.h"

#include <libpq-fe.h>

#ifdef __cplusplus
extern "C" {
#endif

orm_postgres_driver orm_postgres_libpq_driver(PGconn *connection);

#ifdef __cplusplus
}
#endif

#endif

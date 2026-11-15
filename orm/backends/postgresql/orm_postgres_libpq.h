#ifndef ORM_POSTGRES_LIBPQ_H
#define ORM_POSTGRES_LIBPQ_H

#include "orm_postgres_cursor.h"

#include <libpq-fe.h>

struct orm_postgres_direct_tls_bridge;

typedef struct orm_postgres_libpq_context {
  PGconn *connection;
  struct orm_postgres_direct_tls_bridge *bridge;
} orm_postgres_libpq_context;

#ifdef __cplusplus
extern "C" {
#endif

orm_postgres_driver orm_postgres_libpq_driver(PGconn *connection);
orm_postgres_driver orm_postgres_libpq_bridge_driver(
    orm_postgres_libpq_context *context, PGconn *connection,
    struct orm_postgres_direct_tls_bridge *bridge);

#ifdef __cplusplus
}
#endif

#endif

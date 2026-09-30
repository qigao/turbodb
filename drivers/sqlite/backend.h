#ifndef ORM_SQLITE_BACKEND_INTERNAL_H
#define ORM_SQLITE_BACKEND_INTERNAL_H

#include "orm_internal.h"
#include <sqlite3.h>
#include <stdatomic.h>

typedef struct orm_sqlite_backend_state {
  sqlite3 *database;
  int transaction_active;
  int async_active; /* Admission and release belong to the calling owner. */
  atomic_size_t cursor_count;
} orm_sqlite_backend_state;

/* Worker-only entry while async_active reserves the connection. */
orm_status_t orm_sqlite_backend_open_rows(
    orm_sqlite_backend_state *state, const orm_query_plan *plan,
    const orm_limits *limits, orm_row_cursor *out, orm_error_t *error);

orm_status_t orm_sqlite_backend_open_async(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    const orm_async_config_t *config, orm_row_cursor *out, orm_error_t *error);

#endif

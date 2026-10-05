#ifndef ORM_TIDESDB_RELATIONAL_BACKEND_H
#define ORM_TIDESDB_RELATIONAL_BACKEND_H
#include "backend.h"

/* Opt-in raw MySQL execution. Configuration and ownership: sql/design.md.
 * Success publishes a complete backend; failure leaves output empty. */
orm_status_t orm_tidesdb_relational_create(const orm_config_t *config,
    const orm_limits *limits, orm_backend *out, orm_error_t *error);
#endif

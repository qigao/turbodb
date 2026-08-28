#include "orm_internal.h"
#include "orm_postgresql.h"

orm_status_t ORM_C_CALL orm_postgresql_connect(const orm_config_t *config,
                                               orm_connection_t **out_connection,
                                               orm_error_t *error) {
  if (out_connection != NULL) *out_connection = NULL;
  if (config == NULL || (!orm_view_equal_cstr(config->driver, "postgres") &&
                         !orm_view_equal_cstr(config->driver, "postgresql"))) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "PostgreSQL connector requires the postgres or postgresql driver");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  return orm_connect_with_factory_v1(config, orm_postgres_backend_create, out_connection, error);
}

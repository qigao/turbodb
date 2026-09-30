#ifndef ORM_POSTGRES_RUNTIME_FIXTURE_H
#define ORM_POSTGRES_RUNTIME_FIXTURE_H

#include <orm_runtime.h>
#include <stdlib.h>

/* Each case owns its runtime until all connection dependents have gone. */
static orm_status_t pg_runtime_create(
    orm_runtime_t **out_runtime, orm_error_t *error) {
  *out_runtime = NULL;
  const char *path = getenv("ORM_POSTGRESQL_PLUGIN");
  if (path == NULL || path[0] == '\0') {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "PostgreSQL E2E requires ORM_POSTGRESQL_PLUGIN");
    return ORM_STATUS_INVALID_ARGUMENT;
  }

  orm_runtime_config_t config;
  orm_runtime_t *runtime = NULL;
  orm_runtime_config_init(&config);
  orm_status_t status = orm_runtime_create(&config, &runtime, error);
  if (status != ORM_STATUS_OK) return status;

  orm_driver_load_config_t load = {0};
  load.struct_size = (uint32_t)sizeof(load);
  load.abi_version = ORM_RUNTIME_ABI_VERSION;
  load.module_path = orm_view(path);
  load.expected_driver_id = orm_view("postgresql");
  status = orm_runtime_load_driver(runtime, &load, error);
  if (status != ORM_STATUS_OK) {
    orm_runtime_release(runtime);
    return status;
  }
  *out_runtime = runtime;
  return ORM_STATUS_OK;
}

#endif /* ORM_POSTGRES_RUNTIME_FIXTURE_H */

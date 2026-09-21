#include <orm_runtime.h>

#include "orm_module_loader.h"
#include "../driver/orm_driver_contract.h"
#include "../driver/orm_driver_owner_bridge.h"
#include "../driver/orm_driver_plan_view.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RUNTIME_HEADER(T) {(uint32_t)sizeof(T), ORM_DRIVER_ABI_VERSION}
#define RUNTIME_TABLE(p) {(p), (uint32_t)sizeof(*(p)), 0u}
#define RUNTIME_FIELD_END(T, field) \
  ((uint32_t)(offsetof(T, field) + sizeof(((T *)0)->field)))

typedef int32_t (ORM_DRIVER_CALL *orm_runtime_bootstrap_fn)(
    const orm_driver_host_v1 *, uint32_t,
    const orm_driver_api_v1 **, uint32_t *);

typedef struct orm_runtime_id {
  uint32_t size;
  char text[ORM_RUNTIME_DRIVER_ID_CAPACITY];
} orm_runtime_id;

typedef struct orm_runtime_driver {
  orm_module_handle module;
  void *module_context;
  const orm_driver_api_v1 *api;
  uint32_t api_bytes;
  orm_driver_module_ops_v1 module_ops;
  orm_runtime_id canonical;
  uint32_t alias_count;
  uint64_t capabilities;
  uint64_t execution_models;
  uint8_t bundle_id[ORM_DRIVER_BUNDLE_ID_BYTES];
} orm_runtime_driver;

struct orm_runtime {
  uint32_t refs;
  uint32_t closed;
  orm_runtime_config_t config;
  orm_runtime_driver *drivers;
  orm_runtime_id *aliases;
  uint32_t driver_count;
};

static const uint8_t runtime_bundle[ORM_DRIVER_BUNDLE_ID_BYTES] =
    ORM_DRIVER_BUNDLE_ID_INIT;

static orm_status_t runtime_result(orm_error_t *error, orm_status_t status,
                                   const char *message) {
  orm_error_init(error);
  orm_error_set(error, status, message);
  return status;
}

static int runtime_id_valid(orm_string_view_t id) {
  if (id.data == NULL || id.len == 0u || id.len > ORM_DRIVER_ID_MAX_BYTES)
    return 0;
  const unsigned char *text = (const unsigned char *)id.data;
  if (text[0] < 'a' || text[0] > 'z')
    return 0;
  for (size_t i = 1u; i < id.len; ++i) {
    const unsigned char c = text[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '_' || c == '-'))
      return 0;
  }
  return 1;
}

static int runtime_same_bytes(const orm_runtime_id *id,
                              const void *data, uint64_t size) {
  return size == id->size &&
         (size == 0u || memcmp(id->text, data, (size_t)size) == 0);
}

static void runtime_copy_id(orm_runtime_id *out,
                            orm_driver_bytes_v1 value) {
  memset(out, 0, sizeof(*out));
  out->size = (uint32_t)value.size;
  if (value.size != 0u)
    memcpy(out->text, value.data, (size_t)value.size);
}

static orm_runtime_id *runtime_alias_slot(orm_runtime_t *runtime,
                                          uint32_t driver_index,
                                          uint32_t alias_index) {
  const size_t offset =
      (size_t)driver_index * runtime->config.max_aliases_per_driver +
      alias_index;
  return &runtime->aliases[offset];
}

static int runtime_id_conflicts(orm_runtime_t *runtime,
                                orm_driver_bytes_v1 id) {
  for (uint32_t i = 0u; i < runtime->driver_count; ++i) {
    orm_runtime_driver *driver = &runtime->drivers[i];
    if (runtime_same_bytes(&driver->canonical, id.data, id.size))
      return 1;
    for (uint32_t j = 0u; j < driver->alias_count; ++j) {
      if (runtime_same_bytes(runtime_alias_slot(runtime, i, j),
                             id.data, id.size))
        return 1;
    }
  }
  return 0;
}

static orm_runtime_driver *runtime_find_driver(orm_runtime_t *runtime,
                                               orm_string_view_t id) {
  for (uint32_t i = 0u; i < runtime->driver_count; ++i) {
    orm_runtime_driver *driver = &runtime->drivers[i];
    if (runtime_same_bytes(&driver->canonical, id.data, id.len))
      return driver;
    for (uint32_t j = 0u; j < driver->alias_count; ++j) {
      if (runtime_same_bytes(runtime_alias_slot(runtime, i, j),
                             id.data, id.len))
        return driver;
    }
  }
  return NULL;
}

static orm_driver_host_v1 runtime_host(void) {
  const orm_driver_plan_metadata_ops_v1 *metadata =
      orm_driver_plan_metadata_services_v1();
  const orm_driver_plan_value_ops_v1 *values =
      orm_driver_plan_value_services_v1();
  const orm_driver_lifetime_ops_v1 *lifetime =
      orm_driver_owner_services_v1();
  orm_driver_host_v1 host;
  memset(&host, 0, sizeof(host));
  host.header = (orm_driver_header_v1)RUNTIME_HEADER(orm_driver_host_v1);
  memcpy(host.bundle_id, runtime_bundle, sizeof(host.bundle_id));
  host.plan_metadata = (orm_driver_table_v1)RUNTIME_TABLE(metadata);
  host.plan_values = (orm_driver_table_v1)RUNTIME_TABLE(values);
  host.lifetime = (orm_driver_table_v1)RUNTIME_TABLE(lifetime);
  return host;
}

static orm_status_t runtime_validate_config(
    const orm_runtime_config_t *config, orm_error_t *error) {
  if (config == NULL || config->struct_size < sizeof(*config) ||
      config->abi_version != ORM_RUNTIME_ABI_VERSION ||
      config->max_drivers == 0u || config->max_connections == 0u ||
      config->max_pending_operations == 0u ||
      config->max_module_path_bytes == 0u || config->max_control_bytes == 0u ||
      config->execution.struct_size < sizeof(config->execution) ||
      config->execution.abi_version != ORM_RUNTIME_ABI_VERSION)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid runtime configuration");
  if (config->execution.execution_model != ORM_DRIVER_EXEC_CALLER_BLOCKING ||
      config->execution.executor != NULL ||
      config->execution.owner_context != NULL)
    return runtime_result(error, ORM_STATUS_UNSUPPORTED,
                          "runtime execution model is not implemented");
  if (config->max_aliases_per_driver != 0u &&
      (size_t)config->max_drivers >
          SIZE_MAX / (size_t)config->max_aliases_per_driver)
    return runtime_result(error, ORM_STATUS_LIMIT_EXCEEDED,
                          "runtime registry size exceeds addressable memory");
  return ORM_STATUS_OK;
}

void ORM_C_CALL orm_runtime_config_init(orm_runtime_config_t *config) {
  if (config == NULL)
    return;
  memset(config, 0, sizeof(*config));
  config->struct_size = (uint32_t)sizeof(*config);
  config->abi_version = ORM_RUNTIME_ABI_VERSION;
  config->max_drivers = ORM_RUNTIME_DEFAULT_MAX_DRIVERS;
  config->max_aliases_per_driver =
      ORM_RUNTIME_DEFAULT_MAX_ALIASES_PER_DRIVER;
  config->max_connections = ORM_RUNTIME_DEFAULT_MAX_CONNECTIONS;
  config->max_pending_operations =
      ORM_RUNTIME_DEFAULT_MAX_PENDING_OPERATIONS;
  config->max_module_path_bytes =
      ORM_RUNTIME_DEFAULT_MAX_MODULE_PATH_BYTES;
  config->max_control_bytes = ORM_RUNTIME_DEFAULT_MAX_CONTROL_BYTES;
  config->execution.struct_size =
      (uint32_t)sizeof(config->execution);
  config->execution.abi_version = ORM_RUNTIME_ABI_VERSION;
  config->execution.execution_model = ORM_DRIVER_EXEC_CALLER_BLOCKING;
}

orm_status_t ORM_C_CALL
orm_runtime_create(const orm_runtime_config_t *config,
                   orm_runtime_t **out_runtime, orm_error_t *error) {
  if (out_runtime != NULL)
    *out_runtime = NULL;
  if (out_runtime == NULL)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "runtime output is required");
  orm_status_t status = runtime_validate_config(config, error);
  if (status != ORM_STATUS_OK)
    return status;

  orm_runtime_t *runtime = (orm_runtime_t *)calloc(1u, sizeof(*runtime));
  if (runtime == NULL)
    return runtime_result(error, ORM_STATUS_OUT_OF_MEMORY,
                          "allocate runtime");

  runtime->drivers = (orm_runtime_driver *)calloc(
      config->max_drivers, sizeof(*runtime->drivers));
  const size_t alias_count =
      (size_t)config->max_drivers * config->max_aliases_per_driver;
  if (runtime->drivers == NULL ||
      (alias_count != 0u &&
       (runtime->aliases = (orm_runtime_id *)calloc(
            alias_count, sizeof(*runtime->aliases))) == NULL)) {
    free(runtime->aliases);
    free(runtime->drivers);
    free(runtime);
    return runtime_result(error, ORM_STATUS_OUT_OF_MEMORY,
                          "allocate runtime registry");
  }

  runtime->refs = 1u;
  runtime->config = *config;
  *out_runtime = runtime;
  return runtime_result(error, ORM_STATUS_OK, NULL);
}

static orm_status_t runtime_copy_path(orm_runtime_t *runtime,
                                      orm_string_view_t path,
                                      char **out,
                                      orm_error_t *error) {
  *out = NULL;
  if (path.data == NULL || path.len == 0u ||
      path.len > runtime->config.max_module_path_bytes ||
      path.len == SIZE_MAX ||
      memchr(path.data, '\0', path.len) != NULL)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid driver module path");
  char *copy = (char *)malloc(path.len + 1u);
  if (copy == NULL)
    return runtime_result(error, ORM_STATUS_OUT_OF_MEMORY,
                          "allocate driver module path");
  memcpy(copy, path.data, path.len);
  copy[path.len] = '\0';
  *out = copy;
  return ORM_STATUS_OK;
}

orm_status_t ORM_C_CALL
orm_runtime_load_driver(orm_runtime_t *runtime,
                        const orm_driver_load_config_t *config,
                        orm_error_t *error) {
  if (runtime == NULL || config == NULL ||
      config->struct_size < sizeof(*config) ||
      config->abi_version != ORM_RUNTIME_ABI_VERSION ||
      config->flags != 0u || config->reserved != 0u ||
      !runtime_id_valid(config->expected_driver_id))
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid driver load configuration");
  if (runtime->closed != 0u)
    return runtime_result(error, ORM_STATUS_INVALID_STATE,
                          "runtime is closed");
  if (runtime->driver_count == runtime->config.max_drivers)
    return runtime_result(error, ORM_STATUS_LIMIT_EXCEEDED,
                          "runtime driver registry is full");

  char *path = NULL;
  orm_status_t status =
      runtime_copy_path(runtime, config->module_path, &path, error);
  if (status != ORM_STATUS_OK)
    return status;

  orm_module_handle module = {0};
  status = orm_module_open_absolute(path, &module, error);
  free(path);
  if (status != ORM_STATUS_OK)
    return status;

  orm_runtime_bootstrap_fn bootstrap = NULL;
  status = orm_module_symbol(&module, "orm_driver_get_api_v1",
                             &bootstrap, sizeof(bootstrap), error);
  if (status != ORM_STATUS_OK)
    goto fail_module;

  const orm_driver_host_v1 host = runtime_host();
  const orm_driver_api_v1 *api = NULL;
  uint32_t api_bytes = 0u;
  const int32_t bootstrap_status =
      bootstrap(&host, (uint32_t)sizeof(host), &api, &api_bytes);
  if (bootstrap_status != ORM_STATUS_OK) {
    status = (orm_status_t)bootstrap_status;
    runtime_result(error, status, "driver bootstrap failed");
    goto fail_module;
  }
  if (api == NULL) {
    status = ORM_STATUS_ABI_MISMATCH;
    runtime_result(error, status, "driver bootstrap returned no descriptor");
    goto fail_module;
  }

  uint32_t declared = 0u;
  status = orm_driver_check_prefix(
      api, api_bytes, ORM_DRIVER_ABI_VERSION,
      RUNTIME_FIELD_END(orm_driver_api_v1, canonical_id), &declared);
  if (status != ORM_STATUS_OK) {
    runtime_result(error, status, "driver descriptor prefix is invalid");
    goto fail_module;
  }

  orm_driver_bytes_v1 canonical;
  memcpy(&canonical, &api->canonical_id, sizeof(canonical));
  status = orm_driver_validate_api_v1(
      api, api_bytes, runtime_bundle, canonical,
      runtime->config.max_aliases_per_driver, error);
  if (status != ORM_STATUS_OK)
    goto fail_module;

  if (canonical.size != config->expected_driver_id.len ||
      memcmp(canonical.data, config->expected_driver_id.data,
             (size_t)canonical.size) != 0) {
    status = ORM_STATUS_DRIVER_ID_MISMATCH;
    runtime_result(error, status, "driver canonical ID does not match request");
    goto fail_module;
  }
  if ((api->execution_models &
       runtime->config.execution.execution_model) == 0u) {
    status = ORM_STATUS_UNSUPPORTED;
    runtime_result(error, status,
                   "driver does not support runtime execution model");
    goto fail_module;
  }

  if (runtime_id_conflicts(runtime, canonical)) {
    status = ORM_STATUS_DRIVER_ALREADY_REGISTERED;
    runtime_result(error, status, "driver ID is already registered");
    goto fail_module;
  }
  for (uint32_t i = 0u; i < api->alias_count; ++i) {
    orm_driver_bytes_v1 alias;
    memcpy(&alias,
           (const unsigned char *)api->aliases +
               (size_t)i * sizeof(alias),
           sizeof(alias));
    if (runtime_id_conflicts(runtime, alias)) {
      status = ORM_STATUS_DRIVER_ALREADY_REGISTERED;
      runtime_result(error, status, "driver alias is already registered");
      goto fail_module;
    }
  }

  orm_driver_module_ops_v1 module_ops;
  memcpy(&module_ops, api->module_ops.data, sizeof(module_ops));
  void *module_context = NULL;
  status = module_ops.initialize(&host, &module_context, error);
  if (status != ORM_STATUS_OK)
    goto fail_module;
  if (module_context == NULL) {
    status = ORM_STATUS_ABI_MISMATCH;
    runtime_result(error, status,
                   "driver initialize returned no module context");
    goto fail_initialized;
  }

  const uint32_t index = runtime->driver_count;
  orm_runtime_driver *entry = &runtime->drivers[index];
  memset(entry, 0, sizeof(*entry));
  entry->module = module;
  entry->module_context = module_context;
  entry->api = api;
  entry->api_bytes = api_bytes;
  entry->module_ops = module_ops;
  runtime_copy_id(&entry->canonical, canonical);
  entry->alias_count = api->alias_count;
  entry->capabilities = api->capabilities;
  entry->execution_models = api->execution_models;
  memcpy(entry->bundle_id, api->bundle_id, sizeof(entry->bundle_id));
  for (uint32_t i = 0u; i < api->alias_count; ++i) {
    orm_driver_bytes_v1 alias;
    memcpy(&alias,
           (const unsigned char *)api->aliases +
               (size_t)i * sizeof(alias),
           sizeof(alias));
    runtime_copy_id(runtime_alias_slot(runtime, index, i), alias);
  }
  ++runtime->driver_count;
  return runtime_result(error, ORM_STATUS_OK, NULL);

fail_initialized:
  {
    orm_error_t ignored;
    orm_error_init(&ignored);
    (void)module_ops.finalize(module_context, &ignored);
  }
fail_module:
  orm_module_close(&module);
  return status;
}

orm_status_t ORM_C_CALL
orm_runtime_driver_info(orm_runtime_t *runtime, orm_string_view_t id,
                        orm_driver_info_t *out_info, orm_error_t *error) {
  if (out_info != NULL)
    memset(out_info, 0, sizeof(*out_info));
  if (runtime == NULL || out_info == NULL || !runtime_id_valid(id))
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid driver info request");
  if (runtime->closed != 0u)
    return runtime_result(error, ORM_STATUS_INVALID_STATE,
                          "runtime is closed");
  orm_runtime_driver *driver = runtime_find_driver(runtime, id);
  if (driver == NULL)
    return runtime_result(error, ORM_STATUS_DRIVER_NOT_REGISTERED,
                          "driver is not registered");

  out_info->struct_size = (uint32_t)sizeof(*out_info);
  out_info->abi_version = ORM_RUNTIME_ABI_VERSION;
  out_info->canonical_id_size = driver->canonical.size;
  memcpy(out_info->canonical_id, driver->canonical.text,
         driver->canonical.size);
  out_info->capabilities = driver->capabilities;
  out_info->execution_models = driver->execution_models;
  memcpy(out_info->bundle_id, driver->bundle_id,
         sizeof(out_info->bundle_id));
  return runtime_result(error, ORM_STATUS_OK, NULL);
}

orm_status_t ORM_C_CALL
orm_runtime_close(orm_runtime_t *runtime, orm_error_t *error) {
  if (runtime == NULL)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "runtime is required");
  if (runtime->closed == 1u)
    return runtime_result(error, ORM_STATUS_OK, NULL);
  if (runtime->closed != 0u)
    return runtime_result(error, ORM_STATUS_CLEANUP_FAILED,
                          "runtime cleanup is quarantined");

  for (uint32_t remaining = runtime->driver_count;
       remaining != 0u; --remaining) {
    orm_runtime_driver *driver = &runtime->drivers[remaining - 1u];
    orm_error_t finalize_error;
    orm_error_init(&finalize_error);
    orm_status_t status =
        driver->module_ops.finalize(driver->module_context,
                                    &finalize_error);
    if (status != ORM_STATUS_OK) {
      runtime->closed = 2u;
      return runtime_result(error, ORM_STATUS_CLEANUP_FAILED,
                            "driver module finalization failed");
    }
    driver->module_context = NULL;
    orm_module_close(&driver->module);
  }
  runtime->closed = 1u;
  return runtime_result(error, ORM_STATUS_OK, NULL);
}

void ORM_C_CALL orm_runtime_retain(orm_runtime_t *runtime) {
  if (runtime == NULL)
    return;
  if (runtime->refs == UINT32_MAX)
    abort();
  ++runtime->refs;
}

void ORM_C_CALL orm_runtime_release(orm_runtime_t *runtime) {
  if (runtime == NULL)
    return;
  if (runtime->refs == 0u)
    abort();
  --runtime->refs;
  if (runtime->refs != 0u)
    return;

  orm_error_t error;
  orm_error_init(&error);
  const orm_status_t status = orm_runtime_close(runtime, &error);
  if (status != ORM_STATUS_OK) {
    if (runtime->config.on_cleanup_error != NULL) {
      runtime->config.on_cleanup_error(runtime->config.cleanup_context,
                                       &error);
      return;
    }
    abort();
  }
  free(runtime->aliases);
  free(runtime->drivers);
  free(runtime);
}

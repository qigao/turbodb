#include "orm_runtime_internal.h"
#include "../abi/orm_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <stdio.h>

orm_status_t runtime_result(orm_error_t *error, orm_status_t status,
                                   const char *message) {
  orm_error_init(error);
  orm_error_set(error, status, message);
  return status;
}

orm_status_t runtime_plugin_status(
    cmeta_plugin_status status, orm_error_t *error, const char *context) {
  orm_status_t mapped;
  switch (status) {
  case CMETA_PLUGIN_OK:
    mapped = ORM_STATUS_OK;
    break;
  case CMETA_PLUGIN_INVALID_ARGUMENT:
    mapped = ORM_STATUS_INVALID_ARGUMENT;
    break;
  case CMETA_PLUGIN_ALLOCATION_FAILED:
    mapped = ORM_STATUS_OUT_OF_MEMORY;
    break;
  case CMETA_PLUGIN_CAPACITY_EXCEEDED:
    mapped = ORM_STATUS_LIMIT_EXCEEDED;
    break;
  case CMETA_PLUGIN_DUPLICATE_PLUGIN_ID:
  case CMETA_PLUGIN_ALREADY:
    mapped = ORM_STATUS_DRIVER_ALREADY_REGISTERED;
    break;
  case CMETA_PLUGIN_LOAD_FAILED:
    mapped = ORM_STATUS_DRIVER_LOAD_ERROR;
    break;
  case CMETA_PLUGIN_QUERY_MISSING:
  case CMETA_PLUGIN_UNKNOWN_EXPORT:
    mapped = ORM_STATUS_DRIVER_ENTRY_MISSING;
    break;
  case CMETA_PLUGIN_UNSUPPORTED_ABI:
  case CMETA_PLUGIN_INVALID_MANIFEST:
  case CMETA_PLUGIN_QUERY_REJECTED:
  case CMETA_PLUGIN_INCOMPATIBLE_CONTRACT:
    mapped = ORM_STATUS_ABI_MISMATCH;
    break;
  case CMETA_PLUGIN_BUSY:
    mapped = ORM_STATUS_BUSY;
    break;
  case CMETA_PLUGIN_UNLOAD_FAILED:
    mapped = ORM_STATUS_CLEANUP_FAILED;
    break;
  case CMETA_PLUGIN_UNKNOWN_PLUGIN:
  case CMETA_PLUGIN_STALE:
  case CMETA_PLUGIN_INVALID_STATE:
  case CMETA_PLUGIN_DUPLICATE_EXPORT:
  default:
    mapped = ORM_STATUS_INVALID_STATE;
    break;
  }
  if (mapped == ORM_STATUS_OK)
    return runtime_result(error, mapped, NULL);
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message), "%s: %s",
                 context != NULL ? context : "plugin",
                 cmeta_plugin_status_string(status));
  return runtime_result(error, mapped, message);
}

int runtime_id_valid(orm_string_view_t id) {
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

int runtime_same_bytes(const orm_runtime_id *id,
                              const void *data, uint64_t size) {
  return size == id->size &&
         (size == 0u || memcmp(id->text, data, (size_t)size) == 0);
}

orm_runtime_driver *runtime_find_driver(orm_runtime_t *runtime,
                                               orm_string_view_t id) {
  for (uint32_t i = 0u; i < runtime->driver_count; ++i) {
    orm_runtime_driver *driver = &runtime->drivers[i];
    if (runtime_same_bytes(&driver->canonical, id.data, id.len))
      return driver;
  }
  return NULL;
}

orm_status_t runtime_acquire_dependent(
    orm_runtime_t *runtime, orm_error_t *error) {
  cmeta_mutex_lock(&runtime->mutex);
  if (runtime->closed != ORM_RUNTIME_OPEN) {
    cmeta_mutex_unlock(&runtime->mutex);
    return runtime_result(error, ORM_STATUS_INVALID_STATE,
                          "runtime is closed");
  }
  if (runtime->dependents == runtime->config.max_connections) {
    cmeta_mutex_unlock(&runtime->mutex);
    return runtime_result(error, ORM_STATUS_LIMIT_EXCEEDED,
                          "runtime connection budget is full");
  }
  if (runtime->refs == UINT32_MAX) {
    cmeta_mutex_unlock(&runtime->mutex);
    return runtime_result(error, ORM_STATUS_LIMIT_EXCEEDED,
                          "runtime reference budget is full");
  }
  ++runtime->dependents;
  ++runtime->refs;
  cmeta_mutex_unlock(&runtime->mutex);
  return ORM_STATUS_OK;
}

static int runtime_release_dependent_ref(orm_runtime_t *runtime) {
  int last = 0;
  cmeta_mutex_lock(&runtime->mutex);
  if (runtime->dependents == 0u || runtime->refs == 0u) {
    cmeta_mutex_unlock(&runtime->mutex);
    abort();
  }
  --runtime->dependents;
  --runtime->refs;
  last = runtime->refs == 0u;
  cmeta_mutex_unlock(&runtime->mutex);
  return last;
}

void runtime_finish_pending(
    orm_runtime_t *runtime, int load_operation) {
  cmeta_mutex_lock(&runtime->mutex);
  if (runtime->pending_operations == 0u ||
      (load_operation && runtime->load_active == 0u)) {
    cmeta_mutex_unlock(&runtime->mutex);
    abort();
  }
  --runtime->pending_operations;
  if (load_operation) runtime->load_active = 0u;
  cmeta_mutex_unlock(&runtime->mutex);
}

static void runtime_release_last(orm_runtime_t *runtime);

void runtime_drop_dependent(orm_runtime_t *runtime) {
  if (runtime_release_dependent_ref(runtime))
    runtime_release_last(runtime);
}

static int runtime_release_extension_ref(orm_runtime_t *runtime) {
  int last = 0;
  cmeta_mutex_lock(&runtime->mutex);
  if (runtime->extension_count == 0u ||
      runtime->dependents == 0u || runtime->refs == 0u) {
    cmeta_mutex_unlock(&runtime->mutex);
    abort();
  }
  --runtime->extension_count;
  --runtime->dependents;
  --runtime->refs;
  last = runtime->refs == 0u;
  cmeta_mutex_unlock(&runtime->mutex);
  return last;
}

void runtime_drop_extension(orm_runtime_t *runtime) {
  if (runtime_release_extension_ref(runtime))
    runtime_release_last(runtime);
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

  cmeta_mutex_init(&runtime->mutex);
  if (runtime->mutex == NULL) {
    free(runtime);
    return runtime_result(error, ORM_STATUS_OUT_OF_MEMORY,
                          "initialize runtime mutex");
  }

  cmeta_plugin_registry_config plugin_config = {
      (size_t)config->max_drivers};
  const cmeta_plugin_status plugin_status =
      cmeta_plugin_registry_init(&runtime->plugins, &plugin_config);
  if (plugin_status != CMETA_PLUGIN_OK) {
    cmeta_mutex_destroy(&runtime->mutex);
    free(runtime);
    return runtime_plugin_status(plugin_status, error,
                                 "initialize Plugin registry");
  }

  runtime->drivers = (orm_runtime_driver *)calloc(
      config->max_drivers, sizeof(*runtime->drivers));
  if (runtime->drivers == NULL) {
    (void)cmeta_plugin_registry_destroy(&runtime->plugins);
    cmeta_mutex_destroy(&runtime->mutex);
    free(runtime);
    return runtime_result(error, ORM_STATUS_OUT_OF_MEMORY,
                          "allocate runtime driver index");
  }

  runtime->refs = 1u;
  runtime->config = *config;
  *out_runtime = runtime;
  return runtime_result(error, ORM_STATUS_OK, NULL);
}

orm_status_t ORM_C_CALL
orm_runtime_close(orm_runtime_t *runtime, orm_error_t *error) {
  if (runtime == NULL)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "runtime is required");

  cmeta_mutex_lock(&runtime->mutex);
  if (runtime->closed == ORM_RUNTIME_CLOSED) {
    cmeta_mutex_unlock(&runtime->mutex);
    return runtime_result(error, ORM_STATUS_OK, NULL);
  }
  if (runtime->closed == ORM_RUNTIME_FAILED) {
    cmeta_mutex_unlock(&runtime->mutex);
    return runtime_result(error, ORM_STATUS_CLEANUP_FAILED,
                          "runtime cleanup is quarantined");
  }
  if (runtime->closed == ORM_RUNTIME_OPEN) {
    if (runtime->dependents != 0u || runtime->pending_operations != 0u) {
      cmeta_mutex_unlock(&runtime->mutex);
      return runtime_result(error, ORM_STATUS_BUSY,
                            "runtime has active or pending operations");
    }
    runtime->closed = ORM_RUNTIME_CLOSING;
    runtime->close_remaining = runtime->driver_count;
  }
  const uint32_t remaining_snapshot = runtime->close_remaining;
  cmeta_mutex_unlock(&runtime->mutex);

  for (uint32_t remaining = remaining_snapshot; remaining != 0u; --remaining) {
    orm_runtime_driver *driver = &runtime->drivers[remaining - 1u];
    cmeta_plugin_status plugin_status =
        cmeta_plugin_registry_request_stop(&runtime->plugins, driver->plugin);
    if (plugin_status != CMETA_PLUGIN_OK &&
        plugin_status != CMETA_PLUGIN_ALREADY) {
      cmeta_mutex_lock(&runtime->mutex);
      runtime->closed = ORM_RUNTIME_FAILED;
      cmeta_mutex_unlock(&runtime->mutex);
      return runtime_plugin_status(
          plugin_status, error, "request Driver Plugin stop");
    }

    bool quiescent = false;
    plugin_status = cmeta_plugin_registry_poll_quiescent(
        &runtime->plugins, driver->plugin, &quiescent);
    if (plugin_status != CMETA_PLUGIN_OK) {
      cmeta_mutex_lock(&runtime->mutex);
      runtime->closed = ORM_RUNTIME_FAILED;
      cmeta_mutex_unlock(&runtime->mutex);
      return runtime_plugin_status(
          plugin_status, error, "poll Driver Plugin quiescence");
    }
    if (!quiescent)
      return runtime_result(error, ORM_STATUS_BUSY,
                            "driver Plugin is still quiescing");

    plugin_status =
        cmeta_plugin_registry_unload(&runtime->plugins, driver->plugin);
    if (plugin_status != CMETA_PLUGIN_OK) {
      if (plugin_status == CMETA_PLUGIN_BUSY)
        return runtime_plugin_status(
            plugin_status, error, "unload Driver Plugin");
      cmeta_mutex_lock(&runtime->mutex);
      runtime->closed = ORM_RUNTIME_FAILED;
      cmeta_mutex_unlock(&runtime->mutex);
      return runtime_plugin_status(
          plugin_status, error, "unload Driver Plugin");
    }

    free(driver->module_path);
    driver->module_path = NULL;
    driver->binding = NULL;
    driver->plugin = (cmeta_plugin_ref){0};

    cmeta_mutex_lock(&runtime->mutex);
    if (runtime->close_remaining != remaining) {
      cmeta_mutex_unlock(&runtime->mutex);
      abort();
    }
    --runtime->close_remaining;
    cmeta_mutex_unlock(&runtime->mutex);
  }

  const cmeta_plugin_status destroy_status =
      cmeta_plugin_registry_destroy(&runtime->plugins);
  if (destroy_status != CMETA_PLUGIN_OK) {
    cmeta_mutex_lock(&runtime->mutex);
    runtime->closed = ORM_RUNTIME_FAILED;
    cmeta_mutex_unlock(&runtime->mutex);
    return runtime_plugin_status(
        destroy_status, error, "destroy Driver Plugin registry");
  }

  cmeta_mutex_lock(&runtime->mutex);
  runtime->closed = ORM_RUNTIME_CLOSED;
  cmeta_mutex_unlock(&runtime->mutex);
  return runtime_result(error, ORM_STATUS_OK, NULL);
}

static void runtime_release_last(orm_runtime_t *runtime) {
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
  free(runtime->drivers);
  cmeta_mutex_destroy(&runtime->mutex);
  free(runtime);
}

void ORM_C_CALL orm_runtime_retain(orm_runtime_t *runtime) {
  if (runtime == NULL) return;
  cmeta_mutex_lock(&runtime->mutex);
  if (runtime->refs == 0u || runtime->refs == UINT32_MAX) {
    cmeta_mutex_unlock(&runtime->mutex);
    abort();
  }
  ++runtime->refs;
  cmeta_mutex_unlock(&runtime->mutex);
}

void ORM_C_CALL orm_runtime_release(orm_runtime_t *runtime) {
  if (runtime == NULL) return;
  int last = 0;
  cmeta_mutex_lock(&runtime->mutex);
  if (runtime->refs == 0u) {
    cmeta_mutex_unlock(&runtime->mutex);
    abort();
  }
  --runtime->refs;
  last = runtime->refs == 0u;
  cmeta_mutex_unlock(&runtime->mutex);
  if (last) runtime_release_last(runtime);
}

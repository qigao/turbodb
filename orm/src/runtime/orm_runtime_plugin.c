#include "orm_runtime_internal.h"
#include "../abi/orm_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct orm_runtime_driver_extension {
  orm_runtime_t *runtime;
  cmeta_plugin_lease plugin_lease;
};

static const uint8_t runtime_bundle[ORM_DRIVER_BUNDLE_ID_BYTES] =
    ORM_DRIVER_BUNDLE_ID_INIT;

static void runtime_copy_id(orm_runtime_id *out,
                            orm_driver_bytes_v1 value) {
  memset(out, 0, sizeof(*out));
  out->size = (uint32_t)value.size;
  if (value.size != 0u)
    memcpy(out->text, value.data, (size_t)value.size);
}

static int runtime_id_conflicts(orm_runtime_t *runtime,
                                orm_driver_bytes_v1 id) {
  for (uint32_t i = 0u; i < runtime->driver_count; ++i) {
    if (runtime_same_bytes(&runtime->drivers[i].canonical, id.data, id.size))
      return 1;
  }
  return 0;
}

static int runtime_path_registered(orm_runtime_t *runtime, const char *path) {
  if (path == NULL) return 0;
  for (uint32_t i = 0u; i < runtime->driver_count; ++i) {
    const char *registered = runtime->drivers[i].module_path;
    if (registered != NULL && strcmp(registered, path) == 0)
      return 1;
  }
  return 0;
}

static orm_status_t runtime_begin_load(
    orm_runtime_t *runtime, const char *path, orm_error_t *error) {
  orm_status_t status = ORM_STATUS_OK;
  const char *message = NULL;
  cmeta_mutex_lock(&runtime->mutex);
  if (runtime->closed != ORM_RUNTIME_OPEN) {
    status = ORM_STATUS_INVALID_STATE;
    message = "runtime is closed";
  } else if (runtime->pending_operations >=
             runtime->config.max_pending_operations) {
    status = ORM_STATUS_LIMIT_EXCEEDED;
    message = "runtime pending-operation budget is full";
  } else if (runtime->load_active != 0u) {
    status = ORM_STATUS_BUSY;
    message = "runtime already has a driver load in progress";
  } else if (runtime->driver_count == runtime->config.max_drivers) {
    status = ORM_STATUS_LIMIT_EXCEEDED;
    message = "runtime driver registry is full";
  } else if (runtime_path_registered(runtime, path)) {
    status = ORM_STATUS_DRIVER_ALREADY_REGISTERED;
    message = "driver module path is already registered";
  } else {
    runtime->load_active = 1u;
    ++runtime->pending_operations;
  }
  cmeta_mutex_unlock(&runtime->mutex);
  return runtime_result(error, status, message);
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

static cmeta_plugin_status runtime_discard_plugin(
    orm_runtime_t *runtime, cmeta_plugin_ref ref, int started) {
  cmeta_plugin_status status;
  bool quiescent = false;
  if (!cmeta_plugin_ref_valid(ref)) return CMETA_PLUGIN_OK;
  if (started) {
    status = cmeta_plugin_registry_request_stop(&runtime->plugins, ref);
    if (status != CMETA_PLUGIN_OK && status != CMETA_PLUGIN_ALREADY)
      return status;
    status = cmeta_plugin_registry_poll_quiescent(
        &runtime->plugins, ref, &quiescent);
    if (status != CMETA_PLUGIN_OK)
      return status;
    if (!quiescent)
      return CMETA_PLUGIN_BUSY;
  }
  return cmeta_plugin_registry_unload(&runtime->plugins, ref);
}

orm_status_t ORM_C_CALL
orm_runtime_load_driver(orm_runtime_t *runtime,
                        const orm_driver_load_config_t *config,
                        orm_error_t *error) {
  cmeta_plugin_ref plugin = {0};
  cmeta_plugin_lease admission = {0};
  const cmeta_plugin_manifest *manifest = NULL;
  const cmeta_plugin_export *entry = NULL;
  TurboDb_Driver *binding = NULL;
  int started = 0;
  orm_status_t status;
  cmeta_plugin_status plugin_status;

  if (runtime == NULL || config == NULL ||
      config->struct_size < sizeof(*config) ||
      config->abi_version != ORM_RUNTIME_ABI_VERSION ||
      config->flags != 0u || config->reserved != 0u ||
      !runtime_id_valid(config->expected_driver_id))
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid driver load configuration");

  char *path = NULL;
  status = runtime_copy_path(runtime, config->module_path, &path, error);
  if (status != ORM_STATUS_OK)
    return status;

  status = runtime_begin_load(runtime, path, error);
  if (status != ORM_STATUS_OK) {
    free(path);
    return status;
  }

  plugin_status =
      cmeta_plugin_registry_load(&runtime->plugins, path, &plugin);
  if (plugin_status != CMETA_PLUGIN_OK) {
    status = plugin_status == CMETA_PLUGIN_UNSUPPORTED_ABI
                 ? runtime_result(error, ORM_STATUS_ABI_MISMATCH,
                                  "driver Plugin ABI mismatch")
                 : runtime_plugin_status(
                       plugin_status, error, "load driver Plugin");
    goto fail_reserved;
  }

  plugin_status = cmeta_plugin_registry_start(&runtime->plugins, plugin);
  if (plugin_status != CMETA_PLUGIN_OK) {
    status = runtime_plugin_status(plugin_status, error, "start driver Plugin");
    goto fail_plugin;
  }
  started = 1;

  plugin_status = cmeta_plugin_registry_acquire(
      &runtime->plugins, plugin, &admission, &manifest);
  if (plugin_status != CMETA_PLUGIN_OK) {
    status = runtime_plugin_status(
        plugin_status, error, "acquire driver Plugin admission lease");
    goto fail_plugin;
  }

  if (manifest == NULL || manifest->plugin_id == NULL) {
    status = runtime_result(error, ORM_STATUS_ABI_MISMATCH,
                            "driver Plugin has no canonical ID");
    goto fail_admission;
  }

  const size_t plugin_id_size = strlen(manifest->plugin_id);
  const orm_string_view_t plugin_id = {
      manifest->plugin_id, plugin_id_size};
  if (!runtime_id_valid(plugin_id) ||
      plugin_id_size != config->expected_driver_id.len ||
      memcmp(manifest->plugin_id, config->expected_driver_id.data,
             plugin_id_size) != 0) {
    status = runtime_result(error, ORM_STATUS_DRIVER_ID_MISMATCH,
                            "Plugin ID does not match requested driver ID");
    goto fail_admission;
  }

  plugin_status = cmeta_plugin_manifest_find_export(
      manifest, ORM_DRIVER_PLUGIN_EXPORT_ID, &entry);
  if (plugin_status != CMETA_PLUGIN_OK) {
    status = runtime_plugin_status(
        plugin_status, error, "find TurboDb.Driver export");
    goto fail_admission;
  }

  if (entry->kind == CMETA_PLUGIN_EXPORT_INTERFACE &&
      entry->contract_id != NULL &&
      strcmp(entry->contract_id, ORM_DRIVER_INTERFACE_CONTRACT_ID) == 0 &&
      entry->contract_version != ORM_DRIVER_INTERFACE_CONTRACT_VERSION) {
    status = runtime_result(error, ORM_STATUS_ABI_MISMATCH,
                            "TurboDb.Driver contract version mismatch");
    goto fail_admission;
  }
  if (entry->kind == CMETA_PLUGIN_EXPORT_INTERFACE &&
      entry->contract_id != NULL &&
      strcmp(entry->contract_id, ORM_DRIVER_INTERFACE_CONTRACT_ID) == 0 &&
      entry->contract_version == ORM_DRIVER_INTERFACE_CONTRACT_VERSION &&
      (entry->value.interface.desc == NULL ||
       !cmeta_interface_desc_valid(entry->value.interface.desc) ||
       !cmeta_interface_desc_equal(entry->value.interface.desc,
                                   TurboDb_Driver_interface()))) {
    status = runtime_result(error, ORM_STATUS_ABI_MISMATCH,
                            "TurboDb.Driver CMeta Interface shape mismatch");
    goto fail_admission;
  }

  plugin_status = cmeta_plugin_export_require_interface(
      entry, ORM_DRIVER_INTERFACE_CONTRACT_ID,
      ORM_DRIVER_INTERFACE_CONTRACT_VERSION, 0u,
      TurboDb_Driver_interface());
  if (plugin_status != CMETA_PLUGIN_OK) {
    status = runtime_plugin_status(
        plugin_status, error, "admit TurboDb.Driver interface");
    goto fail_admission;
  }

  binding = (TurboDb_Driver *)entry->value.interface.value;
  if (binding == NULL || !TurboDb_Driver_valid(binding) ||
      binding->vtable->capabilities != entry->capabilities ||
      (entry->capabilities & ~ORM_DRIVER_CAP_KNOWN_MASK) != 0u) {
    status = runtime_result(error, ORM_STATUS_ABI_MISMATCH,
                            "TurboDb.Driver binding is invalid");
    goto fail_admission;
  }

  const uint64_t execution_models =
      TurboDb_Driver_execution_models(binding);
  if ((execution_models & ~ORM_DRIVER_EXEC_KNOWN_MASK) != 0u) {
    status = runtime_result(error, ORM_STATUS_UNSUPPORTED,
                            "TurboDb.Driver declares unknown execution models");
    goto fail_admission;
  }
  if ((execution_models & ORM_DRIVER_EXEC_CALLER_BLOCKING) == 0u) {
    status = runtime_result(
        error, ORM_STATUS_UNSUPPORTED,
        "TurboDb.Driver does not support the caller-blocking control plane");
    goto fail_admission;
  }

  const orm_driver_storage_capabilities_v1 *storage_capabilities =
      TurboDb_Driver_storage_capabilities(binding);
  if (!orm_driver_storage_capabilities_valid(storage_capabilities)) {
    status = runtime_result(
        error, ORM_STATUS_ABI_MISMATCH,
        "TurboDb.Driver storage capability descriptor is invalid");
    goto fail_admission;
  }
  /* The descriptor is borrowed Plugin storage. Snapshot it while admission
   * still holds the module lease; do not dereference it after lease release. */
  const orm_driver_storage_capabilities_v1 storage_snapshot =
      *storage_capabilities;
  orm_runtime_id canonical_snapshot;
  runtime_copy_id(
      &canonical_snapshot,
      (orm_driver_bytes_v1){manifest->plugin_id, (uint64_t)plugin_id_size});
  const uint64_t capabilities_snapshot = entry->capabilities;

  if (runtime_id_conflicts(
          runtime,
          (orm_driver_bytes_v1){canonical_snapshot.text,
                                canonical_snapshot.size})) {
    status = runtime_result(error, ORM_STATUS_DRIVER_ALREADY_REGISTERED,
                            "driver ID is already registered");
    goto fail_admission;
  }

  plugin_status =
      cmeta_plugin_registry_release(&runtime->plugins, &admission);
  if (plugin_status != CMETA_PLUGIN_OK) {
    status = runtime_plugin_status(
        plugin_status, error, "release driver admission lease");
    goto fail_plugin;
  }

  cmeta_mutex_lock(&runtime->mutex);
  if (runtime->closed != ORM_RUNTIME_OPEN ||
      runtime->load_active == 0u ||
      runtime->pending_operations == 0u) {
    cmeta_mutex_unlock(&runtime->mutex);
    status = runtime_result(error, ORM_STATUS_INVALID_STATE,
                            "runtime load reservation was lost");
    goto fail_plugin;
  }
  if (runtime_id_conflicts(
          runtime,
          (orm_driver_bytes_v1){canonical_snapshot.text,
                                canonical_snapshot.size})) {
    cmeta_mutex_unlock(&runtime->mutex);
    status = runtime_result(error, ORM_STATUS_DRIVER_ALREADY_REGISTERED,
                            "driver ID is already registered");
    goto fail_plugin;
  }

  const uint32_t index = runtime->driver_count;
  orm_runtime_driver *driver = &runtime->drivers[index];
  memset(driver, 0, sizeof(*driver));
  driver->plugin = plugin;
  driver->binding = binding;
  driver->module_path = path;
  driver->canonical = canonical_snapshot;
  driver->capabilities = capabilities_snapshot;
  driver->execution_models = execution_models;
  driver->storage = storage_snapshot;
  memcpy(driver->bundle_id, runtime_bundle, sizeof(driver->bundle_id));
  ++runtime->driver_count;
  --runtime->pending_operations;
  runtime->load_active = 0u;
  cmeta_mutex_unlock(&runtime->mutex);
  return runtime_result(error, ORM_STATUS_OK, NULL);

fail_admission:
  if (cmeta_plugin_lease_valid(admission)) {
    const cmeta_plugin_status release_status =
        cmeta_plugin_registry_release(&runtime->plugins, &admission);
    if (release_status != CMETA_PLUGIN_OK)
      status = runtime_plugin_status(
          release_status, error, "release failed driver admission lease");
  }
fail_plugin:
  {
    const cmeta_plugin_status discard_status =
        runtime_discard_plugin(runtime, plugin, started);
    if (discard_status != CMETA_PLUGIN_OK)
      status = runtime_plugin_status(
          discard_status, error, "discard failed driver Plugin");
  }
fail_reserved:
  free(path);
  runtime_finish_pending(runtime, 1);
  return status;
}

static orm_status_t runtime_copy_extension_text(
    orm_string_view_t value, char *out, size_t capacity,
    const char *role, orm_error_t *error) {
  if (out == NULL || capacity == 0u || value.data == NULL ||
      value.len == 0u || value.len >= capacity ||
      memchr(value.data, '\0', value.len) != NULL)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT, role);
  memcpy(out, value.data, value.len);
  out[value.len] = '\0';
  return ORM_STATUS_OK;
}

orm_status_t ORM_C_CALL
orm_runtime_driver_acquire_extension(
    orm_runtime_t *runtime, orm_string_view_t id,
    orm_string_view_t export_id, orm_string_view_t contract_id,
    uint32_t contract_version, const cmeta_interface_desc *interface_desc,
    orm_runtime_driver_extension_t **out_extension, void **out_binding,
    orm_error_t *error) {
  enum { EXTENSION_TEXT_CAPACITY = 256 };
  char export_text[EXTENSION_TEXT_CAPACITY];
  char contract_text[EXTENSION_TEXT_CAPACITY];
  cmeta_plugin_ref plugin = {0};
  cmeta_plugin_lease lease = {0};
  const cmeta_plugin_manifest *manifest = NULL;
  const cmeta_plugin_export *entry = NULL;
  orm_runtime_driver_extension_t *extension = NULL;
  orm_status_t status;
  cmeta_plugin_status plugin_status;

  if (out_extension != NULL) *out_extension = NULL;
  if (out_binding != NULL) *out_binding = NULL;
  if (runtime == NULL || out_extension == NULL || out_binding == NULL ||
      interface_desc == NULL || contract_version == 0u ||
      !runtime_id_valid(id))
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid driver extension request");

  status = runtime_copy_extension_text(
      export_id, export_text, sizeof(export_text),
      "invalid driver extension export ID", error);
  if (status != ORM_STATUS_OK) return status;
  status = runtime_copy_extension_text(
      contract_id, contract_text, sizeof(contract_text),
      "invalid driver extension contract ID", error);
  if (status != ORM_STATUS_OK) return status;

  cmeta_mutex_lock(&runtime->mutex);
  if (runtime->closed != ORM_RUNTIME_OPEN) {
    cmeta_mutex_unlock(&runtime->mutex);
    return runtime_result(error, ORM_STATUS_INVALID_STATE,
                          "runtime is closed");
  }
  orm_runtime_driver *driver = runtime_find_driver(runtime, id);
  if (driver == NULL) {
    cmeta_mutex_unlock(&runtime->mutex);
    return runtime_result(error, ORM_STATUS_DRIVER_NOT_REGISTERED,
                          "driver is not registered");
  }
  if (runtime->extension_count >= runtime->config.max_pending_operations ||
      runtime->refs == UINT32_MAX) {
    cmeta_mutex_unlock(&runtime->mutex);
    return runtime_result(error, ORM_STATUS_LIMIT_EXCEEDED,
                          "runtime extension budget is full");
  }
  ++runtime->extension_count;
  ++runtime->dependents;
  ++runtime->refs;
  plugin = driver->plugin;
  cmeta_mutex_unlock(&runtime->mutex);

  plugin_status = cmeta_plugin_registry_acquire(
      &runtime->plugins, plugin, &lease, &manifest);
  if (plugin_status != CMETA_PLUGIN_OK) {
    status = runtime_plugin_status(
        plugin_status, error, "acquire Driver extension Plugin lease");
    runtime_drop_extension(runtime);
    return status;
  }

  plugin_status = cmeta_plugin_manifest_find_export(
      manifest, export_text, &entry);
  if (plugin_status != CMETA_PLUGIN_OK) {
    status = runtime_plugin_status(
        plugin_status, error, "find Driver extension export");
    goto fail;
  }

  plugin_status = cmeta_plugin_export_require_interface(
      entry, contract_text, contract_version, 0u, interface_desc);
  if (plugin_status != CMETA_PLUGIN_OK) {
    status = runtime_plugin_status(
        plugin_status, error, "admit Driver extension interface");
    goto fail;
  }
  if (entry->value.interface.value == NULL) {
    status = runtime_result(error, ORM_STATUS_ABI_MISMATCH,
                            "Driver extension binding is null");
    goto fail;
  }

  extension = (orm_runtime_driver_extension_t *)calloc(1u, sizeof(*extension));
  if (extension == NULL) {
    status = runtime_result(error, ORM_STATUS_OUT_OF_MEMORY,
                            "allocate Driver extension lease");
    goto fail;
  }
  extension->runtime = runtime;
  extension->plugin_lease = lease;
  *out_binding = entry->value.interface.value;
  *out_extension = extension;
  return runtime_result(error, ORM_STATUS_OK, NULL);

fail:
  {
    const cmeta_plugin_status release_status =
        cmeta_plugin_registry_release(&runtime->plugins, &lease);
    if (release_status != CMETA_PLUGIN_OK)
      status = runtime_plugin_status(
          release_status, error, "release failed Driver extension lease");
  }
  runtime_drop_extension(runtime);
  return status;
}

orm_status_t ORM_C_CALL
orm_runtime_driver_release_extension(
    orm_runtime_driver_extension_t *extension, orm_error_t *error) {
  if (extension == NULL || extension->runtime == NULL)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid Driver extension lease");

  orm_runtime_t *runtime = extension->runtime;
  const cmeta_plugin_status plugin_status =
      cmeta_plugin_registry_release(&runtime->plugins,
                                    &extension->plugin_lease);
  if (plugin_status != CMETA_PLUGIN_OK)
    return runtime_plugin_status(
        plugin_status, error, "release Driver extension Plugin lease");

  extension->runtime = NULL;
  free(extension);
  runtime_drop_extension(runtime);
  return runtime_result(error, ORM_STATUS_OK, NULL);
}

orm_status_t ORM_C_CALL
orm_runtime_driver_info(orm_runtime_t *runtime, orm_string_view_t id,
                        orm_driver_info_t *out_info, orm_error_t *error) {
  if (out_info != NULL)
    memset(out_info, 0, sizeof(*out_info));
  if (runtime == NULL || out_info == NULL || !runtime_id_valid(id))
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid driver info request");
  cmeta_mutex_lock(&runtime->mutex);
  if (runtime->closed != ORM_RUNTIME_OPEN) {
    cmeta_mutex_unlock(&runtime->mutex);
    return runtime_result(error, ORM_STATUS_INVALID_STATE,
                          "runtime is closed");
  }
  orm_runtime_driver *driver = runtime_find_driver(runtime, id);
  if (driver == NULL) {
    cmeta_mutex_unlock(&runtime->mutex);
    return runtime_result(error, ORM_STATUS_DRIVER_NOT_REGISTERED,
                          "driver is not registered");
  }

  out_info->struct_size = (uint32_t)sizeof(*out_info);
  out_info->abi_version = ORM_RUNTIME_ABI_VERSION;
  out_info->canonical_id_size = driver->canonical.size;
  memcpy(out_info->canonical_id, driver->canonical.text,
         driver->canonical.size);
  out_info->capabilities = driver->capabilities;
  out_info->execution_models = driver->execution_models;
  memcpy(out_info->bundle_id, driver->bundle_id,
         sizeof(out_info->bundle_id));
  cmeta_mutex_unlock(&runtime->mutex);
  return runtime_result(error, ORM_STATUS_OK, NULL);
}

orm_status_t ORM_C_CALL
orm_runtime_driver_storage_info(
    orm_runtime_t *runtime, orm_string_view_t id,
    orm_driver_storage_capabilities_v1 *out_storage, orm_error_t *error) {
  if (out_storage != NULL)
    memset(out_storage, 0, sizeof(*out_storage));
  if (runtime == NULL || out_storage == NULL || !runtime_id_valid(id))
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid driver storage info request");

  cmeta_mutex_lock(&runtime->mutex);
  if (runtime->closed != ORM_RUNTIME_OPEN) {
    cmeta_mutex_unlock(&runtime->mutex);
    return runtime_result(error, ORM_STATUS_INVALID_STATE,
                          "runtime is closed");
  }

  orm_runtime_driver *driver = runtime_find_driver(runtime, id);
  if (driver == NULL) {
    cmeta_mutex_unlock(&runtime->mutex);
    return runtime_result(error, ORM_STATUS_DRIVER_NOT_REGISTERED,
                          "driver is not registered");
  }

  *out_storage = driver->storage;
  cmeta_mutex_unlock(&runtime->mutex);
  return runtime_result(error, ORM_STATUS_OK, NULL);
}


#include "dbtool_plugin.h"
#include <cmeta_fs.h>

#include <string.h>

static const char dbtool_plugin_load_stage[] = "load-plugin";
static const char dbtool_plugin_validate_stage[] = "validate-plugin";
static const char dbtool_plugin_close_stage[] = "close-plugin";

static dbtool_status dbtool_plugin_failure(cmeta_plugin_status native,
                                          const char *stage,
                                          dbtool_error *error) {
  dbtool_status status = DBTOOL_STATUS_UNSUPPORTED;
  if (native == CMETA_PLUGIN_ALLOCATION_FAILED)
    status = DBTOOL_STATUS_OUT_OF_MEMORY;
  else if (native == CMETA_PLUGIN_INVALID_ARGUMENT)
    status = DBTOOL_STATUS_INVALID_ARGUMENT;
  else if (native == CMETA_PLUGIN_LOAD_FAILED)
    status = DBTOOL_STATUS_FILE_ERROR;
  else if (strcmp(stage, dbtool_plugin_close_stage) == 0)
    status = DBTOOL_STATUS_INTERNAL_ERROR;
  dbtool_error_set(error, status, stage, (int)native,
                   cmeta_plugin_status_string(native));
  return status;
}

dbtool_status dbtool_plugin_load(dbtool_plugin *plugin, const char *path,
                                const char *driver_id, dbtool_error *error) {
  const cmeta_plugin_registry_config config = {1u};
  const cmeta_plugin_manifest *manifest = NULL;
  const cmeta_plugin_export *entry = NULL;
  const TurboDb_SchemaApply *binding;
  cmeta_plugin_status status;
  dbtool_error_init(error);
  if (plugin == NULL || plugin->registry.impl != NULL || path == NULL ||
      path[0] == '\0' || strlen(path) > CMETA_PLUGIN_PATH_MAX ||
      !cmeta_fs_path_is_absolute(path) || driver_id == NULL || driver_id[0] == '\0') {
    dbtool_error_set(error, DBTOOL_STATUS_INVALID_ARGUMENT, dbtool_plugin_load_stage, 0,
                     "an absolute plugin path and driver ID are required");
    return DBTOOL_STATUS_INVALID_ARGUMENT;
  }
  status = cmeta_plugin_registry_init(&plugin->registry, &config);
  if (status != CMETA_PLUGIN_OK)
    return dbtool_plugin_failure(status, dbtool_plugin_load_stage, error);
  status = cmeta_plugin_registry_load(&plugin->registry, path, &plugin->ref);
  if (status != CMETA_PLUGIN_OK)
    return dbtool_plugin_failure(status, dbtool_plugin_load_stage, error);
  status = cmeta_plugin_registry_start(&plugin->registry, plugin->ref);
  if (status != CMETA_PLUGIN_OK)
    return dbtool_plugin_failure(status, "start-plugin", error);
  plugin->started = true;
  status = cmeta_plugin_registry_acquire(&plugin->registry, plugin->ref,
                                         &plugin->lease, &manifest);
  if (status != CMETA_PLUGIN_OK)
    return dbtool_plugin_failure(status, "acquire-plugin", error);
  if (strcmp(manifest->plugin_id, driver_id) != 0) {
    dbtool_error_set(error, DBTOOL_STATUS_UNSUPPORTED, dbtool_plugin_validate_stage, 0,
                     "plugin driver ID does not match the requested database");
    return DBTOOL_STATUS_UNSUPPORTED;
  }
  status = cmeta_plugin_manifest_find_export(manifest, DBTOOL_SCHEMA_EXPORT_ID, &entry);
  if (status == CMETA_PLUGIN_OK)
    status = cmeta_plugin_export_require_interface(entry, DBTOOL_SCHEMA_CONTRACT_ID,
        DBTOOL_SCHEMA_CONTRACT_VERSION, 0u, TurboDb_SchemaApply_interface());
  if (status != CMETA_PLUGIN_OK)
    return dbtool_plugin_failure(status, dbtool_plugin_validate_stage, error);
  binding = (const TurboDb_SchemaApply *)entry->value.interface.value;
  if (binding == NULL || binding->vtable == NULL || binding->vtable->operations == NULL) {
    dbtool_error_set(error, DBTOOL_STATUS_UNSUPPORTED, dbtool_plugin_validate_stage, 0,
                     "schema interface has no operations accessor");
    return DBTOOL_STATUS_UNSUPPORTED;
  }
  plugin->ops = binding->vtable->operations(binding->self);
  if (plugin->ops == NULL || plugin->ops->struct_size != sizeof(*plugin->ops) ||
      plugin->ops->abi_version != DBTOOL_SCHEMA_DRIVER_ABI_VERSION ||
      plugin->ops->open == NULL || plugin->ops->apply == NULL || plugin->ops->close == NULL) {
    plugin->ops = NULL;
    dbtool_error_set(error, DBTOOL_STATUS_UNSUPPORTED, dbtool_plugin_validate_stage, 0,
                     "invalid schema operations contract");
    return DBTOOL_STATUS_UNSUPPORTED;
  }
  return DBTOOL_STATUS_OK;
}

dbtool_status dbtool_plugin_close(dbtool_plugin *plugin, dbtool_error *error) {
  cmeta_plugin_status status;
  bool quiescent = false;
  if (plugin == NULL)
    return dbtool_plugin_failure(CMETA_PLUGIN_INVALID_ARGUMENT, dbtool_plugin_close_stage, error);
  if (plugin->registry.impl == NULL)
    return DBTOOL_STATUS_OK;
  if (cmeta_plugin_lease_valid(plugin->lease)) {
    status = cmeta_plugin_registry_release(&plugin->registry, &plugin->lease);
    if (status != CMETA_PLUGIN_OK)
      return dbtool_plugin_failure(status, dbtool_plugin_close_stage, error);
    plugin->ops = NULL;
  }
  if (plugin->started && !plugin->stopping) {
    status = cmeta_plugin_registry_request_stop(&plugin->registry, plugin->ref);
    if (status != CMETA_PLUGIN_OK && status != CMETA_PLUGIN_ALREADY)
      return dbtool_plugin_failure(status, dbtool_plugin_close_stage, error);
    plugin->stopping = true;
  }
  if (plugin->stopping) {
    status = cmeta_plugin_registry_poll_quiescent(&plugin->registry, plugin->ref, &quiescent);
    if (status != CMETA_PLUGIN_OK || !quiescent)
      return dbtool_plugin_failure(status != CMETA_PLUGIN_OK ? status : CMETA_PLUGIN_BUSY,
                                    dbtool_plugin_close_stage, error);
  }
  if (cmeta_plugin_ref_valid(plugin->ref)) {
    status = cmeta_plugin_registry_unload(&plugin->registry, plugin->ref);
    if (status != CMETA_PLUGIN_OK)
      return dbtool_plugin_failure(status, dbtool_plugin_close_stage, error);
    plugin->ref = (cmeta_plugin_ref){0};
    plugin->started = plugin->stopping = false;
  }
  status = cmeta_plugin_registry_destroy(&plugin->registry);
  if (status != CMETA_PLUGIN_OK)
    return dbtool_plugin_failure(status, dbtool_plugin_close_stage, error);
  *plugin = (dbtool_plugin){0};
  return DBTOOL_STATUS_OK;
}

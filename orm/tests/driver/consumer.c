#include <orm_driver_plugin.h>

#include <string.h>

#define REQUIRE(condition) do { if (!(condition)) return __LINE__; } while (0)

static const orm_driver_storage_capabilities_v1 consumer_storage =
    ORM_DRIVER_STORAGE_CAPABILITIES_NONE_INIT;

static const orm_driver_storage_capabilities_v1 *ORM_DRIVER_CALL
consumer_storage_capabilities(void *self) {
  return self != NULL ? &consumer_storage : NULL;
}

static uint64_t ORM_DRIVER_CALL consumer_execution_models(void *self) {
  return self != NULL ? ORM_DRIVER_EXEC_CALLER_BLOCKING : UINT64_C(0);
}

static orm_status_t ORM_DRIVER_CALL consumer_create(
    void *self, const orm_config_t *config,
    const orm_driver_limits_v1 *limits,
    orm_driver_connection_v1 *out_connection, orm_error_t *error) {
  (void)self;
  if (config == NULL || limits == NULL ||
      out_connection == NULL || error == NULL)
    return ORM_STATUS_INVALID_ARGUMENT;
  memset(out_connection, 0, sizeof(*out_connection));
  memset(error, 0, sizeof(*error));
  error->struct_size = (uint32_t)sizeof(*error);
  error->status = ORM_STATUS_OK;
  return ORM_STATUS_OK;
}

int main(void) {
  static unsigned consumer_state;
  static const TurboDb_Driver_vtable vtable = {
      .implementation = "sdk-consumer",
      .capabilities = ORM_DRIVER_CAP_SELECT,
      .create = consumer_create,
      .execution_models = consumer_execution_models,
      .storage_capabilities = consumer_storage_capabilities};
  TurboDb_Driver driver = TurboDb_Driver_bind(&consumer_state, &vtable);
  salts_plugin_export entry =
      orm_driver_plugin_export(&driver, ORM_DRIVER_CAP_SELECT);
  salts_plugin_manifest manifest = {
      .struct_size = SALTS_PLUGIN_MANIFEST_SIZE,
      .abi_version = SALTS_PLUGIN_ABI_VERSION,
      .plugin_id = "sdk-consumer",
      .version = {1u, 0u, 0u},
      .exports = &entry,
      .export_count = 1u};

  REQUIRE(TurboDb_Driver_valid(&driver));
  REQUIRE(salts_plugin_manifest_validate(&manifest) == SALTS_PLUGIN_OK);
  REQUIRE(salts_plugin_export_require_interface(
              &entry, ORM_DRIVER_INTERFACE_CONTRACT_ID,
              ORM_DRIVER_INTERFACE_CONTRACT_VERSION,
              ORM_DRIVER_CAP_SELECT,
              TurboDb_Driver_interface()) == SALTS_PLUGIN_OK);
  return 0;
}

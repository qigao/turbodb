#include <orm_driver_plugin.h>

#define ORM_DRIVER_WRONG_METHODS(X, I)                                      \
  X(I, F0, orm_status_t, create, io,                                       \
    &orm_driver_status_cmeta_type, CMETA_ABI_SCALAR)

CMETA_INTERFACE(TurboDb_Driver_BadShape, ORM_DRIVER_WRONG_METHODS);

static orm_status_t ORM_DRIVER_CALL bad_shape_create(void *self) {
  (void)self;
  return ORM_STATUS_UNSUPPORTED;
}

static const TurboDb_Driver_BadShape_vtable bad_shape_vtable = {
    .implementation = "bad-cmeta-shape",
    .capabilities = 0u,
    .create = bad_shape_create};

static TurboDb_Driver_BadShape bad_shape_driver = {
    NULL, &bad_shape_vtable};

static const salts_plugin_export bad_shape_exports[] = {{
    .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
    .kind = SALTS_PLUGIN_EXPORT_INTERFACE,
    .contract_version = ORM_DRIVER_INTERFACE_CONTRACT_VERSION,
    .capabilities = 0u,
    .export_id = ORM_DRIVER_PLUGIN_EXPORT_ID,
    .contract_id = ORM_DRIVER_INTERFACE_CONTRACT_ID,
    .value.interface = {
        &TurboDb_Driver_BadShape_interface_meta, &bad_shape_driver}}};

static const salts_plugin_manifest bad_shape_manifest = {
    .struct_size = SALTS_PLUGIN_MANIFEST_SIZE,
    .abi_version = SALTS_PLUGIN_ABI_VERSION,
    .plugin_id = "badshape",
    .version = {1u, 0u, 0u},
    .exports = bad_shape_exports,
    .export_count = 1u};

SALTS_PLUGIN_QUERY_EXPORT const salts_plugin_manifest *SALTS_PLUGIN_CALL
salts_plugin_query(uint32_t host_abi) {
  return host_abi == SALTS_PLUGIN_ABI_VERSION ? &bad_shape_manifest : NULL;
}

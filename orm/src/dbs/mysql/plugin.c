#include <orm_driver_plugin.h>

#include "backend.h"
#include "orm_driver_backend_bridge.h"

#include <mysql.h>

#define ORM_MYSQL_DRIVER_CAPABILITIES                                      \
  (ORM_DRIVER_CAP_SELECT | ORM_DRIVER_CAP_INSERT |                         \
   ORM_DRIVER_CAP_UPDATE | ORM_DRIVER_CAP_DELETE |                         \
   ORM_DRIVER_CAP_RAW_SQL | ORM_DRIVER_CAP_TRANSACTION |                   \
   ORM_DRIVER_CAP_SAVEPOINT | ORM_DRIVER_CAP_INCREMENTAL_ROWS |            \
   ORM_DRIVER_CAP_READ_UNCOMMITTED | ORM_DRIVER_CAP_READ_COMMITTED |       \
   ORM_DRIVER_CAP_REPEATABLE_READ | ORM_DRIVER_CAP_SERIALIZABLE)

static int mysql_driver_identity;

typedef struct mysql_plugin_lifecycle {
  int started;
  int stopping;
} mysql_plugin_lifecycle;

static mysql_plugin_lifecycle mysql_lifecycle;

static salts_plugin_status SALTS_PLUGIN_CALL mysql_plugin_start(void *self) {
  mysql_plugin_lifecycle *state = (mysql_plugin_lifecycle *)self;
  if (state != &mysql_lifecycle)
    return SALTS_PLUGIN_INVALID_ARGUMENT;
  if (mysql_library_init(0, NULL, NULL) != 0)
    return SALTS_PLUGIN_INVALID_STATE;
  state->started = 1;
  state->stopping = 0;
  return SALTS_PLUGIN_OK;
}

static salts_plugin_status SALTS_PLUGIN_CALL
mysql_plugin_request_stop(void *self) {
  mysql_plugin_lifecycle *state = (mysql_plugin_lifecycle *)self;
  if (state != &mysql_lifecycle)
    return SALTS_PLUGIN_INVALID_ARGUMENT;
  state->stopping = 1;
  return SALTS_PLUGIN_OK;
}

static bool SALTS_PLUGIN_CALL mysql_plugin_is_quiescent(const void *self) {
  const mysql_plugin_lifecycle *state =
      (const mysql_plugin_lifecycle *)self;
  return state == &mysql_lifecycle && state->stopping != 0;
}

static void SALTS_PLUGIN_CALL mysql_plugin_destroy(void *self) {
  mysql_plugin_lifecycle *state = (mysql_plugin_lifecycle *)self;
  if (state != &mysql_lifecycle || !state->started)
    return;
  mysql_library_end();
  state->started = 0;
  state->stopping = 0;
}

static uint64_t ORM_DRIVER_CALL mysql_driver_execution_models(void *self) {
  return self == &mysql_driver_identity
             ? (uint64_t)ORM_DRIVER_EXEC_CALLER_BLOCKING
             : UINT64_C(0);
}

static orm_status_t ORM_DRIVER_CALL mysql_driver_create(
    void *self, const orm_config_t *config,
    const orm_driver_limits_v1 *limits,
    orm_driver_connection_v1 *out_connection,
    orm_error_t *error) {
  if (self != &mysql_driver_identity)
    return ORM_STATUS_INVALID_ARGUMENT;
  return orm_driver_backend_connection_create(
      orm_mysql_backend_create, config, limits, out_connection, error);
}

static const TurboDb_Driver_vtable mysql_driver_vtable = {
    .implementation = "mysql",
    .capabilities = ORM_MYSQL_DRIVER_CAPABILITIES,
    .create = mysql_driver_create,
    .execution_models = mysql_driver_execution_models};

static TurboDb_Driver mysql_driver = {
    &mysql_driver_identity, &mysql_driver_vtable};

static const salts_plugin_export mysql_exports[] = {{
    .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
    .kind = SALTS_PLUGIN_EXPORT_INTERFACE,
    .contract_version = ORM_DRIVER_INTERFACE_CONTRACT_VERSION,
    .capabilities = ORM_MYSQL_DRIVER_CAPABILITIES,
    .export_id = ORM_DRIVER_PLUGIN_EXPORT_ID,
    .contract_id = ORM_DRIVER_INTERFACE_CONTRACT_ID,
    .value.interface = {&TurboDb_Driver_interface_meta, &mysql_driver}}};

static const salts_plugin_manifest mysql_manifest = {
    .struct_size = SALTS_PLUGIN_MANIFEST_SIZE,
    .abi_version = SALTS_PLUGIN_ABI_VERSION,
    .plugin_id = "mysql",
    .version = {1u, 0u, 0u},
    .exports = mysql_exports,
    .export_count = 1u,
    .self = &mysql_lifecycle,
    .start = mysql_plugin_start,
    .request_stop = mysql_plugin_request_stop,
    .is_quiescent = mysql_plugin_is_quiescent,
    .destroy = mysql_plugin_destroy};

SALTS_PLUGIN_QUERY_EXPORT const salts_plugin_manifest *SALTS_PLUGIN_CALL
salts_plugin_query(uint32_t host_abi) {
  return host_abi == SALTS_PLUGIN_ABI_VERSION ? &mysql_manifest : NULL;
}

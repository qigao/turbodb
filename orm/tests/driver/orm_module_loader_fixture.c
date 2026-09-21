#include <orm_driver_abi.h>

#include <string.h>

#define FIXTURE_HEADER(T) {(uint32_t)sizeof(T), ORM_DRIVER_ABI_VERSION}
#define FIXTURE_TABLE(p) {(p), (uint32_t)sizeof(*(p)), 0u}

typedef struct fixture_module_context {
  uint32_t live_connections;
} fixture_module_context;
typedef struct fixture_connection_context {
  fixture_module_context *module;
  int live;
} fixture_connection_context;

static fixture_module_context fixture_context;
static fixture_connection_context fixture_connections[4];
static const uint8_t fixture_bundle[ORM_DRIVER_BUNDLE_ID_BYTES] =
    ORM_DRIVER_BUNDLE_ID_INIT;
static const char fixture_id[] = "fixture";
static const char fixture_alias[] = "fixture-alias";

static orm_status_t ORM_DRIVER_CALL fixture_initialize(
    const orm_driver_host_v1 *host, void **out, orm_error_t *error) {
  if (out != NULL) *out = NULL;
  if (host == NULL || out == NULL ||
      host->header.abi_version != ORM_DRIVER_ABI_VERSION ||
      host->header.struct_size < sizeof(*host) ||
      memcmp(host->bundle_id, fixture_bundle, sizeof(fixture_bundle)) != 0) {
    if (error != NULL) {
      memset(error, 0, sizeof(*error));
      error->struct_size = (uint32_t)sizeof(*error);
      error->status = ORM_STATUS_ABI_MISMATCH;
    }
    return ORM_STATUS_ABI_MISMATCH;
  }
  *out = &fixture_context;
  if (error != NULL) {
    memset(error, 0, sizeof(*error));
    error->struct_size = (uint32_t)sizeof(*error);
    error->status = ORM_STATUS_OK;
  }
  return ORM_STATUS_OK;
}

static orm_status_t ORM_DRIVER_CALL fixture_finalize(
    void *context, orm_error_t *error) {
  orm_status_t status = ORM_STATUS_INVALID_ARGUMENT;
  if (context == &fixture_context)
    status = fixture_context.live_connections == 0u
                 ? ORM_STATUS_OK
                 : ORM_STATUS_BUSY;
  if (error != NULL) {
    memset(error, 0, sizeof(*error));
    error->struct_size = (uint32_t)sizeof(*error);
    error->status = status;
  }
  return status;
}

static void ORM_DRIVER_CALL fixture_destroy_connection(void *context) {
  fixture_connection_context *connection = context;
  if (connection == NULL || !connection->live || connection->module == NULL)
    return;
  --connection->module->live_connections;
  connection->module = NULL;
  connection->live = 0;
}

static orm_status_t ORM_DRIVER_CALL fixture_create_connection(
    void *context, const orm_config_t *config,
    const orm_driver_limits_v1 *limits, orm_driver_connection_v1 *out,
    orm_error_t *error) {
  if (out != NULL) memset(out, 0, sizeof(*out));
  orm_status_t status = ORM_STATUS_INVALID_ARGUMENT;
  if (context == &fixture_context && config != NULL && limits != NULL &&
      out != NULL && config->driver.data != NULL &&
      (config->driver.len == sizeof(fixture_id) - 1u ||
       config->driver.len == sizeof(fixture_alias) - 1u)) {
    for (size_t i = 0u;
         i < sizeof(fixture_connections) / sizeof(fixture_connections[0]);
         ++i) {
      if (!fixture_connections[i].live) {
        fixture_connections[i].module = &fixture_context;
        fixture_connections[i].live = 1;
        ++fixture_context.live_connections;
        out->header =
            (orm_driver_header_v1)FIXTURE_HEADER(orm_driver_connection_v1);
        out->context = &fixture_connections[i];
        out->ops = (orm_driver_table_v1)FIXTURE_TABLE(&fixture_connection_ops);
        status = ORM_STATUS_OK;
        break;
      }
    }
    if (status != ORM_STATUS_OK)
      status = ORM_STATUS_LIMIT_EXCEEDED;
  }
  if (error != NULL) {
    memset(error, 0, sizeof(*error));
    error->struct_size = (uint32_t)sizeof(*error);
    error->status = status;
  }
  return status;
}

static const orm_driver_module_ops_v1 fixture_module_ops = {
    FIXTURE_HEADER(orm_driver_module_ops_v1),
    fixture_initialize, fixture_finalize};
static const orm_driver_connection_ops_v1 fixture_connection_ops = {
    FIXTURE_HEADER(orm_driver_connection_ops_v1),
    fixture_destroy_connection, NULL, NULL, NULL};
static const orm_driver_bytes_v1 fixture_aliases[] = {
    {fixture_alias, sizeof(fixture_alias) - 1u}};
static const orm_driver_api_v1 fixture_api = {
    FIXTURE_HEADER(orm_driver_api_v1),
    ORM_DRIVER_BUNDLE_ID_INIT,
    {fixture_id, sizeof(fixture_id) - 1u},
    fixture_aliases,
    1u,
    0u,
    0u,
    ORM_DRIVER_EXEC_CALLER_BLOCKING,
    FIXTURE_TABLE(&fixture_module_ops),
    fixture_create_connection,
    FIXTURE_TABLE(&fixture_connection_ops)};

ORM_DRIVER_EXPORT int32_t ORM_DRIVER_CALL orm_driver_get_api_v1(
    const orm_driver_host_v1 *host, uint32_t host_bytes,
    const orm_driver_api_v1 **out_api, uint32_t *out_api_bytes) {
  if (out_api != NULL) *out_api = NULL;
  if (out_api_bytes != NULL) *out_api_bytes = 0u;
  if (host == NULL || host_bytes < sizeof(*host) ||
      out_api == NULL || out_api_bytes == NULL)
    return ORM_STATUS_INVALID_ARGUMENT;
  *out_api = &fixture_api;
  *out_api_bytes = (uint32_t)sizeof(fixture_api);
  return ORM_STATUS_OK;
}

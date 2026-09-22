#include <orm_driver_abi.h>

#include <string.h>

static const char bad_id[] = "badabi";

static const orm_driver_api_v1 bad_api = {
    {(uint32_t)sizeof(orm_driver_api_v1), ORM_DRIVER_ABI_VERSION + 1u},
    ORM_DRIVER_BUNDLE_ID_INIT,
    {bad_id, sizeof(bad_id) - 1u},
    NULL,
    0u,
    0u,
    0u,
    ORM_DRIVER_EXEC_CALLER_BLOCKING,
    {NULL, 0u, 0u},
    NULL,
    {NULL, 0u, 0u}};

ORM_DRIVER_EXPORT int32_t ORM_DRIVER_CALL orm_driver_get_api_v1(
    const orm_driver_host_v1 *host, uint32_t host_bytes,
    const orm_driver_api_v1 **out_api, uint32_t *out_api_bytes) {
  (void)host;
  (void)host_bytes;
  if (out_api != NULL) *out_api = NULL;
  if (out_api_bytes != NULL) *out_api_bytes = 0u;
  if (out_api == NULL || out_api_bytes == NULL)
    return ORM_STATUS_INVALID_ARGUMENT;
  *out_api = &bad_api;
  *out_api_bytes = (uint32_t)sizeof(bad_api);
  return ORM_STATUS_OK;
}

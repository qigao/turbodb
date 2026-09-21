#include <orm_driver_abi.h>

ORM_DRIVER_EXPORT int32_t ORM_DRIVER_CALL orm_driver_get_api_v1(
    const orm_driver_host_v1 *host, uint32_t host_bytes,
    const orm_driver_api_v1 **out_api, uint32_t *out_api_bytes) {
  (void)host;
  (void)host_bytes;
  if (out_api != NULL) *out_api = NULL;
  if (out_api_bytes != NULL) *out_api_bytes = 0u;
  return ORM_STATUS_INVALID_ARGUMENT;
}

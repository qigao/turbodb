#include <orm_driver_abi.h>
#include <stddef.h>

/* No fixture or private core headers: the C linker must resolve the real entry. */
typedef int32_t (ORM_DRIVER_CALL *entry_fn)(const orm_driver_host_v1 *,
    uint32_t, const orm_driver_api_v1 **, uint32_t *);
_Static_assert(sizeof(orm_driver_header_v1) == 8u, "bootstrap prefix size");
_Static_assert(offsetof(orm_driver_header_v1, abi_version) == 4u,
               "bootstrap version offset");
_Static_assert(_Generic(&orm_driver_get_api_v1, entry_fn: 1, default: 0),
               "bootstrap calling convention changed");

int main(void) {
  entry_fn volatile entry = &orm_driver_get_api_v1;
  const orm_driver_api_v1 sentinel = {0};
  const orm_driver_api_v1 *api = &sentinel;
  uint32_t bytes = UINT32_MAX;
  if (entry(NULL, 0u, &api, &bytes) != ORM_STATUS_INVALID_ARGUMENT ||
      api != NULL || bytes != 0u) return 1;
  bytes = UINT32_MAX;
  if (entry(NULL, 0u, NULL, &bytes) != ORM_STATUS_INVALID_ARGUMENT ||
      bytes != 0u) return 2;
  api = &sentinel;
  if (entry(NULL, 0u, &api, NULL) != ORM_STATUS_INVALID_ARGUMENT ||
      api != NULL) return 3;
  return 0;
}

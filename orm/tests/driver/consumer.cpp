#include <orm_driver_abi.h>
#include <cstddef>
#include <type_traits>

using entry_fn = int32_t (ORM_DRIVER_CALL *)(const orm_driver_host_v1 *,
    uint32_t, const orm_driver_api_v1 **, uint32_t *);
static_assert(std::is_same<decltype(&orm_driver_get_api_v1), entry_fn>::value,
              "bootstrap calling convention changed");
static_assert(sizeof(orm_driver_header_v1) == 8u, "bootstrap prefix size");
static_assert(offsetof(orm_driver_header_v1, abi_version) == 4u,
              "bootstrap version offset");
static_assert(std::is_standard_layout<orm_driver_api_v1>::value,
              "driver descriptor must have C layout");
static_assert(std::is_standard_layout<orm_driver_host_v1>::value,
              "host descriptor must have C layout");
static_assert(std::is_trivially_copyable<orm_driver_api_v1>::value,
              "driver descriptor must remain a POD view");

int main() {
  /* Link a C-compiled fixture, not the MODULE or a C++ reimplementation. */
  entry_fn volatile entry = &orm_driver_get_api_v1;
  const orm_driver_api_v1 sentinel{};
  const orm_driver_api_v1 *api = &sentinel;
  uint32_t bytes = UINT32_MAX;
  if (entry(nullptr, 0u, &api, &bytes) != ORM_STATUS_INVALID_ARGUMENT ||
      api != nullptr || bytes != 0u) return 1;
  bytes = UINT32_MAX;
  if (entry(nullptr, 0u, nullptr, &bytes) != ORM_STATUS_INVALID_ARGUMENT ||
      bytes != 0u) return 2;
  api = &sentinel;
  if (entry(nullptr, 0u, &api, nullptr) != ORM_STATUS_INVALID_ARGUMENT ||
      api != nullptr) return 3;
  return 0;
}

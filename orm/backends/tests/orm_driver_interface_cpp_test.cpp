#include <orm_driver_plugin.h>

#include <cstdint>
#include <type_traits>

using driver_create_type = orm_status_t (ORM_DRIVER_CALL *)(
    void *, const orm_config_t *, const orm_driver_limits_v1 *,
    orm_driver_connection_v1 *, orm_error_t *);
using driver_execution_models_type =
    uint64_t (ORM_DRIVER_CALL *)(void *);
using driver_storage_capabilities_type =
    const orm_driver_storage_capabilities_v1 *(ORM_DRIVER_CALL *)(void *);

static_assert(std::is_standard_layout<TurboDb_Driver>::value,
              "Driver interface must keep C layout");
static_assert(std::is_standard_layout<TurboDb_Driver_vtable>::value,
              "Driver vtable must keep C layout");
static_assert(
    std::is_same<decltype(static_cast<TurboDb_Driver_vtable *>(nullptr)->create),
                 driver_create_type>::value,
    "reflected create dispatch must preserve the existing Driver C ABI");
static_assert(
    std::is_same<
        decltype(static_cast<TurboDb_Driver_vtable *>(nullptr)->execution_models),
        driver_execution_models_type>::value,
    "reflected execution-model dispatch must preserve the Driver C ABI");
static_assert(
    std::is_same<
        decltype(static_cast<TurboDb_Driver_vtable *>(nullptr)->storage_capabilities),
        driver_storage_capabilities_type>::value,
    "reflected storage-capability dispatch must preserve the Driver C ABI");
static_assert(ORM_DRIVER_INTERFACE_CONTRACT_VERSION == 4u,
              "Driver contract version drift");

int main() {
  const cmeta_interface_desc *desc = TurboDb_Driver_interface();
  if (desc == nullptr || !cmeta_interface_desc_valid(desc) ||
      desc->method_count != 3u)
    return 1;
  if (desc->methods[0].function == nullptr ||
      desc->methods[0].abi == nullptr ||
      desc->methods[1].function == nullptr ||
      desc->methods[1].abi == nullptr ||
      desc->methods[2].function == nullptr ||
      desc->methods[2].abi == nullptr)
    return 2;
  return 0;
}

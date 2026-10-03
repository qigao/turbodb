#include <orm_driver_plugin.h>

#include <type_traits>

using driver_create_type = orm_status_t (ORM_DRIVER_CALL *)(
    void *, const orm_config_t *, const orm_driver_limits_v1 *,
    orm_driver_connection_v1 *, orm_error_t *);

static_assert(ORM_DRIVER_INTERFACE_CONTRACT_VERSION == 3u,
              "TurboDb.Driver contract version drift");
static_assert(std::is_standard_layout<TurboDb_Driver>::value,
              "Driver interface must keep C layout");
static_assert(std::is_standard_layout<TurboDb_Driver_vtable>::value,
              "Driver vtable must keep C layout");
static_assert(
    std::is_same<decltype(static_cast<TurboDb_Driver_vtable *>(nullptr)->create),
                 driver_create_type>::value,
    "reflected create dispatch must preserve the typed Driver ABI");

int main() {
  const cmeta_interface_desc *desc = TurboDb_Driver_interface();
  return desc != nullptr && cmeta_interface_desc_valid(desc) &&
                 desc->method_count == 3u
             ? 0
             : 1;
}

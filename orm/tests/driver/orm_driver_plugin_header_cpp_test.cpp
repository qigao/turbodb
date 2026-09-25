#include <orm_driver_plugin.h>

#include <type_traits>

static_assert(ORM_DRIVER_PLUGIN_CONTRACT_VERSION == 1u,
              "TurboDb.Driver contract version changed unexpectedly");
static_assert(std::is_standard_layout<turbodb_driver>::value,
              "reflected Driver handle must be standard-layout");
static_assert(std::is_trivially_copyable<turbodb_driver>::value,
              "reflected Driver handle must remain trivially copyable");
static_assert(std::is_standard_layout<orm_driver_plugin_binding>::value,
              "cached Plugin binding must be standard-layout");
static_assert(std::is_trivially_copyable<orm_driver_plugin_binding>::value,
              "cached Plugin binding must remain trivially copyable");

int main() {
  const cmeta_interface_desc *desc = turbodb_driver_interface();
  const cmeta_function_desc *connect = turbodb_driver_connect_function();
  return desc != nullptr && desc->method_count == 2u &&
                 connect != nullptr && connect->param_count == 4u
             ? 0
             : 1;
}

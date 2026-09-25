#include <orm_driver_plugin.h>

const cmeta_interface_desc *orm_driver_plugin_peer_interface(void) {
  return turbodb_driver_interface();
}

const cmeta_function_desc *orm_driver_plugin_peer_connect_function(void) {
  return turbodb_driver_connect_function();
}

const cmeta_function_abi_desc *orm_driver_plugin_peer_connect_abi(void) {
  return turbodb_driver_connect_function_abi();
}

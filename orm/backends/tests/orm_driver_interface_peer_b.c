#include <orm_driver_interface.h>

const cmeta_interface_desc *orm_driver_interface_peer_b(void) {
  return TurboDb_Driver_interface();
}

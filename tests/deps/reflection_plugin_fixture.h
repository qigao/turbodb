#ifndef TURBODB_REFLECTION_PLUGIN_FIXTURE_H
#define TURBODB_REFLECTION_PLUGIN_FIXTURE_H

#include <cmeta/interface.h>

#define TURBODB_DEPENDENCY_METHODS(X, I) \
  X(I, F0, int, value, value, \
    &cmeta_type_int, CMETA_ABI_SCALAR)

CMETA_INTERFACE(turbodb_dependency_interface, TURBODB_DEPENDENCY_METHODS);

const cmeta_interface_desc *turbodb_dependency_interface_peer_a(void);
const cmeta_interface_desc *turbodb_dependency_interface_peer_b(void);

#endif

#include "reflection_plugin_fixture.h"

#include <salts/plugin.h>

#include <assert.h>
#include <stdint.h>

#define TURBODB_DEPENDENCY_WRONG_METHODS(X, I) \
  X(I, F0, int, different_value, value, \
    &cmeta_type_int, CMETA_ABI_SCALAR)

CMETA_INTERFACE(turbodb_dependency_wrong_interface,
                TURBODB_DEPENDENCY_WRONG_METHODS);

_Static_assert(SALTS_PLUGIN_ABI_VERSION == 2u,
               "TurboDB requires SaltsUtils Plugin ABI 2");

static int dependency_value(void *self) {
  return self != NULL ? *(const int *)self : 0;
}

int main(void) {
  static int state = 17;
  static const turbodb_dependency_interface_vtable vtable = {
      .implementation = "dependency-smoke",
      .capabilities = UINT64_C(1),
      .value = dependency_value};
  static turbodb_dependency_interface binding = {&state, &vtable};

  const cmeta_interface_desc *peer_a =
      turbodb_dependency_interface_peer_a();
  const cmeta_interface_desc *peer_b =
      turbodb_dependency_interface_peer_b();
  const cmeta_interface_desc *wrong =
      turbodb_dependency_wrong_interface_interface();

  assert(peer_a != NULL);
  assert(peer_b != NULL);
  assert(peer_a != peer_b);
  assert(cmeta_interface_desc_valid(peer_a));
  assert(cmeta_interface_desc_valid(peer_b));
  assert(cmeta_interface_desc_equal(peer_a, peer_b));
  assert(peer_a->method_count == 1u);
  assert(peer_a->methods[0].function != NULL);
  assert(peer_a->methods[0].abi != NULL);
  assert(!cmeta_interface_desc_equal(peer_a, wrong));
  assert(turbodb_dependency_interface_value(&binding) == 17);

  salts_plugin_export entry = {
      .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
      .kind = SALTS_PLUGIN_EXPORT_INTERFACE,
      .contract_version = 1u,
      .capabilities = UINT64_C(1),
      .export_id = "dependency-smoke",
      .contract_id = "TurboDb.DependencySmoke",
      .value.interface = {peer_a, &binding}};

  assert(salts_plugin_export_require_interface(
             &entry, "TurboDb.DependencySmoke", 1u, UINT64_C(1), peer_b) ==
         SALTS_PLUGIN_OK);
  assert(salts_plugin_export_require_interface(
             &entry, "TurboDb.DependencySmoke", 1u, UINT64_C(1), wrong) ==
         SALTS_PLUGIN_INCOMPATIBLE_CONTRACT);

  salts_plugin_manifest manifest = {
      .struct_size = SALTS_PLUGIN_MANIFEST_SIZE,
      .abi_version = SALTS_PLUGIN_ABI_VERSION,
      .plugin_id = "turbodb-dependency-smoke",
      .version = {1u, 0u, 0u},
      .exports = &entry,
      .export_count = 1u};

  assert(salts_plugin_manifest_validate(&manifest) == SALTS_PLUGIN_OK);
  manifest.abi_version = SALTS_PLUGIN_ABI_VERSION + 1u;
  assert(salts_plugin_manifest_validate(&manifest) ==
         SALTS_PLUGIN_UNSUPPORTED_ABI);

  return 0;
}

#include <orm_driver_plugin.h>

#include <stdint.h>
#include <string.h>

#define REQUIRE(condition) do { if (!(condition)) return __LINE__; } while (0)

const cmeta_interface_desc *orm_driver_interface_peer_a(void);
const cmeta_interface_desc *orm_driver_interface_peer_b(void);

#define ORM_DRIVER_WRONG_METHODS(X, I)                                      \
  X(I, F0, orm_status_t, create, io,                                       \
    &orm_driver_status_cmeta_type, CMETA_ABI_SCALAR)

CMETA_INTERFACE(TurboDb_Driver_Wrong, ORM_DRIVER_WRONG_METHODS);

static unsigned create_calls;

static orm_status_t ORM_DRIVER_CALL fixture_create(
    void *self, const orm_config_t *config,
    const orm_driver_limits_v1 *limits,
    orm_driver_connection_v1 *out_connection, orm_error_t *error) {
  unsigned *calls = (unsigned *)self;
  if (calls == NULL || config == NULL || limits == NULL ||
      out_connection == NULL || error == NULL)
    return ORM_STATUS_INVALID_ARGUMENT;
  ++*calls;
  out_connection->context = self;
  error->status = ORM_STATUS_OK;
  return ORM_STATUS_OK;
}

int main(void) {
  const cmeta_interface_desc *local = TurboDb_Driver_interface();
  const cmeta_interface_desc *peer_a = orm_driver_interface_peer_a();
  const cmeta_interface_desc *peer_b = orm_driver_interface_peer_b();
  const cmeta_interface_method_desc *method;
  const cmeta_function_desc *function;
  const cmeta_function_abi_desc *abi;

  REQUIRE(local != NULL && peer_a != NULL && peer_b != NULL);
  REQUIRE(local != peer_a && peer_a != peer_b);
  REQUIRE(cmeta_interface_desc_valid(local));
  REQUIRE(cmeta_interface_desc_equal(local, peer_a));
  REQUIRE(cmeta_interface_desc_equal(peer_a, peer_b));
  REQUIRE(strcmp(local->name, "TurboDb_Driver") == 0);
  REQUIRE(local->method_count == 1u);

  method = &local->methods[0];
  REQUIRE(cmeta_interface_method_reflection_valid(method));
  REQUIRE(strcmp(method->name, "create") == 0);
  REQUIRE(method->dispatch_arity == 4u);

  function = method->function;
  abi = method->abi;
  REQUIRE(function != NULL && abi != NULL);
  REQUIRE(strcmp(function->name, "TurboDb_Driver.create") == 0);
  REQUIRE(function->param_count == 4u);
  REQUIRE(function->effects == (CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL));
  REQUIRE(function->params[0].flags ==
         (CMETA_PARAM_IN | CMETA_PARAM_BORROWED));
  REQUIRE(function->params[1].flags ==
         (CMETA_PARAM_IN | CMETA_PARAM_BORROWED));
  REQUIRE(function->params[2].flags ==
         (CMETA_PARAM_OUT | CMETA_PARAM_BORROWED));
  REQUIRE(function->params[3].flags ==
         (CMETA_PARAM_OUT | CMETA_PARAM_BORROWED | CMETA_PARAM_NULLABLE));
  REQUIRE(abi->return_carrier == CMETA_ABI_SCALAR);
  REQUIRE(abi->param_count == 4u);
  REQUIRE(abi->param_carriers[0] == CMETA_ABI_OBJECT_POINTER);
  REQUIRE(abi->param_carriers[1] == CMETA_ABI_OBJECT_POINTER);
  REQUIRE(abi->param_carriers[2] == CMETA_ABI_OBJECT_POINTER);
  REQUIRE(abi->param_carriers[3] == CMETA_ABI_OBJECT_POINTER);

  static const TurboDb_Driver_vtable vtable = {
      .implementation = "fixture",
      .capabilities = ORM_DRIVER_CAP_SELECT | ORM_DRIVER_CAP_TRANSACTION,
      .create = fixture_create};
  TurboDb_Driver driver = TurboDb_Driver_bind(&create_calls, &vtable);
  orm_config_t config = {0};
  orm_driver_limits_v1 limits = {0};
  orm_driver_connection_v1 connection = {0};
  orm_error_t error = {0};

  REQUIRE(TurboDb_Driver_valid(&driver));
  REQUIRE(TurboDb_Driver_create(
             &driver, &config, &limits, &connection, &error) == ORM_STATUS_OK);
  REQUIRE(create_calls == 1u);
  REQUIRE(connection.context == &create_calls);

  salts_plugin_export entry = orm_driver_plugin_export(
      &driver, ORM_DRIVER_CAP_SELECT | ORM_DRIVER_CAP_TRANSACTION);
  REQUIRE(entry.struct_size == SALTS_PLUGIN_EXPORT_SIZE);
  REQUIRE(entry.kind == SALTS_PLUGIN_EXPORT_INTERFACE);
  REQUIRE(strcmp(entry.export_id, ORM_DRIVER_PLUGIN_EXPORT_ID) == 0);
  REQUIRE(strcmp(entry.contract_id, ORM_DRIVER_INTERFACE_CONTRACT_ID) == 0);
  REQUIRE(entry.contract_version == ORM_DRIVER_INTERFACE_CONTRACT_VERSION);
  REQUIRE(entry.value.interface.value == &driver);
  REQUIRE(entry.value.interface.desc == local);

  REQUIRE(salts_plugin_export_require_interface(
             &entry, ORM_DRIVER_INTERFACE_CONTRACT_ID,
             ORM_DRIVER_INTERFACE_CONTRACT_VERSION,
             ORM_DRIVER_CAP_SELECT, peer_a) == SALTS_PLUGIN_OK);
  REQUIRE(salts_plugin_export_require_interface(
             &entry, "TurboDb.NotDriver",
             ORM_DRIVER_INTERFACE_CONTRACT_VERSION,
             ORM_DRIVER_CAP_SELECT, peer_a) ==
         SALTS_PLUGIN_INCOMPATIBLE_CONTRACT);
  REQUIRE(salts_plugin_export_require_interface(
             &entry, ORM_DRIVER_INTERFACE_CONTRACT_ID,
             ORM_DRIVER_INTERFACE_CONTRACT_VERSION + 1u,
             ORM_DRIVER_CAP_SELECT, peer_a) ==
         SALTS_PLUGIN_INCOMPATIBLE_CONTRACT);
  REQUIRE(salts_plugin_export_require_interface(
             &entry, ORM_DRIVER_INTERFACE_CONTRACT_ID,
             ORM_DRIVER_INTERFACE_CONTRACT_VERSION,
             ORM_DRIVER_CAP_DELETE, peer_a) ==
         SALTS_PLUGIN_INCOMPATIBLE_CONTRACT);

  const cmeta_interface_desc *wrong_base = TurboDb_Driver_Wrong_interface();
  cmeta_interface_desc wrong_shape = *wrong_base;
  wrong_shape.name = local->name;
  REQUIRE(cmeta_interface_desc_valid(&wrong_shape));
  salts_plugin_export wrong_entry = entry;
  wrong_entry.value.interface.desc = &wrong_shape;
  REQUIRE(salts_plugin_export_require_interface(
             &wrong_entry, ORM_DRIVER_INTERFACE_CONTRACT_ID,
             ORM_DRIVER_INTERFACE_CONTRACT_VERSION,
             ORM_DRIVER_CAP_SELECT, peer_a) ==
         SALTS_PLUGIN_INCOMPATIBLE_CONTRACT);

  salts_plugin_manifest manifest = {
      .struct_size = SALTS_PLUGIN_MANIFEST_SIZE,
      .abi_version = SALTS_PLUGIN_ABI_VERSION,
      .plugin_id = "turbodb-driver-fixture",
      .version = {1u, 0u, 0u},
      .exports = &entry,
      .export_count = 1u};
  REQUIRE(salts_plugin_manifest_validate(&manifest) == SALTS_PLUGIN_OK);
  manifest.abi_version = SALTS_PLUGIN_ABI_VERSION + 1u;
  REQUIRE(salts_plugin_manifest_validate(&manifest) ==
         SALTS_PLUGIN_UNSUPPORTED_ABI);

  return 0;
}

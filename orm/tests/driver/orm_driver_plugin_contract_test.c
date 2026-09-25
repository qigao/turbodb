#include <orm_driver_plugin.h>
#include <tinytest.h>

#include <string.h>

#define FIXTURE_CAPS (ORM_DRIVER_CAP_SELECT | ORM_DRIVER_CAP_TRANSACTION)

typedef struct plugin_driver_fixture {
  uint64_t execution_models;
  unsigned connect_calls;
} plugin_driver_fixture;

static uint64_t fixture_execution_models(void *self) {
  return ((plugin_driver_fixture *)self)->execution_models;
}

static orm_status_t fixture_connect(
    void *self,
    const orm_config_t *config,
    const orm_driver_limits_v1 *limits,
    orm_driver_connection_v1 *out_connection,
    orm_error_t *error) {
  plugin_driver_fixture *fixture = (plugin_driver_fixture *)self;
  (void)config;
  (void)limits;
  (void)out_connection;
  (void)error;
  ++fixture->connect_calls;
  return ORM_STATUS_OK;
}

CMETA_IMPLEMENTS(
    turbodb_driver, fixture_driver, FIXTURE_CAPS,
    .execution_models = fixture_execution_models,
    .connect = fixture_connect);

#define WRONG_DRIVER_METHODS(X, I) X(I,F0,uint64_t,execution_model,value,&orm_driver_interface_execution_models_type,CMETA_ABI_SCALAR)
CMETA_INTERFACE(turbodb_wrong_driver, WRONG_DRIVER_METHODS);

const cmeta_interface_desc *orm_driver_plugin_peer_interface(void);
const cmeta_function_desc *orm_driver_plugin_peer_connect_function(void);
const cmeta_function_abi_desc *orm_driver_plugin_peer_connect_abi(void);

static salts_plugin_export fixture_export(turbodb_driver *driver) {
  salts_plugin_export entry;
  memset(&entry, 0, sizeof(entry));
  entry.struct_size = SALTS_PLUGIN_EXPORT_SIZE;
  entry.kind = SALTS_PLUGIN_EXPORT_INTERFACE;
  entry.contract_version = ORM_DRIVER_PLUGIN_CONTRACT_VERSION;
  entry.capabilities = FIXTURE_CAPS;
  entry.export_id = "fixture";
  entry.contract_id = ORM_DRIVER_PLUGIN_CONTRACT_ID;
  entry.value.interface.desc = turbodb_driver_interface();
  entry.value.interface.value = driver;
  return entry;
}

spec("TurboDb.Driver reflected Plugin contract") {
  it("publishes complete FunctionDesc and FunctionAbi metadata") {
    const cmeta_function_desc *connect = turbodb_driver_connect_function();
    const cmeta_function_abi_desc *abi = turbodb_driver_connect_function_abi();
    check_true(cmeta_function_desc_valid(connect));
    check_true(cmeta_function_abi_desc_valid(abi));
    check_equal(connect->param_count, (size_t)4u);
    check_true((connect->effects & CMETA_EFFECT_IO) != 0u);
    check_true((connect->effects & CMETA_EFFECT_MAY_FAIL) != 0u);
    check_equal(cmeta_function_param_abi(abi, 0u),
                (cmeta_abi_carrier)CMETA_ABI_OBJECT_POINTER);
    check_equal(cmeta_function_param_abi(abi, 1u),
                (cmeta_abi_carrier)CMETA_ABI_OBJECT_POINTER);
    check_equal(cmeta_function_param_abi(abi, 2u),
                (cmeta_abi_carrier)CMETA_ABI_OBJECT_POINTER);
    check_equal(cmeta_function_param_abi(abi, 3u),
                (cmeta_abi_carrier)CMETA_ABI_OBJECT_POINTER);
  }

  it("matches independently compiled reflection semantically") {
    check_true(cmeta_interface_desc_equal(
        turbodb_driver_interface(), orm_driver_plugin_peer_interface()));
    check_true(cmeta_function_desc_equal(
        turbodb_driver_connect_function(),
        orm_driver_plugin_peer_connect_function()));
    check_true(cmeta_function_abi_desc_equal(
        turbodb_driver_connect_function_abi(),
        orm_driver_plugin_peer_connect_abi()));
  }

  it("admits and caches a typed binding without hot-path lookup") {
    plugin_driver_fixture fixture = {
        ORM_DRIVER_EXEC_CALLER_BLOCKING, 0u};
    turbodb_driver driver = fixture_driver_as_turbodb_driver(&fixture);
    salts_plugin_export entry = fixture_export(&driver);
    orm_driver_plugin_binding binding;

    check_equal(
        orm_driver_plugin_bind_export(
            &entry, ORM_DRIVER_CAP_SELECT, &binding),
        SALTS_PLUGIN_OK);
    check_true(orm_driver_plugin_binding_valid(&binding));
    check_equal(binding.capabilities, (uint64_t)FIXTURE_CAPS);
    check_equal(binding.execution_models,
                (uint64_t)ORM_DRIVER_EXEC_CALLER_BLOCKING);

    check_equal(
        turbodb_driver_connect(
            &binding.driver, NULL, NULL, NULL, NULL),
        ORM_STATUS_OK);
    check_equal(fixture.connect_calls, 1u);
  }

  it("rejects wrong domain contract identity and version") {
    plugin_driver_fixture fixture = {
        ORM_DRIVER_EXEC_CALLER_BLOCKING, 0u};
    turbodb_driver driver = fixture_driver_as_turbodb_driver(&fixture);
    salts_plugin_export entry = fixture_export(&driver);
    orm_driver_plugin_binding binding;

    entry.contract_id = "Other.Driver";
    check_equal(
        orm_driver_plugin_bind_export(&entry, 0u, &binding),
        SALTS_PLUGIN_INCOMPATIBLE_CONTRACT);

    entry = fixture_export(&driver);
    entry.contract_version = ORM_DRIVER_PLUGIN_CONTRACT_VERSION + 1u;
    check_equal(
        orm_driver_plugin_bind_export(&entry, 0u, &binding),
        SALTS_PLUGIN_INCOMPATIBLE_CONTRACT);
  }

  it("rejects missing required capabilities") {
    plugin_driver_fixture fixture = {
        ORM_DRIVER_EXEC_CALLER_BLOCKING, 0u};
    turbodb_driver driver = fixture_driver_as_turbodb_driver(&fixture);
    salts_plugin_export entry = fixture_export(&driver);
    orm_driver_plugin_binding binding;

    check_equal(
        orm_driver_plugin_bind_export(
            &entry, ORM_DRIVER_CAP_RAW_SQL, &binding),
        SALTS_PLUGIN_INCOMPATIBLE_CONTRACT);
  }

  it("rejects a different reflected interface shape") {
    plugin_driver_fixture fixture = {
        ORM_DRIVER_EXEC_CALLER_BLOCKING, 0u};
    turbodb_driver driver = fixture_driver_as_turbodb_driver(&fixture);
    salts_plugin_export entry = fixture_export(&driver);
    orm_driver_plugin_binding binding;

    entry.value.interface.desc = turbodb_wrong_driver_interface();
    check_equal(
        orm_driver_plugin_bind_export(&entry, 0u, &binding),
        SALTS_PLUGIN_INCOMPATIBLE_CONTRACT);
  }

  it("rejects capability disagreement between Plugin and CMeta") {
    plugin_driver_fixture fixture = {
        ORM_DRIVER_EXEC_CALLER_BLOCKING, 0u};
    turbodb_driver driver = fixture_driver_as_turbodb_driver(&fixture);
    salts_plugin_export entry = fixture_export(&driver);
    orm_driver_plugin_binding binding;

    entry.capabilities = ORM_DRIVER_CAP_SELECT;
    check_equal(
        orm_driver_plugin_bind_export(&entry, 0u, &binding),
        SALTS_PLUGIN_INCOMPATIBLE_CONTRACT);
    check_true(!orm_driver_plugin_binding_valid(&binding));
  }

  it("rejects unknown capability and execution-model bits") {
    plugin_driver_fixture fixture = {
        ORM_DRIVER_EXEC_CALLER_BLOCKING, 0u};
    turbodb_driver driver = fixture_driver_as_turbodb_driver(&fixture);
    salts_plugin_export entry = fixture_export(&driver);
    orm_driver_plugin_binding binding;

    entry.capabilities |= UINT64_C(1) << 63;
    check_equal(
        orm_driver_plugin_bind_export(&entry, 0u, &binding),
        SALTS_PLUGIN_INCOMPATIBLE_CONTRACT);

    entry = fixture_export(&driver);
    fixture.execution_models = UINT64_C(1) << 63;
    check_equal(
        orm_driver_plugin_bind_export(&entry, 0u, &binding),
        SALTS_PLUGIN_INCOMPATIBLE_CONTRACT);
  }

  it("rejects unknown host requirements and null binding output") {
    plugin_driver_fixture fixture = {
        ORM_DRIVER_EXEC_CALLER_BLOCKING, 0u};
    turbodb_driver driver = fixture_driver_as_turbodb_driver(&fixture);
    salts_plugin_export entry = fixture_export(&driver);
    orm_driver_plugin_binding binding;

    check_equal(
        orm_driver_plugin_bind_export(
            &entry, UINT64_C(1) << 63, &binding),
        SALTS_PLUGIN_INVALID_ARGUMENT);
    check_equal(
        orm_driver_plugin_bind_export(&entry, 0u, NULL),
        SALTS_PLUGIN_INVALID_ARGUMENT);
  }
}

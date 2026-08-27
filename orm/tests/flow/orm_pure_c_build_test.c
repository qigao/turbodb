#include <orm.h>

#include <tinytest.h>

spec("ORM pure C build contract") {
  it("exposes the retained C11 query surface") {
    orm_config_t config;
    orm_flow_config_t flow;
    orm_error_t error;

    orm_config(&config);
    orm_flow_config(&flow, NULL);
    orm_error_init(&error);

    check_equal(orm_c_abi_version(), (uint32_t)ORM_C_ABI_VERSION);
    check_equal(config.struct_size, (uint32_t)sizeof(config));
    check_equal(config.abi_version, (uint32_t)ORM_C_ABI_VERSION);
    check_equal(flow.struct_size, (uint32_t)sizeof(flow));
    check_equal(error.status, ORM_STATUS_OK);
  }
}

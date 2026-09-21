#include "orm_module_loader.h"
#include <orm_driver_abi.h>

#include <tinytest.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int32_t (ORM_DRIVER_CALL *orm_bootstrap_fn)(
    const orm_driver_host_v1 *, uint32_t,
    const orm_driver_api_v1 **, uint32_t *);

static const char *fixture_path(void) {
  const char *path = getenv("ORM_MODULE_FIXTURE");
  return path == NULL ? "" : path;
}

spec("runtime module loader") {
  it("rejects relative paths and clears the output handle") {
    orm_error_t error;
    orm_module_handle module;
    module.native = &module;
    orm_error_init(&error);

    check_equal(orm_module_open_absolute("relative-driver-module", &module, &error),
                ORM_STATUS_INVALID_ARGUMENT);
    check_null(module.native);
    check_equal(error.status, ORM_STATUS_INVALID_ARGUMENT);
  }

  it("loads an explicit module and resolves only requested symbols") {
    orm_error_t error;
    orm_module_handle module = {0};
    orm_bootstrap_fn bootstrap = NULL;
    const char *path = fixture_path();

    orm_error_init(&error);
    check_true(path[0] != '\0');
    check_equal(orm_module_open_absolute(path, &module, &error), ORM_STATUS_OK);
    check_not_null(module.native);

    check_equal(orm_module_symbol(&module, "orm_driver_get_api_v1",
                                  &bootstrap, sizeof(bootstrap), &error),
                ORM_STATUS_OK);
    check_true(bootstrap != NULL);

    check_equal(orm_module_symbol(&module, "orm_driver_missing_entry",
                                  &bootstrap, sizeof(bootstrap), &error),
                ORM_STATUS_DRIVER_ENTRY_MISSING);
    check_true(bootstrap == NULL);
    check_equal(error.status, ORM_STATUS_DRIVER_ENTRY_MISSING);

    orm_module_close(&module);
    check_null(module.native);
    orm_module_close(&module);
    check_null(module.native);
  }

  it("distinguishes a missing explicit module from loader rejection") {
    orm_error_t error;
    orm_module_handle module = {0};
    const char *path = fixture_path();
    char missing[8192];
    int written;

    orm_error_init(&error);
    check_true(path[0] != '\0');
    written = snprintf(missing, sizeof(missing), "%s.missing", path);
    check_true(written > 0);
    check_true((size_t)written < sizeof(missing));

    check_equal(orm_module_open_absolute(missing, &module, &error),
                ORM_STATUS_DRIVER_MODULE_NOT_FOUND);
    check_null(module.native);
    check_equal(error.status, ORM_STATUS_DRIVER_MODULE_NOT_FOUND);
  }
}

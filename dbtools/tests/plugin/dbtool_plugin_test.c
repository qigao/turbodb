#include "dbtool_plugin.h"
#include "dbtool_cli.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

spec("schema Plugin admission and ownership") {
  (void)ttest_config__;
  static dbtool_plugin plugin;
  static dbtool_error error;
  before_each() {
    plugin = (dbtool_plugin){0};
    dbtool_error_init(&error);
  }
  after_each() {
    check_equal(dbtool_plugin_close(&plugin, &error), DBTOOL_STATUS_OK);
    check_null(plugin.registry.impl);
    check_null(plugin.ops);
  }

  it("rejects relative module paths without allocating a registry") {
    check_equal(dbtool_plugin_load(&plugin, "driver.dll", "sqlite", &error),
                DBTOOL_STATUS_INVALID_ARGUMENT);
    check_null(plugin.registry.impl);
  }

  it("rejects a mismatched canonical driver identity") {
    check_equal(dbtool_plugin_load(&plugin, DBTOOL_SCHEMA_FIXTURE, "postgresql", &error),
                DBTOOL_STATUS_UNSUPPORTED);
    check_null(plugin.ops);
    check_equal(error.stage, "validate-plugin");
  }

  it("rejects a module with no schema export") {
    check_equal(dbtool_plugin_load(&plugin, DBTOOL_MISSING_FIXTURE, "sqlite", &error),
                DBTOOL_STATUS_UNSUPPORTED);
    check_equal(error.native_code, (int)CMETA_PLUGIN_UNKNOWN_EXPORT);
  }

  it("rejects a different Plugin ABI without retrying another ABI") {
    check_equal(dbtool_plugin_load(&plugin, DBTOOL_ABI_FIXTURE, "sqlite", &error),
                DBTOOL_STATUS_UNSUPPORTED);
    check_equal(error.stage, "load-plugin");
  }

  it("rejects an incompatible schema contract version") {
    check_equal(dbtool_plugin_load(&plugin, DBTOOL_CONTRACT_FIXTURE, "sqlite", &error),
                DBTOOL_STATUS_UNSUPPORTED);
    check_equal(error.native_code, (int)CMETA_PLUGIN_INCOMPATIBLE_CONTRACT);
  }

  it("rejects missing operations before opening a connection") {
    check_equal(dbtool_plugin_load(&plugin, DBTOOL_OPS_FIXTURE, "sqlite", &error),
                DBTOOL_STATUS_UNSUPPORTED);
    check_null(plugin.ops);
    check_contains(error.message, "operations");
  }

  it("holds a lease across connection use and prevents premature unload") {
    const dbtool_connection_config config = {":memory:", NULL, 50u};
    const char sql[] = "begin; create table example(id integer); commit;";
    dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
    cmeta_plugin_lifecycle_info info;
    void *context = NULL;
    check_equal(dbtool_plugin_load(&plugin, DBTOOL_SCHEMA_FIXTURE, "sqlite", &error),
                DBTOOL_STATUS_OK);
    check_equal(plugin.ops->open(&context, &config, &error), DBTOOL_STATUS_OK);
    check_equal(cmeta_plugin_registry_get_lifecycle(&plugin.registry, plugin.ref, &info),
                CMETA_PLUGIN_OK);
    check_equal(info.active_leases, (size_t)1u);
    check_equal(cmeta_plugin_registry_unload(&plugin.registry, plugin.ref), CMETA_PLUGIN_BUSY);
    check_equal(plugin.ops->apply(context, sql, sizeof(sql) - 1u, &result, &error),
                DBTOOL_STATUS_OK);
    check_equal(result.statements, (uint64_t)3u);
    plugin.ops->close(context);
    check_equal(dbtool_plugin_close(&plugin, &error), DBTOOL_STATUS_OK);
    check_equal(dbtool_plugin_load(&plugin, DBTOOL_SCHEMA_FIXTURE, "sqlite", &error),
                DBTOOL_STATUS_OK);
  }

  it("parses help without requiring a plugin") {
    const char *argv[] = {"turbodb-sqlite", "--help"};
    dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
    check_equal(dbtool_cli_execute(2, argv, DBTOOL_DRIVER_SQLITE, NULL, &result, &error),
                DBTOOL_STATUS_HELP);
  }

  it("preserves closing state until an outstanding lease is released") {
    cmeta_plugin_lease borrower = {0};
    const cmeta_plugin_manifest *manifest = NULL;
    check_equal(dbtool_plugin_load(&plugin, DBTOOL_SCHEMA_FIXTURE, "sqlite", &error),
                DBTOOL_STATUS_OK);
    check_equal(cmeta_plugin_registry_acquire(&plugin.registry, plugin.ref,
                                             &borrower, &manifest), CMETA_PLUGIN_OK);
    check_equal(dbtool_plugin_close(&plugin, &error), DBTOOL_STATUS_INTERNAL_ERROR);
    check_equal(error.native_code, (int)CMETA_PLUGIN_BUSY);
    check_not_null(plugin.registry.impl);
    check_null(plugin.ops);
    check_equal(cmeta_plugin_registry_release(&plugin.registry, &borrower), CMETA_PLUGIN_OK);
    check_equal(dbtool_plugin_close(&plugin, &error), DBTOOL_STATUS_OK);
  }

  it("reports a missing module without publishing operations") {
    char missing[CMETA_PLUGIN_PATH_MAX + 1u];
    const int length = snprintf(missing, sizeof(missing), "%s.missing", DBTOOL_SCHEMA_FIXTURE);
    check_true(length > 0 && (size_t)length < sizeof(missing));
    check_equal(dbtool_plugin_load(&plugin, missing, "sqlite", &error),
                DBTOOL_STATUS_FILE_ERROR);
    check_null(plugin.ops);
    check_equal(error.stage, "load-plugin");
  }

  it("finishes closing a module whose stop was already requested") {
    check_equal(dbtool_plugin_load(&plugin, DBTOOL_SCHEMA_FIXTURE, "sqlite", &error),
                DBTOOL_STATUS_OK);
    check_equal(cmeta_plugin_registry_request_stop(&plugin.registry, plugin.ref),
                CMETA_PLUGIN_OK);
    check_equal(dbtool_plugin_close(&plugin, &error), DBTOOL_STATUS_OK);
    check_null(plugin.registry.impl);
  }

  it("executes the existing CLI through an explicitly selected module") {
    const char sql[] = "begin; create table cli_test(id integer); commit;";
    char *file = tt_make_temp_file("dbtool-plugin", ".sql");
    const char *argv[] = {"turbodb-sqlite", "schema", "apply", "--database",
                         ":memory:", "--file", file, "--plugin", DBTOOL_SCHEMA_FIXTURE};
    dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
    check_not_null(file);
    check_equal(tt_write_file(file, sql, sizeof(sql) - 1u), 0);
    check_equal(dbtool_cli_execute(9, argv, DBTOOL_DRIVER_SQLITE, NULL, &result, &error),
                DBTOOL_STATUS_OK);
    check_equal(result.statements, (uint64_t)3u);
    check_equal(tt_remove_file(file), 0);
    free(file);
  }
}

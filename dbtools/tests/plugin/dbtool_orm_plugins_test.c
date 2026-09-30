#include "dbtool_plugin.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

spec("dbtools consumes ORM driver schema exports") {
  (void)ttest_config__;
  it("applies a transaction through the SQLite ORM plugin") {
    dbtool_plugin plugin = {0};
    dbtool_error error = DBTOOL_ERROR_INIT;
    const dbtool_connection_config config = {":memory:", NULL, 50u};
    const char sql[] = "begin; create table alpha(id integer);"
                       "create table beta(id integer); drop table beta; commit;";
    dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
    void *context = NULL;
    check_equal(dbtool_plugin_load(&plugin, DBTOOL_SQLITE_PLUGIN, "sqlite", &error),
                DBTOOL_STATUS_OK);
    check_equal(plugin.ops->open(&context, &config, &error), DBTOOL_STATUS_OK);
    check_equal(plugin.ops->apply(context, sql, sizeof(sql) - 1u, &result, &error),
                DBTOOL_STATUS_OK);
    check_equal(result.statements, (uint64_t)5u);
    plugin.ops->close(context);
    check_equal(dbtool_plugin_close(&plugin, &error), DBTOOL_STATUS_OK);
  }

  it("loads the PostgreSQL schema contract without contacting a server") {
    dbtool_plugin plugin = {0};
    dbtool_error error = DBTOOL_ERROR_INIT;
    check_equal(dbtool_plugin_load(&plugin, DBTOOL_POSTGRESQL_PLUGIN, "postgresql", &error),
                DBTOOL_STATUS_OK);
    check_not_null(plugin.ops);
    check_equal(plugin.ops->abi_version, DBTOOL_SCHEMA_DRIVER_ABI_VERSION);
    check_equal(dbtool_plugin_close(&plugin, &error), DBTOOL_STATUS_OK);
  }

  it("loads the MySQL schema contract and rejects missing TLS configuration") {
    dbtool_plugin plugin = {0};
    dbtool_error error = DBTOOL_ERROR_INIT;
    const dbtool_connection_config config = {NULL, "{}", 0u};
    void *context = NULL;
    check_equal(dbtool_plugin_load(&plugin, DBTOOL_MYSQL_PLUGIN, "mysql", &error),
                DBTOOL_STATUS_OK);
    check_not_null(plugin.ops);
    check_equal(plugin.ops->open(&context, &config, &error), DBTOOL_STATUS_INVALID_ARGUMENT);
    check_null(context);
    check_equal(dbtool_plugin_close(&plugin, &error), DBTOOL_STATUS_OK);
  }

  it("closes a failed SQLite script before releasing its module lease") {
    dbtool_plugin plugin = {0};
    dbtool_error error = DBTOOL_ERROR_INIT;
    char *database = tt_make_temp_file("dbtool-plugin-rollback", ".db");
    const dbtool_connection_config config = {database, NULL, 50u};
    const char broken[] = "begin; create table alpha(id integer); invalid SQL; commit;";
    const char valid[] = "create table alpha(id integer);";
    dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
    void *context = NULL;
    check_not_null(database);
    check_equal(dbtool_plugin_load(&plugin, DBTOOL_SQLITE_PLUGIN, "sqlite", &error),
                DBTOOL_STATUS_OK);
    check_equal(plugin.ops->open(&context, &config, &error), DBTOOL_STATUS_OK);
    check_equal(plugin.ops->apply(context, broken, sizeof(broken) - 1u, &result, &error),
                DBTOOL_STATUS_SQL_ERROR);
    check_equal(error.stage, "apply-schema");
    check_equal(result.statements, (uint64_t)0u);
    plugin.ops->close(context);
    check_equal(dbtool_plugin_close(&plugin, &error), DBTOOL_STATUS_OK);
    check_equal(dbtool_plugin_load(&plugin, DBTOOL_SQLITE_PLUGIN, "sqlite", &error),
                DBTOOL_STATUS_OK);
    check_equal(plugin.ops->open(&context, &config, &error), DBTOOL_STATUS_OK);
    check_equal(plugin.ops->apply(context, valid, sizeof(valid) - 1u, &result, &error),
                DBTOOL_STATUS_OK);
    plugin.ops->close(context);
    check_equal(dbtool_plugin_close(&plugin, &error), DBTOOL_STATUS_OK);
    check_equal(tt_remove_file(database), 0);
    free(database);
  }
}

#include "dbtool_plugin.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

spec("MySQL schema plugin against a TLS test server") {
  (void)ttest_config__;
  static dbtool_plugin plugin;
  static dbtool_error error;
  static void *context;
  before_each() {
    memset(&plugin, 0, sizeof(plugin));
    memset(&error, 0, sizeof(error));
    context = NULL;
#ifdef DBTOOL_MYSQL_PLUGIN
    const char *path = DBTOOL_MYSQL_PLUGIN;
#else
    const char *path = getenv("TURBODB_MYSQL_PLUGIN");
#endif
    const char *conninfo = getenv("TURBODB_DBTOOLS_MYSQL_TEST_CONNINFO");
    check_not_null(conninfo);
    check_not_null(path);
    check_equal(dbtool_plugin_load(&plugin, path, "mysql", &error), DBTOOL_STATUS_OK);
    const dbtool_connection_config config = {NULL, conninfo, 0u};
    check_equal(plugin.ops->open(&context, &config, &error), DBTOOL_STATUS_OK);
  }
  after_each() {
    if (context != NULL) plugin.ops->close(context);
    check_equal(dbtool_plugin_close(&plugin, &error), DBTOOL_STATUS_OK);
  }

  it("executes multiple statements on one session including quoted semicolons") {
    const char sql[] = "CREATE TEMPORARY TABLE dbtools_mysql_script(id INT, value TEXT);"
                       "INSERT INTO dbtools_mysql_script VALUES(1, 'a;b');"
                       "DROP TEMPORARY TABLE dbtools_mysql_script;";
    dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
    check_equal(plugin.ops->apply(context, sql, sizeof(sql) - 1u, &result, &error),
                DBTOOL_STATUS_OK);
    check_equal(result.statements, (uint64_t)3u);
  }

  it("returns a later SQL failure and can execute on a fresh session afterwards") {
    const char sql[] = "CREATE TEMPORARY TABLE dbtools_mysql_script(id INT);"
                       "INSERT INTO dbtools_mysql_script(nonexistent) VALUES(1);";
    const char valid[] = "CREATE TEMPORARY TABLE dbtools_mysql_script(id INT);"
                         "DROP TEMPORARY TABLE dbtools_mysql_script;";
    dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
    check_equal(plugin.ops->apply(context, sql, sizeof(sql) - 1u, &result, &error),
                DBTOOL_STATUS_SQL_ERROR);
    check_equal(result.statements, (uint64_t)0u);
    check_equal(plugin.ops->apply(context, valid, sizeof(valid) - 1u, &result, &error),
                DBTOOL_STATUS_OK);
    check_equal(result.statements, (uint64_t)2u);
  }

  it("rejects row-producing scripts") {
    const char sql[] = "SELECT 1;";
    dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
    check_equal(plugin.ops->apply(context, sql, sizeof(sql) - 1u, &result, &error),
                DBTOOL_STATUS_UNSUPPORTED);
  }
}

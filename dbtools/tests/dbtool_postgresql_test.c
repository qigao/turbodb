#include "postgresql/dbtool_postgresql.h"

#include "fake_libpq.h"

#include <string.h>

#include "tinytest.h"

static dbtool_status apply_sql(const char *conninfo, const char *sql,
                               size_t sql_size, dbtool_apply_result *result,
                               dbtool_error *error) {
  const dbtool_schema_driver_ops *ops = dbtool_postgresql_schema_driver();
  const dbtool_connection_config config = {NULL, conninfo, 0u};
  void *context = NULL;
  dbtool_status status = ops->open(&context, &config, error);
  if (status == DBTOOL_STATUS_OK)
    status = ops->apply(context, sql, sql_size, result, error);
  if (context != NULL)
    ops->close(context);
  return status;
}

spec("PostgreSQL standalone schema driver") {
  before_each() { fake_libpq_reset(); }

  it("sends one standard create and drop DDL transaction and drains every result") {
    static const char sql[] =
        "begin; create table alpha(id int); create table beta(id int); "
        "drop table beta; commit;";
    static const ExecStatusType statuses[] = {
        PGRES_COMMAND_OK, PGRES_COMMAND_OK, PGRES_COMMAND_OK,
        PGRES_COMMAND_OK, PGRES_COMMAND_OK};
    dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;
    const fake_libpq_metrics *metrics;

    fake_libpq_set_results(statuses, NULL, 5u);
    check_equal(apply_sql("host=fake", sql, sizeof(sql) - 1u, &result, &error),
                DBTOOL_STATUS_OK);
    metrics = fake_libpq_get_metrics();
    check_equal(metrics->connect_calls, 1);
    check_equal(metrics->send_calls, 1);
    check_equal(metrics->get_result_calls, 6);
    check_equal(metrics->clear_calls, 5);
    check_equal(metrics->finish_calls, 1);
    check_equal(metrics->sql, sql);
    check_equal(result.statements, (uint64_t)5u);
    check_true(fake_libpq_all_results_cleared());
  }

  it("preserves an intermediate SQL error while draining later results") {
    static const ExecStatusType statuses[] = {
        PGRES_COMMAND_OK, PGRES_COMMAND_OK, PGRES_FATAL_ERROR};
    static const char *const messages[] = {"", "", "forced SQL failure"};
    static const char sql[] =
        "begin; create table alpha(id int); broken; commit;";
    dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;
    const fake_libpq_metrics *metrics;

    fake_libpq_set_results(statuses, messages, 3u);
    check_equal(apply_sql(NULL, sql, sizeof(sql) - 1u, &result, &error),
                DBTOOL_STATUS_SQL_ERROR);
    metrics = fake_libpq_get_metrics();
    check_equal(metrics->get_result_calls, 4);
    check_equal(metrics->clear_calls, 3);
    check_true(fake_libpq_all_results_cleared());
    check_contains(error.message, "forced SQL failure");
  }

  it("reports a connection failure when no result follows a successful send") {
    static const char sql[] = "create table alpha(id int);";
    dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;

    fake_libpq_set_connection_error("connection dropped");
    check_equal(apply_sql(NULL, sql, sizeof(sql) - 1u, &result, &error),
                DBTOOL_STATUS_CONNECTION_ERROR);
    check_contains(error.message, "connection dropped");
    check_equal(fake_libpq_get_metrics()->get_result_calls, 1);
  }

  it("rejects row-producing results after releasing them") {
    static const ExecStatusType statuses[] = {PGRES_TUPLES_OK};
    static const char sql[] = "select 1;";
    dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;

    fake_libpq_set_results(statuses, NULL, 1u);
    check_equal(apply_sql(NULL, sql, sizeof(sql) - 1u, &result, &error),
                DBTOOL_STATUS_UNSUPPORTED);
    check_equal(fake_libpq_get_metrics()->clear_calls, 1);
    check_true(fake_libpq_all_results_cleared());
  }

  it("does not copy conninfo secrets into connection errors") {
    static const char secret[] = "fake-password-token";
    static const char conninfo[] =
        "host=fake user=test password=fake-password-token";
    const dbtool_schema_driver_ops *ops = dbtool_postgresql_schema_driver();
    const dbtool_connection_config config = {NULL, conninfo, 0u};
    dbtool_error error = DBTOOL_ERROR_INIT;
    void *context = NULL;

    fake_libpq_set_connect_failure("forced connection failure");
    check_equal(ops->open(&context, &config, &error),
                DBTOOL_STATUS_CONNECTION_ERROR);
    check_null(context);
    check_null(strstr(error.message, secret));
    check_contains(error.message, "forced connection failure");
    check_equal(fake_libpq_get_metrics()->finish_calls, 1);
  }

  it("rejects embedded NUL before sending a query") {
    static const char sql[] = "create table prefix(id int);\0select 1;";
    static const ExecStatusType statuses[] = {PGRES_COMMAND_OK};
    dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;

    fake_libpq_set_results(statuses, NULL, 1u);
    check_equal(apply_sql(NULL, sql, sizeof(sql) - 1u, &result, &error),
                DBTOOL_STATUS_INVALID_ARGUMENT);
    check_equal(fake_libpq_get_metrics()->send_calls, 0);
    check_equal(fake_libpq_get_metrics()->finish_calls, 1);
  }
}

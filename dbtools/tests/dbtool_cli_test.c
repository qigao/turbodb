#include "dbtool_cli.h"

#include <stdlib.h>
#include <string.h>

#include "tinytest.h"

typedef struct fake_driver_state {
  int open_calls;
  int apply_calls;
  int close_calls;
  dbtool_status open_status;
  dbtool_status apply_status;
  dbtool_connection_config config;
  char sql[64];
  size_t sql_size;
} fake_driver_state;

static fake_driver_state fake_state;

static dbtool_status fake_open(void **out_context,
                               const dbtool_connection_config *config,
                               dbtool_error *error) {
  ++fake_state.open_calls;
  fake_state.config = *config;
  if (fake_state.open_status != DBTOOL_STATUS_OK) {
    dbtool_error_set(error, fake_state.open_status, "open-driver", 17,
                     "injected open failure");
    return fake_state.open_status;
  }
  *out_context = &fake_state;
  return DBTOOL_STATUS_OK;
}

static dbtool_status fake_apply(void *context, const char *sql,
                                size_t sql_size, dbtool_apply_result *result,
                                dbtool_error *error) {
  fake_driver_state *state = (fake_driver_state *)context;
  ++state->apply_calls;
  state->sql_size = sql_size;
  if (sql_size < sizeof(state->sql)) {
    memcpy(state->sql, sql, sql_size);
    state->sql[sql_size] = '\0';
  }
  if (state->apply_status != DBTOOL_STATUS_OK) {
    dbtool_error_set(error, state->apply_status, "apply-schema", 23,
                     "injected apply failure");
    return state->apply_status;
  }
  result->statements = 2u;
  return DBTOOL_STATUS_OK;
}

static void fake_close(void *context) {
  fake_driver_state *state = (fake_driver_state *)context;
  ++state->close_calls;
}

static const dbtool_schema_driver_ops fake_ops = {
    sizeof(dbtool_schema_driver_ops), DBTOOL_SCHEMA_DRIVER_ABI_VERSION,
    fake_open, fake_apply, fake_close};

static void fake_reset(void) {
  memset(&fake_state, 0, sizeof(fake_state));
}

spec("standalone database tool CLI") {
  before_each() { fake_reset(); }

  it("maps usage, file, connection, SQL, and internal failures to distinct exits") {
    check_equal(dbtool_status_exit_code(DBTOOL_STATUS_OK), 0);
    check_equal(dbtool_status_exit_code(DBTOOL_STATUS_INVALID_ARGUMENT), 2);
    check_equal(dbtool_status_exit_code(DBTOOL_STATUS_FILE_ERROR), 3);
    check_equal(dbtool_status_exit_code(DBTOOL_STATUS_CONNECTION_ERROR), 4);
    check_equal(dbtool_status_exit_code(DBTOOL_STATUS_SQL_ERROR), 5);
    check_equal(dbtool_status_exit_code(DBTOOL_STATUS_INTERNAL_ERROR), 70);
  }

  it("forwards a validated SQLite schema invocation and closes once") {
    char *path = tt_make_temp_file("dbtool-cli", ".sql");
    const char *argv[] = {"turbodb-sqlite", "schema", "apply",
                          "--database",      ":memory:", "--file",
                          path,              "--max-script-bytes", "64",
                          "--busy-timeout-ms", "15"};
    dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;

    check_not_null(path);
    check_equal(tt_write_file(path, "select 1;", 9u), 0);
    check_equal(dbtool_cli_execute(11, argv, DBTOOL_DRIVER_SQLITE, &fake_ops,
                                   &result, &error),
                DBTOOL_STATUS_OK);
    check_equal(fake_state.open_calls, 1);
    check_equal(fake_state.apply_calls, 1);
    check_equal(fake_state.close_calls, 1);
    check_equal(fake_state.config.database, ":memory:");
    check_equal(fake_state.config.busy_timeout_ms, (uint32_t)15u);
    check_equal(fake_state.sql, "select 1;");
    check_equal(result.statements, (uint64_t)2u);

    check_equal(tt_remove_file(path), 0);
    free(path);
  }

  it("rejects an unknown option before opening the driver") {
    const char *argv[] = {"turbodb-sqlite", "schema", "apply",
                          "--database",      ":memory:", "--file",
                          "x.sql",           "--unknown", "1"};
    dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;

    check_equal(dbtool_cli_execute(9, argv, DBTOOL_DRIVER_SQLITE, &fake_ops,
                                   &result, &error),
                DBTOOL_STATUS_INVALID_ARGUMENT);
    check_equal(fake_state.open_calls, 0);
    check_contains(error.message, "unknown option");
  }

  it("rejects duplicate file options before opening the driver") {
    const char *argv[] = {"turbodb-sqlite", "schema", "apply", "--database",
                          ":memory:", "--file", "a.sql", "--file", "b.sql"};
    dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;

    check_equal(dbtool_cli_execute(9, argv, DBTOOL_DRIVER_SQLITE, &fake_ops,
                                   &result, &error),
                DBTOOL_STATUS_INVALID_ARGUMENT);
    check_equal(fake_state.open_calls, 0);
    check_contains(error.message, "duplicate option");
  }

  it("rejects an overflowing decimal limit") {
    const char *argv[] = {"turbodb-sqlite", "schema", "apply",
                          "--database",      ":memory:", "--file",
                          "x.sql", "--max-script-bytes",
                          "18446744073709551616"};
    dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;

    check_equal(dbtool_cli_execute(9, argv, DBTOOL_DRIVER_SQLITE, &fake_ops,
                                   &result, &error),
                DBTOOL_STATUS_INVALID_ARGUMENT);
    check_equal(fake_state.open_calls, 0);
    check_contains(error.message, "invalid decimal");
  }

  it("does not close a driver whose open failed") {
    char *path = tt_make_temp_file("dbtool-open-fail", ".sql");
    const char *argv[] = {"turbodb-sqlite", "schema", "apply", "--database",
                          ":memory:", "--file", path};
    dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;

    check_not_null(path);
    check_equal(tt_write_file(path, "select 1;", 9u), 0);
    fake_state.open_status = DBTOOL_STATUS_CONNECTION_ERROR;
    check_equal(dbtool_cli_execute(7, argv, DBTOOL_DRIVER_SQLITE, &fake_ops,
                                   &result, &error),
                DBTOOL_STATUS_CONNECTION_ERROR);
    check_equal(fake_state.open_calls, 1);
    check_equal(fake_state.apply_calls, 0);
    check_equal(fake_state.close_calls, 0);

    check_equal(tt_remove_file(path), 0);
    free(path);
  }

  it("closes once when apply fails") {
    char *path = tt_make_temp_file("dbtool-apply-fail", ".sql");
    const char *argv[] = {"turbodb-sqlite", "schema", "apply", "--database",
                          ":memory:", "--file", path};
    dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;

    check_not_null(path);
    check_equal(tt_write_file(path, "select 1;", 9u), 0);
    fake_state.apply_status = DBTOOL_STATUS_SQL_ERROR;
    check_equal(dbtool_cli_execute(7, argv, DBTOOL_DRIVER_SQLITE, &fake_ops,
                                   &result, &error),
                DBTOOL_STATUS_SQL_ERROR);
    check_equal(fake_state.open_calls, 1);
    check_equal(fake_state.apply_calls, 1);
    check_equal(fake_state.close_calls, 1);

    check_equal(tt_remove_file(path), 0);
    free(path);
  }
}

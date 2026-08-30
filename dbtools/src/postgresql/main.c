#include "dbtool_cli.h"
#include "postgresql/dbtool_postgresql.h"

#include <inttypes.h>
#include <stdio.h>

int main(int argc, char **argv) {
  dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
  dbtool_error error = DBTOOL_ERROR_INIT;
  const dbtool_status status = dbtool_cli_execute(
      argc, (const char *const *)argv, DBTOOL_DRIVER_POSTGRESQL,
      dbtool_postgresql_schema_driver(), &result, &error);
  if (status == DBTOOL_STATUS_HELP) {
    dbtool_cli_print_help(DBTOOL_DRIVER_POSTGRESQL,
                          argc > 0 ? argv[0] : NULL);
  } else if (status != DBTOOL_STATUS_OK) {
    dbtool_cli_print_error(&error);
  } else {
    (void)fprintf(stdout,
                  "driver=postgresql operation=schema-apply statements=%"
                  PRIu64 "\n",
                  result.statements);
  }
  return dbtool_status_exit_code(status);
}

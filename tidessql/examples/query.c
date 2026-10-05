#include <tidessql/tidessql.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool succeeded(turbodb_status_t status, const turbodb_error_t *error) {
  if (status == TURBODB_STATUS_OK) return true;
  fprintf(stderr, "TidesSQL error %d: %s\n", (int)status, error->message);
  return false;
}
int main(int argc, char **argv) {
  const bool initialize = argc == 3 && !strcmp(argv[2], "--initialize");
  if (argc != 2 && !initialize) {
    fprintf(stderr, "usage: %s DATABASE [--initialize]\n", argv[0]); return EXIT_FAILURE;
  }
  const turbodb_option_t options[] = {
    {turbodb_view("path"), turbodb_view(argv[1])},
    {turbodb_view("column_family"), turbodb_view("example")},
    {turbodb_view("sql_initialize"), turbodb_view(initialize ? "true" : "false")}
  };
  tdsql_config config = tdsql_config_default();
  config.options = options; config.option_count = sizeof(options)/sizeof(options[0]);
  turbodb_error_t error; turbodb_error_init(&error);
  tdsql_connection *connection = NULL;
  tdsql_result *result = NULL;
  int exit_code = EXIT_FAILURE;
  if (!succeeded(tdsql_connection_open(&config, &connection, &error), &error)) goto cleanup;
  const tdsql_request request = tdsql_request_default(turbodb_view("SELECT 42 AS number"));
  if (!succeeded(tdsql_connection_query(connection, &request, &result, &error), &error)) goto cleanup;
  tdsql_row row = {0};
  if (!succeeded(tdsql_result_next(result, &row, &error), &error)) goto cleanup;
  if (row.state != TDSQL_ROW || row.count != 1 || row.values[0].kind != TURBODB_VALUE_INT64) {
    fprintf(stderr, "expected one integer result\n"); goto cleanup;
  }
  printf("%" PRId64 "\n", row.values[0].data.int64_value); exit_code = EXIT_SUCCESS;
cleanup:
  if (!succeeded(tdsql_result_destroy_checked(result, &error), &error)) exit_code = EXIT_FAILURE;
  if (!succeeded(tdsql_connection_close(connection, &error), &error)) exit_code = EXIT_FAILURE;
  return exit_code;
}

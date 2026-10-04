#include "runtime_fixture.h"

#include <tinytest.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static orm_status_t secure_connect_port(
    const char *port, const char *server_name, const char *ca_file,
    orm_error_t *error) {
  orm_runtime_t *runtime = NULL;
  orm_config_t config;
  orm_option_t options[10];
  orm_connection_t *connection = NULL;
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  int64_t value = 0;
  orm_status_t status;

  if (port == NULL || port[0] == '\0' ||
      server_name == NULL || ca_file == NULL)
    return ORM_STATUS_INVALID_ARGUMENT;

  status = pg_runtime_create(&runtime, error);
  if (status != ORM_STATUS_OK)
    return status;

  orm_config(&config);
  options[0] = (orm_option_t){orm_view("turbodb_pg_transport"),
                              orm_view("direct_tls")};
  options[1] = (orm_option_t){orm_view("turbodb_pg_remote_host"),
                              orm_view("127.0.0.1")};
  options[2] = (orm_option_t){orm_view("turbodb_pg_remote_port"),
                              orm_view(port)};
  options[3] = (orm_option_t){orm_view("turbodb_pg_server_name"),
                              orm_view(server_name)};
  options[4] = (orm_option_t){orm_view("turbodb_pg_ca_file"),
                              orm_view(ca_file)};
  options[5] = (orm_option_t){orm_view("turbodb_pg_connect_timeout_ms"),
                              orm_view("5000")};
  options[6] = (orm_option_t){orm_view("turbodb_pg_handshake_timeout_ms"),
                              orm_view("5000")};
  options[7] = (orm_option_t){orm_view("turbodb_pg_idle_timeout_ms"),
                              orm_view("5000")};
  options[8] = (orm_option_t){orm_view("turbodb_pg_ingress_bytes"),
                              orm_view("65536")};
  options[9] = (orm_option_t){orm_view("conninfo"),
                              orm_view("dbname=turbodb user=turbodb password=turbodb")};
  config.driver = orm_view("postgresql");
  config.options = options;
  config.option_count = 10u;

  status = orm_runtime_connect(runtime, &config, &connection, error);
  if (status != ORM_STATUS_OK) {
    orm_error_t ignored;
    orm_error_init(&ignored);
    (void)orm_runtime_close(runtime, &ignored);
    orm_runtime_release(runtime);
    return status;
  }

  status = orm_raw(connection, orm_view("select 1::bigint"), &query, error);
  if (status == ORM_STATUS_OK)
    status = orm_query_execute(query, &result, error);
  if (status == ORM_STATUS_OK)
    status = orm_result_get_int64(result, 0u, 0u, &value, error);
  if (status == ORM_STATUS_OK && value != 1) {
    error->status = ORM_STATUS_INTERNAL_ERROR;
    (void)snprintf(error->message, sizeof(error->message),
                   "secure PostgreSQL query returned unexpected value");
    status = ORM_STATUS_INTERNAL_ERROR;
  }

  orm_result_destroy(result);
  orm_query_destroy(query);
  if (status != ORM_STATUS_OK) {
    orm_disconnect(connection);
    orm_runtime_close(runtime, error);
    orm_runtime_release(runtime);
    return status;
  }

  orm_disconnect(connection);
  status = orm_runtime_close(runtime, error);
  orm_runtime_release(runtime);
  return status;
}

spec("PostgreSQL 17 direct TLS over CNet/GmSSL") {
  it("connects and executes through verified direct TLS") {
    const char *ca = getenv("TURBODB_PG_TLS_CA");
    orm_error_t error;
    orm_error_init(&error);

    check_not_null(ca);
    if (ca == NULL)
      return;
    {
      const orm_status_t status =
          secure_connect_port(getenv("TURBODB_PG_TLS_PORT"),
                              "localhost", ca, &error);
      if (status != ORM_STATUS_OK)
        (void)fprintf(stderr, "direct TLS connect failed: status=%d message=%s\n",
                      (int)status, error.message);
      check_equal(status, ORM_STATUS_OK);
    }
  }

  it("fails closed for an untrusted CA") {
    const char *wrong_ca = getenv("TURBODB_PG_TLS_WRONG_CA");
    orm_error_t error;
    orm_error_init(&error);

    check_not_null(wrong_ca);
    if (wrong_ca == NULL)
      return;
    check_equal(secure_connect_port(getenv("TURBODB_PG_TLS_PORT"), "localhost", wrong_ca, &error),
                ORM_STATUS_CONNECTION_ERROR);
  }

  it("fails closed when negotiated ALPN is not postgresql") {
    const char *ca = getenv("TURBODB_PG_TLS_CA");
    const char *port = getenv("TURBODB_PG_TLS_WRONG_ALPN_PORT");
    orm_error_t error;
    orm_error_init(&error);

    check_not_null(ca);
    check_not_null(port);
    if (ca == NULL || port == NULL)
      return;
    check_equal(secure_connect_port(port, "localhost", ca, &error),
                ORM_STATUS_CONNECTION_ERROR);
  }

  it("fails closed for a wrong verified server name") {
    const char *ca = getenv("TURBODB_PG_TLS_CA");
    orm_error_t error;
    orm_error_init(&error);

    check_not_null(ca);
    if (ca == NULL)
      return;
    check_equal(secure_connect_port(getenv("TURBODB_PG_TLS_PORT"), "wrong.invalid", ca, &error),
                ORM_STATUS_CONNECTION_ERROR);
  }
}

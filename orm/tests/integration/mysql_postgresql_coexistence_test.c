#include <orm_runtime.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int load_driver(
    orm_runtime_t *runtime, const char *id,
    const char *path, orm_error_t *error) {
  orm_driver_load_config_t load;
  orm_driver_info_t info;

  memset(&load, 0, sizeof(load));
  load.struct_size = (uint32_t)sizeof(load);
  load.abi_version = ORM_RUNTIME_ABI_VERSION;
  load.module_path = orm_view(path);
  load.expected_driver_id = orm_view(id);
  if (orm_runtime_load_driver(runtime, &load, error) !=
      ORM_STATUS_OK)
    return 0;

  memset(&info, 0, sizeof(info));
  if (orm_runtime_driver_info(
          runtime, orm_view(id), &info, error) !=
      ORM_STATUS_OK)
    return 0;
  return info.canonical_id_size == strlen(id) &&
         memcmp(
             info.canonical_id, id,
             info.canonical_id_size) == 0;
}

static orm_connection_t *connect_mysql(
    orm_runtime_t *runtime,
    const char *host, const char *port,
    const char *user, const char *password,
    const char *database, const char *ca_file,
    const char *server_name, orm_error_t *error) {
  orm_config_t config;
  orm_option_t options[8];
  orm_connection_t *connection = NULL;

  orm_config(&config);
  options[0] =
      (orm_option_t){orm_view("host"), orm_view(host)};
  options[1] =
      (orm_option_t){orm_view("port"), orm_view(port)};
  options[2] =
      (orm_option_t){orm_view("username"), orm_view(user)};
  options[3] =
      (orm_option_t){orm_view("password"), orm_view(password)};
  options[4] =
      (orm_option_t){orm_view("database"), orm_view(database)};
  options[5] =
      (orm_option_t){orm_view("ca_file"), orm_view(ca_file)};
  options[6] =
      (orm_option_t){orm_view("server_name"), orm_view(server_name)};
  options[7] =
      (orm_option_t){orm_view("timeout_ms"), orm_view("5000")};
  config.driver = orm_view("mysql");
  config.options = options;
  config.option_count = 8u;

  if (orm_runtime_connect(
          runtime, &config, &connection, error) !=
      ORM_STATUS_OK)
    return NULL;
  return connection;
}

static orm_connection_t *connect_postgresql(
    orm_runtime_t *runtime, const char *conninfo,
    orm_error_t *error) {
  orm_config_t config;
  orm_option_t option;
  orm_connection_t *connection = NULL;

  orm_config(&config);
  option.keyword = orm_view("conninfo");
  option.value = orm_view(conninfo);
  config.driver = orm_view("postgresql");
  config.options = &option;
  config.option_count = 1u;

  if (orm_runtime_connect(
          runtime, &config, &connection, error) !=
      ORM_STATUS_OK)
    return NULL;
  return connection;
}

static int scalar_i64(
    orm_connection_t *connection, const char *sql,
    int64_t expected, orm_error_t *error) {
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  int64_t value = 0;
  int ok = 0;

  if (orm_raw(
          connection, orm_view(sql),
          &query, error) != ORM_STATUS_OK)
    goto cleanup;
  if (orm_query_execute(
          query, &result, error) != ORM_STATUS_OK)
    goto cleanup;
  if (orm_result_get_int64(
          result, 0u, 0u, &value, error) !=
      ORM_STATUS_OK)
    goto cleanup;
  ok = value == expected;

cleanup:
  if (!ok) {
    fprintf(
        stderr,
        "scalar query failed sql=%s expected=%lld got=%lld message=%s\n",
        sql, (long long)expected, (long long)value,
        error != NULL ? error->message : "");
  }
  orm_result_destroy(result);
  orm_query_destroy(query);
  return ok;
}

int main(void) {
  const char *mysql_plugin = getenv("ORM_MYSQL_PLUGIN");
  const char *postgresql_plugin =
      getenv("ORM_POSTGRESQL_PLUGIN");
  const char *host = getenv("ORM_MYSQL_HOST");
  const char *port = getenv("ORM_MYSQL_PORT");
  const char *user = getenv("ORM_MYSQL_USER");
  const char *password = getenv("ORM_MYSQL_PASSWORD");
  const char *database = getenv("ORM_MYSQL_DATABASE");
  const char *ca_file = getenv("ORM_MYSQL_CA_FILE");
  const char *server_name = getenv("ORM_MYSQL_SERVER_NAME");
  const char *conninfo =
      getenv("TURBODB_ORM_PGSQL_TEST_CONNINFO");
  orm_runtime_config_t runtime_config;
  orm_runtime_t *runtime = NULL;
  orm_connection_t *mysql = NULL;
  orm_connection_t *postgresql = NULL;
  orm_error_t error;
  int failed = 0;

  if (mysql_plugin == NULL || mysql_plugin[0] == '\0' ||
      postgresql_plugin == NULL ||
      postgresql_plugin[0] == '\0' ||
      host == NULL || host[0] == '\0' ||
      port == NULL || port[0] == '\0' ||
      user == NULL || password == NULL ||
      database == NULL || database[0] == '\0' ||
      ca_file == NULL || ca_file[0] == '\0' ||
      server_name == NULL || server_name[0] == '\0' ||
      conninfo == NULL || conninfo[0] == '\0') {
    fprintf(stderr, "coexistence environment is incomplete\n");
    return 2;
  }

  orm_runtime_config_init(&runtime_config);
  orm_error_init(&error);
  if (orm_runtime_create(
          &runtime_config, &runtime, &error) !=
      ORM_STATUS_OK)
    return 3;

  if (!load_driver(
          runtime, "mysql", mysql_plugin, &error) ||
      !load_driver(
          runtime, "postgresql",
          postgresql_plugin, &error)) {
    fprintf(stderr, "load coexistence Drivers failed: %s\n",
            error.message);
    failed = 1;
    goto cleanup;
  }

  mysql = connect_mysql(
      runtime, host, port, user, password,
      database, ca_file, server_name, &error);
  postgresql = connect_postgresql(
      runtime, conninfo, &error);
  if (mysql == NULL || postgresql == NULL) {
    fprintf(stderr, "connect coexistence Drivers failed: %s\n",
            error.message);
    failed = 1;
    goto cleanup;
  }

  if (!scalar_i64(
          mysql, "SELECT 42", INT64_C(42), &error) ||
      !scalar_i64(
          postgresql, "SELECT 84", INT64_C(84), &error))
    failed = 1;

  orm_error_init(&error);
  if (orm_runtime_close(runtime, &error) !=
      ORM_STATUS_BUSY) {
    fprintf(stderr,
            "runtime did not retain both Driver leases: %s\n",
            error.message);
    failed = 1;
  }

  orm_disconnect(mysql);
  mysql = NULL;
  if (!scalar_i64(
          postgresql, "SELECT 85", INT64_C(85), &error))
    failed = 1;

  orm_error_init(&error);
  if (orm_runtime_close(runtime, &error) !=
      ORM_STATUS_BUSY) {
    fprintf(stderr,
            "PostgreSQL lease did not survive MySQL disconnect: %s\n",
            error.message);
    failed = 1;
  }

  mysql = connect_mysql(
      runtime, host, port, user, password,
      database, ca_file, server_name, &error);
  if (mysql == NULL ||
      !scalar_i64(
          mysql, "SELECT 43", INT64_C(43), &error)) {
    fprintf(stderr,
            "reconnect MySQL beside PostgreSQL failed: %s\n",
            error.message);
    failed = 1;
  }

  orm_disconnect(postgresql);
  postgresql = NULL;
  if (mysql != NULL &&
      !scalar_i64(
          mysql, "SELECT 44", INT64_C(44), &error))
    failed = 1;

cleanup:
  orm_disconnect(postgresql);
  orm_disconnect(mysql);
  postgresql = NULL;
  mysql = NULL;

  orm_error_init(&error);
  if (runtime != NULL &&
      orm_runtime_close(runtime, &error) !=
          ORM_STATUS_OK) {
    fprintf(stderr, "close coexistence runtime failed: %s\n",
            error.message);
    failed = 1;
  }
  orm_runtime_release(runtime);
  return failed;
}

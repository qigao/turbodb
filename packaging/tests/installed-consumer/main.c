#include <orm.h>
#include <orm_runtime.h>
#include <tidessql/tidessql.h>
#include <turbodb_mysql.h>

#include <stdio.h>
#include <string.h>

static int verify_mysql_export(void) {
  mysql_session_error_t error = {0};
  const mysql_session_status_t status =
      mysql_session_connect_and_ping(NULL, &error);
  if (status != MYSQL_SESSION_INVALID ||
      error.status != MYSQL_SESSION_INVALID ||
      strcmp(error.stage, "config") != 0) {
    (void)fprintf(stderr,
                  "installed MySQL API mismatch: status=%d error=%d stage=%s\n",
                  (int)status, (int)error.status, error.stage);
    return 1;
  }
  return 0;
}

static int verify_orm_export(void) {
  orm_runtime_config_t config;
  orm_runtime_t *runtime = NULL;
  orm_driver_load_config_t load;
  orm_driver_info_t info;
  orm_error_t error;
  orm_status_t status;

  orm_runtime_config_init(&config);
  orm_error_init(&error);
  status = orm_runtime_create(&config, &runtime, &error);
  if (status != ORM_STATUS_OK || runtime == NULL) {
    (void)fprintf(stderr, "installed ORM create failed: status=%d message=%s\n",
                  (int)status, error.message);
    return 1;
  }

  memset(&load, 0, sizeof(load));
  load.struct_size = (uint32_t)sizeof(load);
  load.abi_version = ORM_RUNTIME_ABI_VERSION;
  load.module_path = orm_view(TURBODB_INSTALLED_MYSQL_DRIVER);
  load.expected_driver_id = orm_view("mysql");
  status = orm_runtime_load_driver(runtime, &load, &error);
  if (status != ORM_STATUS_OK) {
    (void)fprintf(stderr, "installed MySQL Driver load failed: status=%d message=%s\n",
                  (int)status, error.message);
    (void)orm_runtime_close(runtime, &error);
    orm_runtime_release(runtime);
    return 1;
  }

  memset(&info, 0, sizeof(info));
  status = orm_runtime_driver_info(runtime, orm_view("mysql"), &info, &error);
  if (status != ORM_STATUS_OK || info.canonical_id_size != 5u ||
      memcmp(info.canonical_id, "mysql", 5u) != 0 ||
      (info.capabilities & ORM_DRIVER_CAP_SERIALIZABLE) == 0u) {
    (void)fprintf(stderr,
                  "installed MySQL Driver metadata mismatch: status=%d id_size=%u capabilities=%llu message=%s\n",
                  (int)status, (unsigned int)info.canonical_id_size,
                  (unsigned long long)info.capabilities,
                  error.message);
    (void)orm_runtime_close(runtime, &error);
    orm_runtime_release(runtime);
    return 1;
  }

  status = orm_runtime_close(runtime, &error);
  orm_runtime_release(runtime);
  if (status != ORM_STATUS_OK) {
    (void)fprintf(stderr, "installed ORM close failed: status=%d message=%s\n",
                  (int)status, error.message);
    return 1;
  }
  return 0;
}

int main(void) {
  if (tdsql_abi_version() != TDSQL_ABI_VERSION) {
    (void)fprintf(stderr, "installed TidesSQL ABI mismatch: actual=%u expected=%u\n",
                  tdsql_abi_version(), TDSQL_ABI_VERSION);
    return 1;
  }
  if (verify_mysql_export() != 0)
    return 1;
  if (verify_orm_export() != 0)
    return 1;
  return 0;
}

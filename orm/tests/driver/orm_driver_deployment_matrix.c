#include <orm_runtime.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int load_one(orm_runtime_t *runtime, const char *id,
                    const char *path, orm_error_t *error) {
  orm_driver_load_config_t load;
  orm_driver_info_t info;

  memset(&load, 0, sizeof(load));
  load.struct_size = (uint32_t)sizeof(load);
  load.abi_version = ORM_RUNTIME_ABI_VERSION;
  load.module_path = orm_view(path);
  load.expected_driver_id = orm_view(id);
  if (orm_runtime_load_driver(runtime, &load, error) != ORM_STATUS_OK)
    return 0;

  memset(&info, 0, sizeof(info));
  if (orm_runtime_driver_info(runtime, orm_view(id), &info, error) !=
      ORM_STATUS_OK)
    return 0;
  return info.canonical_id_size == strlen(id) &&
         memcmp(info.canonical_id, id, info.canonical_id_size) == 0u;
}

int main(int argc, char **argv) {
  orm_runtime_config_t config;
  orm_runtime_t *runtime = NULL;
  orm_error_t error;
  int index;

  if (argc < 3 || ((argc - 1) % 2) != 0) {
    fprintf(stderr, "usage: %s <driver-id> <module-path> [<driver-id> <module-path> ...]\n",
            argc > 0 ? argv[0] : "orm_driver_deployment_matrix_test");
    return 2;
  }

  orm_runtime_config_init(&config);
  orm_error_init(&error);
  if (orm_runtime_create(&config, &runtime, &error) != ORM_STATUS_OK) {
    fprintf(stderr, "runtime create failed: %s\n", error.message);
    return 3;
  }

  for (index = 1; index + 1 < argc; index += 2) {
    if (!load_one(runtime, argv[index], argv[index + 1], &error)) {
      fprintf(stderr, "driver load failed for %s: %s\n",
              argv[index], error.message);
      orm_runtime_release(runtime);
      return 4;
    }
  }

  if (orm_runtime_close(runtime, &error) != ORM_STATUS_OK) {
    fprintf(stderr, "runtime close failed: %s\n", error.message);
    orm_runtime_release(runtime);
    return 5;
  }
  orm_runtime_release(runtime);
  return 0;
}

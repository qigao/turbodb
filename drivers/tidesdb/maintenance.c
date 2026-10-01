#include <orm_tidesdb.h>
#include <salts_fs.h>

#include "bridge.h"
#include "orm_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

enum { ORM_TIDESDB_PROVIDER_PATH_MAX_BYTES = 4096u };

static int tidesdb_maintenance_identity;

static orm_status_t tidesdb_maintenance_fail_fs(
    int code, const char *operation, orm_error_t *error) {
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
  const int native_error = code < 0 ? -code : code;
  (void)snprintf(message, sizeof(message), "%s: %s", operation,
                 strerror(native_error));
  orm_error_set(error, ORM_STATUS_DATASTORE_ERROR, message);
  return ORM_STATUS_DATASTORE_ERROR;
}

static orm_status_t tidesdb_maintenance_copy_path(
    orm_string_view_t value, const char *role, char *output,
    size_t output_size, orm_error_t *error) {
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
  if (!orm_view_valid(value, false) ||
      value.len > ORM_TIDESDB_PROVIDER_PATH_MAX_BYTES ||
      value.len + 1u > output_size ||
      memchr(value.data, '\0', value.len) != NULL) {
    (void)snprintf(message, sizeof(message), "invalid TidesDB %s path", role);
    orm_error_set(error,
                  value.len > ORM_TIDESDB_PROVIDER_PATH_MAX_BYTES
                      ? ORM_STATUS_LIMIT_EXCEEDED
                      : ORM_STATUS_INVALID_ARGUMENT,
                  message);
    return value.len > ORM_TIDESDB_PROVIDER_PATH_MAX_BYTES
               ? ORM_STATUS_LIMIT_EXCEEDED
               : ORM_STATUS_INVALID_ARGUMENT;
  }
  memcpy(output, value.data, value.len);
  output[value.len] = '\0';
  return ORM_STATUS_OK;
}

static int tidesdb_generation_id_valid(orm_string_view_t value) {
  size_t i;
  if (value.data == NULL || value.len == 0u ||
      value.len > ORM_TIDESDB_GENERATION_ID_MAX_BYTES)
    return 0;
  {
    const unsigned char first = (const unsigned char)value.data[0];
    if (!((first >= 'a' && first <= 'z') ||
          (first >= '0' && first <= '9')))
      return 0;
  }
  for (i = 1u; i < value.len; ++i) {
    const unsigned char c = (const unsigned char)value.data[i];
    if (!((c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-'))
      return 0;
  }
  return 1;
}

static orm_status_t tidesdb_copy_generation_id(
    orm_string_view_t value, char out[ORM_TIDESDB_GENERATION_ID_CAPACITY],
    uint32_t *out_size, orm_error_t *error) {
  if (!tidesdb_generation_id_valid(value)) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid TidesDB generation ID");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  memset(out, 0, ORM_TIDESDB_GENERATION_ID_CAPACITY);
  memcpy(out, value.data, value.len);
  if (out_size != NULL) *out_size = (uint32_t)value.len;
  return ORM_STATUS_OK;
}

static orm_status_t tidesdb_join_path(
    char out[ORM_TIDESDB_PROVIDER_PATH_MAX_BYTES + 1u],
    const char *base, const char *child, const char *role,
    orm_error_t *error) {
  if (salts_fs_path_join(
          out, ORM_TIDESDB_PROVIDER_PATH_MAX_BYTES + 1u,
          base, child) != 0) {
    char message[ORM_C_ERROR_MESSAGE_CAPACITY];
    (void)snprintf(message, sizeof(message), "TidesDB %s path exceeds limit",
                   role);
    orm_error_set(error, ORM_STATUS_LIMIT_EXCEEDED, message);
    return ORM_STATUS_LIMIT_EXCEEDED;
  }
  return ORM_STATUS_OK;
}

static orm_status_t tidesdb_require_directory(
    const char *path, const char *role, orm_error_t *error) {
  salts_fs_stat_t stat;
  const int rc = salts_fs_lstat(path, &stat);
  if (rc != 0)
    return tidesdb_maintenance_fail_fs(rc, role, error);
  if (!stat.is_directory || stat.is_symlink) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "TidesDB provider path must be a real directory");
    return ORM_STATUS_INVALID_STATE;
  }
  return ORM_STATUS_OK;
}

static orm_status_t tidesdb_require_nonempty_directory(
    const char *path, orm_error_t *error) {
  salts_fs_dir_t *dir = NULL;
  salts_fs_dirent_t entry;
  int rc = salts_fs_opendir(path, &dir);
  if (rc != 0)
    return tidesdb_maintenance_fail_fs(
        rc, "open TidesDB generation directory", error);
  rc = salts_fs_readdir(dir, &entry);
  {
    const int close_rc = salts_fs_closedir(dir);
    if (rc < 0)
      return tidesdb_maintenance_fail_fs(
          rc, "read TidesDB generation directory", error);
    if (close_rc != 0)
      return tidesdb_maintenance_fail_fs(
          close_rc, "close TidesDB generation directory", error);
  }
  if (rc == 0) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "TidesDB generation directory is empty");
    return ORM_STATUS_INVALID_STATE;
  }
  return ORM_STATUS_OK;
}

static orm_status_t tidesdb_validate_generation(
    const char *generation_path, orm_error_t *error) {
  orm_tidesdb_config_t config = orm_tidesdb_default_config();
  orm_tidesdb_database_t *database = NULL;
  int native_status;

  orm_status_t status = tidesdb_require_directory(
      generation_path, "inspect TidesDB generation directory", error);
  if (status != ORM_STATUS_OK) return status;
  status = tidesdb_require_nonempty_directory(generation_path, error);
  if (status != ORM_STATUS_OK) return status;

  config.db_path = (char *)generation_path;
  native_status = orm_tidesdb_open(&config, &database);
  if (native_status != ORM_TDB_SUCCESS || database == NULL) {
    orm_error_set(error, ORM_STATUS_DATASTORE_ERROR,
                  "TidesDB generation validation open failed");
    return ORM_STATUS_DATASTORE_ERROR;
  }
  native_status = orm_tidesdb_close(database);
  if (native_status != ORM_TDB_SUCCESS) {
    orm_error_set(error, ORM_STATUS_CLEANUP_FAILED,
                  "TidesDB generation validation close failed");
    return ORM_STATUS_CLEANUP_FAILED;
  }
  return ORM_STATUS_OK;
}

static orm_status_t tidesdb_prepare_provider_paths(
    orm_string_view_t provider_root,
    char root[ORM_TIDESDB_PROVIDER_PATH_MAX_BYTES + 1u],
    char generations[ORM_TIDESDB_PROVIDER_PATH_MAX_BYTES + 1u],
    orm_error_t *error) {
  orm_status_t status = tidesdb_maintenance_copy_path(
      provider_root, "provider root", root,
      ORM_TIDESDB_PROVIDER_PATH_MAX_BYTES + 1u, error);
  if (status != ORM_STATUS_OK) return status;
  if (!salts_fs_path_is_absolute(root)) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "TidesDB provider root must be absolute");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  status = tidesdb_require_directory(
      root, "inspect TidesDB provider root", error);
  if (status != ORM_STATUS_OK) return status;
  status = tidesdb_join_path(
      generations, root, "generations", "generations", error);
  if (status != ORM_STATUS_OK) return status;
  return tidesdb_require_directory(
      generations, "inspect TidesDB generations directory", error);
}

static void tidesdb_map_publication_state(
    salts_fs_replace_state_t source,
    orm_tidesdb_generation_publish_result *result) {
  switch (source) {
  case SALTS_FS_REPLACE_PUBLISHED_DURABLE:
    result->publication_state = ORM_TIDESDB_PUBLICATION_PUBLISHED_DURABLE;
    break;
  case SALTS_FS_REPLACE_DURABILITY_UNKNOWN:
    result->publication_state = ORM_TIDESDB_PUBLICATION_DURABILITY_UNKNOWN;
    break;
  case SALTS_FS_REPLACE_NOT_PUBLISHED:
  default:
    result->publication_state = ORM_TIDESDB_PUBLICATION_NOT_PUBLISHED;
    break;
  }
}

static orm_status_t tidesdb_validate_publish_request(
    const orm_tidesdb_generation_publish_config *config,
    orm_tidesdb_generation_publish_result *result,
    char root[ORM_TIDESDB_PROVIDER_PATH_MAX_BYTES + 1u],
    char generations[ORM_TIDESDB_PROVIDER_PATH_MAX_BYTES + 1u],
    char generation_path[ORM_TIDESDB_PROVIDER_PATH_MAX_BYTES + 1u],
    orm_error_t *error) {
  char generation[ORM_TIDESDB_GENERATION_ID_CAPACITY];
  orm_status_t status;

  if (config == NULL || result == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "TidesDB publish config and result are required");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  if (config->struct_size < sizeof(*config) ||
      config->abi_version != ORM_TIDESDB_GENERATION_ABI_VERSION ||
      result->struct_size < sizeof(*result) ||
      result->abi_version != ORM_TIDESDB_GENERATION_ABI_VERSION) {
    orm_error_set(error, ORM_STATUS_ABI_MISMATCH,
                  "TidesDB generation publication ABI mismatch");
    return ORM_STATUS_ABI_MISMATCH;
  }
  if (config->flags != 0u || config->reserved != 0u) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "TidesDB generation publication flags are unsupported");
    return ORM_STATUS_INVALID_ARGUMENT;
  }

  result->publication_state = ORM_TIDESDB_PUBLICATION_NOT_PUBLISHED;
  result->generation_size = 0u;
  memset(result->generation, 0, sizeof(result->generation));

  status = tidesdb_prepare_provider_paths(
      config->provider_root, root, generations, error);
  if (status != ORM_STATUS_OK) return status;

  status = tidesdb_copy_generation_id(
      config->generation_id, generation,
      &result->generation_size, error);
  if (status != ORM_STATUS_OK) return status;
  memcpy(result->generation, generation, result->generation_size);

  status = tidesdb_join_path(
      generation_path, generations, generation, "generation", error);
  if (status != ORM_STATUS_OK) return status;
  return tidesdb_validate_generation(generation_path, error);
}

static orm_status_t ORM_DRIVER_CALL tidesdb_publish_generation(
    void *self, const orm_tidesdb_generation_publish_config *config,
    orm_tidesdb_generation_publish_result *result, orm_error_t *error) {
  char root[ORM_TIDESDB_PROVIDER_PATH_MAX_BYTES + 1u];
  char generations[ORM_TIDESDB_PROVIDER_PATH_MAX_BYTES + 1u];
  char generation_path[ORM_TIDESDB_PROVIDER_PATH_MAX_BYTES + 1u];
  char active_path[ORM_TIDESDB_PROVIDER_PATH_MAX_BYTES + 1u];
  char stage_path[ORM_TIDESDB_PROVIDER_PATH_MAX_BYTES + 1u];
  char lock_path[ORM_TIDESDB_PROVIDER_PATH_MAX_BYTES + 1u];
  salts_file_t lock_file = SALTS_INVALID_FILE;
  salts_fs_replace_state_t replace_state = SALTS_FS_REPLACE_NOT_PUBLISHED;
  orm_status_t status;
  int rc;
  int locked = 0;

  if (self != &tidesdb_maintenance_identity)
    return ORM_STATUS_INVALID_ARGUMENT;

  status = tidesdb_validate_publish_request(
      config, result, root, generations, generation_path, error);
  if (status != ORM_STATUS_OK) return status;

  status = tidesdb_join_path(active_path, root, "ACTIVE", "ACTIVE", error);
  if (status != ORM_STATUS_OK) return status;
  status = tidesdb_join_path(
      stage_path, root, "ACTIVE.stage", "ACTIVE staging", error);
  if (status != ORM_STATUS_OK) return status;
  status = tidesdb_join_path(
      lock_path, root, "ACTIVE.lock", "ACTIVE lock", error);
  if (status != ORM_STATUS_OK) return status;

  lock_file = salts_fs_open(
      lock_path, SALTS_FS_O_RDWR | SALTS_FS_O_CREAT,
      SALTS_FS_DEFAULT_MODE);
  if (lock_file == SALTS_INVALID_FILE) {
    orm_error_set(error, ORM_STATUS_DATASTORE_ERROR,
                  "open TidesDB ACTIVE lock failed");
    return ORM_STATUS_DATASTORE_ERROR;
  }
  rc = salts_fs_lock(
      lock_file, SALTS_FS_LOCK_EXCLUSIVE | SALTS_FS_LOCK_NONBLOCK, 0, 0);
  if (rc != 0) {
    (void)salts_fs_close(lock_file);
    orm_error_set(error, ORM_STATUS_BUSY,
                  "TidesDB ACTIVE publication is busy");
    return ORM_STATUS_BUSY;
  }
  locked = 1;

  rc = salts_fs_access(stage_path, SALTS_FS_ACCESS_EXISTS);
  if (rc == 0) {
    rc = salts_fs_unlink(stage_path);
    if (rc != 0) {
      status = tidesdb_maintenance_fail_fs(
          rc, "remove stale TidesDB ACTIVE staging file", error);
      goto cleanup;
    }
  } else if (rc != -ENOENT) {
    status = tidesdb_maintenance_fail_fs(
        rc, "inspect TidesDB ACTIVE staging file", error);
    goto cleanup;
  }

  {
    salts_fs_buf_t bytes = salts_fs_buf_init(
        result->generation, result->generation_size);
    rc = salts_fs_write_file(stage_path, &bytes);
    if (rc != 0) {
      status = tidesdb_maintenance_fail_fs(
          rc, "write TidesDB ACTIVE staging file", error);
      goto cleanup;
    }
  }

  rc = salts_fs_replace_durable(stage_path, active_path, &replace_state);
  tidesdb_map_publication_state(replace_state, result);
  if (rc != 0) {
    if (replace_state == SALTS_FS_REPLACE_DURABILITY_UNKNOWN) {
      orm_error_set(
          error, ORM_STATUS_COMMIT_UNKNOWN,
          "TidesDB ACTIVE publication durability is unknown; resolve ACTIVE");
      status = ORM_STATUS_COMMIT_UNKNOWN;
    } else {
      status = tidesdb_maintenance_fail_fs(
          rc, "publish TidesDB ACTIVE pointer", error);
    }
    goto cleanup;
  }
  if (replace_state != SALTS_FS_REPLACE_PUBLISHED_DURABLE) {
    orm_error_set(error, ORM_STATUS_INTERNAL_ERROR,
                  "TidesDB ACTIVE publication returned invalid success state");
    status = ORM_STATUS_INTERNAL_ERROR;
    goto cleanup;
  }

  orm_error_set(error, ORM_STATUS_OK, NULL);
  status = ORM_STATUS_OK;

cleanup:
  if (locked) {
    const int unlock_status = salts_fs_unlock(lock_file, 0, 0);
    if (status == ORM_STATUS_OK && unlock_status != 0) {
      orm_error_set(error, ORM_STATUS_CLEANUP_FAILED,
                    "unlock TidesDB ACTIVE publication failed");
      status = ORM_STATUS_CLEANUP_FAILED;
    }
  }
  if (lock_file != SALTS_INVALID_FILE) {
    const int close_status = salts_fs_close(lock_file);
    if (status == ORM_STATUS_OK && close_status != 0) {
      orm_error_set(error, ORM_STATUS_CLEANUP_FAILED,
                    "close TidesDB ACTIVE lock failed");
      status = ORM_STATUS_CLEANUP_FAILED;
    }
  }
  return status;
}

static orm_status_t ORM_DRIVER_CALL tidesdb_resolve_active(
    void *self, const orm_tidesdb_active_resolve_config *config,
    orm_tidesdb_active_result *result, orm_error_t *error) {
  char root[ORM_TIDESDB_PROVIDER_PATH_MAX_BYTES + 1u];
  char generations[ORM_TIDESDB_PROVIDER_PATH_MAX_BYTES + 1u];
  char generation_path[ORM_TIDESDB_PROVIDER_PATH_MAX_BYTES + 1u];
  char active_path[ORM_TIDESDB_PROVIDER_PATH_MAX_BYTES + 1u];
  char lock_path[ORM_TIDESDB_PROVIDER_PATH_MAX_BYTES + 1u];
  salts_fs_stat_t active_stat;
  salts_fs_buf_t active_bytes = {0};
  salts_file_t lock_file = SALTS_INVALID_FILE;
  orm_status_t status;
  int rc;
  int locked = 0;

  if (self != &tidesdb_maintenance_identity)
    return ORM_STATUS_INVALID_ARGUMENT;
  if (config == NULL || result == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "TidesDB resolve config and result are required");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  if (config->struct_size < sizeof(*config) ||
      config->abi_version != ORM_TIDESDB_GENERATION_ABI_VERSION ||
      result->struct_size < sizeof(*result) ||
      result->abi_version != ORM_TIDESDB_GENERATION_ABI_VERSION) {
    orm_error_set(error, ORM_STATUS_ABI_MISMATCH,
                  "TidesDB ACTIVE resolution ABI mismatch");
    return ORM_STATUS_ABI_MISMATCH;
  }
  if (config->flags != 0u || config->reserved != 0u) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "TidesDB ACTIVE resolution flags are unsupported");
    return ORM_STATUS_INVALID_ARGUMENT;
  }

  result->generation_size = 0u;
  result->reserved = 0u;
  memset(result->generation, 0, sizeof(result->generation));

  status = tidesdb_prepare_provider_paths(
      config->provider_root, root, generations, error);
  if (status != ORM_STATUS_OK) return status;
  status = tidesdb_join_path(active_path, root, "ACTIVE", "ACTIVE", error);
  if (status != ORM_STATUS_OK) return status;
  status = tidesdb_join_path(
      lock_path, root, "ACTIVE.lock", "ACTIVE lock", error);
  if (status != ORM_STATUS_OK) return status;

  lock_file = salts_fs_open(
      lock_path, SALTS_FS_O_RDWR | SALTS_FS_O_CREAT,
      SALTS_FS_DEFAULT_MODE);
  if (lock_file == SALTS_INVALID_FILE) {
    orm_error_set(error, ORM_STATUS_DATASTORE_ERROR,
                  "open TidesDB ACTIVE lock failed");
    return ORM_STATUS_DATASTORE_ERROR;
  }
  rc = salts_fs_lock(
      lock_file, SALTS_FS_LOCK_SHARED | SALTS_FS_LOCK_NONBLOCK, 0, 0);
  if (rc != 0) {
    (void)salts_fs_close(lock_file);
    orm_error_set(error, ORM_STATUS_BUSY,
                  "TidesDB ACTIVE resolution is busy");
    return ORM_STATUS_BUSY;
  }
  locked = 1;

  rc = salts_fs_lstat(active_path, &active_stat);
  if (rc != 0) {
    status = tidesdb_maintenance_fail_fs(
        rc, "inspect TidesDB ACTIVE pointer", error);
    goto cleanup;
  }
  if (!active_stat.is_file || active_stat.is_symlink ||
      active_stat.size == 0u ||
      active_stat.size > ORM_TIDESDB_GENERATION_ID_MAX_BYTES) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "TidesDB ACTIVE pointer is invalid");
    status = ORM_STATUS_INVALID_STATE;
    goto cleanup;
  }

  rc = salts_fs_read_file(active_path, &active_bytes);
  if (rc != 0) {
    status = tidesdb_maintenance_fail_fs(
        rc, "read TidesDB ACTIVE pointer", error);
    goto cleanup;
  }
  {
    const orm_string_view_t generation = {
        active_bytes.base, active_bytes.len};
    status = tidesdb_copy_generation_id(
        generation, result->generation, &result->generation_size, error);
  }
  salts_fs_buf_free(&active_bytes);
  active_bytes.base = NULL;
  active_bytes.len = 0u;
  if (status != ORM_STATUS_OK) goto cleanup;

  status = tidesdb_join_path(
      generation_path, generations, result->generation,
      "active generation", error);
  if (status != ORM_STATUS_OK) goto cleanup;

  status = tidesdb_require_directory(
      generation_path, "inspect active TidesDB generation", error);
  if (status != ORM_STATUS_OK) goto cleanup;
  status = tidesdb_require_nonempty_directory(generation_path, error);
  if (status != ORM_STATUS_OK) goto cleanup;

  orm_error_set(error, ORM_STATUS_OK, NULL);
  status = ORM_STATUS_OK;

cleanup:
  if (active_bytes.base != NULL)
    salts_fs_buf_free(&active_bytes);
  if (locked) {
    const int unlock_status = salts_fs_unlock(lock_file, 0, 0);
    if (status == ORM_STATUS_OK && unlock_status != 0) {
      orm_error_set(error, ORM_STATUS_CLEANUP_FAILED,
                    "unlock TidesDB ACTIVE resolution failed");
      status = ORM_STATUS_CLEANUP_FAILED;
    }
  }
  if (lock_file != SALTS_INVALID_FILE) {
    const int close_status = salts_fs_close(lock_file);
    if (status == ORM_STATUS_OK && close_status != 0) {
      orm_error_set(error, ORM_STATUS_CLEANUP_FAILED,
                    "close TidesDB ACTIVE resolution lock failed");
      status = ORM_STATUS_CLEANUP_FAILED;
    }
  }
  return status;
}

static const TurboDb_TidesMaintenance_vtable tidesdb_maintenance_vtable = {
    .publish_generation = tidesdb_publish_generation,
    .resolve_active = tidesdb_resolve_active};

TurboDb_TidesMaintenance orm_tidesdb_maintenance = {
    &tidesdb_maintenance_identity, &tidesdb_maintenance_vtable};

#include <orm_tidesdb.h>
#include <salts_fs.h>

#include "bridge.h"
#include "../../abi/orm_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

enum {
  ORM_TIDESDB_MAINTENANCE_PATH_MAX_BYTES =
      ORM_TIDESDB_GENERATION_PATH_MAX_BYTES
};

static int tidesdb_maintenance_identity;

static orm_status_t tidesdb_maintenance_fail(
    orm_error_t *error, orm_status_t status, const char *message) {
  orm_error_set(error, status, message);
  return status;
}

static orm_status_t tidesdb_maintenance_fail_fs(
    orm_error_t *error, int code, const char *operation) {
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
  const int native_error = code < 0 ? -code : code;
  (void)snprintf(message, sizeof(message), "%s: %s", operation,
                 strerror(native_error));
  return tidesdb_maintenance_fail(
      error, ORM_STATUS_DATASTORE_ERROR, message);
}

static orm_status_t tidesdb_maintenance_fail_native(
    orm_error_t *error, int code, const char *operation) {
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
  orm_status_t status = ORM_STATUS_DATASTORE_ERROR;
  if (code == ORM_TDB_ERR_MEMORY)
    status = ORM_STATUS_OUT_OF_MEMORY;
  else if (code == ORM_TDB_ERR_INVALID_ARGS)
    status = ORM_STATUS_INVALID_ARGUMENT;
  else if (code == ORM_TDB_ERR_EXISTS)
    status = ORM_STATUS_INVALID_STATE;
  else if (code == ORM_TDB_ERR_TOO_LARGE ||
           code == ORM_TDB_ERR_MEMORY_LIMIT)
    status = ORM_STATUS_LIMIT_EXCEEDED;
  else if (code == ORM_TDB_ERR_LOCKED ||
           code == ORM_TDB_ERR_BUSY ||
           code == ORM_TDB_ERR_CONFLICT ||
           code == ORM_TDB_ERR_PRECONDITION)
    status = ORM_STATUS_BUSY;
  (void)snprintf(message, sizeof(message), "%s: TidesDB error %d",
                 operation, code);
  return tidesdb_maintenance_fail(error, status, message);
}

static orm_status_t tidesdb_maintenance_copy_view(
    orm_string_view_t input, const char *role,
    char *output, size_t capacity, orm_error_t *error) {
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
  if (input.data == NULL || input.len == 0u ||
      input.len >= capacity ||
      memchr(input.data, '\0', input.len) != NULL) {
    (void)snprintf(message, sizeof(message),
                   "invalid TidesDB %s", role);
    return tidesdb_maintenance_fail(
        error,
        input.len >= capacity ? ORM_STATUS_LIMIT_EXCEEDED
                              : ORM_STATUS_INVALID_ARGUMENT,
        message);
  }
  memcpy(output, input.data, input.len);
  output[input.len] = '\0';
  return ORM_STATUS_OK;
}

static int tidesdb_generation_id_valid_bytes(
    const char *id, size_t size) {
  size_t index;
  if (id == NULL || size == 0u ||
      size > ORM_TIDESDB_GENERATION_ID_MAX_BYTES)
    return 0;
  if ((size == 1u && id[0] == '.') ||
      (size == 2u && id[0] == '.' && id[1] == '.'))
    return 0;
  for (index = 0u; index < size; ++index) {
    const unsigned char c = (unsigned char)id[index];
    const int alpha = (c >= (unsigned char)'a' &&
                       c <= (unsigned char)'z') ||
                      (c >= (unsigned char)'A' &&
                       c <= (unsigned char)'Z');
    const int digit = c >= (unsigned char)'0' &&
                      c <= (unsigned char)'9';
    if (!(alpha || digit || c == (unsigned char)'-' ||
          c == (unsigned char)'_' || c == (unsigned char)'.'))
      return 0;
  }
  return 1;
}

static orm_status_t tidesdb_maintenance_generation_id(
    orm_string_view_t input, char *output,
    size_t capacity, orm_error_t *error) {
  orm_status_t status = tidesdb_maintenance_copy_view(
      input, "generation ID", output, capacity, error);
  if (status != ORM_STATUS_OK) return status;
  if (!tidesdb_generation_id_valid_bytes(output, input.len))
    return tidesdb_maintenance_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "TidesDB generation ID must be one safe path segment");
  return ORM_STATUS_OK;
}

static orm_status_t tidesdb_maintenance_join(
    const char *base, const char *name,
    char *output, size_t capacity,
    orm_error_t *error) {
  if (salts_fs_path_join(output, capacity, base, name) != 0)
    return tidesdb_maintenance_fail(
        error, ORM_STATUS_LIMIT_EXCEEDED,
        "TidesDB generation path exceeds configured capacity");
  return ORM_STATUS_OK;
}

static orm_status_t tidesdb_maintenance_require_directory(
    const char *path, const char *role, orm_error_t *error) {
  salts_fs_stat_t stat;
  const int code = salts_fs_lstat(path, &stat);
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
  if (code != 0)
    return tidesdb_maintenance_fail_fs(error, code, role);
  if (!stat.is_directory || stat.is_symlink) {
    (void)snprintf(message, sizeof(message),
                   "%s must be a real directory", role);
    return tidesdb_maintenance_fail(
        error, ORM_STATUS_INVALID_ARGUMENT, message);
  }
  return ORM_STATUS_OK;
}

static orm_status_t tidesdb_maintenance_ensure_generations(
    const char *provider_root, char *generations_path,
    size_t capacity, orm_error_t *error) {
  salts_fs_stat_t stat;
  orm_status_t status = tidesdb_maintenance_require_directory(
      provider_root, "inspect TidesDB provider root", error);
  if (status != ORM_STATUS_OK) return status;
  status = tidesdb_maintenance_join(
      provider_root, ORM_TIDESDB_GENERATIONS_DIR_NAME,
      generations_path, capacity, error);
  if (status != ORM_STATUS_OK) return status;
  const int code = salts_fs_lstat(generations_path, &stat);
  if (code == -ENOENT) {
    const int mkdir_status = salts_fs_mkdir(generations_path, 0755);
    if (mkdir_status != 0)
      return tidesdb_maintenance_fail_fs(
          error, mkdir_status,
          "create TidesDB generations directory");
    return ORM_STATUS_OK;
  }
  if (code != 0)
    return tidesdb_maintenance_fail_fs(
        error, code, "inspect TidesDB generations directory");
  if (!stat.is_directory || stat.is_symlink)
    return tidesdb_maintenance_fail(
        error, ORM_STATUS_INVALID_STATE,
        "TidesDB generations path must be a real directory");
  return ORM_STATUS_OK;
}

static orm_status_t tidesdb_maintenance_require_absent(
    const char *path, const char *message, orm_error_t *error) {
  salts_fs_stat_t stat;
  const int code = salts_fs_lstat(path, &stat);
  if (code == -ENOENT) return ORM_STATUS_OK;
  if (code != 0)
    return tidesdb_maintenance_fail_fs(
        error, code, "inspect TidesDB publication path");
  return tidesdb_maintenance_fail(
      error, ORM_STATUS_INVALID_STATE, message);
}

static orm_status_t tidesdb_maintenance_open_validate(
    char *path, orm_error_t *error) {
  orm_tidesdb_config_t config = orm_tidesdb_default_config();
  orm_tidesdb_database_t *database = NULL;
  int code;
  config.db_path = path;
  code = orm_tidesdb_open(&config, &database);
  if (code != ORM_TDB_SUCCESS)
    return tidesdb_maintenance_fail_native(
        error, code, "open TidesDB generation for validation");
  code = orm_tidesdb_close(database);
  if (code != ORM_TDB_SUCCESS)
    return tidesdb_maintenance_fail_native(
        error, code, "close TidesDB generation after validation");
  return ORM_STATUS_OK;
}

static void tidesdb_maintenance_map_publication(
    salts_fs_replace_state_t state,
    orm_tidesdb_restore_result *result) {
  if (state == SALTS_FS_REPLACE_PUBLISHED_DURABLE)
    result->publication_state =
        ORM_TIDESDB_PUBLICATION_PUBLISHED_DURABLE;
  else if (state == SALTS_FS_REPLACE_DURABILITY_UNKNOWN)
    result->publication_state =
        ORM_TIDESDB_PUBLICATION_DURABILITY_UNKNOWN;
  else
    result->publication_state =
        ORM_TIDESDB_PUBLICATION_NOT_PUBLISHED;
}

static orm_status_t tidesdb_maintenance_validate_restore(
    const orm_tidesdb_generation_config *config,
    orm_tidesdb_restore_result *result,
    char *provider_root, char *checkpoint_path,
    char *generation_id, orm_error_t *error) {
  orm_status_t status;
  if (config == NULL || result == NULL)
    return tidesdb_maintenance_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "TidesDB restore config and result are required");
  if (config->struct_size < sizeof(*config) ||
      config->abi_version != ORM_TIDESDB_GENERATION_ABI_VERSION ||
      result->struct_size < sizeof(*result) ||
      result->abi_version != ORM_TIDESDB_GENERATION_ABI_VERSION)
    return tidesdb_maintenance_fail(
        error, ORM_STATUS_ABI_MISMATCH,
        "TidesDB generation ABI does not match this build");
  if (config->reserved != 0u)
    return tidesdb_maintenance_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "TidesDB generation reserved field must be zero");
  result->publication_state =
      ORM_TIDESDB_PUBLICATION_NOT_PUBLISHED;
  result->reserved = 0u;
  result->generation_path[0] = '\0';

  status = tidesdb_maintenance_copy_view(
      config->provider_root, "provider root",
      provider_root,
      ORM_TIDESDB_MAINTENANCE_PATH_MAX_BYTES + 1u, error);
  if (status != ORM_STATUS_OK) return status;
  status = tidesdb_maintenance_copy_view(
      config->checkpoint_path, "checkpoint path",
      checkpoint_path,
      ORM_TIDESDB_MAINTENANCE_PATH_MAX_BYTES + 1u, error);
  if (status != ORM_STATUS_OK) return status;
  status = tidesdb_maintenance_generation_id(
      config->generation_id, generation_id,
      ORM_TIDESDB_GENERATION_ID_MAX_BYTES + 1u, error);
  if (status != ORM_STATUS_OK) return status;
  status = tidesdb_maintenance_require_directory(
      checkpoint_path, "inspect TidesDB checkpoint", error);
  if (status != ORM_STATUS_OK) return status;
  return ORM_STATUS_OK;
}

static orm_status_t ORM_DRIVER_CALL tidesdb_maintenance_restore_publish(
    void *self, const orm_tidesdb_generation_config *config,
    orm_tidesdb_restore_result *result, orm_error_t *error) {
  char provider_root[ORM_TIDESDB_MAINTENANCE_PATH_MAX_BYTES + 1u];
  char checkpoint_path[ORM_TIDESDB_MAINTENANCE_PATH_MAX_BYTES + 1u];
  char generations_path[ORM_TIDESDB_MAINTENANCE_PATH_MAX_BYTES + 1u];
  char generation_id[ORM_TIDESDB_GENERATION_ID_MAX_BYTES + 1u];
  char generation_path[ORM_TIDESDB_MAINTENANCE_PATH_MAX_BYTES + 1u];
  char active_path[ORM_TIDESDB_MAINTENANCE_PATH_MAX_BYTES + 1u];
  char active_stage_name[ORM_TIDESDB_GENERATION_ID_MAX_BYTES + 32u];
  char active_stage_path[ORM_TIDESDB_MAINTENANCE_PATH_MAX_BYTES + 1u];
  orm_tidesdb_config_t source_config;
  orm_tidesdb_database_t *source = NULL;
  salts_fs_replace_state_t replace_state =
      SALTS_FS_REPLACE_NOT_PUBLISHED;
  orm_status_t status;
  int code;

  if (self != &tidesdb_maintenance_identity)
    return tidesdb_maintenance_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "invalid TidesDB maintenance binding");

  status = tidesdb_maintenance_validate_restore(
      config, result, provider_root, checkpoint_path,
      generation_id, error);
  if (status != ORM_STATUS_OK) return status;
  status = tidesdb_maintenance_ensure_generations(
      provider_root, generations_path,
      sizeof(generations_path), error);
  if (status != ORM_STATUS_OK) return status;
  status = tidesdb_maintenance_join(
      generations_path, generation_id, generation_path,
      sizeof(generation_path), error);
  if (status != ORM_STATUS_OK) return status;
  status = tidesdb_maintenance_require_absent(
      generation_path,
      "TidesDB generation already exists", error);
  if (status != ORM_STATUS_OK) return status;

  source_config = orm_tidesdb_default_config();
  source_config.db_path = checkpoint_path;
  code = orm_tidesdb_open(&source_config, &source);
  if (code != ORM_TDB_SUCCESS)
    return tidesdb_maintenance_fail_native(
        error, code, "open completed TidesDB checkpoint");
  code = orm_tidesdb_checkpoint(source, generation_path);
  {
    const int close_code = orm_tidesdb_close(source);
    source = NULL;
    if (code == ORM_TDB_SUCCESS &&
        close_code != ORM_TDB_SUCCESS)
      code = close_code;
  }
  if (code != ORM_TDB_SUCCESS)
    return tidesdb_maintenance_fail_native(
        error, code,
        "materialize immutable TidesDB generation");

  status = tidesdb_maintenance_open_validate(
      generation_path, error);
  if (status != ORM_STATUS_OK) return status;

  status = tidesdb_maintenance_join(
      provider_root, ORM_TIDESDB_ACTIVE_FILE_NAME,
      active_path, sizeof(active_path), error);
  if (status != ORM_STATUS_OK) return status;
  (void)snprintf(active_stage_name, sizeof(active_stage_name),
                 "ACTIVE.%s.stage", generation_id);
  status = tidesdb_maintenance_join(
      provider_root, active_stage_name,
      active_stage_path, sizeof(active_stage_path), error);
  if (status != ORM_STATUS_OK) return status;
  status = tidesdb_maintenance_require_absent(
      active_stage_path,
      "TidesDB ACTIVE staging file already exists", error);
  if (status != ORM_STATUS_OK) return status;

  {
    salts_fs_buf_t active_data =
        salts_fs_buf_init(generation_id, strlen(generation_id));
    code = salts_fs_write_file(active_stage_path, &active_data);
  }
  if (code != 0)
    return tidesdb_maintenance_fail_fs(
        error, code, "write TidesDB ACTIVE staging file");

  code = salts_fs_replace_durable(
      active_stage_path, active_path, &replace_state);
  tidesdb_maintenance_map_publication(
      replace_state, result);
  (void)snprintf(result->generation_path,
                 sizeof(result->generation_path), "%s",
                 generation_path);
  if (code != 0)
    return tidesdb_maintenance_fail_fs(
        error, code, "publish TidesDB ACTIVE generation");
  if (replace_state != SALTS_FS_REPLACE_PUBLISHED_DURABLE)
    return tidesdb_maintenance_fail(
        error, ORM_STATUS_INTERNAL_ERROR,
        "TidesDB ACTIVE publication returned invalid success state");

  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

static orm_status_t tidesdb_maintenance_read_active_id(
    const char *active_path, char *generation_id,
    orm_tidesdb_active_result *result, orm_error_t *error) {
  salts_fs_stat_t stat;
  salts_file_t file;
  int code;
  int second;

  code = salts_fs_lstat(active_path, &stat);
  if (code == -ENOENT) {
    result->found = 0u;
    orm_error_set(error, ORM_STATUS_OK, NULL);
    return ORM_STATUS_OK;
  }
  if (code != 0)
    return tidesdb_maintenance_fail_fs(
        error, code, "inspect TidesDB ACTIVE file");
  if (!stat.is_file || stat.is_symlink)
    return tidesdb_maintenance_fail(
        error, ORM_STATUS_INVALID_STATE,
        "TidesDB ACTIVE must be a regular file");
  if (stat.size == 0u ||
      stat.size > ORM_TIDESDB_GENERATION_ID_MAX_BYTES)
    return tidesdb_maintenance_fail(
        error, ORM_STATUS_LIMIT_EXCEEDED,
        "TidesDB ACTIVE generation ID is outside the bounded size");

  file = salts_fs_open(active_path, SALTS_FS_O_RDONLY, 0);
  if (file == SALTS_INVALID_FILE)
    return tidesdb_maintenance_fail(
        error, ORM_STATUS_DATASTORE_ERROR,
        "open TidesDB ACTIVE file");
  code = salts_fs_read(
      file, generation_id,
      ORM_TIDESDB_GENERATION_ID_MAX_BYTES + 1u);
  if (code < 0) {
    (void)salts_fs_close(file);
    return tidesdb_maintenance_fail_fs(
        error, code, "read TidesDB ACTIVE file");
  }
  if ((uint32_t)code > ORM_TIDESDB_GENERATION_ID_MAX_BYTES) {
    (void)salts_fs_close(file);
    return tidesdb_maintenance_fail(
        error, ORM_STATUS_LIMIT_EXCEEDED,
        "TidesDB ACTIVE generation ID exceeds bounded size");
  }
  second = salts_fs_read(file, generation_id + code, 1u);
  (void)salts_fs_close(file);
  if (second < 0)
    return tidesdb_maintenance_fail_fs(
        error, second, "finish TidesDB ACTIVE read");
  if (second != 0)
    return tidesdb_maintenance_fail(
        error, ORM_STATUS_LIMIT_EXCEEDED,
        "TidesDB ACTIVE file has trailing data");
  if (code == 0 ||
      !tidesdb_generation_id_valid_bytes(
          generation_id, (size_t)code))
    return tidesdb_maintenance_fail(
        error, ORM_STATUS_INVALID_STATE,
        "TidesDB ACTIVE contains an invalid generation ID");
  generation_id[code] = '\0';
  return ORM_STATUS_OK;
}

static orm_status_t ORM_DRIVER_CALL tidesdb_maintenance_resolve_active(
    void *self, const orm_tidesdb_generation_config *config,
    orm_tidesdb_active_result *result, orm_error_t *error) {
  char provider_root[ORM_TIDESDB_MAINTENANCE_PATH_MAX_BYTES + 1u];
  char generations_path[ORM_TIDESDB_MAINTENANCE_PATH_MAX_BYTES + 1u];
  char active_path[ORM_TIDESDB_MAINTENANCE_PATH_MAX_BYTES + 1u];
  char generation_id[ORM_TIDESDB_GENERATION_ID_MAX_BYTES + 2u];
  char generation_path[ORM_TIDESDB_MAINTENANCE_PATH_MAX_BYTES + 1u];
  orm_status_t status;

  if (self != &tidesdb_maintenance_identity ||
      config == NULL || result == NULL)
    return tidesdb_maintenance_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "invalid TidesDB ACTIVE resolve request");
  if (config->struct_size < sizeof(*config) ||
      config->abi_version != ORM_TIDESDB_GENERATION_ABI_VERSION ||
      result->struct_size < sizeof(*result) ||
      result->abi_version != ORM_TIDESDB_GENERATION_ABI_VERSION)
    return tidesdb_maintenance_fail(
        error, ORM_STATUS_ABI_MISMATCH,
        "TidesDB generation ABI does not match this build");
  if (config->reserved != 0u)
    return tidesdb_maintenance_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "TidesDB generation reserved field must be zero");

  result->found = 0u;
  result->reserved = 0u;
  result->generation_id[0] = '\0';
  result->generation_path[0] = '\0';
  status = tidesdb_maintenance_copy_view(
      config->provider_root, "provider root",
      provider_root, sizeof(provider_root), error);
  if (status != ORM_STATUS_OK) return status;
  status = tidesdb_maintenance_require_directory(
      provider_root, "inspect TidesDB provider root", error);
  if (status != ORM_STATUS_OK) return status;
  status = tidesdb_maintenance_join(
      provider_root, ORM_TIDESDB_ACTIVE_FILE_NAME,
      active_path, sizeof(active_path), error);
  if (status != ORM_STATUS_OK) return status;
  status = tidesdb_maintenance_read_active_id(
      active_path, generation_id, result, error);
  if (status != ORM_STATUS_OK || result->found == 0u) {
    if (status == ORM_STATUS_OK &&
        generation_id[0] != '\0')
      result->found = 1u;
    if (result->found == 0u) return status;
  }

  status = tidesdb_maintenance_join(
      provider_root, ORM_TIDESDB_GENERATIONS_DIR_NAME,
      generations_path, sizeof(generations_path), error);
  if (status != ORM_STATUS_OK) return status;
  status = tidesdb_maintenance_join(
      generations_path, generation_id,
      generation_path, sizeof(generation_path), error);
  if (status != ORM_STATUS_OK) return status;
  status = tidesdb_maintenance_require_directory(
      generation_path, "validate TidesDB ACTIVE generation", error);
  if (status != ORM_STATUS_OK) return status;
  status = tidesdb_maintenance_open_validate(
      generation_path, error);
  if (status != ORM_STATUS_OK) return status;

  result->found = 1u;
  (void)snprintf(result->generation_id,
                 sizeof(result->generation_id), "%s",
                 generation_id);
  (void)snprintf(result->generation_path,
                 sizeof(result->generation_path), "%s",
                 generation_path);
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

static const TurboDb_TidesMaintenance_vtable
    tidesdb_maintenance_vtable = {
        .restore_publish = tidesdb_maintenance_restore_publish,
        .resolve_active = tidesdb_maintenance_resolve_active};

TurboDb_TidesMaintenance orm_tidesdb_maintenance = {
    &tidesdb_maintenance_identity,
    &tidesdb_maintenance_vtable};

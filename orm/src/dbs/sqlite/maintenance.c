#include "orm_internal.h"

#include <orm_sqlite.h>
#include <salts_fs.h>
#include <sqlite3.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

enum {
  ORM_SQLITE_MAINTENANCE_PATH_MAX_BYTES = 4096u,
  ORM_SQLITE_MAINTENANCE_MAX_BUSY_RETRIES = 1000000u
};

static orm_status_t orm_sqlite_maintenance_status(int code,
                                                   orm_status_t fallback) {
  switch (code & 0xff) {
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
      return ORM_STATUS_BUSY;
    case SQLITE_NOMEM:
      return ORM_STATUS_OUT_OF_MEMORY;
    case SQLITE_TOOBIG:
      return ORM_STATUS_LIMIT_EXCEEDED;
    case SQLITE_RANGE:
      return ORM_STATUS_OUT_OF_RANGE;
    case SQLITE_READONLY:
      return ORM_STATUS_INVALID_STATE;
    case SQLITE_CONSTRAINT:
      return ORM_STATUS_CONSTRAINT;
    default:
      return fallback;
  }
}

static orm_status_t orm_sqlite_maintenance_fail(
    sqlite3 *database, int code, orm_status_t fallback, const char *operation,
    orm_error_t *error) {
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
  const orm_status_t status =
      orm_sqlite_maintenance_status(code, fallback);
  const char *detail = database != NULL ? sqlite3_errmsg(database)
                                        : sqlite3_errstr(code);
  (void)snprintf(message, sizeof(message), "%s: %s", operation,
                 detail != NULL ? detail : orm_status_message(status));
  orm_error_set(error, status, message);
  return status;
}

static orm_status_t orm_sqlite_maintenance_fail_fs(
    int code, const char *operation, orm_error_t *error) {
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
  const int native_error = code < 0 ? -code : code;
  (void)snprintf(message, sizeof(message), "%s: %s", operation,
                 strerror(native_error));
  orm_error_set(error, ORM_STATUS_DATASTORE_ERROR, message);
  return ORM_STATUS_DATASTORE_ERROR;
}

static orm_status_t orm_sqlite_copy_path(orm_string_view_t value,
                                         const char *role,
                                         char *output,
                                         size_t output_size,
                                         orm_error_t *error) {
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
  if (!orm_view_valid(value, false) ||
      value.len > ORM_SQLITE_MAINTENANCE_PATH_MAX_BYTES ||
      value.len + 1u > output_size ||
      memchr(value.data, '\0', value.len) != NULL) {
    (void)snprintf(message, sizeof(message), "invalid SQLite %s path", role);
    orm_error_set(error,
                  value.len > ORM_SQLITE_MAINTENANCE_PATH_MAX_BYTES
                      ? ORM_STATUS_LIMIT_EXCEEDED
                      : ORM_STATUS_INVALID_ARGUMENT,
                  message);
    return value.len > ORM_SQLITE_MAINTENANCE_PATH_MAX_BYTES
               ? ORM_STATUS_LIMIT_EXCEEDED
               : ORM_STATUS_INVALID_ARGUMENT;
  }
  memcpy(output, value.data, value.len);
  output[value.len] = '\0';
  return ORM_STATUS_OK;
}

static orm_status_t orm_sqlite_validate_copy_request(
    const orm_sqlite_file_copy_config *config,
    orm_sqlite_file_copy_result *result,
    char *source_path,
    char *staging_path,
    char *destination_path,
    orm_error_t *error) {
  orm_status_t status;
  int access_status;

  if (config == NULL || result == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "SQLite file-copy config and result are required");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  if (config->struct_size < sizeof(*config) ||
      config->abi_version != ORM_SQLITE_FILE_COPY_ABI_VERSION ||
      result->struct_size < sizeof(*result) ||
      result->abi_version != ORM_SQLITE_FILE_COPY_ABI_VERSION) {
    orm_error_set(error, ORM_STATUS_ABI_MISMATCH,
                  "SQLite file-copy ABI does not match this build");
    return ORM_STATUS_ABI_MISMATCH;
  }

  result->publication_state = ORM_SQLITE_PUBLICATION_NOT_PUBLISHED;
  result->reserved = 0u;
  result->pages_copied = 0u;

  if (config->reserved != 0u || config->busy_timeout_ms == 0u ||
      config->busy_timeout_ms > (uint32_t)INT_MAX ||
      config->pages_per_step == 0u ||
      config->pages_per_step > (uint32_t)INT_MAX ||
      config->max_busy_retries == 0u ||
      config->max_busy_retries > ORM_SQLITE_MAINTENANCE_MAX_BUSY_RETRIES) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid SQLite file-copy limits");
    return ORM_STATUS_INVALID_ARGUMENT;
  }

  status = orm_sqlite_copy_path(
      config->source_path, "source", source_path,
      ORM_SQLITE_MAINTENANCE_PATH_MAX_BYTES + 1u, error);
  if (status != ORM_STATUS_OK)
    return status;
  status = orm_sqlite_copy_path(
      config->staging_path, "staging", staging_path,
      ORM_SQLITE_MAINTENANCE_PATH_MAX_BYTES + 1u, error);
  if (status != ORM_STATUS_OK)
    return status;
  status = orm_sqlite_copy_path(
      config->destination_path, "destination", destination_path,
      ORM_SQLITE_MAINTENANCE_PATH_MAX_BYTES + 1u, error);
  if (status != ORM_STATUS_OK)
    return status;

  if (strcmp(source_path, staging_path) == 0 ||
      strcmp(source_path, destination_path) == 0 ||
      strcmp(staging_path, destination_path) == 0 ||
      strcmp(source_path, ":memory:") == 0) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "SQLite source, staging, and destination paths must be distinct file paths");
    return ORM_STATUS_INVALID_ARGUMENT;
  }

  access_status = salts_fs_access(staging_path, SALTS_FS_ACCESS_EXISTS);
  if (access_status == 0) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "SQLite staging path already exists");
    return ORM_STATUS_INVALID_STATE;
  }
  if (access_status != -ENOENT) {
    return orm_sqlite_maintenance_fail_fs(
        access_status, "inspect SQLite staging path", error);
  }

  return ORM_STATUS_OK;
}

static orm_status_t orm_sqlite_open_maintenance(
    const char *path, int flags, uint32_t busy_timeout_ms,
    sqlite3 **out_database, const char *operation, orm_error_t *error) {
  sqlite3 *database = NULL;
  int code;

  *out_database = NULL;
  code = sqlite3_open_v2(path, &database, flags | SQLITE_OPEN_NOMUTEX, NULL);
  if (code != SQLITE_OK) {
    const orm_status_t status =
        orm_sqlite_maintenance_fail(database, code,
                                    ORM_STATUS_DATASTORE_ERROR,
                                    operation, error);
    if (database != NULL)
      (void)sqlite3_close_v2(database);
    return status;
  }

  code = sqlite3_extended_result_codes(database, 1);
  if (code != SQLITE_OK) {
    const orm_status_t status =
        orm_sqlite_maintenance_fail(database, code,
                                    ORM_STATUS_DATASTORE_ERROR,
                                    "enable SQLite extended result codes", error);
    (void)sqlite3_close_v2(database);
    return status;
  }

  code = sqlite3_busy_timeout(database, (int)busy_timeout_ms);
  if (code != SQLITE_OK) {
    const orm_status_t status =
        orm_sqlite_maintenance_fail(database, code,
                                    ORM_STATUS_DATASTORE_ERROR,
                                    "configure SQLite busy timeout", error);
    (void)sqlite3_close_v2(database);
    return status;
  }

  *out_database = database;
  return ORM_STATUS_OK;
}

static orm_status_t orm_sqlite_backup_to_stage(
    const orm_sqlite_file_copy_config *config,
    const char *source_path,
    const char *staging_path,
    orm_sqlite_file_copy_result *result,
    orm_error_t *error) {
  sqlite3 *source = NULL;
  sqlite3 *staging = NULL;
  sqlite3_backup *backup = NULL;
  orm_status_t status;
  int step_status = SQLITE_OK;
  int finish_status = SQLITE_OK;
  uint32_t retries = 0u;

  status = orm_sqlite_open_maintenance(
      source_path, SQLITE_OPEN_READONLY, config->busy_timeout_ms,
      &source, "open SQLite source database", error);
  if (status != ORM_STATUS_OK)
    goto cleanup;

  status = orm_sqlite_open_maintenance(
      staging_path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
      config->busy_timeout_ms, &staging,
      "open SQLite staging database", error);
  if (status != ORM_STATUS_OK)
    goto cleanup;

  backup = sqlite3_backup_init(staging, "main", source, "main");
  if (backup == NULL) {
    status = orm_sqlite_maintenance_fail(
        staging, sqlite3_errcode(staging), ORM_STATUS_DATASTORE_ERROR,
        "initialize SQLite online backup", error);
    goto cleanup;
  }

  for (;;) {
    step_status = sqlite3_backup_step(backup, (int)config->pages_per_step);
    if (step_status == SQLITE_DONE)
      break;
    if (step_status == SQLITE_OK)
      continue;
    if ((step_status == SQLITE_BUSY || step_status == SQLITE_LOCKED) &&
        retries < config->max_busy_retries) {
      ++retries;
      (void)sqlite3_sleep(1);
      continue;
    }
    status = orm_sqlite_maintenance_fail(
        staging, step_status,
        (step_status == SQLITE_BUSY || step_status == SQLITE_LOCKED)
            ? ORM_STATUS_BUSY
            : ORM_STATUS_DATASTORE_ERROR,
        "copy SQLite backup pages", error);
    goto cleanup;
  }

  {
    const int page_count = sqlite3_backup_pagecount(backup);
    if (page_count > 0)
      result->pages_copied = (uint64_t)page_count;
  }

  finish_status = sqlite3_backup_finish(backup);
  backup = NULL;
  if (finish_status != SQLITE_OK) {
    status = orm_sqlite_maintenance_fail(
        staging, finish_status, ORM_STATUS_DATASTORE_ERROR,
        "finish SQLite online backup", error);
    goto cleanup;
  }

  status = ORM_STATUS_OK;

cleanup:
  if (backup != NULL) {
    const int cleanup_status = sqlite3_backup_finish(backup);
    if (status == ORM_STATUS_OK && cleanup_status != SQLITE_OK)
      status = orm_sqlite_maintenance_fail(
          staging, cleanup_status, ORM_STATUS_DATASTORE_ERROR,
          "finish SQLite online backup", error);
  }
  if (staging != NULL) {
    const int close_status = sqlite3_close_v2(staging);
    if (status == ORM_STATUS_OK && close_status != SQLITE_OK)
      status = orm_sqlite_maintenance_fail(
          staging, close_status, ORM_STATUS_DATASTORE_ERROR,
          "close SQLite staging database", error);
  }
  if (source != NULL) {
    const int close_status = sqlite3_close_v2(source);
    if (status == ORM_STATUS_OK && close_status != SQLITE_OK)
      status = orm_sqlite_maintenance_fail(
          source, close_status, ORM_STATUS_DATASTORE_ERROR,
          "close SQLite source database", error);
  }
  return status;
}

static orm_status_t orm_sqlite_validate_stage(
    const char *staging_path, uint32_t busy_timeout_ms,
    orm_error_t *error) {
  sqlite3 *database = NULL;
  sqlite3_stmt *statement = NULL;
  orm_status_t status;
  int code;

  status = orm_sqlite_open_maintenance(
      staging_path, SQLITE_OPEN_READONLY, busy_timeout_ms,
      &database, "open SQLite staging database for validation", error);
  if (status != ORM_STATUS_OK)
    return status;

  code = sqlite3_prepare_v2(database, "pragma quick_check(1)", -1,
                            &statement, NULL);
  if (code != SQLITE_OK) {
    status = orm_sqlite_maintenance_fail(
        database, code, ORM_STATUS_DATASTORE_ERROR,
        "prepare SQLite quick_check", error);
    goto cleanup;
  }

  code = sqlite3_step(statement);
  if (code != SQLITE_ROW) {
    status = orm_sqlite_maintenance_fail(
        database, code, ORM_STATUS_DATASTORE_ERROR,
        "run SQLite quick_check", error);
    goto cleanup;
  }

  {
    const unsigned char *text = sqlite3_column_text(statement, 0);
    if (text == NULL || strcmp((const char *)text, "ok") != 0) {
      orm_error_set(error, ORM_STATUS_DATASTORE_ERROR,
                    "SQLite quick_check rejected staging database");
      status = ORM_STATUS_DATASTORE_ERROR;
      goto cleanup;
    }
  }

  code = sqlite3_step(statement);
  if (code != SQLITE_DONE) {
    status = orm_sqlite_maintenance_fail(
        database, code, ORM_STATUS_DATASTORE_ERROR,
        "finish SQLite quick_check", error);
    goto cleanup;
  }

  status = ORM_STATUS_OK;

cleanup:
  if (statement != NULL)
    (void)sqlite3_finalize(statement);
  if (database != NULL)
    (void)sqlite3_close_v2(database);
  return status;
}

static void orm_sqlite_map_publication_state(
    salts_fs_replace_state_t source,
    orm_sqlite_file_copy_result *result) {
  switch (source) {
    case SALTS_FS_REPLACE_PUBLISHED_DURABLE:
      result->publication_state = ORM_SQLITE_PUBLICATION_PUBLISHED_DURABLE;
      break;
    case SALTS_FS_REPLACE_DURABILITY_UNKNOWN:
      result->publication_state = ORM_SQLITE_PUBLICATION_DURABILITY_UNKNOWN;
      break;
    case SALTS_FS_REPLACE_NOT_PUBLISHED:
    default:
      result->publication_state = ORM_SQLITE_PUBLICATION_NOT_PUBLISHED;
      break;
  }
}

static orm_status_t orm_sqlite_copy_and_publish(
    const orm_sqlite_file_copy_config *config,
    orm_sqlite_file_copy_result *result,
    const char *operation,
    orm_error_t *error) {
  char source_path[ORM_SQLITE_MAINTENANCE_PATH_MAX_BYTES + 1u];
  char staging_path[ORM_SQLITE_MAINTENANCE_PATH_MAX_BYTES + 1u];
  char destination_path[ORM_SQLITE_MAINTENANCE_PATH_MAX_BYTES + 1u];
  salts_fs_replace_state_t replace_state = SALTS_FS_REPLACE_NOT_PUBLISHED;
  orm_status_t status;
  int replace_status;

  status = orm_sqlite_validate_copy_request(
      config, result, source_path, staging_path, destination_path, error);
  if (status != ORM_STATUS_OK)
    return status;

  status = orm_sqlite_backup_to_stage(
      config, source_path, staging_path, result, error);
  if (status != ORM_STATUS_OK)
    return status;

  status = orm_sqlite_validate_stage(
      staging_path, config->busy_timeout_ms, error);
  if (status != ORM_STATUS_OK)
    return status;

  replace_status =
      salts_fs_replace_durable(staging_path, destination_path, &replace_state);
  orm_sqlite_map_publication_state(replace_state, result);
  if (replace_status != 0) {
    return orm_sqlite_maintenance_fail_fs(
        replace_status, operation, error);
  }
  if (replace_state != SALTS_FS_REPLACE_PUBLISHED_DURABLE) {
    orm_error_set(error, ORM_STATUS_INTERNAL_ERROR,
                  "durable SQLite publication returned an invalid success state");
    return ORM_STATUS_INTERNAL_ERROR;
  }

  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

void orm_sqlite_file_copy_config_init(orm_sqlite_file_copy_config *config) {
  if (config == NULL)
    return;
  memset(config, 0, sizeof(*config));
  config->struct_size = (uint32_t)sizeof(*config);
  config->abi_version = ORM_SQLITE_FILE_COPY_ABI_VERSION;
  config->busy_timeout_ms = ORM_SQLITE_DEFAULT_BUSY_TIMEOUT_MS;
  config->pages_per_step = ORM_SQLITE_DEFAULT_PAGES_PER_STEP;
  config->max_busy_retries = ORM_SQLITE_DEFAULT_MAX_BUSY_RETRIES;
}

void orm_sqlite_file_copy_result_init(orm_sqlite_file_copy_result *result) {
  if (result == NULL)
    return;
  memset(result, 0, sizeof(*result));
  result->struct_size = (uint32_t)sizeof(*result);
  result->abi_version = ORM_SQLITE_FILE_COPY_ABI_VERSION;
  result->publication_state = ORM_SQLITE_PUBLICATION_NOT_PUBLISHED;
}

orm_status_t orm_sqlite_checkpoint_create(
    const orm_sqlite_file_copy_config *config,
    orm_sqlite_file_copy_result *result,
    orm_error_t *error) {
  return orm_sqlite_copy_and_publish(
      config, result, "publish SQLite checkpoint", error);
}

orm_status_t orm_sqlite_restore_publish(
    const orm_sqlite_file_copy_config *config,
    orm_sqlite_file_copy_result *result,
    orm_error_t *error) {
  return orm_sqlite_copy_and_publish(
      config, result, "publish restored SQLite database", error);
}

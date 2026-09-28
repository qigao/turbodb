#ifndef ORM_SQLITE_H
#define ORM_SQLITE_H

#include "orm_driver_interface.h"
#include "orm_runtime.h"

#include <string.h>

#define ORM_SQLITE_MAINTENANCE_EXPORT_ID "sqlite.maintenance"
#define ORM_SQLITE_MAINTENANCE_CONTRACT_ID "TurboDb.SqliteMaintenance"
#define ORM_SQLITE_MAINTENANCE_CONTRACT_VERSION UINT32_C(1)
#define ORM_SQLITE_FILE_COPY_ABI_VERSION UINT32_C(1)
#define ORM_SQLITE_DEFAULT_BUSY_TIMEOUT_MS UINT32_C(5000)
#define ORM_SQLITE_DEFAULT_PAGES_PER_STEP UINT32_C(64)
#define ORM_SQLITE_MAX_PAGES_PER_STEP UINT32_C(1024)
#define ORM_SQLITE_MAX_PAGE_BYTES UINT64_C(65536)
#define ORM_SQLITE_MAX_RESTORE_CHUNK_BYTES \
  (UINT64_C(1024) * ORM_SQLITE_MAX_PAGE_BYTES)
#define ORM_SQLITE_DEFAULT_MAX_BUSY_RETRIES UINT32_C(64)

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t orm_sqlite_publication_state_t;
enum {
  ORM_SQLITE_PUBLICATION_NOT_PUBLISHED = 0u,
  ORM_SQLITE_PUBLICATION_PUBLISHED_DURABLE = 1u,
  ORM_SQLITE_PUBLICATION_DURABILITY_UNKNOWN = 2u
};

typedef struct orm_sqlite_file_copy_config {
  uint32_t struct_size;
  uint32_t abi_version;
  orm_string_view_t source_path;
  orm_string_view_t staging_path;
  orm_string_view_t destination_path;
  uint32_t busy_timeout_ms;
  uint32_t pages_per_step;
  uint32_t max_busy_retries;
  uint32_t reserved;
} orm_sqlite_file_copy_config;

typedef struct orm_sqlite_file_copy_result {
  uint32_t struct_size;
  uint32_t abi_version;
  orm_sqlite_publication_state_t publication_state;
  uint32_t reserved;
  uint64_t pages_copied;
} orm_sqlite_file_copy_result;

static inline void orm_sqlite_file_copy_config_init(
    orm_sqlite_file_copy_config *config) {
  if (config == NULL) return;
  memset(config, 0, sizeof(*config));
  config->struct_size = (uint32_t)sizeof(*config);
  config->abi_version = ORM_SQLITE_FILE_COPY_ABI_VERSION;
  config->busy_timeout_ms = ORM_SQLITE_DEFAULT_BUSY_TIMEOUT_MS;
  config->pages_per_step = ORM_SQLITE_DEFAULT_PAGES_PER_STEP;
  config->max_busy_retries = ORM_SQLITE_DEFAULT_MAX_BUSY_RETRIES;
}

static inline void orm_sqlite_file_copy_result_init(
    orm_sqlite_file_copy_result *result) {
  if (result == NULL) return;
  memset(result, 0, sizeof(*result));
  result->struct_size = (uint32_t)sizeof(*result);
  result->abi_version = ORM_SQLITE_FILE_COPY_ABI_VERSION;
  result->publication_state = ORM_SQLITE_PUBLICATION_NOT_PUBLISHED;
}

CMETA_LOCAL const cmeta_type_identity
    orm_sqlite_file_copy_config_type_identity =
        CMETA_TYPE_ID_ATOM_INIT("TurboDb.SqliteFileCopyConfig");
CMETA_LOCAL const cmeta_type_identity
    orm_sqlite_file_copy_config_const_type_identity =
        CMETA_TYPE_ID_CONST_INIT(&orm_sqlite_file_copy_config_type_identity);
CMETA_LOCAL const cmeta_type_desc
    orm_sqlite_file_copy_config_const_cmeta_type = {
        "const orm_sqlite_file_copy_config",
        sizeof(orm_sqlite_file_copy_config),
        CMETA_ALIGNOF(orm_sqlite_file_copy_config),
        CMETA_T_OBJECT, NULL, NULL,
        &orm_sqlite_file_copy_config_const_type_identity};
CMETA_LOCAL const cmeta_type_identity
    orm_sqlite_file_copy_config_ptr_type_identity =
        CMETA_TYPE_ID_POINTER_INIT(
            &orm_sqlite_file_copy_config_const_type_identity);
CMETA_LOCAL const cmeta_type_desc
    orm_sqlite_file_copy_config_ptr_cmeta_type = {
        "const orm_sqlite_file_copy_config *",
        sizeof(const orm_sqlite_file_copy_config *),
        CMETA_ALIGNOF(const orm_sqlite_file_copy_config *),
        CMETA_T_POINTER,
        &orm_sqlite_file_copy_config_const_cmeta_type, NULL,
        &orm_sqlite_file_copy_config_ptr_type_identity};

CMETA_LOCAL const cmeta_type_identity
    orm_sqlite_file_copy_result_type_identity =
        CMETA_TYPE_ID_ATOM_INIT("TurboDb.SqliteFileCopyResult");
CMETA_LOCAL const cmeta_type_desc
    orm_sqlite_file_copy_result_cmeta_type = {
        "orm_sqlite_file_copy_result",
        sizeof(orm_sqlite_file_copy_result),
        CMETA_ALIGNOF(orm_sqlite_file_copy_result),
        CMETA_T_OBJECT, NULL, NULL,
        &orm_sqlite_file_copy_result_type_identity};
CMETA_LOCAL const cmeta_type_identity
    orm_sqlite_file_copy_result_ptr_type_identity =
        CMETA_TYPE_ID_POINTER_INIT(&orm_sqlite_file_copy_result_type_identity);
CMETA_LOCAL const cmeta_type_desc
    orm_sqlite_file_copy_result_ptr_cmeta_type = {
        "orm_sqlite_file_copy_result *",
        sizeof(orm_sqlite_file_copy_result *),
        CMETA_ALIGNOF(orm_sqlite_file_copy_result *),
        CMETA_T_POINTER,
        &orm_sqlite_file_copy_result_cmeta_type, NULL,
        &orm_sqlite_file_copy_result_ptr_type_identity};

#define ORM_SQLITE_MAINTENANCE_METHODS(X, I)                                  \
  X(I, F3, orm_status_t, checkpoint_create, io,                               \
    &orm_driver_status_cmeta_type, CMETA_ABI_SCALAR,                          \
    (const orm_sqlite_file_copy_config *, config,                             \
     CMETA_PARAM_IN | CMETA_PARAM_BORROWED,                                   \
     &orm_sqlite_file_copy_config_ptr_cmeta_type, CMETA_ABI_OBJECT_POINTER),  \
    (orm_sqlite_file_copy_result *, result,                                   \
     CMETA_PARAM_OUT | CMETA_PARAM_BORROWED,                                  \
     &orm_sqlite_file_copy_result_ptr_cmeta_type, CMETA_ABI_OBJECT_POINTER),  \
    (orm_error_t *, error,                                                    \
     CMETA_PARAM_OUT | CMETA_PARAM_BORROWED | CMETA_PARAM_NULLABLE,           \
     &orm_driver_error_ptr_cmeta_type, CMETA_ABI_OBJECT_POINTER))              \
  X(I, F3, orm_status_t, restore_publish, io,                                 \
    &orm_driver_status_cmeta_type, CMETA_ABI_SCALAR,                          \
    (const orm_sqlite_file_copy_config *, config,                             \
     CMETA_PARAM_IN | CMETA_PARAM_BORROWED,                                   \
     &orm_sqlite_file_copy_config_ptr_cmeta_type, CMETA_ABI_OBJECT_POINTER),  \
    (orm_sqlite_file_copy_result *, result,                                   \
     CMETA_PARAM_OUT | CMETA_PARAM_BORROWED,                                  \
     &orm_sqlite_file_copy_result_ptr_cmeta_type, CMETA_ABI_OBJECT_POINTER),  \
    (orm_error_t *, error,                                                    \
     CMETA_PARAM_OUT | CMETA_PARAM_BORROWED | CMETA_PARAM_NULLABLE,           \
     &orm_driver_error_ptr_cmeta_type, CMETA_ABI_OBJECT_POINTER))

CMETA_INTERFACE(TurboDb_SqliteMaintenance, ORM_SQLITE_MAINTENANCE_METHODS);

static inline orm_status_t orm_runtime_sqlite_checkpoint_create(
    orm_runtime_t *runtime, orm_string_view_t driver_id,
    const orm_sqlite_file_copy_config *config,
    orm_sqlite_file_copy_result *result, orm_error_t *error) {
  orm_runtime_driver_extension_t *extension = NULL;
  void *binding = NULL;
  orm_status_t status = orm_runtime_driver_acquire_extension(
      runtime, driver_id, orm_view(ORM_SQLITE_MAINTENANCE_EXPORT_ID),
      orm_view(ORM_SQLITE_MAINTENANCE_CONTRACT_ID),
      ORM_SQLITE_MAINTENANCE_CONTRACT_VERSION,
      TurboDb_SqliteMaintenance_interface(), &extension, &binding, error);
  if (status != ORM_STATUS_OK) return status;
  TurboDb_SqliteMaintenance *maintenance =
      (TurboDb_SqliteMaintenance *)binding;
  if (!TurboDb_SqliteMaintenance_valid(maintenance)) {
    (void)orm_runtime_driver_release_extension(extension, NULL);
    return ORM_STATUS_ABI_MISMATCH;
  }
  status = TurboDb_SqliteMaintenance_checkpoint_create(
      maintenance, config, result, error);
  orm_error_t release_error;
  orm_error_init(&release_error);
  const orm_status_t release_status =
      orm_runtime_driver_release_extension(extension, &release_error);
  if (status == ORM_STATUS_OK && release_status != ORM_STATUS_OK) {
    if (error != NULL) *error = release_error;
    return release_status;
  }
  return status;
}

static inline orm_status_t orm_runtime_sqlite_restore_publish(
    orm_runtime_t *runtime, orm_string_view_t driver_id,
    const orm_sqlite_file_copy_config *config,
    orm_sqlite_file_copy_result *result, orm_error_t *error) {
  orm_runtime_driver_extension_t *extension = NULL;
  void *binding = NULL;
  orm_status_t status = orm_runtime_driver_acquire_extension(
      runtime, driver_id, orm_view(ORM_SQLITE_MAINTENANCE_EXPORT_ID),
      orm_view(ORM_SQLITE_MAINTENANCE_CONTRACT_ID),
      ORM_SQLITE_MAINTENANCE_CONTRACT_VERSION,
      TurboDb_SqliteMaintenance_interface(), &extension, &binding, error);
  if (status != ORM_STATUS_OK) return status;
  TurboDb_SqliteMaintenance *maintenance =
      (TurboDb_SqliteMaintenance *)binding;
  if (!TurboDb_SqliteMaintenance_valid(maintenance)) {
    (void)orm_runtime_driver_release_extension(extension, NULL);
    return ORM_STATUS_ABI_MISMATCH;
  }
  status = TurboDb_SqliteMaintenance_restore_publish(
      maintenance, config, result, error);
  orm_error_t release_error;
  orm_error_init(&release_error);
  const orm_status_t release_status =
      orm_runtime_driver_release_extension(extension, &release_error);
  if (status == ORM_STATUS_OK && release_status != ORM_STATUS_OK) {
    if (error != NULL) *error = release_error;
    return release_status;
  }
  return status;
}

#ifdef __cplusplus
}
#endif
#endif /* ORM_SQLITE_H */

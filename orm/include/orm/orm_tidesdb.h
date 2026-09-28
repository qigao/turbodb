#ifndef ORM_TIDESDB_H
#define ORM_TIDESDB_H

#include "orm_driver_interface.h"
#include "orm_runtime.h"

#include <string.h>

#define ORM_TIDESDB_MAINTENANCE_EXPORT_ID "tidesdb.maintenance"
#define ORM_TIDESDB_MAINTENANCE_CONTRACT_ID "TurboDb.TidesMaintenance"
#define ORM_TIDESDB_MAINTENANCE_CONTRACT_VERSION UINT32_C(1)
#define ORM_TIDESDB_GENERATION_ABI_VERSION UINT32_C(1)
#define ORM_TIDESDB_GENERATION_ID_MAX_BYTES UINT32_C(63)
#define ORM_TIDESDB_GENERATION_PATH_MAX_BYTES UINT32_C(4096)
#define ORM_TIDESDB_ACTIVE_FILE_NAME "ACTIVE"
#define ORM_TIDESDB_GENERATIONS_DIR_NAME "generations"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t orm_tidesdb_publication_state_t;
enum {
  ORM_TIDESDB_PUBLICATION_NOT_PUBLISHED = 0u,
  ORM_TIDESDB_PUBLICATION_PUBLISHED_DURABLE = 1u,
  ORM_TIDESDB_PUBLICATION_DURABILITY_UNKNOWN = 2u
};

typedef struct orm_tidesdb_generation_config {
  uint32_t struct_size;
  uint32_t abi_version;
  orm_string_view_t provider_root;
  orm_string_view_t checkpoint_path;
  orm_string_view_t generation_id;
  uint32_t reserved;
} orm_tidesdb_generation_config;

typedef struct orm_tidesdb_restore_result {
  uint32_t struct_size;
  uint32_t abi_version;
  orm_tidesdb_publication_state_t publication_state;
  uint32_t reserved;
  char generation_path[ORM_TIDESDB_GENERATION_PATH_MAX_BYTES + 1u];
} orm_tidesdb_restore_result;

typedef struct orm_tidesdb_active_result {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t found;
  uint32_t reserved;
  char generation_id[ORM_TIDESDB_GENERATION_ID_MAX_BYTES + 1u];
  char generation_path[ORM_TIDESDB_GENERATION_PATH_MAX_BYTES + 1u];
} orm_tidesdb_active_result;

static inline void orm_tidesdb_generation_config_init(
    orm_tidesdb_generation_config *config) {
  if (config == NULL) return;
  memset(config, 0, sizeof(*config));
  config->struct_size = (uint32_t)sizeof(*config);
  config->abi_version = ORM_TIDESDB_GENERATION_ABI_VERSION;
}

static inline void orm_tidesdb_restore_result_init(
    orm_tidesdb_restore_result *result) {
  if (result == NULL) return;
  memset(result, 0, sizeof(*result));
  result->struct_size = (uint32_t)sizeof(*result);
  result->abi_version = ORM_TIDESDB_GENERATION_ABI_VERSION;
}

static inline void orm_tidesdb_active_result_init(
    orm_tidesdb_active_result *result) {
  if (result == NULL) return;
  memset(result, 0, sizeof(*result));
  result->struct_size = (uint32_t)sizeof(*result);
  result->abi_version = ORM_TIDESDB_GENERATION_ABI_VERSION;
}

CMETA_LOCAL const cmeta_type_identity
    orm_tidesdb_generation_config_type_identity =
        CMETA_TYPE_ID_ATOM_INIT("TurboDb.TidesGenerationConfig");
CMETA_LOCAL const cmeta_type_identity
    orm_tidesdb_generation_config_const_type_identity =
        CMETA_TYPE_ID_CONST_INIT(&orm_tidesdb_generation_config_type_identity);
CMETA_LOCAL const cmeta_type_desc
    orm_tidesdb_generation_config_const_cmeta_type = {
        "const orm_tidesdb_generation_config",
        sizeof(orm_tidesdb_generation_config),
        CMETA_ALIGNOF(orm_tidesdb_generation_config),
        CMETA_T_OBJECT, NULL, NULL,
        &orm_tidesdb_generation_config_const_type_identity};
CMETA_LOCAL const cmeta_type_identity
    orm_tidesdb_generation_config_ptr_type_identity =
        CMETA_TYPE_ID_POINTER_INIT(
            &orm_tidesdb_generation_config_const_type_identity);
CMETA_LOCAL const cmeta_type_desc
    orm_tidesdb_generation_config_ptr_cmeta_type = {
        "const orm_tidesdb_generation_config *",
        sizeof(const orm_tidesdb_generation_config *),
        CMETA_ALIGNOF(const orm_tidesdb_generation_config *),
        CMETA_T_POINTER,
        &orm_tidesdb_generation_config_const_cmeta_type, NULL,
        &orm_tidesdb_generation_config_ptr_type_identity};

CMETA_LOCAL const cmeta_type_identity
    orm_tidesdb_restore_result_type_identity =
        CMETA_TYPE_ID_ATOM_INIT("TurboDb.TidesRestoreResult");
CMETA_LOCAL const cmeta_type_desc
    orm_tidesdb_restore_result_cmeta_type = {
        "orm_tidesdb_restore_result",
        sizeof(orm_tidesdb_restore_result),
        CMETA_ALIGNOF(orm_tidesdb_restore_result),
        CMETA_T_OBJECT, NULL, NULL,
        &orm_tidesdb_restore_result_type_identity};
CMETA_LOCAL const cmeta_type_identity
    orm_tidesdb_restore_result_ptr_type_identity =
        CMETA_TYPE_ID_POINTER_INIT(&orm_tidesdb_restore_result_type_identity);
CMETA_LOCAL const cmeta_type_desc
    orm_tidesdb_restore_result_ptr_cmeta_type = {
        "orm_tidesdb_restore_result *",
        sizeof(orm_tidesdb_restore_result *),
        CMETA_ALIGNOF(orm_tidesdb_restore_result *),
        CMETA_T_POINTER,
        &orm_tidesdb_restore_result_cmeta_type, NULL,
        &orm_tidesdb_restore_result_ptr_type_identity};

CMETA_LOCAL const cmeta_type_identity
    orm_tidesdb_active_result_type_identity =
        CMETA_TYPE_ID_ATOM_INIT("TurboDb.TidesActiveResult");
CMETA_LOCAL const cmeta_type_desc
    orm_tidesdb_active_result_cmeta_type = {
        "orm_tidesdb_active_result",
        sizeof(orm_tidesdb_active_result),
        CMETA_ALIGNOF(orm_tidesdb_active_result),
        CMETA_T_OBJECT, NULL, NULL,
        &orm_tidesdb_active_result_type_identity};
CMETA_LOCAL const cmeta_type_identity
    orm_tidesdb_active_result_ptr_type_identity =
        CMETA_TYPE_ID_POINTER_INIT(&orm_tidesdb_active_result_type_identity);
CMETA_LOCAL const cmeta_type_desc
    orm_tidesdb_active_result_ptr_cmeta_type = {
        "orm_tidesdb_active_result *",
        sizeof(orm_tidesdb_active_result *),
        CMETA_ALIGNOF(orm_tidesdb_active_result *),
        CMETA_T_POINTER,
        &orm_tidesdb_active_result_cmeta_type, NULL,
        &orm_tidesdb_active_result_ptr_type_identity};

#define ORM_TIDESDB_MAINTENANCE_METHODS(X, I)                               \
  X(I, F3, orm_status_t, restore_publish, io,                               \
    &orm_driver_status_cmeta_type, CMETA_ABI_SCALAR,                        \
    (const orm_tidesdb_generation_config *, config,                         \
     CMETA_PARAM_IN | CMETA_PARAM_BORROWED,                                 \
     &orm_tidesdb_generation_config_ptr_cmeta_type,                         \
     CMETA_ABI_OBJECT_POINTER),                                              \
    (orm_tidesdb_restore_result *, result,                                  \
     CMETA_PARAM_OUT | CMETA_PARAM_BORROWED,                                \
     &orm_tidesdb_restore_result_ptr_cmeta_type, CMETA_ABI_OBJECT_POINTER), \
    (orm_error_t *, error,                                                  \
     CMETA_PARAM_OUT | CMETA_PARAM_BORROWED | CMETA_PARAM_NULLABLE,         \
     &orm_driver_error_ptr_cmeta_type, CMETA_ABI_OBJECT_POINTER))           \
  X(I, F3, orm_status_t, resolve_active, io,                                \
    &orm_driver_status_cmeta_type, CMETA_ABI_SCALAR,                        \
    (const orm_tidesdb_generation_config *, config,                         \
     CMETA_PARAM_IN | CMETA_PARAM_BORROWED,                                 \
     &orm_tidesdb_generation_config_ptr_cmeta_type,                         \
     CMETA_ABI_OBJECT_POINTER),                                              \
    (orm_tidesdb_active_result *, result,                                   \
     CMETA_PARAM_OUT | CMETA_PARAM_BORROWED,                                \
     &orm_tidesdb_active_result_ptr_cmeta_type, CMETA_ABI_OBJECT_POINTER),  \
    (orm_error_t *, error,                                                  \
     CMETA_PARAM_OUT | CMETA_PARAM_BORROWED | CMETA_PARAM_NULLABLE,         \
     &orm_driver_error_ptr_cmeta_type, CMETA_ABI_OBJECT_POINTER))

CMETA_INTERFACE(TurboDb_TidesMaintenance, ORM_TIDESDB_MAINTENANCE_METHODS);

static inline orm_status_t orm_runtime_tidesdb_restore_publish(
    orm_runtime_t *runtime, orm_string_view_t driver_id,
    const orm_tidesdb_generation_config *config,
    orm_tidesdb_restore_result *result, orm_error_t *error) {
  orm_runtime_driver_extension_t *extension = NULL;
  void *binding = NULL;
  orm_status_t status = orm_runtime_driver_acquire_extension(
      runtime, driver_id, orm_view(ORM_TIDESDB_MAINTENANCE_EXPORT_ID),
      orm_view(ORM_TIDESDB_MAINTENANCE_CONTRACT_ID),
      ORM_TIDESDB_MAINTENANCE_CONTRACT_VERSION,
      TurboDb_TidesMaintenance_interface(), &extension, &binding, error);
  if (status != ORM_STATUS_OK) return status;
  TurboDb_TidesMaintenance *maintenance = (TurboDb_TidesMaintenance *)binding;
  status = TurboDb_TidesMaintenance_restore_publish(
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

static inline orm_status_t orm_runtime_tidesdb_resolve_active(
    orm_runtime_t *runtime, orm_string_view_t driver_id,
    const orm_tidesdb_generation_config *config,
    orm_tidesdb_active_result *result, orm_error_t *error) {
  orm_runtime_driver_extension_t *extension = NULL;
  void *binding = NULL;
  orm_status_t status = orm_runtime_driver_acquire_extension(
      runtime, driver_id, orm_view(ORM_TIDESDB_MAINTENANCE_EXPORT_ID),
      orm_view(ORM_TIDESDB_MAINTENANCE_CONTRACT_ID),
      ORM_TIDESDB_MAINTENANCE_CONTRACT_VERSION,
      TurboDb_TidesMaintenance_interface(), &extension, &binding, error);
  if (status != ORM_STATUS_OK) return status;
  TurboDb_TidesMaintenance *maintenance = (TurboDb_TidesMaintenance *)binding;
  status = TurboDb_TidesMaintenance_resolve_active(
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
#endif /* ORM_TIDESDB_H */

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
#define ORM_TIDESDB_GENERATION_ID_CAPACITY \
  (ORM_TIDESDB_GENERATION_ID_MAX_BYTES + UINT32_C(1))

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t orm_tidesdb_publication_state_t;
enum {
  ORM_TIDESDB_PUBLICATION_NOT_PUBLISHED = 0u,
  ORM_TIDESDB_PUBLICATION_PUBLISHED_DURABLE = 1u,
  ORM_TIDESDB_PUBLICATION_DURABILITY_UNKNOWN = 2u
};

typedef struct orm_tidesdb_generation_publish_config {
  uint32_t struct_size;
  uint32_t abi_version;
  orm_string_view_t provider_root;
  orm_string_view_t generation_id;
  uint32_t flags;
  uint32_t reserved;
} orm_tidesdb_generation_publish_config;

typedef struct orm_tidesdb_active_resolve_config {
  uint32_t struct_size;
  uint32_t abi_version;
  orm_string_view_t provider_root;
  uint32_t flags;
  uint32_t reserved;
} orm_tidesdb_active_resolve_config;

typedef struct orm_tidesdb_generation_publish_result {
  uint32_t struct_size;
  uint32_t abi_version;
  orm_tidesdb_publication_state_t publication_state;
  uint32_t generation_size;
  char generation[ORM_TIDESDB_GENERATION_ID_CAPACITY];
} orm_tidesdb_generation_publish_result;

typedef struct orm_tidesdb_active_result {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t generation_size;
  uint32_t reserved;
  char generation[ORM_TIDESDB_GENERATION_ID_CAPACITY];
} orm_tidesdb_active_result;

static inline void orm_tidesdb_generation_publish_config_init(
    orm_tidesdb_generation_publish_config *config) {
  if (config == NULL) return;
  memset(config, 0, sizeof(*config));
  config->struct_size = (uint32_t)sizeof(*config);
  config->abi_version = ORM_TIDESDB_GENERATION_ABI_VERSION;
}

static inline void orm_tidesdb_active_resolve_config_init(
    orm_tidesdb_active_resolve_config *config) {
  if (config == NULL) return;
  memset(config, 0, sizeof(*config));
  config->struct_size = (uint32_t)sizeof(*config);
  config->abi_version = ORM_TIDESDB_GENERATION_ABI_VERSION;
}

static inline void orm_tidesdb_generation_publish_result_init(
    orm_tidesdb_generation_publish_result *result) {
  if (result == NULL) return;
  memset(result, 0, sizeof(*result));
  result->struct_size = (uint32_t)sizeof(*result);
  result->abi_version = ORM_TIDESDB_GENERATION_ABI_VERSION;
  result->publication_state = ORM_TIDESDB_PUBLICATION_NOT_PUBLISHED;
}

static inline void orm_tidesdb_active_result_init(
    orm_tidesdb_active_result *result) {
  if (result == NULL) return;
  memset(result, 0, sizeof(*result));
  result->struct_size = (uint32_t)sizeof(*result);
  result->abi_version = ORM_TIDESDB_GENERATION_ABI_VERSION;
}

#define ORM_TIDESDB_DECLARE_OBJECT_TYPE(prefix, c_type, stable_name)             \
  CMETA_LOCAL const cmeta_type_identity prefix##_type_identity =                 \
      CMETA_TYPE_ID_ATOM_INIT(stable_name);                                      \
  CMETA_LOCAL const cmeta_type_desc prefix##_cmeta_type = {                      \
      #c_type, sizeof(c_type), CMETA_ALIGNOF(c_type), CMETA_T_OBJECT,            \
      NULL, NULL, &prefix##_type_identity};                                      \
  CMETA_LOCAL const cmeta_type_identity prefix##_ptr_type_identity =             \
      CMETA_TYPE_ID_POINTER_INIT(&prefix##_type_identity);                       \
  CMETA_LOCAL const cmeta_type_desc prefix##_ptr_cmeta_type = {                  \
      #c_type " *", sizeof(c_type *), CMETA_ALIGNOF(c_type *), CMETA_T_POINTER,  \
      &prefix##_cmeta_type, NULL, &prefix##_ptr_type_identity}

#define ORM_TIDESDB_DECLARE_CONST_OBJECT_TYPE(prefix, c_type, stable_name)       \
  CMETA_LOCAL const cmeta_type_identity prefix##_type_identity =                 \
      CMETA_TYPE_ID_ATOM_INIT(stable_name);                                      \
  CMETA_LOCAL const cmeta_type_identity prefix##_const_type_identity =           \
      CMETA_TYPE_ID_CONST_INIT(&prefix##_type_identity);                         \
  CMETA_LOCAL const cmeta_type_desc prefix##_const_cmeta_type = {                \
      "const " #c_type, sizeof(c_type), CMETA_ALIGNOF(c_type), CMETA_T_OBJECT,   \
      NULL, NULL, &prefix##_const_type_identity};                                \
  CMETA_LOCAL const cmeta_type_identity prefix##_ptr_type_identity =             \
      CMETA_TYPE_ID_POINTER_INIT(&prefix##_const_type_identity);                 \
  CMETA_LOCAL const cmeta_type_desc prefix##_ptr_cmeta_type = {                  \
      "const " #c_type " *", sizeof(const c_type *),                            \
      CMETA_ALIGNOF(const c_type *), CMETA_T_POINTER,                            \
      &prefix##_const_cmeta_type, NULL, &prefix##_ptr_type_identity}

ORM_TIDESDB_DECLARE_CONST_OBJECT_TYPE(
    orm_tidesdb_publish_config, orm_tidesdb_generation_publish_config,
    "TurboDb.TidesGenerationPublishConfig");
ORM_TIDESDB_DECLARE_CONST_OBJECT_TYPE(
    orm_tidesdb_resolve_config, orm_tidesdb_active_resolve_config,
    "TurboDb.TidesActiveResolveConfig");
ORM_TIDESDB_DECLARE_OBJECT_TYPE(
    orm_tidesdb_publish_result, orm_tidesdb_generation_publish_result,
    "TurboDb.TidesGenerationPublishResult");
ORM_TIDESDB_DECLARE_OBJECT_TYPE(
    orm_tidesdb_active_result, orm_tidesdb_active_result,
    "TurboDb.TidesActiveResult");

#undef ORM_TIDESDB_DECLARE_OBJECT_TYPE
#undef ORM_TIDESDB_DECLARE_CONST_OBJECT_TYPE

#define ORM_TIDESDB_MAINTENANCE_METHODS(X, I)                                 \
  X(I, F3, orm_status_t, publish_generation, io,                              \
    &orm_driver_status_cmeta_type, CMETA_ABI_SCALAR,                          \
    (const orm_tidesdb_generation_publish_config *, config,                   \
     CMETA_PARAM_IN | CMETA_PARAM_BORROWED,                                   \
     &orm_tidesdb_publish_config_ptr_cmeta_type, CMETA_ABI_OBJECT_POINTER),   \
    (orm_tidesdb_generation_publish_result *, result,                         \
     CMETA_PARAM_OUT | CMETA_PARAM_BORROWED,                                  \
     &orm_tidesdb_publish_result_ptr_cmeta_type, CMETA_ABI_OBJECT_POINTER),   \
    (orm_error_t *, error,                                                    \
     CMETA_PARAM_OUT | CMETA_PARAM_BORROWED | CMETA_PARAM_NULLABLE,           \
     &orm_driver_error_ptr_cmeta_type, CMETA_ABI_OBJECT_POINTER))             \
  X(I, F3, orm_status_t, resolve_active, io,                                  \
    &orm_driver_status_cmeta_type, CMETA_ABI_SCALAR,                          \
    (const orm_tidesdb_active_resolve_config *, config,                       \
     CMETA_PARAM_IN | CMETA_PARAM_BORROWED,                                   \
     &orm_tidesdb_resolve_config_ptr_cmeta_type, CMETA_ABI_OBJECT_POINTER),   \
    (orm_tidesdb_active_result *, result,                                     \
     CMETA_PARAM_OUT | CMETA_PARAM_BORROWED,                                  \
     &orm_tidesdb_active_result_ptr_cmeta_type, CMETA_ABI_OBJECT_POINTER),    \
    (orm_error_t *, error,                                                    \
     CMETA_PARAM_OUT | CMETA_PARAM_BORROWED | CMETA_PARAM_NULLABLE,           \
     &orm_driver_error_ptr_cmeta_type, CMETA_ABI_OBJECT_POINTER))

CMETA_INTERFACE(TurboDb_TidesMaintenance, ORM_TIDESDB_MAINTENANCE_METHODS);

static inline orm_status_t orm_runtime_tidesdb_publish_generation(
    orm_runtime_t *runtime, orm_string_view_t driver_id,
    const orm_tidesdb_generation_publish_config *config,
    orm_tidesdb_generation_publish_result *result, orm_error_t *error) {
  orm_runtime_driver_extension_t *extension = NULL;
  void *binding = NULL;
  orm_status_t status = orm_runtime_driver_acquire_extension(
      runtime, driver_id, orm_view(ORM_TIDESDB_MAINTENANCE_EXPORT_ID),
      orm_view(ORM_TIDESDB_MAINTENANCE_CONTRACT_ID),
      ORM_TIDESDB_MAINTENANCE_CONTRACT_VERSION,
      TurboDb_TidesMaintenance_interface(), &extension, &binding, error);
  if (status != ORM_STATUS_OK) return status;

  TurboDb_TidesMaintenance *maintenance = (TurboDb_TidesMaintenance *)binding;
  if (!TurboDb_TidesMaintenance_valid(maintenance)) {
    (void)orm_runtime_driver_release_extension(extension, NULL);
    return ORM_STATUS_ABI_MISMATCH;
  }

  status = TurboDb_TidesMaintenance_publish_generation(
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
    const orm_tidesdb_active_resolve_config *config,
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
  if (!TurboDb_TidesMaintenance_valid(maintenance)) {
    (void)orm_runtime_driver_release_extension(extension, NULL);
    return ORM_STATUS_ABI_MISMATCH;
  }

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

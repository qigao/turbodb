#ifndef ORM_RUNTIME_H
#define ORM_RUNTIME_H

#include <orm_driver_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ORM_RUNTIME_ABI_VERSION UINT32_C(1)
#define ORM_RUNTIME_DEFAULT_MAX_DRIVERS UINT32_C(16)
#define ORM_RUNTIME_DEFAULT_MAX_ALIASES_PER_DRIVER UINT32_C(4)
#define ORM_RUNTIME_DEFAULT_MAX_CONNECTIONS UINT32_C(256)
#define ORM_RUNTIME_DEFAULT_MAX_MODULE_PATH_BYTES UINT64_C(4096)
#define ORM_RUNTIME_DEFAULT_MAX_PENDING_OPERATIONS UINT32_C(256)
#define ORM_RUNTIME_DEFAULT_MAX_CONTROL_BYTES UINT64_C(1048576)
#define ORM_RUNTIME_DRIVER_ID_CAPACITY (ORM_DRIVER_ID_MAX_BYTES + UINT32_C(1))

typedef struct orm_runtime orm_runtime_t;

typedef void (ORM_C_CALL *orm_runtime_cleanup_error_fn)(
    void *context, const orm_error_t *error);

typedef struct orm_runtime_execution_config {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t execution_model;
  void *executor;
  void *owner_context;
} orm_runtime_execution_config_t;

typedef struct orm_runtime_config {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t max_drivers;
  uint32_t max_aliases_per_driver;
  uint32_t max_connections;
  uint32_t max_pending_operations;
  uint64_t max_module_path_bytes;
  uint64_t max_control_bytes;
  orm_runtime_execution_config_t execution;
  orm_runtime_cleanup_error_fn on_cleanup_error;
  void *cleanup_context;
} orm_runtime_config_t;

typedef struct orm_driver_load_config {
  uint32_t struct_size;
  uint32_t abi_version;
  orm_string_view_t module_path;
  orm_string_view_t expected_driver_id;
  uint32_t flags;
  uint32_t reserved;
} orm_driver_load_config_t;

typedef struct orm_driver_info {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t canonical_id_size;
  uint32_t reserved;
  char canonical_id[ORM_RUNTIME_DRIVER_ID_CAPACITY];
  uint64_t capabilities;
  uint64_t execution_models;
  uint8_t bundle_id[ORM_DRIVER_BUNDLE_ID_BYTES];
} orm_driver_info_t;

/* Initializes bounded caller-blocking defaults and installs the default
 * fail-fast delayed-cleanup handler. This function allocates nothing. */
ORM_C_API void ORM_C_CALL
orm_runtime_config_init(orm_runtime_config_t *config);

/* Creates an explicit empty registry. Failure clears *out_runtime. */
ORM_C_API orm_status_t ORM_C_CALL
orm_runtime_create(const orm_runtime_config_t *config,
                   orm_runtime_t **out_runtime, orm_error_t *error);

/* Loads exactly the caller-supplied absolute module path. No directory scan,
 * fallback, download, alias inference, or implicit connect occurs. The
 * expected ID must equal the module's canonical ID. */
ORM_C_API orm_status_t ORM_C_CALL
orm_runtime_load_driver(orm_runtime_t *runtime,
                        const orm_driver_load_config_t *config,
                        orm_error_t *error);

/* Copies bounded metadata for a canonical ID or registered alias. */
ORM_C_API orm_status_t ORM_C_CALL
orm_runtime_driver_info(orm_runtime_t *runtime, orm_string_view_t id,
                        orm_driver_info_t *out_info, orm_error_t *error);

/* Checked close finalizes modules in reverse registration order and unloads
 * them only after all runtime dependents/admissions are gone. BUSY changes no
 * state. Repeated close on a held closed runtime returns OK. */
ORM_C_API orm_status_t ORM_C_CALL
orm_runtime_close(orm_runtime_t *runtime, orm_error_t *error);

/* Caller must already own a valid strong runtime reference. retain is not a
 * stale-pointer probe; release consumes one reference and NULL is a no-op. */
ORM_C_API void ORM_C_CALL orm_runtime_retain(orm_runtime_t *runtime);
ORM_C_API void ORM_C_CALL orm_runtime_release(orm_runtime_t *runtime);

#ifdef __cplusplus
}
#endif
#endif

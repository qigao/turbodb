#ifndef ORM_RUNTIME_INTERNAL_H
#define ORM_RUNTIME_INTERNAL_H

#include <orm_runtime.h>
#include <orm_driver_plugin.h>
#include <salts/thread.h>

/* Runtime state is private to this directory. The mutex protects reservations,
 * counters and lifecycle transitions; plugin callbacks run outside that lock.
 * Driver slots are bounded and never relocated. Borrowed bindings may only be
 * invoked under a Salts Plugin lease, while a dependent pins the runtime. */
typedef struct orm_runtime_id {
  uint32_t size;
  char text[ORM_RUNTIME_DRIVER_ID_CAPACITY];
} orm_runtime_id;

typedef struct orm_runtime_driver {
  salts_plugin_ref plugin;
  TurboDb_Driver *binding;
  char *module_path;
  orm_runtime_id canonical;
  uint64_t capabilities;
  uint64_t execution_models;
  orm_driver_storage_capabilities_v1 storage;
  uint8_t bundle_id[ORM_DRIVER_BUNDLE_ID_BYTES];
} orm_runtime_driver;

enum {
  ORM_RUNTIME_OPEN = 0u,
  ORM_RUNTIME_CLOSED = 1u,
  ORM_RUNTIME_FAILED = 2u,
  ORM_RUNTIME_CLOSING = 3u
};

struct orm_runtime {
  salts_mutex_t mutex;
  uint32_t refs;
  uint32_t closed;
  orm_runtime_config_t config;
  salts_plugin_registry plugins;
  orm_runtime_driver *drivers;
  uint32_t driver_count;
  uint32_t close_remaining;
  uint32_t dependents;
  uint32_t pending_operations;
  uint32_t extension_count;
  uint32_t load_active;
};

orm_status_t runtime_result(
    orm_error_t *error, orm_status_t status, const char *message);
orm_status_t runtime_plugin_status(
    salts_plugin_status status, orm_error_t *error, const char *context);
int runtime_id_valid(orm_string_view_t id);
int runtime_same_bytes(const orm_runtime_id *id, const void *data, uint64_t size);
/* Caller holds the mutex or a serialized load reservation. */
orm_runtime_driver *runtime_find_driver(
    orm_runtime_t *runtime, orm_string_view_t id);
orm_status_t runtime_acquire_dependent(orm_runtime_t *runtime, orm_error_t *error);
void runtime_finish_pending(orm_runtime_t *runtime, int load_operation);
void runtime_drop_dependent(orm_runtime_t *runtime);
void runtime_drop_extension(orm_runtime_t *runtime);

#endif /* ORM_RUNTIME_INTERNAL_H */

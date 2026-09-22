#include <orm_runtime.h>

#include "orm_module_loader.h"
#include "../abi/orm_internal.h"
#include "../driver/orm_driver_contract.h"
#include "../driver/orm_driver_owner_bridge.h"
#include "../driver/orm_driver_plan_view.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RUNTIME_HEADER(T) {(uint32_t)sizeof(T), ORM_DRIVER_ABI_VERSION}
#define RUNTIME_TABLE(p) {(p), (uint32_t)sizeof(*(p)), 0u}
#define RUNTIME_FIELD_END(T, field) \
  ((uint32_t)(offsetof(T, field) + sizeof(((T *)0)->field)))

typedef int32_t (ORM_DRIVER_CALL *orm_runtime_bootstrap_fn)(
    const orm_driver_host_v1 *, uint32_t,
    const orm_driver_api_v1 **, uint32_t *);

typedef struct orm_runtime_id {
  uint32_t size;
  char text[ORM_RUNTIME_DRIVER_ID_CAPACITY];
} orm_runtime_id;

typedef struct orm_runtime_driver {
  orm_module_handle module;
  void *module_context;
  const orm_driver_api_v1 *api;
  uint32_t api_bytes;
  orm_driver_module_ops_v1 module_ops;
  orm_driver_create_fn create_connection;
  orm_driver_connection_ops_v1 connection_ops;
  char *module_path;
  orm_runtime_id canonical;
  uint32_t alias_count;
  uint64_t capabilities;
  uint64_t execution_models;
  uint8_t bundle_id[ORM_DRIVER_BUNDLE_ID_BYTES];
} orm_runtime_driver;

struct orm_runtime {
  uint32_t refs;
  uint32_t closed;
  orm_runtime_config_t config;
  orm_runtime_driver *drivers;
  orm_runtime_id *aliases;
  uint32_t driver_count;
  uint32_t dependents;
};

static const uint8_t runtime_bundle[ORM_DRIVER_BUNDLE_ID_BYTES] =
    ORM_DRIVER_BUNDLE_ID_INIT;

static orm_status_t runtime_result(orm_error_t *error, orm_status_t status,
                                   const char *message) {
  orm_error_init(error);
  orm_error_set(error, status, message);
  return status;
}

static int runtime_id_valid(orm_string_view_t id) {
  if (id.data == NULL || id.len == 0u || id.len > ORM_DRIVER_ID_MAX_BYTES)
    return 0;
  const unsigned char *text = (const unsigned char *)id.data;
  if (text[0] < 'a' || text[0] > 'z')
    return 0;
  for (size_t i = 1u; i < id.len; ++i) {
    const unsigned char c = text[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '_' || c == '-'))
      return 0;
  }
  return 1;
}

static int runtime_same_bytes(const orm_runtime_id *id,
                              const void *data, uint64_t size) {
  return size == id->size &&
         (size == 0u || memcmp(id->text, data, (size_t)size) == 0);
}

static void runtime_copy_id(orm_runtime_id *out,
                            orm_driver_bytes_v1 value) {
  memset(out, 0, sizeof(*out));
  out->size = (uint32_t)value.size;
  if (value.size != 0u)
    memcpy(out->text, value.data, (size_t)value.size);
}

static orm_runtime_id *runtime_alias_slot(orm_runtime_t *runtime,
                                          uint32_t driver_index,
                                          uint32_t alias_index) {
  const size_t offset =
      (size_t)driver_index * runtime->config.max_aliases_per_driver +
      alias_index;
  return &runtime->aliases[offset];
}

static int runtime_id_conflicts(orm_runtime_t *runtime,
                                orm_driver_bytes_v1 id) {
  for (uint32_t i = 0u; i < runtime->driver_count; ++i) {
    orm_runtime_driver *driver = &runtime->drivers[i];
    if (runtime_same_bytes(&driver->canonical, id.data, id.size))
      return 1;
    for (uint32_t j = 0u; j < driver->alias_count; ++j) {
      if (runtime_same_bytes(runtime_alias_slot(runtime, i, j),
                             id.data, id.size))
        return 1;
    }
  }
  return 0;
}

static orm_runtime_driver *runtime_find_driver(orm_runtime_t *runtime,
                                               orm_string_view_t id) {
  for (uint32_t i = 0u; i < runtime->driver_count; ++i) {
    orm_runtime_driver *driver = &runtime->drivers[i];
    if (runtime_same_bytes(&driver->canonical, id.data, id.len))
      return driver;
    for (uint32_t j = 0u; j < driver->alias_count; ++j) {
      if (runtime_same_bytes(runtime_alias_slot(runtime, i, j),
                             id.data, id.len))
        return driver;
    }
  }
  return NULL;
}

static int runtime_path_registered(orm_runtime_t *runtime, const char *path) {
  if (path == NULL) return 0;
  for (uint32_t i = 0u; i < runtime->driver_count; ++i) {
    const char *registered = runtime->drivers[i].module_path;
    if (registered != NULL && strcmp(registered, path) == 0)
      return 1;
  }
  return 0;
}

static orm_status_t runtime_acquire_dependent(
    orm_runtime_t *runtime, orm_error_t *error) {
  if (runtime->closed != 0u)
    return runtime_result(error, ORM_STATUS_INVALID_STATE,
                          "runtime is closed");
  if (runtime->dependents == runtime->config.max_connections)
    return runtime_result(error, ORM_STATUS_LIMIT_EXCEEDED,
                          "runtime connection budget is full");
  ++runtime->dependents;
  orm_runtime_retain(runtime);
  return ORM_STATUS_OK;
}

static void runtime_release_dependent(orm_runtime_t *runtime) {
  if (runtime == NULL || runtime->dependents == 0u)
    abort();
  --runtime->dependents;
  orm_runtime_release(runtime);
}

static orm_driver_limits_v1 runtime_driver_limits(const orm_limits *limits) {
  orm_driver_limits_v1 out;
  memset(&out, 0, sizeof(out));
  out.header.struct_size = (uint32_t)sizeof(out);
  out.header.abi_version = ORM_DRIVER_ABI_VERSION;
  out.max_parameters = (uint64_t)limits->max_parameters;
  out.max_columns = (uint64_t)limits->max_columns;
  out.max_predicates = (uint64_t)limits->max_predicates;
  out.max_assignments = (uint64_t)limits->max_assignments;
  out.max_query_bytes = (uint64_t)limits->max_query_bytes;
  out.max_parameter_bytes = (uint64_t)limits->max_parameter_bytes;
  out.max_result_rows = limits->max_result_rows;
  out.max_result_bytes = limits->max_result_bytes;
  return out;
}

typedef struct orm_runtime_backend {
  orm_runtime_t *runtime;
  orm_runtime_driver *driver;
  orm_driver_connection_v1 native;
  orm_driver_connection_ops_v1 ops;
} orm_runtime_backend;

static void runtime_backend_destroy(void *context) {
  orm_runtime_backend *backend = context;
  if (backend == NULL) return;
  if (backend->native.context != NULL)
    backend->ops.destroy(backend->native.context);
  orm_runtime_t *runtime = backend->runtime;
  free(backend);
  runtime_release_dependent(runtime);
}

typedef struct orm_runtime_cursor {
  orm_driver_cursor_v1 native;
  orm_driver_cursor_ops_v1 ops;
  uint64_t execution_models;
  char error_message[ORM_C_ERROR_MESSAGE_CAPACITY];
} orm_runtime_cursor;

static void runtime_cursor_error(orm_runtime_cursor *cursor,
                                 orm_status_t status,
                                 const char *message) {
  if (cursor == NULL) return;
  (void)snprintf(cursor->error_message, sizeof(cursor->error_message), "%s",
                 message != NULL && message[0] != '\0'
                     ? message
                     : orm_status_message(status));
}

static orm_row_cursor_step runtime_cursor_next(
    void *context, cserde_reader *reader) {
  orm_runtime_cursor *cursor = context;
  orm_row_cursor_step out = ORM_ROW_CURSOR_STEP_INIT;
  if (cursor == NULL || reader == NULL) {
    out.kind = ORM_ROW_CURSOR_ERROR;
    out.status = ORM_STATUS_INVALID_ARGUMENT;
    out.message = "invalid runtime driver cursor next";
    return out;
  }

  orm_driver_step_v1 step;
  memset(&step, 0, sizeof(step));
  step.header = (orm_driver_header_v1)RUNTIME_HEADER(orm_driver_step_v1);
  orm_error_t error;
  orm_error_init(&error);
  const orm_status_t status =
      cursor->ops.next(cursor->native.context, reader, &step, &error);
  if (status != ORM_STATUS_OK) {
    runtime_cursor_error(cursor, status, error.message);
    out.kind = ORM_ROW_CURSOR_ERROR;
    out.status = status;
    out.message = cursor->error_message;
    return out;
  }

  uint32_t declared = 0u;
  const orm_status_t header_status = orm_driver_check_prefix(
      &step, (uint32_t)sizeof(step), ORM_DRIVER_ABI_VERSION,
      (uint32_t)sizeof(step), &declared);
  if (header_status != ORM_STATUS_OK || step.reserved != 0u) {
    runtime_cursor_error(cursor, ORM_STATUS_ABI_MISMATCH,
                         "driver returned an invalid cursor step");
    out.kind = ORM_ROW_CURSOR_ERROR;
    out.status = ORM_STATUS_ABI_MISMATCH;
    out.message = cursor->error_message;
    return out;
  }

  switch (step.kind) {
  case ORM_DRIVER_STEP_ROW:
    out.kind = ORM_ROW_CURSOR_ROW;
    return out;
  case ORM_DRIVER_STEP_ROW_AND_DONE:
    out.kind = ORM_ROW_CURSOR_ROW_AND_DONE;
    return out;
  case ORM_DRIVER_STEP_WAIT:
    if ((cursor->execution_models & ORM_DRIVER_EXEC_NATIVE_WAIT) == 0u) {
      runtime_cursor_error(cursor, ORM_STATUS_UNSUPPORTED,
                           "driver returned WAIT without declaring native WAIT");
      out.kind = ORM_ROW_CURSOR_ERROR;
      out.status = ORM_STATUS_UNSUPPORTED;
      out.message = cursor->error_message;
      return out;
    }
    out.kind = ORM_ROW_CURSOR_WAIT;
    out.waitable = step.waitable;
    return out;
  case ORM_DRIVER_STEP_DONE:
    out.kind = ORM_ROW_CURSOR_DONE;
    return out;
  case ORM_DRIVER_STEP_ERROR:
    runtime_cursor_error(cursor, ORM_STATUS_DATASTORE_ERROR,
                         "driver returned ERROR without failure status");
    out.kind = ORM_ROW_CURSOR_ERROR;
    out.status = ORM_STATUS_DATASTORE_ERROR;
    out.message = cursor->error_message;
    return out;
  default:
    runtime_cursor_error(cursor, ORM_STATUS_ABI_MISMATCH,
                         "driver returned an unknown cursor step");
    out.kind = ORM_ROW_CURSOR_ERROR;
    out.status = ORM_STATUS_ABI_MISMATCH;
    out.message = cursor->error_message;
    return out;
  }
}

static void runtime_cursor_cancel(void *context) {
  orm_runtime_cursor *cursor = context;
  if (cursor != NULL && cursor->native.context != NULL)
    cursor->ops.cancel(cursor->native.context);
}

static void runtime_cursor_destroy(void *context) {
  orm_runtime_cursor *cursor = context;
  if (cursor == NULL) return;
  if (cursor->native.context != NULL)
    cursor->ops.destroy(cursor->native.context);
  free(cursor);
}

static orm_status_t runtime_cursor_configure_shape(
    void *context, const cmeta_data_desc *shape, orm_error_t *error) {
  orm_runtime_cursor *cursor = context;
  if (cursor == NULL || shape == NULL)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid runtime driver cursor shape");
  if (cursor->ops.configure_shape == NULL)
    return runtime_result(error, ORM_STATUS_OK, NULL);
  return cursor->ops.configure_shape(cursor->native.context, shape, error);
}

static orm_status_t runtime_cursor_column_count(
    void *context, uint64_t *out_count) {
  orm_runtime_cursor *cursor = context;
  if (out_count != NULL) *out_count = 0u;
  if (cursor == NULL || out_count == NULL)
    return ORM_STATUS_INVALID_ARGUMENT;
  if (cursor->ops.column_count == NULL)
    return ORM_STATUS_UNSUPPORTED;
  orm_error_t error;
  orm_error_init(&error);
  return cursor->ops.column_count(cursor->native.context, out_count, &error);
}

static const orm_row_cursor_ops runtime_cursor_ops = {
    sizeof(orm_row_cursor_ops), ORM_ROW_CURSOR_OPS_ABI_VERSION,
    "runtime-driver", runtime_cursor_next, runtime_cursor_cancel,
    runtime_cursor_destroy, runtime_cursor_configure_shape,
    runtime_cursor_column_count};

static orm_status_t runtime_backend_open_cursor(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    orm_row_cursor *out_cursor, orm_error_t *error) {
  if (out_cursor != NULL) memset(out_cursor, 0, sizeof(*out_cursor));
  orm_runtime_backend *backend = context;
  if (backend == NULL || plan == NULL || limits == NULL ||
      out_cursor == NULL || backend->ops.open_cursor == NULL)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid runtime driver cursor open");

  orm_runtime_cursor *cursor =
      (orm_runtime_cursor *)calloc(1u, sizeof(*cursor));
  if (cursor == NULL)
    return runtime_result(error, ORM_STATUS_OUT_OF_MEMORY,
                          "allocate runtime driver cursor adapter");

  orm_driver_plan_view_v1 view;
  memset(&view, 0, sizeof(view));
  orm_status_t status = orm_driver_plan_borrow(plan, &view, error);
  if (status != ORM_STATUS_OK) {
    free(cursor);
    return status;
  }

  const orm_driver_limits_v1 driver_limits = runtime_driver_limits(limits);
  orm_driver_cursor_v1 native;
  memset(&native, 0, sizeof(native));
  status = backend->ops.open_cursor(
      backend->native.context, &view, &driver_limits, &native, error);
  if (status != ORM_STATUS_OK) {
    free(cursor);
    return status;
  }

  status = orm_driver_validate_cursor_v1(
      &native, (uint32_t)sizeof(native), backend->driver->capabilities, error);
  if (status != ORM_STATUS_OK) {
    /* A driver that reports successful creation with an invalid ownership
     * descriptor has violated the ABI. No unvalidated callback is invoked. */
    free(cursor);
    return status;
  }

  cursor->native = native;
  cursor->execution_models = backend->driver->execution_models;
  {
    size_t cursor_ops_bytes = native.ops.bytes;
    if (cursor_ops_bytes > sizeof(cursor->ops))
      cursor_ops_bytes = sizeof(cursor->ops);
    memcpy(&cursor->ops, native.ops.data, cursor_ops_bytes);
  }

  out_cursor->ops = &runtime_cursor_ops;
  out_cursor->context = cursor;
  return runtime_result(error, ORM_STATUS_OK, NULL);
}

static orm_status_t runtime_backend_execute_command(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    uint64_t *affected_rows, orm_error_t *error) {
  (void)context;
  (void)plan;
  (void)limits;
  if (affected_rows != NULL) *affected_rows = 0u;
  orm_error_set(error, ORM_STATUS_UNSUPPORTED,
                "runtime driver command adapter is not implemented");
  return ORM_STATUS_UNSUPPORTED;
}

static orm_status_t runtime_backend_begin_transaction(
    void *context, orm_isolation_t isolation,
    orm_transaction_backend *out_transaction, orm_error_t *error) {
  (void)context;
  (void)isolation;
  if (out_transaction != NULL)
    memset(out_transaction, 0, sizeof(*out_transaction));
  orm_error_set(error, ORM_STATUS_UNSUPPORTED,
                "runtime driver transaction adapter is not implemented");
  return ORM_STATUS_UNSUPPORTED;
}

static const orm_backend_ops runtime_backend_ops = {
    sizeof(orm_backend_ops), ORM_BACKEND_OPS_ABI_VERSION,
    runtime_backend_destroy, runtime_backend_open_cursor,
    runtime_backend_execute_command, runtime_backend_begin_transaction};

typedef struct orm_runtime_factory_context {
  orm_runtime_t *runtime;
  orm_runtime_driver *driver;
} orm_runtime_factory_context;

static orm_status_t runtime_backend_factory(
    const orm_config_t *config, const orm_limits *limits, void *context,
    orm_backend *out_backend, orm_error_t *error) {
  if (out_backend != NULL) memset(out_backend, 0, sizeof(*out_backend));
  if (config == NULL || limits == NULL || context == NULL ||
      out_backend == NULL)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid runtime driver connection factory");

  orm_runtime_factory_context *factory = context;
  orm_status_t status = runtime_acquire_dependent(factory->runtime, error);
  if (status != ORM_STATUS_OK) return status;

  orm_driver_connection_v1 native;
  memset(&native, 0, sizeof(native));
  const orm_driver_limits_v1 driver_limits = runtime_driver_limits(limits);
  status = factory->driver->create_connection(
      factory->driver->module_context, config, &driver_limits, &native, error);
  if (status != ORM_STATUS_OK) {
    runtime_release_dependent(factory->runtime);
    return status;
  }

  status = orm_driver_validate_connection_v1(
      &native, (uint32_t)sizeof(native), factory->driver->capabilities, error);
  if (status != ORM_STATUS_OK) {
    if (native.context != NULL)
      factory->driver->connection_ops.destroy(native.context);
    runtime_release_dependent(factory->runtime);
    return status;
  }

  orm_runtime_backend *backend =
      (orm_runtime_backend *)calloc(1u, sizeof(*backend));
  if (backend == NULL) {
    factory->driver->connection_ops.destroy(native.context);
    runtime_release_dependent(factory->runtime);
    return runtime_result(error, ORM_STATUS_OUT_OF_MEMORY,
                          "allocate runtime driver connection adapter");
  }

  backend->runtime = factory->runtime;
  backend->driver = factory->driver;
  backend->native = native;
  memset(&backend->ops, 0, sizeof(backend->ops));
  {
    size_t connection_ops_bytes = native.ops.bytes;
    if (connection_ops_bytes > sizeof(backend->ops))
      connection_ops_bytes = sizeof(backend->ops);
    memcpy(&backend->ops, native.ops.data, connection_ops_bytes);
  }
  out_backend->ops = &runtime_backend_ops;
  out_backend->context = backend;
  return runtime_result(error, ORM_STATUS_OK, NULL);
}

static orm_driver_host_v1 runtime_host(void) {
  const orm_driver_plan_metadata_ops_v1 *metadata =
      orm_driver_plan_metadata_services_v1();
  const orm_driver_plan_value_ops_v1 *values =
      orm_driver_plan_value_services_v1();
  const orm_driver_lifetime_ops_v1 *lifetime =
      orm_driver_owner_services_v1();
  orm_driver_host_v1 host;
  memset(&host, 0, sizeof(host));
  host.header = (orm_driver_header_v1)RUNTIME_HEADER(orm_driver_host_v1);
  memcpy(host.bundle_id, runtime_bundle, sizeof(host.bundle_id));
  host.plan_metadata = (orm_driver_table_v1)RUNTIME_TABLE(metadata);
  host.plan_values = (orm_driver_table_v1)RUNTIME_TABLE(values);
  host.lifetime = (orm_driver_table_v1)RUNTIME_TABLE(lifetime);
  return host;
}

static orm_status_t runtime_validate_config(
    const orm_runtime_config_t *config, orm_error_t *error) {
  if (config == NULL || config->struct_size < sizeof(*config) ||
      config->abi_version != ORM_RUNTIME_ABI_VERSION ||
      config->max_drivers == 0u || config->max_connections == 0u ||
      config->max_pending_operations == 0u ||
      config->max_module_path_bytes == 0u || config->max_control_bytes == 0u ||
      config->execution.struct_size < sizeof(config->execution) ||
      config->execution.abi_version != ORM_RUNTIME_ABI_VERSION)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid runtime configuration");
  if (config->execution.execution_model != ORM_DRIVER_EXEC_CALLER_BLOCKING ||
      config->execution.executor != NULL ||
      config->execution.owner_context != NULL)
    return runtime_result(error, ORM_STATUS_UNSUPPORTED,
                          "runtime execution model is not implemented");
  if (config->max_aliases_per_driver != 0u &&
      (size_t)config->max_drivers >
          SIZE_MAX / (size_t)config->max_aliases_per_driver)
    return runtime_result(error, ORM_STATUS_LIMIT_EXCEEDED,
                          "runtime registry size exceeds addressable memory");
  return ORM_STATUS_OK;
}

void ORM_C_CALL orm_runtime_config_init(orm_runtime_config_t *config) {
  if (config == NULL)
    return;
  memset(config, 0, sizeof(*config));
  config->struct_size = (uint32_t)sizeof(*config);
  config->abi_version = ORM_RUNTIME_ABI_VERSION;
  config->max_drivers = ORM_RUNTIME_DEFAULT_MAX_DRIVERS;
  config->max_aliases_per_driver =
      ORM_RUNTIME_DEFAULT_MAX_ALIASES_PER_DRIVER;
  config->max_connections = ORM_RUNTIME_DEFAULT_MAX_CONNECTIONS;
  config->max_pending_operations =
      ORM_RUNTIME_DEFAULT_MAX_PENDING_OPERATIONS;
  config->max_module_path_bytes =
      ORM_RUNTIME_DEFAULT_MAX_MODULE_PATH_BYTES;
  config->max_control_bytes = ORM_RUNTIME_DEFAULT_MAX_CONTROL_BYTES;
  config->execution.struct_size =
      (uint32_t)sizeof(config->execution);
  config->execution.abi_version = ORM_RUNTIME_ABI_VERSION;
  config->execution.execution_model = ORM_DRIVER_EXEC_CALLER_BLOCKING;
}

orm_status_t ORM_C_CALL
orm_runtime_create(const orm_runtime_config_t *config,
                   orm_runtime_t **out_runtime, orm_error_t *error) {
  if (out_runtime != NULL)
    *out_runtime = NULL;
  if (out_runtime == NULL)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "runtime output is required");
  orm_status_t status = runtime_validate_config(config, error);
  if (status != ORM_STATUS_OK)
    return status;

  orm_runtime_t *runtime = (orm_runtime_t *)calloc(1u, sizeof(*runtime));
  if (runtime == NULL)
    return runtime_result(error, ORM_STATUS_OUT_OF_MEMORY,
                          "allocate runtime");

  runtime->drivers = (orm_runtime_driver *)calloc(
      config->max_drivers, sizeof(*runtime->drivers));
  const size_t alias_count =
      (size_t)config->max_drivers * config->max_aliases_per_driver;
  if (runtime->drivers == NULL ||
      (alias_count != 0u &&
       (runtime->aliases = (orm_runtime_id *)calloc(
            alias_count, sizeof(*runtime->aliases))) == NULL)) {
    free(runtime->aliases);
    free(runtime->drivers);
    free(runtime);
    return runtime_result(error, ORM_STATUS_OUT_OF_MEMORY,
                          "allocate runtime registry");
  }

  runtime->refs = 1u;
  runtime->config = *config;
  *out_runtime = runtime;
  return runtime_result(error, ORM_STATUS_OK, NULL);
}

static orm_status_t runtime_copy_path(orm_runtime_t *runtime,
                                      orm_string_view_t path,
                                      char **out,
                                      orm_error_t *error) {
  *out = NULL;
  if (path.data == NULL || path.len == 0u ||
      path.len > runtime->config.max_module_path_bytes ||
      path.len == SIZE_MAX ||
      memchr(path.data, '\0', path.len) != NULL)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid driver module path");
  char *copy = (char *)malloc(path.len + 1u);
  if (copy == NULL)
    return runtime_result(error, ORM_STATUS_OUT_OF_MEMORY,
                          "allocate driver module path");
  memcpy(copy, path.data, path.len);
  copy[path.len] = '\0';
  *out = copy;
  return ORM_STATUS_OK;
}

orm_status_t ORM_C_CALL
orm_runtime_load_driver(orm_runtime_t *runtime,
                        const orm_driver_load_config_t *config,
                        orm_error_t *error) {
  if (runtime == NULL || config == NULL ||
      config->struct_size < sizeof(*config) ||
      config->abi_version != ORM_RUNTIME_ABI_VERSION ||
      config->flags != 0u || config->reserved != 0u ||
      !runtime_id_valid(config->expected_driver_id))
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid driver load configuration");
  if (runtime->closed != 0u)
    return runtime_result(error, ORM_STATUS_INVALID_STATE,
                          "runtime is closed");
  if (runtime->driver_count == runtime->config.max_drivers)
    return runtime_result(error, ORM_STATUS_LIMIT_EXCEEDED,
                          "runtime driver registry is full");

  char *path = NULL;
  orm_status_t status =
      runtime_copy_path(runtime, config->module_path, &path, error);
  if (status != ORM_STATUS_OK)
    return status;

  if (runtime_path_registered(runtime, path)) {
    free(path);
    return runtime_result(error, ORM_STATUS_DRIVER_ALREADY_REGISTERED,
                          "driver module path is already registered");
  }

  orm_module_handle module = {0};
  status = orm_module_open_absolute(path, &module, error);
  if (status != ORM_STATUS_OK) {
    free(path);
    return status;
  }

  orm_runtime_bootstrap_fn bootstrap = NULL;
  status = orm_module_symbol(&module, "orm_driver_get_api_v1",
                             &bootstrap, sizeof(bootstrap), error);
  if (status != ORM_STATUS_OK)
    goto fail_module;

  const orm_driver_host_v1 host = runtime_host();
  const orm_driver_api_v1 *api = NULL;
  uint32_t api_bytes = 0u;
  const int32_t bootstrap_status =
      bootstrap(&host, (uint32_t)sizeof(host), &api, &api_bytes);
  if (bootstrap_status != ORM_STATUS_OK) {
    status = (orm_status_t)bootstrap_status;
    runtime_result(error, status, "driver bootstrap failed");
    goto fail_module;
  }
  if (api == NULL) {
    status = ORM_STATUS_ABI_MISMATCH;
    runtime_result(error, status, "driver bootstrap returned no descriptor");
    goto fail_module;
  }

  uint32_t declared = 0u;
  status = orm_driver_check_prefix(
      api, api_bytes, ORM_DRIVER_ABI_VERSION,
      RUNTIME_FIELD_END(orm_driver_api_v1, canonical_id), &declared);
  if (status != ORM_STATUS_OK) {
    runtime_result(error, status, "driver descriptor prefix is invalid");
    goto fail_module;
  }

  orm_driver_bytes_v1 canonical;
  memcpy(&canonical, &api->canonical_id, sizeof(canonical));
  status = orm_driver_validate_api_v1(
      api, api_bytes, runtime_bundle, canonical,
      runtime->config.max_aliases_per_driver, error);
  if (status != ORM_STATUS_OK)
    goto fail_module;

  if (canonical.size != config->expected_driver_id.len ||
      memcmp(canonical.data, config->expected_driver_id.data,
             (size_t)canonical.size) != 0) {
    status = ORM_STATUS_DRIVER_ID_MISMATCH;
    runtime_result(error, status, "driver canonical ID does not match request");
    goto fail_module;
  }
  if ((api->execution_models &
       runtime->config.execution.execution_model) == 0u) {
    status = ORM_STATUS_UNSUPPORTED;
    runtime_result(error, status,
                   "driver does not support runtime execution model");
    goto fail_module;
  }

  if (runtime_id_conflicts(runtime, canonical)) {
    status = ORM_STATUS_DRIVER_ALREADY_REGISTERED;
    runtime_result(error, status, "driver ID is already registered");
    goto fail_module;
  }
  for (uint32_t i = 0u; i < api->alias_count; ++i) {
    orm_driver_bytes_v1 alias;
    memcpy(&alias,
           (const unsigned char *)api->aliases +
               (size_t)i * sizeof(alias),
           sizeof(alias));
    if (runtime_id_conflicts(runtime, alias)) {
      status = ORM_STATUS_DRIVER_ALREADY_REGISTERED;
      runtime_result(error, status, "driver alias is already registered");
      goto fail_module;
    }
  }

  orm_driver_module_ops_v1 module_ops;
  memcpy(&module_ops, api->module_ops.data, sizeof(module_ops));
  void *module_context = NULL;
  status = module_ops.initialize(&host, &module_context, error);
  if (status != ORM_STATUS_OK)
    goto fail_module;
  if (module_context == NULL) {
    status = ORM_STATUS_ABI_MISMATCH;
    runtime_result(error, status,
                   "driver initialize returned no module context");
    goto fail_module;
  }

  const uint32_t index = runtime->driver_count;
  orm_runtime_driver *entry = &runtime->drivers[index];
  memset(entry, 0, sizeof(*entry));
  entry->module = module;
  entry->module_context = module_context;
  entry->module_path = path;
  entry->api = api;
  entry->api_bytes = api_bytes;
  entry->module_ops = module_ops;
  entry->create_connection = api->create_connection;
  memset(&entry->connection_ops, 0, sizeof(entry->connection_ops));
  {
    size_t connection_ops_bytes = api->connection_ops.bytes;
    if (connection_ops_bytes > sizeof(entry->connection_ops))
      connection_ops_bytes = sizeof(entry->connection_ops);
    memcpy(&entry->connection_ops, api->connection_ops.data,
           connection_ops_bytes);
  }
  runtime_copy_id(&entry->canonical, canonical);
  entry->alias_count = api->alias_count;
  entry->capabilities = api->capabilities;
  entry->execution_models = api->execution_models;
  memcpy(entry->bundle_id, api->bundle_id, sizeof(entry->bundle_id));
  for (uint32_t i = 0u; i < api->alias_count; ++i) {
    orm_driver_bytes_v1 alias;
    memcpy(&alias,
           (const unsigned char *)api->aliases +
               (size_t)i * sizeof(alias),
           sizeof(alias));
    runtime_copy_id(runtime_alias_slot(runtime, index, i), alias);
  }
  ++runtime->driver_count;
  return runtime_result(error, ORM_STATUS_OK, NULL);

fail_module:
  orm_module_close(&module);
  free(path);
  return status;
}

orm_status_t ORM_C_CALL
orm_runtime_driver_info(orm_runtime_t *runtime, orm_string_view_t id,
                        orm_driver_info_t *out_info, orm_error_t *error) {
  if (out_info != NULL)
    memset(out_info, 0, sizeof(*out_info));
  if (runtime == NULL || out_info == NULL || !runtime_id_valid(id))
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid driver info request");
  if (runtime->closed != 0u)
    return runtime_result(error, ORM_STATUS_INVALID_STATE,
                          "runtime is closed");
  orm_runtime_driver *driver = runtime_find_driver(runtime, id);
  if (driver == NULL)
    return runtime_result(error, ORM_STATUS_DRIVER_NOT_REGISTERED,
                          "driver is not registered");

  out_info->struct_size = (uint32_t)sizeof(*out_info);
  out_info->abi_version = ORM_RUNTIME_ABI_VERSION;
  out_info->canonical_id_size = driver->canonical.size;
  memcpy(out_info->canonical_id, driver->canonical.text,
         driver->canonical.size);
  out_info->capabilities = driver->capabilities;
  out_info->execution_models = driver->execution_models;
  memcpy(out_info->bundle_id, driver->bundle_id,
         sizeof(out_info->bundle_id));
  return runtime_result(error, ORM_STATUS_OK, NULL);
}

orm_status_t ORM_C_CALL
orm_runtime_connect(orm_runtime_t *runtime, const orm_config_t *config,
                    orm_connection_t **out_connection, orm_error_t *error) {
  if (out_connection != NULL) *out_connection = NULL;
  if (runtime == NULL || config == NULL || out_connection == NULL)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid runtime connect request");
  if (config->struct_size != sizeof(*config) ||
      config->abi_version != ORM_C_ABI_VERSION)
    return runtime_result(error, ORM_STATUS_ABI_MISMATCH,
                          "ORM configuration ABI does not match this build");
  if (!runtime_id_valid(config->driver))
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid runtime driver ID");
  if (runtime->closed != 0u)
    return runtime_result(error, ORM_STATUS_INVALID_STATE,
                          "runtime is closed");

  orm_runtime_driver *driver = runtime_find_driver(runtime, config->driver);
  if (driver == NULL)
    return runtime_result(error, ORM_STATUS_DRIVER_NOT_REGISTERED,
                          "driver is not registered");

  orm_config_t canonical_config = *config;
  canonical_config.driver.data = driver->canonical.text;
  canonical_config.driver.len = driver->canonical.size;
  orm_runtime_factory_context factory = {runtime, driver};
  return orm_connect_with_factory_context_v1(
      &canonical_config, runtime_backend_factory, &factory,
      out_connection, error);
}

orm_status_t ORM_C_CALL
orm_runtime_close(orm_runtime_t *runtime, orm_error_t *error) {
  if (runtime == NULL)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "runtime is required");
  if (runtime->closed == 1u)
    return runtime_result(error, ORM_STATUS_OK, NULL);
  if (runtime->dependents != 0u)
    return runtime_result(error, ORM_STATUS_BUSY,
                          "runtime has active connections");
  if (runtime->closed != 0u)
    return runtime_result(error, ORM_STATUS_CLEANUP_FAILED,
                          "runtime cleanup is quarantined");

  for (uint32_t remaining = runtime->driver_count;
       remaining != 0u; --remaining) {
    orm_runtime_driver *driver = &runtime->drivers[remaining - 1u];
    orm_error_t finalize_error;
    orm_error_init(&finalize_error);
    orm_status_t status =
        driver->module_ops.finalize(driver->module_context,
                                    &finalize_error);
    if (status != ORM_STATUS_OK) {
      runtime->closed = 2u;
      return runtime_result(error, ORM_STATUS_CLEANUP_FAILED,
                            "driver module finalization failed");
    }
    driver->module_context = NULL;
    orm_module_close(&driver->module);
    free(driver->module_path);
    driver->module_path = NULL;
  }
  runtime->closed = 1u;
  return runtime_result(error, ORM_STATUS_OK, NULL);
}

void ORM_C_CALL orm_runtime_retain(orm_runtime_t *runtime) {
  if (runtime == NULL)
    return;
  if (runtime->refs == UINT32_MAX)
    abort();
  ++runtime->refs;
}

void ORM_C_CALL orm_runtime_release(orm_runtime_t *runtime) {
  if (runtime == NULL)
    return;
  if (runtime->refs == 0u)
    abort();
  --runtime->refs;
  if (runtime->refs != 0u)
    return;

  orm_error_t error;
  orm_error_init(&error);
  const orm_status_t status = orm_runtime_close(runtime, &error);
  if (status != ORM_STATUS_OK) {
    if (runtime->config.on_cleanup_error != NULL) {
      runtime->config.on_cleanup_error(runtime->config.cleanup_context,
                                       &error);
      return;
    }
    abort();
  }
  free(runtime->aliases);
  free(runtime->drivers);
  free(runtime);
}

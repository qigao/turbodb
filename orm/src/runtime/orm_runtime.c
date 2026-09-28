#include <orm_runtime.h>
#include <orm_driver_plugin.h>
#include <salts/plugin.h>
#include <salts/thread.h>

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
  uint32_t load_active;
};

static const uint8_t runtime_bundle[ORM_DRIVER_BUNDLE_ID_BYTES] =
    ORM_DRIVER_BUNDLE_ID_INIT;

static orm_status_t runtime_result(orm_error_t *error, orm_status_t status,
                                   const char *message) {
  orm_error_init(error);
  orm_error_set(error, status, message);
  return status;
}

static orm_status_t runtime_plugin_status(
    salts_plugin_status status, orm_error_t *error, const char *context) {
  orm_status_t mapped;
  switch (status) {
  case SALTS_PLUGIN_OK:
    mapped = ORM_STATUS_OK;
    break;
  case SALTS_PLUGIN_INVALID_ARGUMENT:
    mapped = ORM_STATUS_INVALID_ARGUMENT;
    break;
  case SALTS_PLUGIN_ALLOCATION_FAILED:
    mapped = ORM_STATUS_OUT_OF_MEMORY;
    break;
  case SALTS_PLUGIN_CAPACITY_EXCEEDED:
    mapped = ORM_STATUS_LIMIT_EXCEEDED;
    break;
  case SALTS_PLUGIN_DUPLICATE_PLUGIN_ID:
  case SALTS_PLUGIN_ALREADY:
    mapped = ORM_STATUS_DRIVER_ALREADY_REGISTERED;
    break;
  case SALTS_PLUGIN_LOAD_FAILED:
    mapped = ORM_STATUS_DRIVER_LOAD_ERROR;
    break;
  case SALTS_PLUGIN_QUERY_MISSING:
  case SALTS_PLUGIN_UNKNOWN_EXPORT:
    mapped = ORM_STATUS_DRIVER_ENTRY_MISSING;
    break;
  case SALTS_PLUGIN_UNSUPPORTED_ABI:
  case SALTS_PLUGIN_INVALID_MANIFEST:
  case SALTS_PLUGIN_QUERY_REJECTED:
  case SALTS_PLUGIN_INCOMPATIBLE_CONTRACT:
    mapped = ORM_STATUS_ABI_MISMATCH;
    break;
  case SALTS_PLUGIN_BUSY:
    mapped = ORM_STATUS_BUSY;
    break;
  case SALTS_PLUGIN_UNLOAD_FAILED:
    mapped = ORM_STATUS_CLEANUP_FAILED;
    break;
  case SALTS_PLUGIN_UNKNOWN_PLUGIN:
  case SALTS_PLUGIN_STALE:
  case SALTS_PLUGIN_INVALID_STATE:
  case SALTS_PLUGIN_DUPLICATE_EXPORT:
  default:
    mapped = ORM_STATUS_INVALID_STATE;
    break;
  }
  if (mapped == ORM_STATUS_OK)
    return runtime_result(error, mapped, NULL);
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message), "%s: %s",
                 context != NULL ? context : "plugin",
                 salts_plugin_status_string(status));
  return runtime_result(error, mapped, message);
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

static int runtime_id_conflicts(orm_runtime_t *runtime,
                                orm_driver_bytes_v1 id) {
  for (uint32_t i = 0u; i < runtime->driver_count; ++i) {
    if (runtime_same_bytes(&runtime->drivers[i].canonical, id.data, id.size))
      return 1;
  }
  return 0;
}

static orm_runtime_driver *runtime_find_driver(orm_runtime_t *runtime,
                                               orm_string_view_t id) {
  for (uint32_t i = 0u; i < runtime->driver_count; ++i) {
    orm_runtime_driver *driver = &runtime->drivers[i];
    if (runtime_same_bytes(&driver->canonical, id.data, id.len))
      return driver;
  }
  return NULL;
}

static orm_status_t runtime_begin_connect(
    orm_runtime_t *runtime, orm_string_view_t id,
    orm_runtime_driver **out_driver, orm_error_t *error) {
  *out_driver = NULL;
  orm_status_t status = ORM_STATUS_OK;
  const char *message = NULL;
  salts_mutex_lock(&runtime->mutex);
  if (runtime->closed != ORM_RUNTIME_OPEN) {
    status = ORM_STATUS_INVALID_STATE;
    message = "runtime is closed";
  } else if (runtime->pending_operations >=
             runtime->config.max_pending_operations) {
    status = ORM_STATUS_LIMIT_EXCEEDED;
    message = "runtime pending-operation budget is full";
  } else {
    orm_runtime_driver *driver = runtime_find_driver(runtime, id);
    if (driver == NULL) {
      status = ORM_STATUS_DRIVER_NOT_REGISTERED;
      message = "driver is not registered";
    } else {
      ++runtime->pending_operations;
      *out_driver = driver;
    }
  }
  salts_mutex_unlock(&runtime->mutex);
  return runtime_result(error, status, message);
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

static orm_status_t runtime_begin_load(
    orm_runtime_t *runtime, const char *path, orm_error_t *error) {
  orm_status_t status = ORM_STATUS_OK;
  const char *message = NULL;
  salts_mutex_lock(&runtime->mutex);
  if (runtime->closed != ORM_RUNTIME_OPEN) {
    status = ORM_STATUS_INVALID_STATE;
    message = "runtime is closed";
  } else if (runtime->pending_operations >=
             runtime->config.max_pending_operations) {
    status = ORM_STATUS_LIMIT_EXCEEDED;
    message = "runtime pending-operation budget is full";
  } else if (runtime->load_active != 0u) {
    status = ORM_STATUS_BUSY;
    message = "runtime already has a driver load in progress";
  } else if (runtime->driver_count == runtime->config.max_drivers) {
    status = ORM_STATUS_LIMIT_EXCEEDED;
    message = "runtime driver registry is full";
  } else if (runtime_path_registered(runtime, path)) {
    status = ORM_STATUS_DRIVER_ALREADY_REGISTERED;
    message = "driver module path is already registered";
  } else {
    runtime->load_active = 1u;
    ++runtime->pending_operations;
  }
  salts_mutex_unlock(&runtime->mutex);
  return runtime_result(error, status, message);
}

static orm_status_t runtime_acquire_dependent(
    orm_runtime_t *runtime, orm_error_t *error) {
  salts_mutex_lock(&runtime->mutex);
  if (runtime->closed != ORM_RUNTIME_OPEN) {
    salts_mutex_unlock(&runtime->mutex);
    return runtime_result(error, ORM_STATUS_INVALID_STATE,
                          "runtime is closed");
  }
  if (runtime->dependents == runtime->config.max_connections) {
    salts_mutex_unlock(&runtime->mutex);
    return runtime_result(error, ORM_STATUS_LIMIT_EXCEEDED,
                          "runtime connection budget is full");
  }
  if (runtime->refs == UINT32_MAX) {
    salts_mutex_unlock(&runtime->mutex);
    return runtime_result(error, ORM_STATUS_LIMIT_EXCEEDED,
                          "runtime reference budget is full");
  }
  ++runtime->dependents;
  ++runtime->refs;
  salts_mutex_unlock(&runtime->mutex);
  return ORM_STATUS_OK;
}

static int runtime_release_dependent_ref(orm_runtime_t *runtime) {
  int last = 0;
  salts_mutex_lock(&runtime->mutex);
  if (runtime->dependents == 0u || runtime->refs == 0u) {
    salts_mutex_unlock(&runtime->mutex);
    abort();
  }
  --runtime->dependents;
  --runtime->refs;
  last = runtime->refs == 0u;
  salts_mutex_unlock(&runtime->mutex);
  return last;
}

static void runtime_finish_pending(
    orm_runtime_t *runtime, int load_operation) {
  salts_mutex_lock(&runtime->mutex);
  if (runtime->pending_operations == 0u ||
      (load_operation && runtime->load_active == 0u)) {
    salts_mutex_unlock(&runtime->mutex);
    abort();
  }
  --runtime->pending_operations;
  if (load_operation) runtime->load_active = 0u;
  salts_mutex_unlock(&runtime->mutex);
}

static void runtime_release_last(orm_runtime_t *runtime);

static void runtime_drop_dependent(orm_runtime_t *runtime) {
  if (runtime_release_dependent_ref(runtime))
    runtime_release_last(runtime);
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
  salts_plugin_lease plugin_lease;
  orm_driver_connection_v1 native;
  orm_driver_connection_ops_v1 ops;
} orm_runtime_backend;

static void runtime_backend_destroy(void *context) {
  orm_runtime_backend *backend = context;
  if (backend == NULL) return;
  if (backend->native.context != NULL)
    backend->ops.destroy(backend->native.context);
  orm_runtime_t *runtime = backend->runtime;
  const salts_plugin_status release_status =
      salts_plugin_registry_release(&runtime->plugins, &backend->plugin_lease);
  free(backend);
  if (release_status != SALTS_PLUGIN_OK) {
    orm_error_t cleanup_error;
    (void)runtime_plugin_status(
        release_status, &cleanup_error, "release Driver Plugin lease");
    if (runtime->config.on_cleanup_error != NULL)
      runtime->config.on_cleanup_error(
          runtime->config.cleanup_context, &cleanup_error);
    else
      abort();
  }
  if (runtime_release_dependent_ref(runtime))
    runtime_release_last(runtime);
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

static orm_status_t runtime_driver_open_cursor(
    void *native_context, orm_driver_open_cursor_fn open_cursor,
    orm_runtime_driver *driver, const orm_query_plan *plan,
    const orm_limits *limits, orm_row_cursor *out_cursor,
    orm_error_t *error) {
  if (out_cursor != NULL) memset(out_cursor, 0, sizeof(*out_cursor));
  if (native_context == NULL || open_cursor == NULL || driver == NULL ||
      plan == NULL || limits == NULL || out_cursor == NULL)
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
  status = open_cursor(
      native_context, &view, &driver_limits, &native, error);
  if (status != ORM_STATUS_OK) {
    free(cursor);
    return status;
  }

  status = orm_driver_validate_cursor_v1(
      &native, (uint32_t)sizeof(native), driver->capabilities, error);
  if (status != ORM_STATUS_OK) {
    /* A driver that reports successful creation with an invalid ownership
     * descriptor has violated the ABI. No unvalidated callback is invoked. */
    free(cursor);
    return status;
  }

  cursor->native = native;
  cursor->execution_models = driver->execution_models;
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

static orm_status_t runtime_backend_open_cursor(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    orm_row_cursor *out_cursor, orm_error_t *error) {
  orm_runtime_backend *backend = context;
  if (backend == NULL)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid runtime driver backend");
  return runtime_driver_open_cursor(
      backend->native.context, backend->ops.open_cursor, backend->driver,
      plan, limits, out_cursor, error);
}

static orm_status_t runtime_driver_execute_command(
    void *native_context, orm_driver_command_fn execute_command,
    const orm_query_plan *plan, const orm_limits *limits,
    uint64_t *affected_rows, orm_error_t *error) {
  if (affected_rows != NULL) *affected_rows = 0u;
  if (native_context == NULL || execute_command == NULL ||
      plan == NULL || limits == NULL || affected_rows == NULL)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid runtime driver command execution");

  orm_driver_plan_view_v1 view;
  memset(&view, 0, sizeof(view));
  orm_status_t status = orm_driver_plan_borrow(plan, &view, error);
  if (status != ORM_STATUS_OK) return status;

  const orm_driver_limits_v1 driver_limits = runtime_driver_limits(limits);
  status = execute_command(
      native_context, &view, &driver_limits, affected_rows, error);
  if (status != ORM_STATUS_OK) *affected_rows = 0u;
  return status;
}

static orm_status_t runtime_backend_execute_command(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    uint64_t *affected_rows, orm_error_t *error) {
  orm_runtime_backend *backend = context;
  if (backend == NULL)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid runtime driver backend");
  return runtime_driver_execute_command(
      backend->native.context, backend->ops.execute_command,
      plan, limits, affected_rows, error);
}

typedef struct orm_runtime_transaction {
  orm_runtime_driver *driver;
  orm_driver_transaction_v1 native;
  orm_driver_transaction_ops_v1 ops;
} orm_runtime_transaction;

static void runtime_transaction_destroy(void *context) {
  orm_runtime_transaction *transaction = context;
  if (transaction == NULL) return;
  if (transaction->native.context != NULL)
    transaction->ops.destroy(transaction->native.context);
  free(transaction);
}

static orm_status_t runtime_transaction_open_cursor(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    orm_row_cursor *out_cursor, orm_error_t *error) {
  orm_runtime_transaction *transaction = context;
  if (transaction == NULL)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid runtime driver transaction");
  return runtime_driver_open_cursor(
      transaction->native.context, transaction->ops.open_cursor,
      transaction->driver, plan, limits, out_cursor, error);
}

static orm_status_t runtime_transaction_execute_command(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    uint64_t *affected_rows, orm_error_t *error) {
  orm_runtime_transaction *transaction = context;
  if (transaction == NULL)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid runtime driver transaction");
  return runtime_driver_execute_command(
      transaction->native.context, transaction->ops.execute_command,
      plan, limits, affected_rows, error);
}

static orm_status_t runtime_transaction_commit(
    void *context, orm_error_t *error) {
  orm_runtime_transaction *transaction = context;
  if (transaction == NULL || transaction->ops.commit == NULL)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid runtime driver transaction commit");
  return transaction->ops.commit(transaction->native.context, error);
}

static orm_status_t runtime_transaction_rollback(
    void *context, orm_error_t *error) {
  orm_runtime_transaction *transaction = context;
  if (transaction == NULL || transaction->ops.rollback == NULL)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid runtime driver transaction rollback");
  return transaction->ops.rollback(transaction->native.context, error);
}

static orm_status_t runtime_transaction_savepoint(
    void *context, vstr name, orm_error_t *error) {
  orm_runtime_transaction *transaction = context;
  if (transaction == NULL || transaction->ops.savepoint == NULL)
    return runtime_result(error, ORM_STATUS_UNSUPPORTED,
                          "driver does not support savepoints");
  const orm_driver_bytes_v1 driver_name = {name.data, name.len};
  return transaction->ops.savepoint(
      transaction->native.context, driver_name, error);
}

static orm_status_t runtime_transaction_rollback_to_savepoint(
    void *context, vstr name, orm_error_t *error) {
  orm_runtime_transaction *transaction = context;
  if (transaction == NULL || transaction->ops.rollback_to_savepoint == NULL)
    return runtime_result(error, ORM_STATUS_UNSUPPORTED,
                          "driver does not support savepoints");
  const orm_driver_bytes_v1 driver_name = {name.data, name.len};
  return transaction->ops.rollback_to_savepoint(
      transaction->native.context, driver_name, error);
}

static orm_status_t runtime_transaction_release_savepoint(
    void *context, vstr name, orm_error_t *error) {
  orm_runtime_transaction *transaction = context;
  if (transaction == NULL || transaction->ops.release_savepoint == NULL)
    return runtime_result(error, ORM_STATUS_UNSUPPORTED,
                          "driver does not support savepoints");
  const orm_driver_bytes_v1 driver_name = {name.data, name.len};
  return transaction->ops.release_savepoint(
      transaction->native.context, driver_name, error);
}

static const orm_transaction_backend_ops runtime_transaction_ops = {
    sizeof(orm_transaction_backend_ops), ORM_TRANSACTION_BACKEND_OPS_ABI_VERSION,
    runtime_transaction_destroy, runtime_transaction_open_cursor,
    runtime_transaction_execute_command, runtime_transaction_commit,
    runtime_transaction_rollback, runtime_transaction_savepoint,
    runtime_transaction_rollback_to_savepoint,
    runtime_transaction_release_savepoint};

static orm_status_t runtime_backend_begin_transaction(
    void *context, orm_isolation_t isolation,
    orm_transaction_backend *out_transaction, orm_error_t *error) {
  if (out_transaction != NULL)
    memset(out_transaction, 0, sizeof(*out_transaction));
  orm_runtime_backend *backend = context;
  if (backend == NULL || out_transaction == NULL ||
      backend->ops.begin_transaction == NULL)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid runtime driver transaction begin");

  orm_runtime_transaction *transaction =
      (orm_runtime_transaction *)calloc(1u, sizeof(*transaction));
  if (transaction == NULL)
    return runtime_result(error, ORM_STATUS_OUT_OF_MEMORY,
                          "allocate runtime driver transaction adapter");

  orm_driver_transaction_v1 native;
  memset(&native, 0, sizeof(native));
  orm_status_t status = backend->ops.begin_transaction(
      backend->native.context, isolation, &native, error);
  if (status != ORM_STATUS_OK) {
    free(transaction);
    return status;
  }

  status = orm_driver_validate_transaction_v1(
      &native, (uint32_t)sizeof(native), backend->driver->capabilities, error);
  if (status != ORM_STATUS_OK) {
    /* Successful creation with an invalid ownership descriptor is a driver ABI
     * violation. Do not invoke an unvalidated destroy callback. */
    free(transaction);
    return status;
  }

  transaction->driver = backend->driver;
  transaction->native = native;
  {
    size_t transaction_ops_bytes = native.ops.bytes;
    if (transaction_ops_bytes > sizeof(transaction->ops))
      transaction_ops_bytes = sizeof(transaction->ops);
    memcpy(&transaction->ops, native.ops.data, transaction_ops_bytes);
  }

  out_transaction->ops = &runtime_transaction_ops;
  out_transaction->context = transaction;
  return runtime_result(error, ORM_STATUS_OK, NULL);
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
  salts_plugin_lease lease = {0};
  const salts_plugin_manifest *manifest = NULL;

  if (out_backend != NULL) memset(out_backend, 0, sizeof(*out_backend));
  if (config == NULL || limits == NULL || context == NULL ||
      out_backend == NULL)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid runtime driver connection factory");

  orm_runtime_factory_context *factory = context;
  orm_status_t status = runtime_acquire_dependent(factory->runtime, error);
  if (status != ORM_STATUS_OK) return status;

  const salts_plugin_status acquire_status =
      salts_plugin_registry_acquire(
          &factory->runtime->plugins, factory->driver->plugin,
          &lease, &manifest);
  if (acquire_status != SALTS_PLUGIN_OK) {
    runtime_drop_dependent(factory->runtime);
    return runtime_plugin_status(
        acquire_status, error, "acquire Driver Plugin lease");
  }
  (void)manifest;

  orm_driver_connection_v1 native;
  memset(&native, 0, sizeof(native));
  const orm_driver_limits_v1 driver_limits = runtime_driver_limits(limits);
  status = TurboDb_Driver_create(
      factory->driver->binding, config, &driver_limits, &native, error);
  if (status != ORM_STATUS_OK) {
    (void)salts_plugin_registry_release(&factory->runtime->plugins, &lease);
    runtime_drop_dependent(factory->runtime);
    return status;
  }

  status = orm_driver_validate_connection_v1(
      &native, (uint32_t)sizeof(native), factory->driver->capabilities, error);
  if (status != ORM_STATUS_OK) {
    (void)salts_plugin_registry_release(&factory->runtime->plugins, &lease);
    runtime_drop_dependent(factory->runtime);
    return status;
  }

  orm_driver_connection_ops_v1 connection_ops;
  memset(&connection_ops, 0, sizeof(connection_ops));
  {
    size_t connection_ops_bytes = native.ops.bytes;
    if (connection_ops_bytes > sizeof(connection_ops))
      connection_ops_bytes = sizeof(connection_ops);
    memcpy(&connection_ops, native.ops.data, connection_ops_bytes);
  }

  orm_runtime_backend *backend =
      (orm_runtime_backend *)calloc(1u, sizeof(*backend));
  if (backend == NULL) {
    connection_ops.destroy(native.context);
    (void)salts_plugin_registry_release(&factory->runtime->plugins, &lease);
    runtime_drop_dependent(factory->runtime);
    return runtime_result(error, ORM_STATUS_OUT_OF_MEMORY,
                          "allocate runtime driver connection adapter");
  }

  backend->runtime = factory->runtime;
  backend->driver = factory->driver;
  backend->plugin_lease = lease;
  backend->native = native;
  backend->ops = connection_ops;
  out_backend->ops = &runtime_backend_ops;
  out_backend->context = backend;
  return runtime_result(error, ORM_STATUS_OK, NULL);
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

  salts_mutex_init(&runtime->mutex);
  if (runtime->mutex == NULL) {
    free(runtime);
    return runtime_result(error, ORM_STATUS_OUT_OF_MEMORY,
                          "initialize runtime mutex");
  }

  salts_plugin_registry_config plugin_config = {
      (size_t)config->max_drivers};
  const salts_plugin_status plugin_status =
      salts_plugin_registry_init(&runtime->plugins, &plugin_config);
  if (plugin_status != SALTS_PLUGIN_OK) {
    salts_mutex_destroy(&runtime->mutex);
    free(runtime);
    return runtime_plugin_status(plugin_status, error,
                                 "initialize Plugin registry");
  }

  runtime->drivers = (orm_runtime_driver *)calloc(
      config->max_drivers, sizeof(*runtime->drivers));
  if (runtime->drivers == NULL) {
    (void)salts_plugin_registry_destroy(&runtime->plugins);
    salts_mutex_destroy(&runtime->mutex);
    free(runtime);
    return runtime_result(error, ORM_STATUS_OUT_OF_MEMORY,
                          "allocate runtime driver index");
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

static salts_plugin_status runtime_discard_plugin(
    orm_runtime_t *runtime, salts_plugin_ref ref, int started) {
  salts_plugin_status status;
  bool quiescent = false;
  if (!salts_plugin_ref_valid(ref)) return SALTS_PLUGIN_OK;
  if (started) {
    status = salts_plugin_registry_request_stop(&runtime->plugins, ref);
    if (status != SALTS_PLUGIN_OK && status != SALTS_PLUGIN_ALREADY)
      return status;
    status = salts_plugin_registry_poll_quiescent(
        &runtime->plugins, ref, &quiescent);
    if (status != SALTS_PLUGIN_OK)
      return status;
    if (!quiescent)
      return SALTS_PLUGIN_BUSY;
  }
  return salts_plugin_registry_unload(&runtime->plugins, ref);
}

orm_status_t ORM_C_CALL
orm_runtime_load_driver(orm_runtime_t *runtime,
                        const orm_driver_load_config_t *config,
                        orm_error_t *error) {
  salts_plugin_ref plugin = {0};
  salts_plugin_lease admission = {0};
  const salts_plugin_manifest *manifest = NULL;
  const salts_plugin_export *entry = NULL;
  TurboDb_Driver *binding = NULL;
  int started = 0;
  orm_status_t status;
  salts_plugin_status plugin_status;

  if (runtime == NULL || config == NULL ||
      config->struct_size < sizeof(*config) ||
      config->abi_version != ORM_RUNTIME_ABI_VERSION ||
      config->flags != 0u || config->reserved != 0u ||
      !runtime_id_valid(config->expected_driver_id))
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid driver load configuration");

  char *path = NULL;
  status = runtime_copy_path(runtime, config->module_path, &path, error);
  if (status != ORM_STATUS_OK)
    return status;

  status = runtime_begin_load(runtime, path, error);
  if (status != ORM_STATUS_OK) {
    free(path);
    return status;
  }

  plugin_status =
      salts_plugin_registry_load(&runtime->plugins, path, &plugin);
  if (plugin_status != SALTS_PLUGIN_OK) {
    status = plugin_status == SALTS_PLUGIN_UNSUPPORTED_ABI
                 ? runtime_result(error, ORM_STATUS_ABI_MISMATCH,
                                  "driver Plugin ABI mismatch")
                 : runtime_plugin_status(
                       plugin_status, error, "load driver Plugin");
    goto fail_reserved;
  }

  plugin_status = salts_plugin_registry_start(&runtime->plugins, plugin);
  if (plugin_status != SALTS_PLUGIN_OK) {
    status = runtime_plugin_status(plugin_status, error, "start driver Plugin");
    goto fail_plugin;
  }
  started = 1;

  plugin_status = salts_plugin_registry_acquire(
      &runtime->plugins, plugin, &admission, &manifest);
  if (plugin_status != SALTS_PLUGIN_OK) {
    status = runtime_plugin_status(
        plugin_status, error, "acquire driver Plugin admission lease");
    goto fail_plugin;
  }

  if (manifest == NULL || manifest->plugin_id == NULL) {
    status = runtime_result(error, ORM_STATUS_ABI_MISMATCH,
                            "driver Plugin has no canonical ID");
    goto fail_admission;
  }

  const size_t plugin_id_size = strlen(manifest->plugin_id);
  const orm_string_view_t plugin_id = {
      manifest->plugin_id, plugin_id_size};
  if (!runtime_id_valid(plugin_id) ||
      plugin_id_size != config->expected_driver_id.len ||
      memcmp(manifest->plugin_id, config->expected_driver_id.data,
             plugin_id_size) != 0) {
    status = runtime_result(error, ORM_STATUS_DRIVER_ID_MISMATCH,
                            "Plugin ID does not match requested driver ID");
    goto fail_admission;
  }

  plugin_status = salts_plugin_manifest_find_export(
      manifest, ORM_DRIVER_PLUGIN_EXPORT_ID, &entry);
  if (plugin_status != SALTS_PLUGIN_OK) {
    status = runtime_plugin_status(
        plugin_status, error, "find TurboDb.Driver export");
    goto fail_admission;
  }

  if (entry->kind == SALTS_PLUGIN_EXPORT_INTERFACE &&
      entry->contract_id != NULL &&
      strcmp(entry->contract_id, ORM_DRIVER_INTERFACE_CONTRACT_ID) == 0 &&
      entry->contract_version != ORM_DRIVER_INTERFACE_CONTRACT_VERSION) {
    status = runtime_result(error, ORM_STATUS_ABI_MISMATCH,
                            "TurboDb.Driver contract version mismatch");
    goto fail_admission;
  }
  if (entry->kind == SALTS_PLUGIN_EXPORT_INTERFACE &&
      entry->contract_id != NULL &&
      strcmp(entry->contract_id, ORM_DRIVER_INTERFACE_CONTRACT_ID) == 0 &&
      entry->contract_version == ORM_DRIVER_INTERFACE_CONTRACT_VERSION &&
      (entry->value.interface.desc == NULL ||
       !cmeta_interface_desc_valid(entry->value.interface.desc) ||
       !cmeta_interface_desc_equal(entry->value.interface.desc,
                                   TurboDb_Driver_interface()))) {
    status = runtime_result(error, ORM_STATUS_ABI_MISMATCH,
                            "TurboDb.Driver CMeta Interface shape mismatch");
    goto fail_admission;
  }

  plugin_status = salts_plugin_export_require_interface(
      entry, ORM_DRIVER_INTERFACE_CONTRACT_ID,
      ORM_DRIVER_INTERFACE_CONTRACT_VERSION, 0u,
      TurboDb_Driver_interface());
  if (plugin_status != SALTS_PLUGIN_OK) {
    status = runtime_plugin_status(
        plugin_status, error, "admit TurboDb.Driver interface");
    goto fail_admission;
  }

  binding = (TurboDb_Driver *)entry->value.interface.value;
  if (binding == NULL || !TurboDb_Driver_valid(binding) ||
      binding->vtable->capabilities != entry->capabilities ||
      (entry->capabilities & ~ORM_DRIVER_CAP_KNOWN_MASK) != 0u) {
    status = runtime_result(error, ORM_STATUS_ABI_MISMATCH,
                            "TurboDb.Driver binding is invalid");
    goto fail_admission;
  }

  if (runtime_id_conflicts(
          runtime,
          (orm_driver_bytes_v1){manifest->plugin_id,
                                (uint64_t)plugin_id_size})) {
    status = runtime_result(error, ORM_STATUS_DRIVER_ALREADY_REGISTERED,
                            "driver ID is already registered");
    goto fail_admission;
  }

  plugin_status =
      salts_plugin_registry_release(&runtime->plugins, &admission);
  if (plugin_status != SALTS_PLUGIN_OK) {
    status = runtime_plugin_status(
        plugin_status, error, "release driver admission lease");
    goto fail_plugin;
  }

  salts_mutex_lock(&runtime->mutex);
  if (runtime->closed != ORM_RUNTIME_OPEN ||
      runtime->load_active == 0u ||
      runtime->pending_operations == 0u) {
    salts_mutex_unlock(&runtime->mutex);
    status = runtime_result(error, ORM_STATUS_INVALID_STATE,
                            "runtime load reservation was lost");
    goto fail_plugin;
  }
  if (runtime_id_conflicts(
          runtime,
          (orm_driver_bytes_v1){manifest->plugin_id,
                                (uint64_t)plugin_id_size})) {
    salts_mutex_unlock(&runtime->mutex);
    status = runtime_result(error, ORM_STATUS_DRIVER_ALREADY_REGISTERED,
                            "driver ID is already registered");
    goto fail_plugin;
  }

  const uint32_t index = runtime->driver_count;
  orm_runtime_driver *driver = &runtime->drivers[index];
  memset(driver, 0, sizeof(*driver));
  driver->plugin = plugin;
  driver->binding = binding;
  driver->module_path = path;
  runtime_copy_id(
      &driver->canonical,
      (orm_driver_bytes_v1){manifest->plugin_id, (uint64_t)plugin_id_size});
  driver->capabilities = entry->capabilities;
  driver->execution_models = ORM_DRIVER_EXEC_CALLER_BLOCKING;
  memcpy(driver->bundle_id, runtime_bundle, sizeof(driver->bundle_id));
  ++runtime->driver_count;
  --runtime->pending_operations;
  runtime->load_active = 0u;
  salts_mutex_unlock(&runtime->mutex);
  return runtime_result(error, ORM_STATUS_OK, NULL);

fail_admission:
  if (salts_plugin_lease_valid(admission)) {
    const salts_plugin_status release_status =
        salts_plugin_registry_release(&runtime->plugins, &admission);
    if (release_status != SALTS_PLUGIN_OK)
      status = runtime_plugin_status(
          release_status, error, "release failed driver admission lease");
  }
fail_plugin:
  {
    const salts_plugin_status discard_status =
        runtime_discard_plugin(runtime, plugin, started);
    if (discard_status != SALTS_PLUGIN_OK)
      status = runtime_plugin_status(
          discard_status, error, "discard failed driver Plugin");
  }
fail_reserved:
  free(path);
  runtime_finish_pending(runtime, 1);
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
  salts_mutex_lock(&runtime->mutex);
  if (runtime->closed != ORM_RUNTIME_OPEN) {
    salts_mutex_unlock(&runtime->mutex);
    return runtime_result(error, ORM_STATUS_INVALID_STATE,
                          "runtime is closed");
  }
  orm_runtime_driver *driver = runtime_find_driver(runtime, id);
  if (driver == NULL) {
    salts_mutex_unlock(&runtime->mutex);
    return runtime_result(error, ORM_STATUS_DRIVER_NOT_REGISTERED,
                          "driver is not registered");
  }

  out_info->struct_size = (uint32_t)sizeof(*out_info);
  out_info->abi_version = ORM_RUNTIME_ABI_VERSION;
  out_info->canonical_id_size = driver->canonical.size;
  memcpy(out_info->canonical_id, driver->canonical.text,
         driver->canonical.size);
  out_info->capabilities = driver->capabilities;
  out_info->execution_models = driver->execution_models;
  memcpy(out_info->bundle_id, driver->bundle_id,
         sizeof(out_info->bundle_id));
  salts_mutex_unlock(&runtime->mutex);
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
  orm_runtime_driver *driver = NULL;
  orm_status_t status =
      runtime_begin_connect(runtime, config->driver, &driver, error);
  if (status != ORM_STATUS_OK) return status;

  orm_config_t canonical_config = *config;
  canonical_config.driver.data = driver->canonical.text;
  canonical_config.driver.len = driver->canonical.size;
  orm_runtime_factory_context factory = {runtime, driver};
  status = orm_connect_with_factory_context_v1(
      &canonical_config, runtime_backend_factory, &factory,
      out_connection, error);
  runtime_finish_pending(runtime, 0);
  return status;
}

orm_status_t ORM_C_CALL
orm_runtime_close(orm_runtime_t *runtime, orm_error_t *error) {
  if (runtime == NULL)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "runtime is required");

  salts_mutex_lock(&runtime->mutex);
  if (runtime->closed == ORM_RUNTIME_CLOSED) {
    salts_mutex_unlock(&runtime->mutex);
    return runtime_result(error, ORM_STATUS_OK, NULL);
  }
  if (runtime->closed == ORM_RUNTIME_FAILED) {
    salts_mutex_unlock(&runtime->mutex);
    return runtime_result(error, ORM_STATUS_CLEANUP_FAILED,
                          "runtime cleanup is quarantined");
  }
  if (runtime->closed == ORM_RUNTIME_OPEN) {
    if (runtime->dependents != 0u || runtime->pending_operations != 0u) {
      salts_mutex_unlock(&runtime->mutex);
      return runtime_result(error, ORM_STATUS_BUSY,
                            "runtime has active or pending operations");
    }
    runtime->closed = ORM_RUNTIME_CLOSING;
    runtime->close_remaining = runtime->driver_count;
  }
  const uint32_t remaining_snapshot = runtime->close_remaining;
  salts_mutex_unlock(&runtime->mutex);

  for (uint32_t remaining = remaining_snapshot; remaining != 0u; --remaining) {
    orm_runtime_driver *driver = &runtime->drivers[remaining - 1u];
    salts_plugin_status plugin_status =
        salts_plugin_registry_request_stop(&runtime->plugins, driver->plugin);
    if (plugin_status != SALTS_PLUGIN_OK &&
        plugin_status != SALTS_PLUGIN_ALREADY) {
      salts_mutex_lock(&runtime->mutex);
      runtime->closed = ORM_RUNTIME_FAILED;
      salts_mutex_unlock(&runtime->mutex);
      return runtime_plugin_status(
          plugin_status, error, "request Driver Plugin stop");
    }

    bool quiescent = false;
    plugin_status = salts_plugin_registry_poll_quiescent(
        &runtime->plugins, driver->plugin, &quiescent);
    if (plugin_status != SALTS_PLUGIN_OK) {
      salts_mutex_lock(&runtime->mutex);
      runtime->closed = ORM_RUNTIME_FAILED;
      salts_mutex_unlock(&runtime->mutex);
      return runtime_plugin_status(
          plugin_status, error, "poll Driver Plugin quiescence");
    }
    if (!quiescent)
      return runtime_result(error, ORM_STATUS_BUSY,
                            "driver Plugin is still quiescing");

    plugin_status =
        salts_plugin_registry_unload(&runtime->plugins, driver->plugin);
    if (plugin_status != SALTS_PLUGIN_OK) {
      if (plugin_status == SALTS_PLUGIN_BUSY)
        return runtime_plugin_status(
            plugin_status, error, "unload Driver Plugin");
      salts_mutex_lock(&runtime->mutex);
      runtime->closed = ORM_RUNTIME_FAILED;
      salts_mutex_unlock(&runtime->mutex);
      return runtime_plugin_status(
          plugin_status, error, "unload Driver Plugin");
    }

    free(driver->module_path);
    driver->module_path = NULL;
    driver->binding = NULL;
    driver->plugin = (salts_plugin_ref){0};

    salts_mutex_lock(&runtime->mutex);
    if (runtime->close_remaining != remaining) {
      salts_mutex_unlock(&runtime->mutex);
      abort();
    }
    --runtime->close_remaining;
    salts_mutex_unlock(&runtime->mutex);
  }

  const salts_plugin_status destroy_status =
      salts_plugin_registry_destroy(&runtime->plugins);
  if (destroy_status != SALTS_PLUGIN_OK) {
    salts_mutex_lock(&runtime->mutex);
    runtime->closed = ORM_RUNTIME_FAILED;
    salts_mutex_unlock(&runtime->mutex);
    return runtime_plugin_status(
        destroy_status, error, "destroy Driver Plugin registry");
  }

  salts_mutex_lock(&runtime->mutex);
  runtime->closed = ORM_RUNTIME_CLOSED;
  salts_mutex_unlock(&runtime->mutex);
  return runtime_result(error, ORM_STATUS_OK, NULL);
}

static void runtime_release_last(orm_runtime_t *runtime) {
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
  free(runtime->drivers);
  salts_mutex_destroy(&runtime->mutex);
  free(runtime);
}

void ORM_C_CALL orm_runtime_retain(orm_runtime_t *runtime) {
  if (runtime == NULL) return;
  salts_mutex_lock(&runtime->mutex);
  if (runtime->refs == 0u || runtime->refs == UINT32_MAX) {
    salts_mutex_unlock(&runtime->mutex);
    abort();
  }
  ++runtime->refs;
  salts_mutex_unlock(&runtime->mutex);
}

void ORM_C_CALL orm_runtime_release(orm_runtime_t *runtime) {
  if (runtime == NULL) return;
  int last = 0;
  salts_mutex_lock(&runtime->mutex);
  if (runtime->refs == 0u) {
    salts_mutex_unlock(&runtime->mutex);
    abort();
  }
  --runtime->refs;
  last = runtime->refs == 0u;
  salts_mutex_unlock(&runtime->mutex);
  if (last) runtime_release_last(runtime);
}

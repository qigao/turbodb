#include "orm_runtime_internal.h"
#include "../abi/orm_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../driver/orm_driver_contract.h"
#include "../driver/orm_driver_owner_bridge.h"
#include "../driver/orm_driver_plan_view.h"

#define RUNTIME_HEADER(T) {(uint32_t)sizeof(T), ORM_DRIVER_ABI_VERSION}

static orm_status_t runtime_begin_connect(
    orm_runtime_t *runtime, orm_string_view_t id,
    orm_runtime_driver **out_driver, orm_error_t *error) {
  *out_driver = NULL;
  orm_status_t status = ORM_STATUS_OK;
  const char *message = NULL;
  cmeta_mutex_lock(&runtime->mutex);
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
  cmeta_mutex_unlock(&runtime->mutex);
  return runtime_result(error, status, message);
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
  cmeta_plugin_lease plugin_lease;
  orm_driver_connection_v1 native;
  orm_driver_connection_ops_v2 ops;
} orm_runtime_backend;

static void runtime_backend_destroy(void *context) {
  orm_runtime_backend *backend = context;
  if (backend == NULL) return;
  if (backend->native.context != NULL)
    backend->ops.destroy(backend->native.context);
  orm_runtime_t *runtime = backend->runtime;
  const cmeta_plugin_status release_status =
      cmeta_plugin_registry_release(&runtime->plugins, &backend->plugin_lease);
  free(backend);
  if (release_status != CMETA_PLUGIN_OK) {
    orm_error_t cleanup_error;
    (void)runtime_plugin_status(
        release_status, &cleanup_error, "release Driver Plugin lease");
    if (runtime->config.on_cleanup_error != NULL)
      runtime->config.on_cleanup_error(
          runtime->config.cleanup_context, &cleanup_error);
    else
      abort();
  }
  runtime_drop_dependent(runtime);
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
    if ((cursor->execution_models & (ORM_DRIVER_EXEC_NATIVE_WAIT |
                                     ORM_DRIVER_EXEC_OWNER_EXECUTOR)) == 0u) {
      runtime_cursor_error(cursor, ORM_STATUS_UNSUPPORTED,
                           "driver returned WAIT without an async execution model");
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

static orm_status_t runtime_cursor_cancel_checked(void *context, orm_error_t *error) {
  orm_runtime_cursor *cursor = context;
  if (cursor != NULL && cursor->native.context != NULL)
    return cursor->ops.cancel(cursor->native.context, error);
  return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT, "invalid cursor cancellation");
}

static void runtime_cursor_cancel(void *context) {
  orm_error_t error;
  orm_error_init(&error);
  (void)runtime_cursor_cancel_checked(context, &error);
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
    orm_error_t *error, orm_driver_open_async_cursor_fn open_async,
    const orm_async_config_t *async_config) {
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
  if (async_config != NULL && open_async == NULL) {
    free(cursor);
    return runtime_result(error, ORM_STATUS_UNSUPPORTED, "driver has no async query entry");
  }
  status = async_config != NULL
      ? open_async(native_context, &view, &driver_limits, async_config, &native, error)
      : open_cursor(native_context, &view, &driver_limits, &native, error);
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
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  out_cursor->cancel_checked = runtime_cursor_cancel_checked;
#endif
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
      plan, limits, out_cursor, error, NULL, NULL);
}

static orm_status_t runtime_backend_open_async_cursor(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    const orm_async_config_t *async_config, orm_row_cursor *out_cursor,
    orm_error_t *error) {
  orm_runtime_backend *backend = context;
  if (backend == NULL)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT, "invalid async backend");
  return runtime_driver_open_cursor(backend->native.context,
      backend->ops.open_cursor, backend->driver, plan, limits, out_cursor, error,
      backend->ops.open_async_cursor, async_config);
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
      transaction->driver, plan, limits, out_cursor, error, NULL, NULL);
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
    runtime_backend_execute_command, runtime_backend_begin_transaction,
    runtime_backend_open_async_cursor};

typedef struct orm_runtime_factory_context {
  orm_runtime_t *runtime;
  orm_runtime_driver *driver;
} orm_runtime_factory_context;

static orm_status_t runtime_backend_factory(
    const orm_config_t *config, const orm_limits *limits, void *context,
    orm_backend *out_backend, orm_error_t *error) {
  cmeta_plugin_lease lease = {0};
  const cmeta_plugin_manifest *manifest = NULL;

  if (out_backend != NULL) memset(out_backend, 0, sizeof(*out_backend));
  if (config == NULL || limits == NULL || context == NULL ||
      out_backend == NULL)
    return runtime_result(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid runtime driver connection factory");

  orm_runtime_factory_context *factory = context;
  orm_status_t status = runtime_acquire_dependent(factory->runtime, error);
  if (status != ORM_STATUS_OK) return status;

  const cmeta_plugin_status acquire_status =
      cmeta_plugin_registry_acquire(
          &factory->runtime->plugins, factory->driver->plugin,
          &lease, &manifest);
  if (acquire_status != CMETA_PLUGIN_OK) {
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
    (void)cmeta_plugin_registry_release(&factory->runtime->plugins, &lease);
    runtime_drop_dependent(factory->runtime);
    return status;
  }

  status = orm_driver_validate_connection_v1(
      &native, (uint32_t)sizeof(native), factory->driver->capabilities, error);
  if (status != ORM_STATUS_OK) {
    (void)cmeta_plugin_registry_release(&factory->runtime->plugins, &lease);
    runtime_drop_dependent(factory->runtime);
    return status;
  }

  orm_driver_connection_ops_v2 connection_ops;
  memcpy(&connection_ops, native.ops.data, sizeof(connection_ops));

  orm_runtime_backend *backend =
      (orm_runtime_backend *)calloc(1u, sizeof(*backend));
  if (backend == NULL) {
    connection_ops.destroy(native.context);
    (void)cmeta_plugin_registry_release(&factory->runtime->plugins, &lease);
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


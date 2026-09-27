#include <orm_driver_plugin.h>

#include <stdlib.h>
#include <string.h>

#define FIXTURE_HEADER(T) {(uint32_t)sizeof(T), ORM_DRIVER_ABI_VERSION}
#define FIXTURE_TABLE(p) {(p), (uint32_t)sizeof(*(p)), 0u}

typedef struct fixture_module_context {
  uint32_t live_connections;
} fixture_module_context;

typedef struct fixture_connection_context {
  fixture_module_context *module;
  uint32_t active_cursors;
  uint32_t active_transactions;
  int live;
} fixture_connection_context;

typedef struct fixture_transaction_context {
  fixture_connection_context *connection;
  int committed;
  int rolled_back;
  int live;
} fixture_transaction_context;

typedef struct fixture_cursor_context {
  fixture_connection_context *connection;
  int emitted;
  int live;
} fixture_cursor_context;

static fixture_module_context fixture_module;
static fixture_connection_context fixture_connections[4];
static fixture_transaction_context fixture_transactions[4];
static fixture_cursor_context fixture_cursors[8];
static const char fixture_id[] = "fixture";

static void fixture_error(orm_error_t *error, orm_status_t status) {
  if (error == NULL) return;
  memset(error, 0, sizeof(*error));
  error->struct_size = (uint32_t)sizeof(*error);
  error->status = status;
}

static cserde_status fixture_reader_next(void *context, cserde_token *out) {
  fixture_cursor_context *cursor = context;
  if (cursor == NULL || out == NULL) return CSERDE_INVALID_ARGUMENT;
  if (cursor->emitted) return CSERDE_DONE;
  memset(out, 0, sizeof(*out));
  out->kind = CSERDE_SINT;
  out->value.sint = INT64_C(7);
  cursor->emitted = 1;
  return CSERDE_OK;
}

static const cserde_reader_ops fixture_reader_ops = {
    sizeof(cserde_reader_ops), CSERDE_READER_OPS_ABI_VERSION,
    fixture_reader_next};

static orm_status_t ORM_DRIVER_CALL fixture_cursor_next(
    void *context, cserde_reader *reader, orm_driver_step_v1 *step,
    orm_error_t *error) {
  if (reader != NULL) memset(reader, 0, sizeof(*reader));
  if (step != NULL) {
    memset(step, 0, sizeof(*step));
    step->header =
        (orm_driver_header_v1)FIXTURE_HEADER(orm_driver_step_v1);
    step->kind = ORM_DRIVER_STEP_ERROR;
  }
  fixture_cursor_context *cursor = context;
  if (cursor == NULL || !cursor->live || reader == NULL || step == NULL) {
    fixture_error(error, ORM_STATUS_INVALID_ARGUMENT);
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  if (cursor->emitted) {
    step->kind = ORM_DRIVER_STEP_DONE;
    fixture_error(error, ORM_STATUS_OK);
    return ORM_STATUS_OK;
  }
  const cserde_status reader_status =
      cserde_reader_init(reader, &fixture_reader_ops, cursor);
  if (reader_status != CSERDE_OK) {
    fixture_error(error, ORM_STATUS_INTERNAL_ERROR);
    return ORM_STATUS_INTERNAL_ERROR;
  }
  step->kind = ORM_DRIVER_STEP_ROW_AND_DONE;
  fixture_error(error, ORM_STATUS_OK);
  return ORM_STATUS_OK;
}

static void ORM_DRIVER_CALL fixture_cursor_cancel(void *context) {
  fixture_cursor_context *cursor = context;
  if (cursor != NULL && cursor->live) cursor->emitted = 1;
}

static void ORM_DRIVER_CALL fixture_cursor_destroy(void *context) {
  fixture_cursor_context *cursor = context;
  if (cursor == NULL || !cursor->live || cursor->connection == NULL) return;
  if (cursor->connection->active_cursors == 0u) abort();
  --cursor->connection->active_cursors;
  memset(cursor, 0, sizeof(*cursor));
}

static const orm_driver_cursor_ops_v1 fixture_cursor_ops = {
    FIXTURE_HEADER(orm_driver_cursor_ops_v1),
    fixture_cursor_next, fixture_cursor_cancel, fixture_cursor_destroy,
    NULL, NULL};

static orm_status_t ORM_DRIVER_CALL fixture_open_cursor(
    void *context, const orm_driver_plan_view_v1 *plan,
    const orm_driver_limits_v1 *limits, orm_driver_cursor_v1 *out,
    orm_error_t *error) {
  if (out != NULL) memset(out, 0, sizeof(*out));
  fixture_connection_context *connection = context;
  if (connection == NULL || !connection->live ||
      connection->module == NULL || plan == NULL || limits == NULL ||
      out == NULL) {
    fixture_error(error, ORM_STATUS_INVALID_ARGUMENT);
    return ORM_STATUS_INVALID_ARGUMENT;
  }

  if (plan->metadata.data == NULL ||
      plan->metadata.bytes < sizeof(orm_driver_plan_metadata_ops_v1)) {
    fixture_error(error, ORM_STATUS_ABI_MISMATCH);
    return ORM_STATUS_ABI_MISMATCH;
  }
  orm_driver_plan_metadata_ops_v1 metadata;
  memcpy(&metadata, plan->metadata.data, sizeof(metadata));
  orm_driver_plan_meta_v1 meta;
  memset(&meta, 0, sizeof(meta));
  meta.header =
      (orm_driver_header_v1)FIXTURE_HEADER(orm_driver_plan_meta_v1);
  orm_status_t status =
      metadata.describe(plan->context, &meta, error);
  if (status != ORM_STATUS_OK) return status;
  if (meta.kind != ORM_DRIVER_PLAN_SELECT ||
      limits->max_result_rows == 0u ||
      limits->max_result_bytes < sizeof(int64_t)) {
    fixture_error(error, ORM_STATUS_UNSUPPORTED);
    return ORM_STATUS_UNSUPPORTED;
  }

  fixture_cursor_context *cursor = NULL;
  for (size_t i = 0u; i < sizeof(fixture_cursors) / sizeof(fixture_cursors[0]);
       ++i) {
    if (!fixture_cursors[i].live) {
      cursor = &fixture_cursors[i];
      break;
    }
  }
  if (cursor == NULL) {
    fixture_error(error, ORM_STATUS_LIMIT_EXCEEDED);
    return ORM_STATUS_LIMIT_EXCEEDED;
  }

  memset(cursor, 0, sizeof(*cursor));
  cursor->connection = connection;
  cursor->live = 1;
  ++connection->active_cursors;

  out->header =
      (orm_driver_header_v1)FIXTURE_HEADER(orm_driver_cursor_v1);
  out->context = cursor;
  out->ops = (orm_driver_table_v1)FIXTURE_TABLE(&fixture_cursor_ops);
  fixture_error(error, ORM_STATUS_OK);
  return ORM_STATUS_OK;
}

static orm_status_t ORM_DRIVER_CALL fixture_execute_command(
    void *context, const orm_driver_plan_view_v1 *plan,
    const orm_driver_limits_v1 *limits, uint64_t *affected_rows,
    orm_error_t *error) {
  if (affected_rows != NULL) *affected_rows = 0u;
  fixture_connection_context *connection = context;
  if (connection == NULL || !connection->live ||
      connection->module == NULL || plan == NULL || limits == NULL ||
      affected_rows == NULL) {
    fixture_error(error, ORM_STATUS_INVALID_ARGUMENT);
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  if (plan->metadata.data == NULL ||
      plan->metadata.bytes < sizeof(orm_driver_plan_metadata_ops_v1)) {
    fixture_error(error, ORM_STATUS_ABI_MISMATCH);
    return ORM_STATUS_ABI_MISMATCH;
  }
  orm_driver_plan_metadata_ops_v1 metadata;
  memcpy(&metadata, plan->metadata.data, sizeof(metadata));
  orm_driver_plan_meta_v1 meta;
  memset(&meta, 0, sizeof(meta));
  meta.header =
      (orm_driver_header_v1)FIXTURE_HEADER(orm_driver_plan_meta_v1);
  const orm_status_t status =
      metadata.describe(plan->context, &meta, error);
  if (status != ORM_STATUS_OK) return status;
  if (meta.kind != ORM_DRIVER_PLAN_INSERT) {
    fixture_error(error, ORM_STATUS_UNSUPPORTED);
    return ORM_STATUS_UNSUPPORTED;
  }
  *affected_rows = UINT64_C(3);
  fixture_error(error, ORM_STATUS_OK);
  return ORM_STATUS_OK;
}

static void ORM_DRIVER_CALL fixture_transaction_destroy(void *context) {
  fixture_transaction_context *transaction = context;
  if (transaction == NULL || !transaction->live ||
      transaction->connection == NULL) return;
  if (transaction->connection->active_transactions == 0u) abort();
  --transaction->connection->active_transactions;
  memset(transaction, 0, sizeof(*transaction));
}

static orm_status_t ORM_DRIVER_CALL fixture_transaction_open_cursor(
    void *context, const orm_driver_plan_view_v1 *plan,
    const orm_driver_limits_v1 *limits, orm_driver_cursor_v1 *out,
    orm_error_t *error) {
  fixture_transaction_context *transaction = context;
  if (transaction == NULL || !transaction->live)
    return ORM_STATUS_INVALID_ARGUMENT;
  return fixture_open_cursor(transaction->connection, plan, limits, out, error);
}

static orm_status_t ORM_DRIVER_CALL fixture_transaction_execute_command(
    void *context, const orm_driver_plan_view_v1 *plan,
    const orm_driver_limits_v1 *limits, uint64_t *affected_rows,
    orm_error_t *error) {
  fixture_transaction_context *transaction = context;
  if (transaction == NULL || !transaction->live)
    return ORM_STATUS_INVALID_ARGUMENT;
  return fixture_execute_command(
      transaction->connection, plan, limits, affected_rows, error);
}

static orm_status_t ORM_DRIVER_CALL fixture_transaction_commit(
    void *context, orm_error_t *error) {
  fixture_transaction_context *transaction = context;
  if (transaction == NULL || !transaction->live ||
      transaction->committed || transaction->rolled_back) {
    fixture_error(error, ORM_STATUS_INVALID_STATE);
    return ORM_STATUS_INVALID_STATE;
  }
  transaction->committed = 1;
  fixture_error(error, ORM_STATUS_OK);
  return ORM_STATUS_OK;
}

static orm_status_t ORM_DRIVER_CALL fixture_transaction_rollback(
    void *context, orm_error_t *error) {
  fixture_transaction_context *transaction = context;
  if (transaction == NULL || !transaction->live ||
      transaction->committed || transaction->rolled_back) {
    fixture_error(error, ORM_STATUS_INVALID_STATE);
    return ORM_STATUS_INVALID_STATE;
  }
  transaction->rolled_back = 1;
  fixture_error(error, ORM_STATUS_OK);
  return ORM_STATUS_OK;
}

static const orm_driver_transaction_ops_v1 fixture_transaction_ops = {
    FIXTURE_HEADER(orm_driver_transaction_ops_v1),
    fixture_transaction_destroy,
    fixture_transaction_open_cursor,
    fixture_transaction_execute_command,
    fixture_transaction_commit,
    fixture_transaction_rollback,
    NULL, NULL, NULL};

static orm_status_t ORM_DRIVER_CALL fixture_begin_transaction(
    void *context, orm_isolation_t isolation,
    orm_driver_transaction_v1 *out, orm_error_t *error) {
  (void)isolation;
  if (out != NULL) memset(out, 0, sizeof(*out));
  fixture_connection_context *connection = context;
  if (connection == NULL || !connection->live || out == NULL) {
    fixture_error(error, ORM_STATUS_INVALID_ARGUMENT);
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  for (size_t i = 0u;
       i < sizeof(fixture_transactions) / sizeof(fixture_transactions[0]);
       ++i) {
    if (!fixture_transactions[i].live) {
      fixture_transactions[i].connection = connection;
      fixture_transactions[i].live = 1;
      ++connection->active_transactions;
      out->header =
          (orm_driver_header_v1)FIXTURE_HEADER(orm_driver_transaction_v1);
      out->context = &fixture_transactions[i];
      out->ops = (orm_driver_table_v1)FIXTURE_TABLE(&fixture_transaction_ops);
      fixture_error(error, ORM_STATUS_OK);
      return ORM_STATUS_OK;
    }
  }
  fixture_error(error, ORM_STATUS_LIMIT_EXCEEDED);
  return ORM_STATUS_LIMIT_EXCEEDED;
}

static void ORM_DRIVER_CALL fixture_destroy_connection(void *context) {
  fixture_connection_context *connection = context;
  if (connection == NULL || !connection->live || connection->module == NULL)
    return;
  if (connection->active_cursors != 0u ||
      connection->active_transactions != 0u) abort();
  --connection->module->live_connections;
  memset(connection, 0, sizeof(*connection));
}

static const orm_driver_connection_ops_v1 fixture_connection_ops = {
    FIXTURE_HEADER(orm_driver_connection_ops_v1),
    fixture_destroy_connection, fixture_open_cursor,
    fixture_execute_command, fixture_begin_transaction};

static orm_status_t ORM_DRIVER_CALL fixture_create_connection(
    void *context, const orm_config_t *config,
    const orm_driver_limits_v1 *limits, orm_driver_connection_v1 *out,
    orm_error_t *error) {
  if (out != NULL) memset(out, 0, sizeof(*out));
  orm_status_t status = ORM_STATUS_INVALID_ARGUMENT;
  fixture_module_context *module =
      context == &fixture_module ? &fixture_module : NULL;

  if (module != NULL && config != NULL && limits != NULL &&
      out != NULL && config->driver.data != NULL &&
      config->driver.len == sizeof(fixture_id) - 1u &&
      memcmp(config->driver.data, fixture_id,
             sizeof(fixture_id) - 1u) == 0) {
    for (size_t i = 0u;
         i < sizeof(fixture_connections) / sizeof(fixture_connections[0]);
         ++i) {
      if (!fixture_connections[i].live) {
        fixture_connections[i].module = module;
        fixture_connections[i].live = 1;
        ++module->live_connections;
        out->header =
            (orm_driver_header_v1)FIXTURE_HEADER(orm_driver_connection_v1);
        out->context = &fixture_connections[i];
        out->ops = (orm_driver_table_v1)FIXTURE_TABLE(&fixture_connection_ops);
        status = ORM_STATUS_OK;
        break;
      }
    }
    if (status != ORM_STATUS_OK)
      status = ORM_STATUS_LIMIT_EXCEEDED;
  }

  fixture_error(error, status);
  return status;
}

#define FIXTURE_CAPABILITIES \
  (ORM_DRIVER_CAP_SELECT | ORM_DRIVER_CAP_INSERT | \
   ORM_DRIVER_CAP_TRANSACTION | ORM_DRIVER_CAP_INCREMENTAL_ROWS)

static const TurboDb_Driver_vtable fixture_driver_vtable = {
    .implementation = "fixture",
    .capabilities = FIXTURE_CAPABILITIES,
    .create = fixture_create_connection};

static TurboDb_Driver fixture_driver = {
    &fixture_module, &fixture_driver_vtable};

static const salts_plugin_export fixture_exports[] = {{
    .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
    .kind = SALTS_PLUGIN_EXPORT_INTERFACE,
    .contract_version = ORM_DRIVER_INTERFACE_CONTRACT_VERSION,
    .capabilities = FIXTURE_CAPABILITIES,
    .export_id = ORM_DRIVER_PLUGIN_EXPORT_ID,
    .contract_id = ORM_DRIVER_INTERFACE_CONTRACT_ID,
    .value.interface = {&TurboDb_Driver_interface_meta, &fixture_driver}}};

static const salts_plugin_manifest fixture_manifest = {
    .struct_size = SALTS_PLUGIN_MANIFEST_SIZE,
    .abi_version = SALTS_PLUGIN_ABI_VERSION,
    .plugin_id = fixture_id,
    .version = {1u, 0u, 0u},
    .exports = fixture_exports,
    .export_count = 1u};

SALTS_PLUGIN_QUERY_EXPORT const salts_plugin_manifest *SALTS_PLUGIN_CALL
salts_plugin_query(uint32_t host_abi) {
  return host_abi == SALTS_PLUGIN_ABI_VERSION ? &fixture_manifest : NULL;
}

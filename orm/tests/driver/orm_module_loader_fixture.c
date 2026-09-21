#include <orm_driver_abi.h>

#include <stdlib.h>
#include <string.h>

#define FIXTURE_HEADER(T) {(uint32_t)sizeof(T), ORM_DRIVER_ABI_VERSION}
#define FIXTURE_TABLE(p) {(p), (uint32_t)sizeof(*(p)), 0u}

typedef struct fixture_module_context {
  orm_driver_plan_metadata_ops_v1 metadata;
  orm_driver_lifetime_ops_v1 lifetime;
  uint32_t live_connections;
  int live;
} fixture_module_context;

typedef struct fixture_connection_context {
  fixture_module_context *module;
  uint32_t active_cursors;
  int live;
} fixture_connection_context;

typedef struct fixture_cursor_context {
  fixture_connection_context *connection;
  void *ticket;
  int emitted;
  int live;
} fixture_cursor_context;

static fixture_module_context fixture_modules[4];
static fixture_connection_context fixture_connections[4];
static fixture_cursor_context fixture_cursors[8];
static const uint8_t fixture_bundle[ORM_DRIVER_BUNDLE_ID_BYTES] =
    ORM_DRIVER_BUNDLE_ID_INIT;
static const char fixture_id[] = "fixture";
static const char fixture_alias[] = "fixture-alias";

static void fixture_error(orm_error_t *error, orm_status_t status) {
  if (error == NULL) return;
  memset(error, 0, sizeof(*error));
  error->struct_size = (uint32_t)sizeof(*error);
  error->status = status;
}

static orm_status_t ORM_DRIVER_CALL fixture_initialize(
    const orm_driver_host_v1 *host, void **out, orm_error_t *error) {
  if (out != NULL) *out = NULL;
  if (host == NULL || out == NULL ||
      host->header.abi_version != ORM_DRIVER_ABI_VERSION ||
      host->header.struct_size < sizeof(*host) ||
      memcmp(host->bundle_id, fixture_bundle, sizeof(fixture_bundle)) != 0 ||
      host->plan_metadata.data == NULL ||
      host->plan_metadata.bytes < sizeof(orm_driver_plan_metadata_ops_v1) ||
      host->lifetime.data == NULL ||
      host->lifetime.bytes < sizeof(orm_driver_lifetime_ops_v1)) {
    fixture_error(error, ORM_STATUS_ABI_MISMATCH);
    return ORM_STATUS_ABI_MISMATCH;
  }

  for (size_t i = 0u; i < sizeof(fixture_modules) / sizeof(fixture_modules[0]);
       ++i) {
    if (!fixture_modules[i].live) {
      fixture_modules[i].live = 1;
      fixture_modules[i].live_connections = 0u;
      memcpy(&fixture_modules[i].metadata, host->plan_metadata.data,
             sizeof(fixture_modules[i].metadata));
      memcpy(&fixture_modules[i].lifetime, host->lifetime.data,
             sizeof(fixture_modules[i].lifetime));
      *out = &fixture_modules[i];
      fixture_error(error, ORM_STATUS_OK);
      return ORM_STATUS_OK;
    }
  }

  fixture_error(error, ORM_STATUS_LIMIT_EXCEEDED);
  return ORM_STATUS_LIMIT_EXCEEDED;
}

static orm_status_t ORM_DRIVER_CALL fixture_finalize(
    void *context, orm_error_t *error) {
  orm_status_t status = ORM_STATUS_INVALID_ARGUMENT;
  for (size_t i = 0u; i < sizeof(fixture_modules) / sizeof(fixture_modules[0]);
       ++i) {
    if (context == &fixture_modules[i] && fixture_modules[i].live) {
      if (fixture_modules[i].live_connections != 0u) {
        status = ORM_STATUS_BUSY;
      } else {
        memset(&fixture_modules[i], 0, sizeof(fixture_modules[i]));
        status = ORM_STATUS_OK;
      }
      break;
    }
  }
  fixture_error(error, status);
  return status;
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
  fixture_module_context *module = cursor->connection->module;
  if (module != NULL && cursor->ticket != NULL)
    module->lifetime.release(cursor->ticket);
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

  orm_driver_plan_meta_v1 meta;
  memset(&meta, 0, sizeof(meta));
  meta.header =
      (orm_driver_header_v1)FIXTURE_HEADER(orm_driver_plan_meta_v1);
  orm_status_t status =
      connection->module->metadata.describe(plan->context, &meta, error);
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

  void *ticket = NULL;
  status = connection->module->lifetime.acquire(
      (void *)plan->context, &ticket, error);
  if (status != ORM_STATUS_OK) return status;

  memset(cursor, 0, sizeof(*cursor));
  cursor->connection = connection;
  cursor->ticket = ticket;
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
  orm_driver_plan_meta_v1 meta;
  memset(&meta, 0, sizeof(meta));
  meta.header =
      (orm_driver_header_v1)FIXTURE_HEADER(orm_driver_plan_meta_v1);
  const orm_status_t status =
      connection->module->metadata.describe(plan->context, &meta, error);
  if (status != ORM_STATUS_OK) return status;
  if (meta.kind != ORM_DRIVER_PLAN_INSERT) {
    fixture_error(error, ORM_STATUS_UNSUPPORTED);
    return ORM_STATUS_UNSUPPORTED;
  }
  *affected_rows = UINT64_C(3);
  fixture_error(error, ORM_STATUS_OK);
  return ORM_STATUS_OK;
}

static void ORM_DRIVER_CALL fixture_destroy_connection(void *context) {
  fixture_connection_context *connection = context;
  if (connection == NULL || !connection->live || connection->module == NULL)
    return;
  if (connection->active_cursors != 0u) abort();
  --connection->module->live_connections;
  memset(connection, 0, sizeof(*connection));
}

static const orm_driver_connection_ops_v1 fixture_connection_ops = {
    FIXTURE_HEADER(orm_driver_connection_ops_v1),
    fixture_destroy_connection, fixture_open_cursor,
    fixture_execute_command, NULL};

static orm_status_t ORM_DRIVER_CALL fixture_create_connection(
    void *context, const orm_config_t *config,
    const orm_driver_limits_v1 *limits, orm_driver_connection_v1 *out,
    orm_error_t *error) {
  if (out != NULL) memset(out, 0, sizeof(*out));
  orm_status_t status = ORM_STATUS_INVALID_ARGUMENT;
  fixture_module_context *module = NULL;
  for (size_t i = 0u; i < sizeof(fixture_modules) / sizeof(fixture_modules[0]);
       ++i) {
    if (context == &fixture_modules[i] && fixture_modules[i].live) {
      module = &fixture_modules[i];
      break;
    }
  }

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

static const orm_driver_module_ops_v1 fixture_module_ops = {
    FIXTURE_HEADER(orm_driver_module_ops_v1),
    fixture_initialize, fixture_finalize};
static const orm_driver_bytes_v1 fixture_aliases[] = {
    {fixture_alias, sizeof(fixture_alias) - 1u}};
static const orm_driver_api_v1 fixture_api = {
    FIXTURE_HEADER(orm_driver_api_v1),
    ORM_DRIVER_BUNDLE_ID_INIT,
    {fixture_id, sizeof(fixture_id) - 1u},
    fixture_aliases,
    1u,
    0u,
    ORM_DRIVER_CAP_SELECT | ORM_DRIVER_CAP_INSERT |
        ORM_DRIVER_CAP_INCREMENTAL_ROWS,
    ORM_DRIVER_EXEC_CALLER_BLOCKING,
    FIXTURE_TABLE(&fixture_module_ops),
    fixture_create_connection,
    FIXTURE_TABLE(&fixture_connection_ops)};

ORM_DRIVER_EXPORT int32_t ORM_DRIVER_CALL orm_driver_get_api_v1(
    const orm_driver_host_v1 *host, uint32_t host_bytes,
    const orm_driver_api_v1 **out_api, uint32_t *out_api_bytes) {
  if (out_api != NULL) *out_api = NULL;
  if (out_api_bytes != NULL) *out_api_bytes = 0u;
  if (host == NULL || host_bytes < sizeof(*host) ||
      out_api == NULL || out_api_bytes == NULL)
    return ORM_STATUS_INVALID_ARGUMENT;
  *out_api = &fixture_api;
  *out_api_bytes = (uint32_t)sizeof(fixture_api);
  return ORM_STATUS_OK;
}

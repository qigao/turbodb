#include "orm_driver_fixture.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define HEADER(T) {sizeof(T), ORM_DRIVER_ABI_VERSION}
#define TABLE(p) {(p), sizeof(*(p)), 0u}
#define FIELD_END(T, f) (offsetof(T, f) + sizeof(((T *)0)->f))
#define TOKEN_COUNT 4u

/* The fixture is single-threaded and bounded. These counters/tickets observe
 * test allocations, not core ownership; #28 remains the production owner. */
static orm_driver_fixture_stats stats;
static uint32_t failure_point;
static const uint8_t bundle[ORM_DRIVER_BUNDLE_ID_BYTES] = ORM_DRIVER_BUNDLE_ID_INIT;
static const unsigned char column_name[] = ORM_DRIVER_FIXTURE_OPTION;

typedef struct fixture_ticket { void *parent; } fixture_ticket;
static fixture_ticket tickets[ORM_DRIVER_FIXTURE_TICKETS];
typedef struct fixture_module {
  orm_driver_plan_metadata_ops_v1 metadata;
  orm_driver_lifetime_ops_v1 lifetime;
  uint32_t live_connections;
} fixture_module;
typedef struct fixture_connection {
  fixture_module *module;
  unsigned char value[ORM_DRIVER_FIXTURE_VALUE_BYTES];
  size_t size;
  uint64_t value_budget;
  uint32_t active_cursors;
} fixture_connection;
typedef struct fixture_cursor {
  fixture_connection *connection;
  void *ticket;
  cserde_token tokens[TOKEN_COUNT];
  size_t index;
  int finished;
} fixture_cursor;

/* Declare callbacks rather than tentative const objects (MSVC C4132). */
static void ORM_DRIVER_CALL destroy_connection(void *context);
static orm_status_t ORM_DRIVER_CALL open_cursor(void *context,
    const orm_driver_plan_view_v1 *plan, const orm_driver_limits_v1 *limits,
    orm_driver_cursor_v1 *out, orm_error_t *error);
static orm_status_t ORM_DRIVER_CALL next(void *context, cserde_reader *reader,
    orm_driver_step_v1 *step, orm_error_t *error);
static void ORM_DRIVER_CALL cancel(void *context);
static void ORM_DRIVER_CALL destroy_cursor(void *context);

static const orm_driver_connection_ops_v1 connection_ops = {
    HEADER(orm_driver_connection_ops_v1), destroy_connection, open_cursor, NULL, NULL};
static const orm_driver_cursor_ops_v1 cursor_ops = {
    HEADER(orm_driver_cursor_ops_v1), next, cancel, destroy_cursor, NULL, NULL};

static orm_status_t result(orm_error_t *error, orm_status_t status) {
  if (error != NULL) {
    memset(error, 0, sizeof(*error));
    error->struct_size = sizeof(*error);
    error->status = status;
    if (status != ORM_STATUS_OK) {
      static const char message[] = "driver fixture contract failure";
      memcpy(error->message, message, sizeof(message));
    }
  }
  return status;
}

static int fail_at(uint32_t point) {
  if (failure_point != point) return 0;
  failure_point = ORM_DRIVER_FIXTURE_NO_FAILURE;
  return 1;
}

static void *allocate(size_t bytes) {
  void *p = calloc(1u, bytes);
  if (p != NULL) ++stats.allocations;
  return p;
}

static void deallocate(void *p) {
  if (p != NULL) { ++stats.deallocations; free(p); }
}

static orm_status_t check_header(orm_driver_header_v1 header, size_t required) {
  if (header.abi_version != ORM_DRIVER_ABI_VERSION || header.struct_size < required)
    return ORM_STATUS_ABI_MISMATCH;
  if (header.struct_size > ORM_DRIVER_DESCRIPTOR_MAX_BYTES)
    return ORM_STATUS_LIMIT_EXCEEDED;
  return ORM_STATUS_OK;
}

/* Pure test-host plan accessors: the context is a complete public SELECT DTO,
 * not orm_query_plan. No driver implementation includes a private plan header. */
static orm_status_t ORM_DRIVER_CALL describe(const void *context,
    orm_driver_plan_meta_v1 *out, orm_error_t *error) {
  if (out != NULL) memset(out, 0, sizeof(*out));
  if (context == NULL || out == NULL) return result(error, ORM_STATUS_INVALID_ARGUMENT);
  const orm_driver_plan_meta_v1 *meta = context;
  orm_status_t status = check_header(meta->header, sizeof(*meta));
  if (status == ORM_STATUS_OK) *out = *meta;
  return result(error, status);
}

static orm_status_t ORM_DRIVER_CALL column_at(const void *context, uint64_t index,
    orm_driver_bytes_v1 *out, orm_error_t *error) {
  if (out != NULL) memset(out, 0, sizeof(*out));
  if (context == NULL || out == NULL) return result(error, ORM_STATUS_INVALID_ARGUMENT);
  if (index != 0u) return result(error, ORM_STATUS_OUT_OF_RANGE);
  *out = (orm_driver_bytes_v1){column_name, sizeof(column_name) - 1u};
  return result(error, ORM_STATUS_OK);
}

static orm_status_t ORM_DRIVER_CALL ordering(const void *context,
    orm_driver_ordering_v1 *out, orm_error_t *error) {
  if (out != NULL) memset(out, 0, sizeof(*out));
  if (context == NULL || out == NULL) return result(error, ORM_STATUS_INVALID_ARGUMENT);
  out->header = (orm_driver_header_v1)HEADER(orm_driver_ordering_v1);
  return result(error, ORM_STATUS_OK);
}

/* This fixture SELECT has no assignments, predicates or raw parameters. */
#define EMPTY_ACCESSOR(name, T) \
static orm_status_t ORM_DRIVER_CALL name(const void *context, uint64_t index, \
    T *out, orm_error_t *error) { \
  (void)index; \
  if (out != NULL) memset(out, 0, sizeof(*out)); \
  return result(error, context == NULL || out == NULL ? \
      ORM_STATUS_INVALID_ARGUMENT : ORM_STATUS_OUT_OF_RANGE); \
}
EMPTY_ACCESSOR(assignment_at, orm_driver_assignment_v1)
EMPTY_ACCESSOR(predicate_at, orm_driver_predicate_v1)
EMPTY_ACCESSOR(parameter_at, orm_driver_value_v1)
#undef EMPTY_ACCESSOR

static orm_status_t ORM_DRIVER_CALL acquire(void *parent, void **out, orm_error_t *error) {
  if (out != NULL) *out = NULL;
  if (parent == NULL || out == NULL) return result(error, ORM_STATUS_INVALID_ARGUMENT);
  for (uint32_t i = 0u; i < ORM_DRIVER_FIXTURE_TICKETS; ++i) {
    if (tickets[i].parent == NULL) {
      tickets[i].parent = parent; *out = &tickets[i];
      ++stats.lease_acquires; ++stats.live_tickets;
      return result(error, ORM_STATUS_OK);
    }
  }
  return result(error, ORM_STATUS_LIMIT_EXCEEDED);
}

static void ORM_DRIVER_CALL release(void *ticket) {
  for (uint32_t i = 0u; i < ORM_DRIVER_FIXTURE_TICKETS; ++i) {
    if (ticket == &tickets[i] && tickets[i].parent != NULL) {
      tickets[i].parent = NULL;
      ++stats.lease_releases; --stats.live_tickets;
      return;
    }
  }
  /* Calling a consumed/foreign ticket is a test-harness programming error. */
  abort();
}

static const orm_driver_plan_metadata_ops_v1 metadata_ops = {
    HEADER(orm_driver_plan_metadata_ops_v1), describe, column_at, ordering};
static const orm_driver_plan_value_ops_v1 value_ops = {
    HEADER(orm_driver_plan_value_ops_v1), assignment_at, predicate_at, parameter_at};
static const orm_driver_lifetime_ops_v1 lifetime_ops = {
    HEADER(orm_driver_lifetime_ops_v1), acquire, release};

void orm_driver_fixture_reset(void) {
  /* Never hide a leak left by an earlier case by zeroing its counters. */
  if (stats.live_connections != 0u || stats.live_modules != 0u ||
      stats.live_cursors != 0u || stats.live_tickets != 0u ||
      stats.allocations != stats.deallocations) abort();
  memset(&stats, 0, sizeof(stats));
  memset(tickets, 0, sizeof(tickets));
  failure_point = ORM_DRIVER_FIXTURE_NO_FAILURE;
}
orm_driver_fixture_stats orm_driver_fixture_stats_get(void) { return stats; }
const uint8_t *orm_driver_fixture_bundle(void) { return bundle; }
void orm_driver_fixture_fail_next(uint32_t point) {
  if (point > ORM_DRIVER_FIXTURE_CONNECTION_AFTER_ALLOC) abort();
  failure_point = point;
}
orm_driver_host_v1 orm_driver_fixture_host(void) {
  orm_driver_host_v1 host = {0};
  host.header = (orm_driver_header_v1)HEADER(orm_driver_host_v1);
  memcpy(host.bundle_id, bundle, sizeof(bundle));
  host.plan_metadata = (orm_driver_table_v1)TABLE(&metadata_ops);
  host.plan_values = (orm_driver_table_v1)TABLE(&value_ops);
  host.lifetime = (orm_driver_table_v1)TABLE(&lifetime_ops);
  return host;
}

static orm_status_t ORM_DRIVER_CALL initialize(const orm_driver_host_v1 *host,
    void **out, orm_error_t *error) {
  if (out != NULL) *out = NULL;
  if (out == NULL) return result(error, ORM_STATUS_INVALID_ARGUMENT);
  orm_status_t status = orm_driver_validate_host_v1(host, sizeof(*host), bundle, error);
  if (status != ORM_STATUS_OK) return status;
  ++stats.init_calls;
  if (stats.live_modules == ORM_DRIVER_FIXTURE_MODULES)
    return result(error, ORM_STATUS_LIMIT_EXCEEDED);
  if (fail_at(ORM_DRIVER_FIXTURE_MODULE_BEFORE_ALLOC))
    return result(error, ORM_STATUS_OUT_OF_MEMORY);
  fixture_module *module = allocate(sizeof(*module));
  if (module == NULL) return result(error, ORM_STATUS_OUT_OF_MEMORY);
  if (fail_at(ORM_DRIVER_FIXTURE_MODULE_AFTER_ALLOC)) {
    deallocate(module); return result(error, ORM_STATUS_OUT_OF_MEMORY);
  }
  /* Only complete, previously validated prefixes are copied. Neither the host
   * object nor its table storage needs to survive initialize. Callback code
   * and its own backing state must remain alive for the module lifetime. */
  orm_driver_host_v1 admitted;
  memcpy(&admitted, host, sizeof(admitted));
  memcpy(&module->metadata, admitted.plan_metadata.data,
         FIELD_END(orm_driver_plan_metadata_ops_v1, ordering));
  memcpy(&module->lifetime, admitted.lifetime.data,
         FIELD_END(orm_driver_lifetime_ops_v1, release));
  ++stats.live_modules; *out = module;
  return result(error, ORM_STATUS_OK);
}

static orm_status_t ORM_DRIVER_CALL finalize(void *context, orm_error_t *error) {
  if (context == NULL) return result(error, ORM_STATUS_INVALID_ARGUMENT);
  fixture_module *module = context;
  ++stats.finalize_calls;
  if (module->live_connections != 0u) return result(error, ORM_STATUS_BUSY);
  --stats.live_modules; deallocate(module);
  return result(error, ORM_STATUS_OK);
}

static orm_status_t copy_value(fixture_connection *c, orm_driver_bytes_v1 value) {
  if (value.data == NULL && value.size != 0u) return ORM_STATUS_INVALID_ARGUMENT;
  if (value.size > ORM_DRIVER_FIXTURE_VALUE_BYTES || value.size > c->value_budget)
    return ORM_STATUS_LIMIT_EXCEEDED;
  if (c->active_cursors != 0u) return ORM_STATUS_BUSY;
  if (value.size != 0u) memmove(c->value, value.data, (size_t)value.size);
  c->size = (size_t)value.size;
  return ORM_STATUS_OK;
}

orm_status_t orm_driver_fixture_set_value(orm_driver_connection_v1 *connection,
                                         orm_driver_bytes_v1 value) {
  if (connection == NULL || connection->context == NULL ||
      connection->ops.data != &connection_ops) return ORM_STATUS_INVALID_ARGUMENT;
  return copy_value(connection->context, value);
}

static orm_status_t check_config(const orm_config_t *cfg,
    const orm_driver_limits_v1 *limits, orm_driver_bytes_v1 *value) {
  if (cfg == NULL || limits == NULL) return ORM_STATUS_INVALID_ARGUMENT;
  if (cfg->struct_size < sizeof(*cfg) || cfg->abi_version != ORM_C_ABI_VERSION)
    return ORM_STATUS_ABI_MISMATCH;
  orm_status_t status = check_header(limits->header, sizeof(*limits));
  if (status != ORM_STATUS_OK) return status;
  if (cfg->driver.data == NULL || cfg->driver.len != sizeof(ORM_DRIVER_FIXTURE_ID)-1u ||
      memcmp(cfg->driver.data, ORM_DRIVER_FIXTURE_ID, cfg->driver.len) != 0)
    return ORM_STATUS_INVALID_ARGUMENT;
  if (cfg->option_count > 1u) return ORM_STATUS_LIMIT_EXCEEDED;
  if (cfg->option_count == 0u || cfg->options == NULL) return ORM_STATUS_INVALID_ARGUMENT;
  const orm_option_t *option = cfg->options;
  if (option->keyword.data == NULL || option->keyword.len != sizeof(column_name)-1u ||
      memcmp(option->keyword.data, column_name, option->keyword.len) != 0)
    return ORM_STATUS_INVALID_ARGUMENT;
  *value = (orm_driver_bytes_v1){option->value.data, option->value.len};
  if (value->data == NULL && value->size != 0u) return ORM_STATUS_INVALID_ARGUMENT;
  if (value->size > ORM_DRIVER_FIXTURE_VALUE_BYTES || value->size > limits->max_parameter_bytes)
    return ORM_STATUS_LIMIT_EXCEEDED;
  return ORM_STATUS_OK;
}

static orm_status_t ORM_DRIVER_CALL create_connection(void *context, const orm_config_t *cfg,
    const orm_driver_limits_v1 *limits, orm_driver_connection_v1 *out, orm_error_t *error) {
  if (out != NULL) memset(out, 0, sizeof(*out));
  if (context == NULL || out == NULL) return result(error, ORM_STATUS_INVALID_ARGUMENT);
  orm_driver_bytes_v1 value = {0};
  orm_status_t status = check_config(cfg, limits, &value);
  if (status != ORM_STATUS_OK) return result(error, status);
  fixture_module *module = context;
  ++stats.create_calls;
  if (stats.live_connections == ORM_DRIVER_FIXTURE_CONNECTIONS)
    return result(error, ORM_STATUS_LIMIT_EXCEEDED);
  if (fail_at(ORM_DRIVER_FIXTURE_CONNECTION_BEFORE_ALLOC))
    return result(error, ORM_STATUS_OUT_OF_MEMORY);
  fixture_connection *c = allocate(sizeof(*c));
  if (c == NULL) return result(error, ORM_STATUS_OUT_OF_MEMORY);
  if (fail_at(ORM_DRIVER_FIXTURE_CONNECTION_AFTER_ALLOC)) {
    status = ORM_STATUS_OUT_OF_MEMORY; goto cleanup;
  }
  c->module = module; c->value_budget = limits->max_parameter_bytes;
  status = copy_value(c, value);
  if (status != ORM_STATUS_OK) goto cleanup;
  out->header = (orm_driver_header_v1)HEADER(orm_driver_connection_v1);
  out->context = c; out->ops = (orm_driver_table_v1)TABLE(&connection_ops);
  ++module->live_connections; ++stats.live_connections;
  return result(error, ORM_STATUS_OK);
cleanup:
  deallocate(c);
  return result(error, status);
}

static void ORM_DRIVER_CALL destroy_connection(void *context) {
  if (context == NULL) return;
  fixture_connection *c = context;
  /* The test driver has no deferred-close policy; callers destroy cursors first. */
  if (c->active_cursors != 0u) abort();
  --c->module->live_connections; --stats.live_connections; ++stats.destroy_calls;
  deallocate(c);
}

static cserde_status reader_next(void *context, cserde_token *out) {
  if (context == NULL || out == NULL) return CSERDE_INVALID_ARGUMENT;
  fixture_cursor *cursor = context;
  if (cursor->index == TOKEN_COUNT) return CSERDE_DONE;
  *out = cursor->tokens[cursor->index++];
  return CSERDE_OK;
}
static const cserde_reader_ops reader_ops = {
    sizeof(cserde_reader_ops), CSERDE_READER_OPS_ABI_VERSION, reader_next};

static orm_status_t ORM_DRIVER_CALL next(void *context, cserde_reader *reader,
    orm_driver_step_v1 *step, orm_error_t *error) {
  if (reader != NULL) memset(reader, 0, sizeof(*reader));
  if (step != NULL) {
    memset(step, 0, sizeof(*step));
    step->header = (orm_driver_header_v1)HEADER(orm_driver_step_v1);
    step->kind = ORM_DRIVER_STEP_ERROR;
  }
  if (context == NULL || reader == NULL || step == NULL)
    return result(error, ORM_STATUS_INVALID_ARGUMENT);
  fixture_cursor *cursor = context;
  ++stats.next_calls;
  if (cursor->finished) { step->kind = ORM_DRIVER_STEP_DONE; return result(error, ORM_STATUS_OK); }
  cserde_status status = cserde_reader_init(reader, &reader_ops, cursor);
  if (status != CSERDE_OK) return result(error, ORM_STATUS_INTERNAL_ERROR);
  cursor->finished = 1;
  step->kind = ORM_DRIVER_STEP_ROW;
  return result(error, ORM_STATUS_OK);
}

static void ORM_DRIVER_CALL cancel(void *context) {
  if (context == NULL) return;
  fixture_cursor *cursor = context;
  ++stats.cancel_calls; cursor->finished = 1;
  /* Do not release a delivered reader, its bytes, or its operation ticket. */
}

static void ORM_DRIVER_CALL destroy_cursor(void *context) {
  if (context == NULL) return;
  fixture_cursor *cursor = context;
  fixture_connection *c = cursor->connection;
  c->module->lifetime.release(cursor->ticket);
  --c->active_cursors; --stats.live_cursors; ++stats.cursor_destroy_calls;
  deallocate(cursor);
}

static orm_status_t ORM_DRIVER_CALL open_cursor(void *context,
    const orm_driver_plan_view_v1 *plan, const orm_driver_limits_v1 *limits,
    orm_driver_cursor_v1 *out, orm_error_t *error) {
  if (out != NULL) memset(out, 0, sizeof(*out));
  if (context == NULL || plan == NULL || limits == NULL || out == NULL)
    return result(error, ORM_STATUS_INVALID_ARGUMENT);
  orm_status_t status = check_header(plan->header, sizeof(*plan));
  if (status != ORM_STATUS_OK) return result(error, status);
  status = check_header(limits->header, sizeof(*limits));
  if (status != ORM_STATUS_OK) return result(error, status);
  fixture_connection *c = context;
  if (c->active_cursors != 0u) return result(error, ORM_STATUS_BUSY);
  orm_driver_plan_meta_v1 meta = {0};
  status = c->module->metadata.describe(plan->context, &meta, error);
  if (status != ORM_STATUS_OK) return status;
  if (meta.kind != ORM_DRIVER_PLAN_SELECT || meta.flags != 0u ||
      meta.column_count != 1u || meta.assignment_count != 0u ||
      meta.predicate_count != 0u || meta.raw_parameter_count != 0u ||
      meta.raw_sql.size != 0u || meta.table.size != 0u)
    return result(error, ORM_STATUS_UNSUPPORTED);
  if (limits->max_columns == 0u || limits->max_result_rows == 0u ||
      limits->max_result_bytes < sizeof(column_name)-1u + c->size)
    return result(error, ORM_STATUS_LIMIT_EXCEEDED);
  fixture_cursor *cursor = allocate(sizeof(*cursor));
  if (cursor == NULL) return result(error, ORM_STATUS_OUT_OF_MEMORY);
  status = c->module->lifetime.acquire((void *)plan->context, &cursor->ticket, error);
  if (status != ORM_STATUS_OK) { deallocate(cursor); return status; }
  cursor->connection = c;
  cursor->tokens[0].kind = CSERDE_MAP_BEGIN;
  cursor->tokens[1].kind = CSERDE_STRING;
  cursor->tokens[1].value.slice = (cserde_slice){column_name, sizeof(column_name)-1u, CSERDE_VIEW_STABLE};
  cursor->tokens[2].kind = CSERDE_STRING;
  cursor->tokens[2].value.slice = (cserde_slice){c->value, c->size, CSERDE_VIEW_TRANSIENT};
  cursor->tokens[3].kind = CSERDE_MAP_END;
  ++c->active_cursors; ++stats.live_cursors;
  out->header = (orm_driver_header_v1)HEADER(orm_driver_cursor_v1);
  out->context = cursor; out->ops = (orm_driver_table_v1)TABLE(&cursor_ops);
  return result(error, ORM_STATUS_OK);
}

static const orm_driver_module_ops_v1 module_ops = {
    HEADER(orm_driver_module_ops_v1), initialize, finalize};
static const orm_driver_api_v1 api = {
    HEADER(orm_driver_api_v1), ORM_DRIVER_BUNDLE_ID_INIT,
    {ORM_DRIVER_FIXTURE_ID, sizeof(ORM_DRIVER_FIXTURE_ID)-1u}, NULL, 0u, 0u,
    ORM_DRIVER_FIXTURE_CAPS, ORM_DRIVER_EXEC_CALLER_BLOCKING,
    TABLE(&module_ops), create_connection, TABLE(&connection_ops)};

ORM_DRIVER_EXPORT int32_t ORM_DRIVER_CALL orm_driver_get_api_v1(
    const orm_driver_host_v1 *host, uint32_t bytes,
    const orm_driver_api_v1 **out, uint32_t *out_bytes) {
  if (out != NULL) *out = NULL;
  if (out_bytes != NULL) *out_bytes = 0u;
  if (out == NULL || out_bytes == NULL) return ORM_STATUS_INVALID_ARGUMENT;
  orm_status_t status = orm_driver_validate_host_v1(host, bytes, bundle, NULL);
  if (status != ORM_STATUS_OK) return status;
  *out = &api; *out_bytes = sizeof(api);
  return ORM_STATUS_OK;
}

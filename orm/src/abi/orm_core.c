#include "orm_internal.h"
#include "orm_command_publisher.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool orm_backend_valid(const orm_backend *backend) {
  return backend != NULL && backend->ops != NULL && backend->context != NULL &&
         backend->ops->struct_size >= sizeof(*backend->ops) &&
         backend->ops->abi_version == ORM_BACKEND_OPS_ABI_VERSION &&
         backend->ops->destroy != NULL && backend->ops->open_cursor != NULL &&
         backend->ops->execute_command != NULL &&
         backend->ops->begin_transaction != NULL;
}

static bool orm_transaction_backend_valid(
    const orm_transaction_backend *backend) {
  return backend != NULL && backend->ops != NULL && backend->context != NULL &&
         backend->ops->struct_size >= sizeof(*backend->ops) &&
         backend->ops->abi_version == ORM_TRANSACTION_BACKEND_OPS_ABI_VERSION &&
         backend->ops->destroy != NULL && backend->ops->open_cursor != NULL &&
         backend->ops->execute_command != NULL && backend->ops->commit != NULL &&
         backend->ops->rollback != NULL;
}

void orm_error_set(orm_error_t *error, orm_status_t status,
                   const char *message) {
  if (error == NULL || error->struct_size < sizeof(*error))
    return;
  error->status = status;
  (void)snprintf(error->message, sizeof(error->message), "%s",
                 status == ORM_STATUS_OK
                     ? ""
                     : (message != NULL ? message
                                        : orm_status_message(status)));
}

bool orm_view_valid(vstr value, bool allow_empty) {
  return (allow_empty || value.len != 0u) &&
         (value.len == 0u || value.data != NULL);
}

bool orm_view_equal_cstr(vstr value, const char *text) {
  size_t length;
  if (text == NULL || !orm_view_valid(value, true))
    return false;
  length = strlen(text);
  return value.len == length &&
         (length == 0u || memcmp(value.data, text, length) == 0);
}

const orm_option_t *orm_option_find(const orm_config_t *config,
                                    const char *keyword) {
  uint32_t index;
  if (config == NULL || keyword == NULL)
    return NULL;
  for (index = 0u; index < config->option_count; ++index) {
    if (orm_view_equal_cstr(config->options[index].keyword, keyword))
      return &config->options[index];
  }
  return NULL;
}

static bool orm_u64_to_size(uint64_t input, size_t *output) {
  if (output == NULL || input > (uint64_t)SIZE_MAX)
    return false;
  *output = (size_t)input;
  return true;
}

static orm_status_t orm_limits_from_config(const orm_config_t *config,
                                           orm_limits *limits,
                                           orm_error_t *error) {
  if (config == NULL || limits == NULL ||
      config->struct_size != sizeof(*config) ||
      config->abi_version != ORM_C_ABI_VERSION) {
    orm_error_set(error, ORM_STATUS_ABI_MISMATCH,
                  "ORM configuration ABI does not match this build");
    return ORM_STATUS_ABI_MISMATCH;
  }
  if (!orm_view_valid(config->driver, false) ||
      (config->option_count != 0u && config->options == NULL) ||
      config->max_parameters == 0u || config->max_columns == 0u ||
      config->max_predicates == 0u || config->max_assignments == 0u ||
      config->max_query_bytes == 0u || config->max_parameter_bytes == 0u ||
      config->max_result_rows == 0u || config->max_result_bytes == 0u ||
      !orm_u64_to_size(config->max_query_bytes, &limits->max_query_bytes) ||
      !orm_u64_to_size(config->max_parameter_bytes,
                       &limits->max_parameter_bytes)) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid ORM configuration limits");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  limits->max_parameters = config->max_parameters;
  limits->max_columns = config->max_columns;
  limits->max_predicates = config->max_predicates;
  limits->max_assignments = config->max_assignments;
  limits->max_result_rows = config->max_result_rows;
  limits->max_result_bytes = config->max_result_bytes;
  return ORM_STATUS_OK;
}

static orm_status_t orm_backend_create(const orm_config_t *config,
                                       const orm_limits *limits,
                                       orm_backend *backend,
                                       orm_error_t *error) {
  orm_status_t status = ORM_STATUS_UNSUPPORTED;
  (void)config;
  (void)limits;
  memset(backend, 0, sizeof(*backend));
#if defined(ORM_WITH_SQLITE)
  if (orm_view_equal_cstr(config->driver, "sqlite"))
    status = orm_sqlite_backend_create(config, limits, backend, error);
  else
#endif
#if defined(ORM_WITH_REDIS)
  if (orm_view_equal_cstr(config->driver, "redis"))
    status = orm_redis_backend_create(config, limits, backend, error);
  else
#endif
#if defined(ORM_WITH_TIDESDB)
  if (orm_view_equal_cstr(config->driver, "tidesdb"))
    status = orm_tidesdb_backend_create(config, limits, backend, error);
  else
#endif
#if defined(ORM_WITH_MONGO)
  if (orm_view_equal_cstr(config->driver, "mongo") ||
      orm_view_equal_cstr(config->driver, "mongodb"))
    status = orm_mongo_backend_create(config, limits, backend, error);
  else
#endif
  {
    orm_error_set(error, ORM_STATUS_UNSUPPORTED,
                  "requested ORM driver is not enabled");
    status = ORM_STATUS_UNSUPPORTED;
  }
  return status;
}

static bool orm_valid_isolation(orm_isolation_t isolation) {
  return isolation >= ORM_ISOLATION_READ_UNCOMMITTED &&
         isolation <= ORM_ISOLATION_SERIALIZABLE;
}

static bool orm_sql_has_keyword(const unsigned char *sql, size_t size,
                                const char *keyword) {
  size_t index = 0u;
  const size_t keyword_size = strlen(keyword);
  while (index < size) {
    if (sql[index] == '\'' || sql[index] == '"') {
      const unsigned char quote = sql[index++];
      while (index < size) {
        if (sql[index++] != quote)
          continue;
        if (index < size && sql[index] == quote) {
          ++index;
          continue;
        }
        break;
      }
      continue;
    }
    if (index + 1u < size && sql[index] == '-' && sql[index + 1u] == '-') {
      index += 2u;
      while (index < size && sql[index] != '\n')
        ++index;
      continue;
    }
    if (index + 1u < size && sql[index] == '/' && sql[index + 1u] == '*') {
      index += 2u;
      while (index + 1u < size &&
             !(sql[index] == '*' && sql[index + 1u] == '/'))
        ++index;
      index = index + 1u < size ? index + 2u : size;
      continue;
    }
    if ((index == 0u || !(isalnum(sql[index - 1u]) || sql[index - 1u] == '_')) &&
        index + keyword_size <= size) {
      size_t offset = 0u;
      while (offset < keyword_size &&
             tolower(sql[index + offset]) == (unsigned char)keyword[offset])
        ++offset;
      if (offset == keyword_size &&
          (index + offset == size ||
           !(isalnum(sql[index + offset]) || sql[index + offset] == '_')))
        return true;
    }
    ++index;
  }
  return false;
}

bool orm_query_returns_rows(const orm_query_plan *plan) {
  const unsigned char *cursor;
  size_t remaining;
  char keyword[8];
  size_t size = 0u;
  if (plan == NULL || plan->raw_sql == NULL)
    return false;
  cursor = (const unsigned char *)plan->raw_sql;
  remaining = tstr_len(plan->raw_sql);
  while (remaining != 0u && isspace(*cursor)) {
    ++cursor;
    --remaining;
  }
  while (remaining != 0u && size + 1u < sizeof(keyword) &&
         isalpha(*cursor)) {
    keyword[size++] = (char)tolower(*cursor);
    ++cursor;
    --remaining;
  }
  keyword[size] = '\0';
  return strcmp(keyword, "select") == 0 || strcmp(keyword, "with") == 0 ||
         strcmp(keyword, "pragma") == 0 || strcmp(keyword, "explain") == 0 ||
         orm_sql_has_keyword((const unsigned char *)plan->raw_sql,
                             tstr_len(plan->raw_sql), "returning");
}

static orm_status_t orm_query_make(orm_connection_t *connection, vstr input,
                                   orm_query_kind kind,
                                   orm_query_t **out_query,
                                   orm_error_t *error) {
  orm_query_t *query;
  orm_status_t status;
  if (out_query != NULL)
    *out_query = NULL;
  if (connection == NULL || !orm_backend_valid(&connection->backend) ||
      out_query == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid ORM query creation arguments");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  query = (orm_query_t *)calloc(1u, sizeof(*query));
  if (query == NULL) {
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY, "allocate ORM query");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  query->connection = connection;
  status = orm_plan_init(&query->plan, kind, input, &connection->limits, error);
  if (status != ORM_STATUS_OK) {
    free(query);
    return status;
  }
  *out_query = query;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

typedef struct orm_lazy_command_state {
  orm_query_t *query;
  orm_backend *database;
  orm_transaction_backend *transaction;
  char error_message[ORM_C_ERROR_MESSAGE_CAPACITY];
} orm_lazy_command_state;

static orm_command_driver_result orm_lazy_command_execute(void *context) {
  orm_lazy_command_state *state = (orm_lazy_command_state *)context;
  orm_command_driver_result output = ORM_COMMAND_DRIVER_RESULT_INIT;
  orm_error_t error;
  uint64_t affected = 0u;
  orm_status_t status;
  orm_error_init(&error);
  if (state == NULL || state->query == NULL) {
    output.status = ORM_STATUS_INVALID_STATE;
    output.message = "invalid lazy ORM command state";
    return output;
  }
  if (state->database != NULL) {
    status = state->database->ops->execute_command(
        state->database->context, &state->query->plan,
        &state->query->connection->limits, &affected, &error);
  } else {
    status = state->transaction->ops->execute_command(
        state->transaction->context, &state->query->plan,
        &state->query->connection->limits, &affected, &error);
  }
  output.status = status;
  output.affected_rows = affected;
  if (status != ORM_STATUS_OK) {
    (void)snprintf(state->error_message, sizeof(state->error_message), "%s",
                   error.message[0] != '\0' ? error.message
                                             : orm_status_message(status));
    output.message = state->error_message;
  }
  return output;
}

static void orm_lazy_command_destroy(void *context) { free(context); }

static const orm_command_driver_ops orm_lazy_command_ops = {
    sizeof(orm_command_driver_ops), ORM_COMMAND_DRIVER_OPS_ABI_VERSION,
    orm_lazy_command_execute, orm_lazy_command_destroy};

static orm_status_t orm_open_command(orm_query_t *query,
                                     orm_backend *database,
                                     orm_transaction_backend *transaction,
                                     cflow_publisher *out_publisher,
                                     orm_error_t *error) {
  orm_lazy_command_state *state;
  orm_command_driver driver;
  orm_status_t status;
  if (query == NULL || query->connection == NULL || out_publisher == NULL ||
      cflow_publisher_valid(out_publisher) ||
      (query->plan.kind == ORM_QUERY_SELECT ||
       (query->plan.kind == ORM_QUERY_RAW &&
        orm_query_returns_rows(&query->plan)))) {
    orm_error_set(error, out_publisher != NULL && cflow_publisher_valid(out_publisher)
                             ? ORM_STATUS_INVALID_STATE
                             : ORM_STATUS_INVALID_ARGUMENT,
                  "invalid ORM command Publisher open");
    return out_publisher != NULL && cflow_publisher_valid(out_publisher)
               ? ORM_STATUS_INVALID_STATE
               : ORM_STATUS_INVALID_ARGUMENT;
  }
  state = (orm_lazy_command_state *)calloc(1u, sizeof(*state));
  if (state == NULL) {
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "allocate lazy ORM command");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  state->query = query;
  state->database = database;
  state->transaction = transaction;
  driver.ops = &orm_lazy_command_ops;
  driver.context = state;
  status = orm_command_publisher_init(out_publisher, &driver, error);
  if (status != ORM_STATUS_OK)
    free(state);
  return status;
}

static orm_status_t orm_open_rows(orm_query_t *query, orm_backend *database,
                                  orm_transaction_backend *transaction,
                                  const orm_flow_config_t *config,
                                  cflow_publisher *out_publisher,
                                  orm_error_t *error) {
  orm_row_cursor cursor = {0};
  orm_cbind_publisher_config publisher_config;
  cflow_publisher timed_publisher = {0};
  uint64_t wait_timeout_ns;
  orm_status_t status;
  if (query == NULL || query->connection == NULL || config == NULL ||
      config->struct_size != sizeof(*config) ||
      config->abi_version != ORM_C_ABI_VERSION || config->row_shape == NULL ||
      out_publisher == NULL || cflow_publisher_valid(out_publisher) ||
      (query->plan.kind != ORM_QUERY_SELECT &&
       !(query->plan.kind == ORM_QUERY_RAW &&
         orm_query_returns_rows(&query->plan)))) {
    orm_error_set(error, out_publisher != NULL && cflow_publisher_valid(out_publisher)
                             ? ORM_STATUS_INVALID_STATE
                             : ORM_STATUS_INVALID_ARGUMENT,
                  "invalid ORM row Publisher open");
    return out_publisher != NULL && cflow_publisher_valid(out_publisher)
               ? ORM_STATUS_INVALID_STATE
               : ORM_STATUS_INVALID_ARGUMENT;
  }
  status = database != NULL
               ? database->ops->open_cursor(
                     database->context, &query->plan,
                     &query->connection->limits, &cursor, error)
               : transaction->ops->open_cursor(
                     transaction->context, &query->plan,
                     &query->connection->limits, &cursor, error);
  if (status != ORM_STATUS_OK)
    return status;
  if (!orm_row_cursor_valid(&cursor)) {
    orm_row_cursor_dispose(&cursor);
    orm_error_set(error, ORM_STATUS_INTERNAL_ERROR,
                  "ORM backend returned an invalid cursor");
    return ORM_STATUS_INTERNAL_ERROR;
  }
  publisher_config = (orm_cbind_publisher_config)ORM_CBIND_PUBLISHER_CONFIG_INIT(
      config->row_shape, config->scratch_bytes, config->max_depth,
      config->max_container_items, config->max_buffer_bytes);
  wait_timeout_ns = cursor.wait_timeout_ns;
  status = orm_cbind_publisher_init(out_publisher, &cursor, &publisher_config, error);
  if (status != ORM_STATUS_OK) {
    orm_row_cursor_dispose(&cursor);
    return status;
  }
  if (wait_timeout_ns != 0u &&
      !cflow_publisher_timeout(&timed_publisher, out_publisher,
                            cflow_duration_from_ns(wait_timeout_ns))) {
    cflow_publisher_destroy(out_publisher);
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "allocate ORM row WAIT timeout source");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  if (cflow_publisher_valid(&timed_publisher)) *out_publisher = timed_publisher;
  return status;
}

uint32_t ORM_C_CALL orm_c_abi_version(void) { return ORM_C_ABI_VERSION; }

const char *ORM_C_CALL orm_status_message(orm_status_t status) {
  switch (status) {
    case ORM_STATUS_OK: return "ok";
    case ORM_STATUS_INVALID_ARGUMENT: return "invalid argument";
    case ORM_STATUS_ABI_MISMATCH: return "ABI mismatch";
    case ORM_STATUS_OUT_OF_MEMORY: return "out of memory";
    case ORM_STATUS_CONNECTION_ERROR: return "connection error";
    case ORM_STATUS_SQL_ERROR: return "SQL error";
    case ORM_STATUS_TYPE_ERROR: return "type error";
    case ORM_STATUS_OUT_OF_RANGE: return "out of range";
    case ORM_STATUS_LIMIT_EXCEEDED: return "limit exceeded";
    case ORM_STATUS_INVALID_STATE: return "invalid state";
    case ORM_STATUS_NULL_VALUE: return "null value";
    case ORM_STATUS_INTERNAL_ERROR: return "internal error";
    case ORM_STATUS_BUSY: return "busy";
    case ORM_STATUS_UNSUPPORTED: return "unsupported";
    case ORM_STATUS_DATASTORE_ERROR: return "datastore error";
    case ORM_STATUS_CONSTRAINT: return "constraint violation";
    default: return "unknown ORM status";
  }
}

void ORM_C_CALL orm_error_init(orm_error_t *error) {
  if (error == NULL)
    return;
  memset(error, 0, sizeof(*error));
  error->struct_size = sizeof(*error);
}

void ORM_C_CALL orm_config(orm_config_t *config) {
  if (config == NULL)
    return;
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->abi_version = ORM_C_ABI_VERSION;
  config->max_parameters = ORM_C_DEFAULT_MAX_PARAMETERS;
  config->max_columns = ORM_C_DEFAULT_MAX_COLUMNS;
  config->max_predicates = ORM_C_DEFAULT_MAX_PREDICATES;
  config->max_assignments = ORM_C_DEFAULT_MAX_ASSIGNMENTS;
  config->max_query_bytes = ORM_C_DEFAULT_MAX_QUERY_BYTES;
  config->max_parameter_bytes = ORM_C_DEFAULT_MAX_PARAMETER_BYTES;
  config->max_result_rows = ORM_C_DEFAULT_MAX_RESULT_ROWS;
  config->max_result_bytes = ORM_C_DEFAULT_MAX_RESULT_BYTES;
}

void ORM_C_CALL orm_flow_config(orm_flow_config_t *config,
                               const cmeta_data_desc *row_shape) {
  if (config == NULL)
    return;
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->abi_version = ORM_C_ABI_VERSION;
  config->row_shape = row_shape;
  config->scratch_bytes = ORM_C_DEFAULT_FLOW_SCRATCH_BYTES;
  config->max_depth = ORM_C_DEFAULT_FLOW_MAX_DEPTH;
  config->max_container_items = ORM_C_DEFAULT_FLOW_MAX_CONTAINER_ITEMS;
  config->max_buffer_bytes = ORM_C_DEFAULT_FLOW_MAX_BUFFER_BYTES;
}

orm_status_t ORM_C_CALL orm_connect(const orm_config_t *config,
                                   orm_connection_t **out_connection,
                                   orm_error_t *error) {
  return orm_connect_with_factory_v1(config, orm_backend_create,
                                     out_connection, error);
}

orm_status_t ORM_C_CALL orm_connect_with_factory_v1(
    const orm_config_t *config, orm_backend_factory_v1 factory,
    orm_connection_t **out_connection, orm_error_t *error) {
  orm_connection_t *connection;
  orm_status_t status;
  if (out_connection != NULL)
    *out_connection = NULL;
  if (out_connection == NULL || factory == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid ORM connection factory arguments");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  connection = (orm_connection_t *)calloc(1u, sizeof(*connection));
  if (connection == NULL) {
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "allocate ORM connection");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  status = orm_limits_from_config(config, &connection->limits, error);
  if (status == ORM_STATUS_OK)
    status = factory(config, &connection->limits, &connection->backend, error);
  if (status != ORM_STATUS_OK) {
    free(connection);
    return status;
  }
  if (!orm_backend_valid(&connection->backend)) {
    if (connection->backend.ops != NULL &&
        connection->backend.ops->destroy != NULL &&
        connection->backend.context != NULL)
      connection->backend.ops->destroy(connection->backend.context);
    free(connection);
    orm_error_set(error, ORM_STATUS_INTERNAL_ERROR,
                  "ORM backend factory returned an invalid handle");
    return ORM_STATUS_INTERNAL_ERROR;
  }
  *out_connection = connection;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

void ORM_C_CALL orm_disconnect(orm_connection_t *connection) {
  if (connection == NULL)
    return;
  if (connection->backend.ops != NULL && connection->backend.context != NULL)
    connection->backend.ops->destroy(connection->backend.context);
  free(connection);
}

orm_status_t ORM_C_CALL orm_transaction_begin(
    orm_connection_t *connection, orm_isolation_t isolation,
    orm_transaction_t **out_transaction, orm_error_t *error) {
  orm_transaction_t *transaction;
  orm_status_t status;
  if (out_transaction != NULL)
    *out_transaction = NULL;
  if (connection == NULL || !orm_backend_valid(&connection->backend) ||
      out_transaction == NULL || !orm_valid_isolation(isolation)) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid ORM transaction arguments");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  transaction = (orm_transaction_t *)calloc(1u, sizeof(*transaction));
  if (transaction == NULL) {
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "allocate ORM transaction");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  status = connection->backend.ops->begin_transaction(
      connection->backend.context, isolation, &transaction->backend, error);
  if (status != ORM_STATUS_OK) {
    free(transaction);
    return status;
  }
  if (!orm_transaction_backend_valid(&transaction->backend)) {
    if (transaction->backend.ops != NULL &&
        transaction->backend.ops->destroy != NULL &&
        transaction->backend.context != NULL)
      transaction->backend.ops->destroy(transaction->backend.context);
    free(transaction);
    orm_error_set(error, ORM_STATUS_INTERNAL_ERROR,
                  "backend returned an invalid transaction");
    return ORM_STATUS_INTERNAL_ERROR;
  }
  transaction->connection = connection;
  transaction->state = ORM_TRANSACTION_ACTIVE;
  *out_transaction = transaction;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

static orm_status_t orm_transaction_finish(orm_transaction_t *transaction,
                                           bool commit,
                                           orm_error_t *error) {
  orm_status_t status;
  if (transaction == NULL ||
      !orm_transaction_backend_valid(&transaction->backend) ||
      transaction->state != ORM_TRANSACTION_ACTIVE) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "ORM transaction is not active");
    return ORM_STATUS_INVALID_STATE;
  }
  status = commit ? transaction->backend.ops->commit(
                        transaction->backend.context, error)
                  : transaction->backend.ops->rollback(
                        transaction->backend.context, error);
  if (status == ORM_STATUS_OK)
    transaction->state = commit ? ORM_TRANSACTION_COMMITTED
                                : ORM_TRANSACTION_ROLLED_BACK;
  return status;
}

orm_status_t ORM_C_CALL orm_transaction_commit(orm_transaction_t *transaction,
                                              orm_error_t *error) {
  return orm_transaction_finish(transaction, true, error);
}

orm_status_t ORM_C_CALL orm_transaction_rollback(
    orm_transaction_t *transaction, orm_error_t *error) {
  return orm_transaction_finish(transaction, false, error);
}

typedef orm_status_t (*orm_savepoint_fn)(void *, vstr, orm_error_t *);

static orm_status_t orm_transaction_savepoint_call(
    orm_transaction_t *transaction, vstr name, orm_savepoint_fn function,
    orm_error_t *error) {
  if (transaction == NULL || transaction->state != ORM_TRANSACTION_ACTIVE ||
      !orm_view_valid(name, false) || memchr(name.data, '\0', name.len) != NULL ||
      function == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "invalid ORM savepoint operation");
    return ORM_STATUS_INVALID_STATE;
  }
  return function(transaction->backend.context, name, error);
}

orm_status_t ORM_C_CALL orm_transaction_savepoint(
    orm_transaction_t *transaction, vstr name, orm_error_t *error) {
  return orm_transaction_savepoint_call(
      transaction, name,
      transaction != NULL ? transaction->backend.ops->savepoint : NULL, error);
}

orm_status_t ORM_C_CALL orm_transaction_rollback_to_savepoint(
    orm_transaction_t *transaction, vstr name, orm_error_t *error) {
  return orm_transaction_savepoint_call(
      transaction, name,
      transaction != NULL ? transaction->backend.ops->rollback_to_savepoint
                          : NULL,
      error);
}

orm_status_t ORM_C_CALL orm_transaction_release_savepoint(
    orm_transaction_t *transaction, vstr name, orm_error_t *error) {
  return orm_transaction_savepoint_call(
      transaction, name,
      transaction != NULL ? transaction->backend.ops->release_savepoint
                          : NULL,
      error);
}

void ORM_C_CALL orm_transaction_destroy(orm_transaction_t *transaction) {
  orm_error_t ignored;
  if (transaction == NULL)
    return;
  if (transaction->state == ORM_TRANSACTION_ACTIVE &&
      orm_transaction_backend_valid(&transaction->backend)) {
    orm_error_init(&ignored);
    (void)transaction->backend.ops->rollback(transaction->backend.context,
                                             &ignored);
  }
  if (transaction->backend.ops != NULL &&
      transaction->backend.ops->destroy != NULL &&
      transaction->backend.context != NULL)
    transaction->backend.ops->destroy(transaction->backend.context);
  free(transaction);
}

orm_status_t ORM_C_CALL orm_query_create(orm_connection_t *connection,
                                        vstr table, orm_query_t **out_query,
                                        orm_error_t *error) {
  return orm_query_make(connection, table, ORM_QUERY_SELECT, out_query, error);
}

orm_status_t ORM_C_CALL orm_insert(orm_connection_t *connection, vstr table,
                                  orm_query_t **out_query,
                                  orm_error_t *error) {
  return orm_query_make(connection, table, ORM_QUERY_INSERT, out_query, error);
}

orm_status_t ORM_C_CALL orm_update(orm_connection_t *connection, vstr table,
                                  orm_query_t **out_query,
                                  orm_error_t *error) {
  return orm_query_make(connection, table, ORM_QUERY_UPDATE, out_query, error);
}

orm_status_t ORM_C_CALL orm_delete(orm_connection_t *connection, vstr table,
                                  orm_query_t **out_query,
                                  orm_error_t *error) {
  return orm_query_make(connection, table, ORM_QUERY_DELETE, out_query, error);
}

orm_status_t ORM_C_CALL orm_raw(orm_connection_t *connection, vstr sql,
                               orm_query_t **out_query, orm_error_t *error) {
  return orm_query_make(connection, sql, ORM_QUERY_RAW, out_query, error);
}

void ORM_C_CALL orm_query_destroy(orm_query_t *query) {
  if (query == NULL)
    return;
  orm_plan_destroy(&query->plan);
  free(query);
}

orm_status_t ORM_C_CALL orm_query_select_all(orm_query_t *query,
                                            orm_error_t *error) {
  return orm_plan_select_all(query != NULL ? &query->plan : NULL, error);
}

orm_status_t ORM_C_CALL orm_query_add_column(orm_query_t *query, vstr column,
                                            orm_error_t *error) {
  if (query == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  return orm_plan_add_column(&query->plan, column, &query->connection->limits,
                             error);
}

orm_status_t ORM_C_CALL orm_query_set(orm_query_t *query, vstr column,
                                     orm_value_t value, orm_error_t *error) {
  if (query == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  return orm_plan_add_assignment(&query->plan, column, value,
                                 &query->connection->limits, error);
}

orm_status_t ORM_C_CALL orm_query_where(orm_query_t *query, vstr column,
                                       orm_compare_t comparison,
                                       orm_value_t value,
                                       orm_error_t *error) {
  if (query == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  return orm_plan_add_predicate(&query->plan, column, comparison, value,
                                &query->connection->limits, error);
}

orm_status_t ORM_C_CALL orm_query_where_key(orm_query_t *query,
                                            const orm_key_part_t *parts,
                                            uint32_t part_count,
                                            orm_error_t *error) {
  if (query == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  return orm_plan_add_key(&query->plan, parts, part_count,
                          &query->connection->limits, error);
}

orm_status_t ORM_C_CALL orm_query_bind(orm_query_t *query, orm_value_t value,
                                      orm_error_t *error) {
  if (query == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  return orm_plan_add_bind(&query->plan, value, &query->connection->limits,
                           error);
}

orm_status_t ORM_C_CALL orm_query_order_by(orm_query_t *query, vstr column,
                                          orm_order_t order,
                                          orm_error_t *error) {
  if (query == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  return orm_plan_set_order(&query->plan, column, order,
                            &query->connection->limits, error);
}

orm_status_t ORM_C_CALL orm_query_set_limit(orm_query_t *query, uint64_t limit,
                                           orm_error_t *error) {
  if (query == NULL || query->plan.kind != ORM_QUERY_SELECT) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "limit requires a SELECT query");
    return ORM_STATUS_INVALID_STATE;
  }
  if (limit > query->connection->limits.max_result_rows) {
    orm_error_set(error, ORM_STATUS_LIMIT_EXCEEDED,
                  "query limit exceeds max_result_rows");
    return ORM_STATUS_LIMIT_EXCEEDED;
  }
  query->plan.limit = limit;
  query->plan.has_limit = true;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

orm_status_t ORM_C_CALL orm_query_set_offset(orm_query_t *query,
                                            uint64_t offset,
                                            orm_error_t *error) {
  if (query == NULL || query->plan.kind != ORM_QUERY_SELECT) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "offset requires a SELECT query");
    return ORM_STATUS_INVALID_STATE;
  }
  query->plan.offset = offset;
  query->plan.has_offset = true;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

orm_status_t ORM_C_CALL orm_query_open_flow(
    orm_query_t *query, const orm_flow_config_t *config,
    cflow_publisher *out_publisher, orm_error_t *error) {
  return orm_open_rows(query,
                       query != NULL ? &query->connection->backend : NULL,
                       NULL, config, out_publisher, error);
}

orm_status_t ORM_C_CALL orm_query_open_flow_in_transaction(
    orm_query_t *query, orm_transaction_t *transaction,
    const orm_flow_config_t *config, cflow_publisher *out_publisher,
    orm_error_t *error) {
  if (query == NULL || transaction == NULL ||
      transaction->state != ORM_TRANSACTION_ACTIVE ||
      query->connection != transaction->connection) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "query and transaction do not share an active connection");
    return ORM_STATUS_INVALID_STATE;
  }
  return orm_open_rows(query, NULL, &transaction->backend, config, out_publisher,
                       error);
}

orm_status_t ORM_C_CALL orm_query_open_command_flow(
    orm_query_t *query, cflow_publisher *out_publisher, orm_error_t *error) {
  return orm_open_command(query,
                          query != NULL ? &query->connection->backend : NULL,
                          NULL, out_publisher, error);
}

orm_status_t ORM_C_CALL orm_query_open_command_flow_in_transaction(
    orm_query_t *query, orm_transaction_t *transaction,
    cflow_publisher *out_publisher, orm_error_t *error) {
  if (query == NULL || transaction == NULL ||
      transaction->state != ORM_TRANSACTION_ACTIVE ||
      query->connection != transaction->connection) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "query and transaction do not share an active connection");
    return ORM_STATUS_INVALID_STATE;
  }
  return orm_open_command(query, NULL, &transaction->backend, out_publisher,
                          error);
}

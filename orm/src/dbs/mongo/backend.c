#include "orm_internal.h"
#include "orm_mongo_cursor.h"
#include "orm_mongo_lib.h"
#include "query.h"

#include <mongoc/mongoc.h>
#include <turbo/thread.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  ORM_MONGO_URI_MAX_BYTES = 4096u,
  ORM_MONGO_IDENTIFIER_MAX_BYTES = 63u,
  ORM_MONGO_ILLEGAL_OPERATION_CODE = 20
};

static const char orm_mongo_default_uri[] =
    "mongodb://127.0.0.1:27017/?serverSelectionTimeoutMS=5000";
static const char orm_mongo_default_id_column[] = "id";
static const char orm_mongo_transaction_probe[] = "__orm_transaction_probe";

typedef struct orm_mongo_backend_state {
  mongoc_client_t *client;
  mongoc_database_t *database;
  orm_mongo_settings settings;
  int transaction_active;
} orm_mongo_backend_state;

typedef struct orm_mongo_transaction_state {
  orm_mongo_backend_state *owner;
  mongoc_client_session_t *session;
  int active;
} orm_mongo_transaction_state;

static turbo_once_t orm_mongo_once = TURBO_ONCE_INIT;
static int orm_mongo_cleanup_registered;

static void orm_mongo_process_init(void) {
  mongoc_init();
  orm_mongo_cleanup_registered = atexit(mongoc_cleanup) == 0;
}

static orm_status_t orm_mongo_error_status(const bson_error_t *error,
                                           orm_status_t fallback) {
  if (error == NULL)
    return fallback;
  if (error->domain == MONGOC_ERROR_SERVER_SELECTION ||
      error->domain == MONGOC_ERROR_STREAM ||
      error->domain == MONGOC_ERROR_CLIENT_AUTHENTICATE)
    return ORM_STATUS_CONNECTION_ERROR;
  if (error->domain == MONGOC_ERROR_SERVER &&
      error->code == ORM_MONGO_ILLEGAL_OPERATION_CODE)
    return ORM_STATUS_UNSUPPORTED;
  if (error->domain == MONGOC_ERROR_PROTOCOL ||
      error->domain == MONGOC_ERROR_BSON ||
      error->domain == MONGOC_ERROR_SERVER)
    return ORM_STATUS_DATASTORE_ERROR;
  return fallback;
}

static orm_status_t orm_mongo_fail(orm_error_t *out,
                                   const bson_error_t *native,
                                   orm_status_t fallback,
                                   const char *operation) {
  const orm_status_t status = orm_mongo_error_status(native, fallback);
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message), "%s: %s", operation,
                 native != NULL && native->message[0] != '\0'
                     ? native->message
                     : orm_status_message(status));
  orm_error_set(out, status, message);
  return status;
}

static int orm_mongo_identifier(vstr value) {
  size_t index;
  if (!orm_view_valid(value, false) ||
      value.len > ORM_MONGO_IDENTIFIER_MAX_BYTES)
    return 0;
  for (index = 0u; index < value.len; ++index) {
    const unsigned char next = (unsigned char)value.data[index];
    const int alpha = (next >= (unsigned char)'a' &&
                       next <= (unsigned char)'z') ||
                      (next >= (unsigned char)'A' &&
                       next <= (unsigned char)'Z') ||
                      next == (unsigned char)'_';
    if ((index == 0u && !alpha) ||
        (index != 0u && !alpha &&
         !(next >= (unsigned char)'0' && next <= (unsigned char)'9')))
      return 0;
  }
  return 1;
}

static void orm_mongo_settings_destroy(orm_mongo_settings *settings) {
  if (settings == NULL)
    return;
  tstr_freep(&settings->uri);
  tstr_freep(&settings->database);
  tstr_freep(&settings->id_column);
}

static orm_status_t orm_mongo_settings_parse(
    const orm_config_t *config, orm_mongo_settings *settings,
    orm_error_t *error) {
  uint32_t index;
  int has_database = 0;
  memset(settings, 0, sizeof(*settings));
  settings->uri = tstr_dup(orm_mongo_default_uri);
  settings->id_column = tstr_dup(orm_mongo_default_id_column);
  if (settings->uri == NULL || settings->id_column == NULL) {
    orm_mongo_settings_destroy(settings);
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "initialize MongoDB connection settings");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  for (index = 0u; index < config->option_count; ++index) {
    const orm_option_t *option = &config->options[index];
    uint32_t previous;
    for (previous = 0u; previous < index; ++previous) {
      const vstr prior = config->options[previous].keyword;
      if (option->keyword.len == prior.len &&
          (prior.len == 0u ||
           memcmp(option->keyword.data, prior.data, prior.len) == 0)) {
        orm_mongo_settings_destroy(settings);
        orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                      "duplicate MongoDB connection option");
        return ORM_STATUS_INVALID_ARGUMENT;
      }
    }
    if (orm_view_equal_cstr(option->keyword, "uri")) {
      if (!orm_view_valid(option->value, false) ||
          option->value.len > ORM_MONGO_URI_MAX_BYTES ||
          memchr(option->value.data, '\0', option->value.len) != NULL) {
        orm_mongo_settings_destroy(settings);
        orm_error_set(error,
                      option->value.len > ORM_MONGO_URI_MAX_BYTES
                          ? ORM_STATUS_LIMIT_EXCEEDED
                          : ORM_STATUS_INVALID_ARGUMENT,
                      "invalid MongoDB uri option");
        return option->value.len > ORM_MONGO_URI_MAX_BYTES
                   ? ORM_STATUS_LIMIT_EXCEEDED
                   : ORM_STATUS_INVALID_ARGUMENT;
      }
      tstr_freep(&settings->uri);
      settings->uri = tstr_from_v(option->value);
    } else if (orm_view_equal_cstr(option->keyword, "database")) {
      if (!orm_mongo_identifier(option->value)) {
        orm_mongo_settings_destroy(settings);
        orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                      "invalid MongoDB database option");
        return ORM_STATUS_INVALID_ARGUMENT;
      }
      tstr_freep(&settings->database);
      settings->database = tstr_from_v(option->value);
      has_database = 1;
    } else if (orm_view_equal_cstr(option->keyword, "id_column")) {
      if (!orm_mongo_identifier(option->value)) {
        orm_mongo_settings_destroy(settings);
        orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                      "invalid MongoDB id_column option");
        return ORM_STATUS_INVALID_ARGUMENT;
      }
      tstr_freep(&settings->id_column);
      settings->id_column = tstr_from_v(option->value);
    } else {
      orm_mongo_settings_destroy(settings);
      orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                    "unknown MongoDB connection option");
      return ORM_STATUS_INVALID_ARGUMENT;
    }
    if (settings->uri == NULL ||
        (settings->database == NULL && has_database) ||
        settings->id_column == NULL) {
      orm_mongo_settings_destroy(settings);
      orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                    "copy MongoDB connection option");
      return ORM_STATUS_OUT_OF_MEMORY;
    }
  }
  if (!has_database) {
    orm_mongo_settings_destroy(settings);
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "MongoDB database option is required");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  return ORM_STATUS_OK;
}

static mongoc_collection_t *orm_mongo_collection(
    const orm_mongo_backend_state *state, tstr table,
    orm_error_t *error) {
  mongoc_collection_t *collection = mongoc_client_get_collection(
      state->client, state->settings.database, table);
  if (collection == NULL)
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "create MongoDB collection handle failed");
  return collection;
}

static orm_status_t orm_mongo_append_session(
    bson_t *options, mongoc_client_session_t *session,
    orm_error_t *error) {
  bson_error_t native = {0};
  if (session == NULL)
    return ORM_STATUS_OK;
  if (!mongoc_client_session_append(session, options, &native))
    return orm_mongo_fail(error, &native, ORM_STATUS_INTERNAL_ERROR,
                          "attach MongoDB session");
  return ORM_STATUS_OK;
}

static uint64_t orm_mongo_reply_count(const bson_t *reply,
                                      const char *key,
                                      orm_error_t *error,
                                      orm_status_t *status) {
  bson_iter_t iterator;
  *status = ORM_STATUS_OK;
  if (reply == NULL || !bson_iter_init_find(&iterator, reply, key))
    return 0u;
  switch (bson_iter_type(&iterator)) {
    case BSON_TYPE_INT32: {
      const int32_t value = bson_iter_int32(&iterator);
      if (value >= 0)
        return (uint64_t)value;
      break;
    }
    case BSON_TYPE_INT64: {
      const int64_t value = bson_iter_int64(&iterator);
      if (value >= 0)
        return (uint64_t)value;
      break;
    }
    case BSON_TYPE_DOUBLE: {
      const double value = bson_iter_double(&iterator);
      if (value >= 0.0 && value <= (double)UINT64_MAX)
        return (uint64_t)value;
      break;
    }
    default:
      return 0u;
  }
  *status = ORM_STATUS_DATASTORE_ERROR;
  orm_error_set(error, *status,
                "MongoDB reply count is negative or out of range");
  return 0u;
}

static const orm_predicate *orm_mongo_require_id(
    const orm_query_plan *plan, const orm_mongo_settings *settings,
    orm_error_t *error) {
  const orm_predicate *id = NULL;
  size_t index;
  for (index = 0u; index < vec_size(&plan->predicates); ++index) {
    const orm_predicate *predicate =
        (const orm_predicate *)vec_at_const(&plan->predicates, index);
    if (predicate->comparison != ORM_COMPARE_EQUAL ||
        predicate->value.kind == ORM_VALUE_NULL) {
      orm_error_set(error, ORM_STATUS_UNSUPPORTED,
                    "MongoDB UPDATE/DELETE requires non-null equality predicates");
      return NULL;
    }
    if (tstr_cmp(predicate->column, settings->id_column) == 0) {
      if (id != NULL) {
        orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                      "MongoDB UPDATE/DELETE has duplicate id predicates");
        return NULL;
      }
      id = predicate;
    }
  }
  if (id == NULL)
    orm_error_set(error, ORM_STATUS_UNSUPPORTED,
                  "MongoDB UPDATE/DELETE requires an id equality predicate");
  return id;
}

static orm_status_t orm_mongo_open_select(
    orm_mongo_backend_state *state, const orm_query_plan *plan,
    const orm_limits *limits, orm_row_cursor *out_cursor,
    orm_error_t *error) {
  mongoc_collection_t *collection = NULL;
  mongoc_cursor_t *native_cursor = NULL;
  bson_t filter = BSON_INITIALIZER;
  bson_t options = BSON_INITIALIZER;
  orm_mongo_field *fields = NULL;
  orm_mongo_driver driver = {0};
  orm_mongo_cursor_config cursor_config;
  orm_status_t status;
  size_t index;
  void *owned_cursor;
  if (plan->kind != ORM_QUERY_SELECT) {
    orm_error_set(error, ORM_STATUS_UNSUPPORTED,
                  "MongoDB row Source requires SELECT");
    return ORM_STATUS_UNSUPPORTED;
  }
  collection = orm_mongo_collection(state, plan->table, error);
  if (collection == NULL)
    return ORM_STATUS_OUT_OF_MEMORY;
  status = orm_mongo_append_filter(&filter, plan, &state->settings, limits,
                                   error);
  if (status == ORM_STATUS_OK)
    status = orm_mongo_append_find_options(&options, plan, &state->settings,
                                           error);
  if (status != ORM_STATUS_OK)
    goto cleanup;
  native_cursor = mongoc_collection_find_with_opts(collection, &filter,
                                                    &options, NULL);
  if (native_cursor == NULL) {
    status = ORM_STATUS_INTERNAL_ERROR;
    orm_error_set(error, status, "MongoDB find returned a null cursor");
    goto cleanup;
  }
  fields = (orm_mongo_field *)calloc(vec_size(&plan->columns),
                                     sizeof(*fields));
  if (fields == NULL) {
    status = ORM_STATUS_OUT_OF_MEMORY;
    orm_error_set(error, status, "allocate MongoDB result field mapping");
    goto cleanup;
  }
  for (index = 0u; index < vec_size(&plan->columns); ++index) {
    const tstr column = *(const tstr *)vec_at_const(&plan->columns, index);
    const char *source =
        tstr_cmp(column, state->settings.id_column) == 0 ? "_id" : column;
    fields[index].output_name.data = (const unsigned char *)column;
    fields[index].output_name.size = tstr_len(column);
    fields[index].source_path.data = (const unsigned char *)source;
    fields[index].source_path.size = strlen(source);
  }
  owned_cursor = native_cursor;
  status = orm_mongo_driver_from_cursor(&driver, &owned_cursor, error);
  native_cursor = (mongoc_cursor_t *)owned_cursor;
  if (status != ORM_STATUS_OK)
    goto cleanup;
  cursor_config = (orm_mongo_cursor_config)ORM_MONGO_CURSOR_CONFIG_INIT(
      (size_t)limits->max_result_rows, (size_t)limits->max_result_bytes);
  status = orm_mongo_cursor_start(out_cursor, &driver, fields,
                                  vec_size(&plan->columns), &cursor_config,
                                  error);
  if (status != ORM_STATUS_OK && driver.ops != NULL)
    driver.ops->destroy(driver.context);

cleanup:
  free(fields);
  if (native_cursor != NULL)
    mongoc_cursor_destroy(native_cursor);
  mongoc_collection_destroy(collection);
  bson_destroy(&options);
  bson_destroy(&filter);
  return status;
}

static orm_status_t orm_mongo_execute(
    orm_mongo_backend_state *state, const orm_query_plan *plan,
    const orm_limits *limits, mongoc_client_session_t *session,
    uint64_t *affected_rows, orm_error_t *error) {
  mongoc_collection_t *collection = NULL;
  bson_t first = BSON_INITIALIZER;
  bson_t second = BSON_INITIALIZER;
  bson_t options = BSON_INITIALIZER;
  bson_t reply = BSON_INITIALIZER;
  bson_error_t native = {0};
  orm_status_t status = ORM_STATUS_OK;
  uint64_t affected = 0u;
  if (plan->kind == ORM_QUERY_SELECT) {
    orm_error_set(error, ORM_STATUS_UNSUPPORTED,
                  "MongoDB SELECT must be opened as a row Source");
    return ORM_STATUS_UNSUPPORTED;
  }
  if (plan->kind == ORM_QUERY_RAW) {
    orm_error_set(error, ORM_STATUS_UNSUPPORTED,
                  "MongoDB does not execute raw SQL");
    return ORM_STATUS_UNSUPPORTED;
  }
  collection = orm_mongo_collection(state, plan->table, error);
  if (collection == NULL)
    return ORM_STATUS_OUT_OF_MEMORY;
  status = orm_mongo_append_session(&options, session, error);
  if (status != ORM_STATUS_OK)
    goto cleanup;
  if (plan->kind == ORM_QUERY_INSERT) {
    status = orm_mongo_append_insert_document(&first, plan,
                                              &state->settings, error);
    if (status == ORM_STATUS_OK &&
        !mongoc_collection_insert_one(collection, &first, &options, &reply,
                                      &native))
      status = orm_mongo_fail(error, &native, ORM_STATUS_DATASTORE_ERROR,
                              "insert MongoDB document");
    if (status == ORM_STATUS_OK)
      affected = 1u;
  } else if (plan->kind == ORM_QUERY_UPDATE ||
             plan->kind == ORM_QUERY_DELETE) {
    if (orm_mongo_require_id(plan, &state->settings, error) == NULL) {
      status = error != NULL ? error->status : ORM_STATUS_UNSUPPORTED;
      goto cleanup;
    }
    status = orm_mongo_append_filter(&first, plan, &state->settings, limits,
                                     error);
    if (status != ORM_STATUS_OK)
      goto cleanup;
    if (plan->kind == ORM_QUERY_UPDATE) {
      status = orm_mongo_append_update_document(&second, plan,
                                                &state->settings, error);
      if (status == ORM_STATUS_OK &&
          !mongoc_collection_update_one(collection, &first, &second,
                                        &options, &reply, &native))
        status = orm_mongo_fail(error, &native, ORM_STATUS_DATASTORE_ERROR,
                                "update MongoDB document");
      if (status == ORM_STATUS_OK)
        affected = orm_mongo_reply_count(&reply, "matchedCount", error,
                                         &status);
    } else {
      if (!mongoc_collection_delete_one(collection, &first, &options,
                                        &reply, &native))
        status = orm_mongo_fail(error, &native, ORM_STATUS_DATASTORE_ERROR,
                                "delete MongoDB document");
      if (status == ORM_STATUS_OK)
        affected = orm_mongo_reply_count(&reply, "deletedCount", error,
                                         &status);
    }
  } else {
    status = ORM_STATUS_INTERNAL_ERROR;
    orm_error_set(error, status, "unknown MongoDB query kind");
  }
  if (status == ORM_STATUS_OK && affected_rows != NULL)
    *affected_rows = affected;

cleanup:
  bson_destroy(&reply);
  bson_destroy(&options);
  bson_destroy(&second);
  bson_destroy(&first);
  mongoc_collection_destroy(collection);
  return status;
}

static void orm_mongo_backend_destroy(void *context) {
  orm_mongo_backend_state *state = (orm_mongo_backend_state *)context;
  if (state == NULL)
    return;
  mongoc_database_destroy(state->database);
  mongoc_client_destroy(state->client);
  orm_mongo_settings_destroy(&state->settings);
  free(state);
}

static orm_status_t orm_mongo_backend_open(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    orm_row_cursor *out_cursor, orm_error_t *error) {
  orm_mongo_backend_state *state = (orm_mongo_backend_state *)context;
  if (state->transaction_active) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "use the MongoDB transaction handle while active");
    return ORM_STATUS_INVALID_STATE;
  }
  return orm_mongo_open_select(state, plan, limits, out_cursor, error);
}

static orm_status_t orm_mongo_backend_execute(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    uint64_t *affected_rows, orm_error_t *error) {
  orm_mongo_backend_state *state = (orm_mongo_backend_state *)context;
  if (state->transaction_active) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "use the MongoDB transaction handle while active");
    return ORM_STATUS_INVALID_STATE;
  }
  return orm_mongo_execute(state, plan, limits, NULL, affected_rows, error);
}

static void orm_mongo_transaction_destroy(void *context) {
  orm_mongo_transaction_state *transaction =
      (orm_mongo_transaction_state *)context;
  if (transaction == NULL)
    return;
  if (transaction->active) {
    bson_error_t ignored = {0};
    (void)mongoc_client_session_abort_transaction(transaction->session,
                                                  &ignored);
    transaction->owner->transaction_active = 0;
  }
  mongoc_client_session_destroy(transaction->session);
  free(transaction);
}

static orm_status_t orm_mongo_transaction_open(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    orm_row_cursor *out_cursor, orm_error_t *error) {
  (void)context;
  (void)plan;
  (void)limits;
  (void)out_cursor;
  orm_error_set(error, ORM_STATUS_UNSUPPORTED,
                "MongoDB row Sources inside transactions are unsupported");
  return ORM_STATUS_UNSUPPORTED;
}

static orm_status_t orm_mongo_transaction_execute(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    uint64_t *affected_rows, orm_error_t *error) {
  orm_mongo_transaction_state *transaction =
      (orm_mongo_transaction_state *)context;
  if (transaction == NULL || !transaction->active) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "MongoDB transaction is no longer active");
    return ORM_STATUS_INVALID_STATE;
  }
  return orm_mongo_execute(transaction->owner, plan, limits,
                           transaction->session, affected_rows, error);
}

static orm_status_t orm_mongo_transaction_commit(void *context,
                                                 orm_error_t *error) {
  orm_mongo_transaction_state *transaction =
      (orm_mongo_transaction_state *)context;
  bson_t reply = BSON_INITIALIZER;
  bson_error_t native = {0};
  if (transaction == NULL || !transaction->active) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "MongoDB transaction is no longer active");
    return ORM_STATUS_INVALID_STATE;
  }
  if (!mongoc_client_session_commit_transaction(transaction->session, &reply,
                                                &native)) {
    bson_destroy(&reply);
    return orm_mongo_fail(error, &native, ORM_STATUS_DATASTORE_ERROR,
                          "commit MongoDB transaction");
  }
  bson_destroy(&reply);
  transaction->active = 0;
  transaction->owner->transaction_active = 0;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

static orm_status_t orm_mongo_transaction_rollback(void *context,
                                                   orm_error_t *error) {
  orm_mongo_transaction_state *transaction =
      (orm_mongo_transaction_state *)context;
  bson_error_t native = {0};
  if (transaction == NULL || !transaction->active) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "MongoDB transaction is no longer active");
    return ORM_STATUS_INVALID_STATE;
  }
  if (!mongoc_client_session_abort_transaction(transaction->session,
                                               &native))
    return orm_mongo_fail(error, &native, ORM_STATUS_DATASTORE_ERROR,
                          "abort MongoDB transaction");
  transaction->active = 0;
  transaction->owner->transaction_active = 0;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

static orm_status_t orm_mongo_transaction_savepoint(void *context, vstr name,
                                                    orm_error_t *error) {
  (void)context;
  (void)name;
  orm_error_set(error, ORM_STATUS_UNSUPPORTED,
                "MongoDB transactions do not support savepoints");
  return ORM_STATUS_UNSUPPORTED;
}

static const orm_transaction_backend_ops orm_mongo_transaction_ops = {
    sizeof(orm_transaction_backend_ops),
    ORM_TRANSACTION_BACKEND_OPS_ABI_VERSION,
    orm_mongo_transaction_destroy,
    orm_mongo_transaction_open,
    orm_mongo_transaction_execute,
    orm_mongo_transaction_commit,
    orm_mongo_transaction_rollback,
    orm_mongo_transaction_savepoint,
    orm_mongo_transaction_savepoint,
    orm_mongo_transaction_savepoint};

static orm_status_t orm_mongo_probe_transaction(
    orm_mongo_transaction_state *transaction, orm_error_t *error) {
  mongoc_collection_t *collection = NULL;
  mongoc_cursor_t *cursor = NULL;
  bson_t filter = BSON_INITIALIZER;
  bson_t options = BSON_INITIALIZER;
  bson_error_t native = {0};
  const bson_t *document = NULL;
  orm_status_t status;
  collection = mongoc_client_get_collection(
      transaction->owner->client, transaction->owner->settings.database,
      orm_mongo_transaction_probe);
  if (collection == NULL) {
    status = ORM_STATUS_OUT_OF_MEMORY;
    orm_error_set(error, status,
                  "create MongoDB transaction probe collection");
    goto cleanup;
  }
  if (!bson_append_int64(&options, "limit", -1, 1)) {
    status = ORM_STATUS_INTERNAL_ERROR;
    orm_error_set(error, status, "build MongoDB transaction probe");
    goto cleanup;
  }
  status = orm_mongo_append_session(&options, transaction->session, error);
  if (status != ORM_STATUS_OK)
    goto cleanup;
  cursor = mongoc_collection_find_with_opts(collection, &filter, &options,
                                            NULL);
  if (cursor == NULL) {
    status = ORM_STATUS_INTERNAL_ERROR;
    orm_error_set(error, status,
                  "MongoDB transaction probe returned a null cursor");
    goto cleanup;
  }
  (void)mongoc_cursor_next(cursor, &document);
  if (mongoc_cursor_error(cursor, &native))
    status = orm_mongo_fail(error, &native, ORM_STATUS_DATASTORE_ERROR,
                            "probe MongoDB transaction support");
  else
    status = ORM_STATUS_OK;

cleanup:
  mongoc_cursor_destroy(cursor);
  mongoc_collection_destroy(collection);
  bson_destroy(&options);
  bson_destroy(&filter);
  return status;
}

static orm_status_t orm_mongo_backend_begin(
    void *context, orm_isolation_t isolation,
    orm_transaction_backend *out_transaction, orm_error_t *error) {
  orm_mongo_backend_state *state = (orm_mongo_backend_state *)context;
  orm_mongo_transaction_state *transaction = NULL;
  mongoc_transaction_opt_t *options = NULL;
  mongoc_read_concern_t *read_concern = NULL;
  mongoc_write_concern_t *write_concern = NULL;
  bson_error_t native = {0};
  orm_status_t status = ORM_STATUS_OK;
  if (state == NULL || out_transaction == NULL ||
      isolation < ORM_ISOLATION_READ_UNCOMMITTED ||
      isolation > ORM_ISOLATION_SERIALIZABLE) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid MongoDB transaction request");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  memset(out_transaction, 0, sizeof(*out_transaction));
  if (state->transaction_active) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "MongoDB connection already has an active transaction");
    return ORM_STATUS_INVALID_STATE;
  }
  transaction = (orm_mongo_transaction_state *)calloc(1u,
                                                       sizeof(*transaction));
  options = mongoc_transaction_opts_new();
  if (transaction == NULL || options == NULL) {
    status = ORM_STATUS_OUT_OF_MEMORY;
    orm_error_set(error, status, "allocate MongoDB transaction");
    goto cleanup;
  }
  transaction->owner = state;
  transaction->session = mongoc_client_start_session(state->client, NULL,
                                                     &native);
  if (transaction->session == NULL) {
    status = orm_mongo_fail(error, &native, ORM_STATUS_CONNECTION_ERROR,
                            "start MongoDB session");
    goto cleanup;
  }
  if (isolation == ORM_ISOLATION_REPEATABLE_READ ||
      isolation == ORM_ISOLATION_SNAPSHOT ||
      isolation == ORM_ISOLATION_SERIALIZABLE) {
    read_concern = mongoc_read_concern_new();
    if (read_concern == NULL) {
      status = ORM_STATUS_OUT_OF_MEMORY;
      orm_error_set(error, status, "allocate MongoDB read concern");
      goto cleanup;
    }
    mongoc_read_concern_set_level(read_concern, "snapshot");
    mongoc_transaction_opts_set_read_concern(options, read_concern);
  }
  if (isolation == ORM_ISOLATION_SERIALIZABLE) {
    write_concern = mongoc_write_concern_new();
    if (write_concern == NULL) {
      status = ORM_STATUS_OUT_OF_MEMORY;
      orm_error_set(error, status, "allocate MongoDB write concern");
      goto cleanup;
    }
    mongoc_write_concern_set_wmajority(write_concern, 0);
    mongoc_transaction_opts_set_write_concern(options, write_concern);
  }
  if (!mongoc_client_session_start_transaction(transaction->session, options,
                                               &native)) {
    status = orm_mongo_fail(error, &native, ORM_STATUS_UNSUPPORTED,
                            "start MongoDB transaction");
    goto cleanup;
  }
  transaction->active = 1;
  status = orm_mongo_probe_transaction(transaction, error);
  if (status != ORM_STATUS_OK)
    goto cleanup;
  state->transaction_active = 1;
  out_transaction->ops = &orm_mongo_transaction_ops;
  out_transaction->context = transaction;
  transaction = NULL;
  orm_error_set(error, ORM_STATUS_OK, NULL);

cleanup:
  mongoc_write_concern_destroy(write_concern);
  mongoc_read_concern_destroy(read_concern);
  mongoc_transaction_opts_destroy(options);
  if (transaction != NULL)
    orm_mongo_transaction_destroy(transaction);
  return status;
}

static const orm_backend_ops orm_mongo_backend_ops = {
    sizeof(orm_backend_ops), ORM_BACKEND_OPS_ABI_VERSION,
    orm_mongo_backend_destroy, orm_mongo_backend_open,
    orm_mongo_backend_execute, orm_mongo_backend_begin};

orm_status_t orm_mongo_backend_create(const orm_config_t *config,
                                      const orm_limits *limits,
                                      orm_backend *out_backend,
                                      orm_error_t *error) {
  orm_mongo_backend_state *state;
  bson_t ping = BSON_INITIALIZER;
  bson_error_t native = {0};
  orm_status_t status;
  (void)limits;
  if (config == NULL || out_backend == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid MongoDB backend request");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  memset(out_backend, 0, sizeof(*out_backend));
  turbo_once(&orm_mongo_once, orm_mongo_process_init);
  if (!orm_mongo_cleanup_registered) {
    orm_error_set(error, ORM_STATUS_INTERNAL_ERROR,
                  "register MongoDB process cleanup failed");
    return ORM_STATUS_INTERNAL_ERROR;
  }
  state = (orm_mongo_backend_state *)calloc(1u, sizeof(*state));
  if (state == NULL) {
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "allocate MongoDB backend");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  status = orm_mongo_settings_parse(config, &state->settings, error);
  if (status != ORM_STATUS_OK)
    goto fail;
  state->client = mongoc_client_new(state->settings.uri);
  if (state->client == NULL) {
    status = ORM_STATUS_INVALID_ARGUMENT;
    orm_error_set(error, status, "invalid MongoDB connection URI");
    goto fail;
  }
  mongoc_client_set_error_api(state->client, MONGOC_ERROR_API_VERSION_2);
  state->database = mongoc_client_get_database(state->client,
                                               state->settings.database);
  if (state->database == NULL) {
    status = ORM_STATUS_OUT_OF_MEMORY;
    orm_error_set(error, status, "create MongoDB database handle");
    goto fail;
  }
  if (!bson_append_int32(&ping, "ping", -1, 1) ||
      !mongoc_client_command_simple(state->client, "admin", &ping, NULL,
                                    NULL, &native)) {
    status = native.message[0] != '\0'
                 ? orm_mongo_fail(error, &native,
                                  ORM_STATUS_CONNECTION_ERROR,
                                  "ping MongoDB server")
                 : ORM_STATUS_INTERNAL_ERROR;
    if (native.message[0] == '\0')
      orm_error_set(error, status, "build MongoDB ping command failed");
    goto fail;
  }
  bson_destroy(&ping);
  out_backend->ops = &orm_mongo_backend_ops;
  out_backend->context = state;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;

fail:
  bson_destroy(&ping);
  orm_mongo_backend_destroy(state);
  return status;
}

#include "orm_internal.h"
#include "orm_tidesdb_cursor.h"
#include "bridge.h"
#include "row.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
  ORM_TIDESDB_DEFAULT_MAX_SCAN_ROWS = 100000u,
  ORM_TIDESDB_DEFAULT_MAX_SCAN_BYTES = 64u * 1024u * 1024u,
  ORM_TIDESDB_IDENTIFIER_MAX_BYTES = 63u
};

static const char orm_tidesdb_default_column_family[] = "orm";
static const char orm_tidesdb_default_id_column[] = "id";
static const char orm_tidesdb_default_key_prefix[] = "orm:";

typedef struct orm_tidesdb_settings {
  tstr path;
  tstr column_family;
  tstr id_column;
  tstr key_prefix;
  uint64_t ttl_seconds;
  uint64_t max_scan_rows;
  uint64_t max_scan_bytes;
  int create_if_missing;
} orm_tidesdb_settings;

typedef struct orm_tidesdb_backend_state orm_tidesdb_backend_state;

typedef struct orm_tidesdb_transaction_state {
  orm_tidesdb_backend_state *owner;
  orm_tidesdb_transaction_t *native;
  size_t references;
  size_t cursors;
  int active;
  int owner_released;
} orm_tidesdb_transaction_state;

struct orm_tidesdb_backend_state {
  orm_tidesdb_database_t *database;
  orm_tidesdb_column_family_t *column_family;
  orm_tidesdb_settings settings;
  orm_limits limits;
  int transaction_active;
};

typedef struct orm_tidesdb_flow_state {
  orm_tidesdb_backend_state *owner;
  const orm_query_plan *plan;
  orm_limits limits;
  orm_tidesdb_transaction_t *native;
  orm_tidesdb_transaction_state *explicit_transaction;
  orm_tidesdb_iterator_t *iterator;
  tstr prefix;
  tstr encoded;
  uint64_t scanned_rows;
  uint64_t scanned_bytes;
  uint64_t matched_rows;
  uint64_t yielded_rows;
  uint64_t yielded_bytes;
  int owns_transaction;
  int needs_advance;
  int done;
  char error_message[ORM_C_ERROR_MESSAGE_CAPACITY];
} orm_tidesdb_flow_state;

static orm_status_t orm_tidesdb_fail(orm_error_t *error,
                                     orm_status_t status,
                                     const char *message) {
  orm_error_set(error, status, message);
  return status;
}

static const char *orm_tidesdb_error_name(int code) {
  switch (code) {
    case ORM_TDB_SUCCESS: return "success";
    case ORM_TDB_ERR_MEMORY: return "memory allocation failed";
    case ORM_TDB_ERR_INVALID_ARGS: return "invalid arguments";
    case ORM_TDB_ERR_NOT_FOUND: return "not found";
    case ORM_TDB_ERR_IO: return "I/O failure";
    case ORM_TDB_ERR_CORRUPTION: return "data corruption";
    case ORM_TDB_ERR_EXISTS: return "already exists";
    case ORM_TDB_ERR_CONFLICT: return "transaction conflict";
    case ORM_TDB_ERR_TOO_LARGE: return "value too large";
    case ORM_TDB_ERR_MEMORY_LIMIT: return "memory limit exceeded";
    case ORM_TDB_ERR_INVALID_DB: return "invalid database";
    case ORM_TDB_ERR_LOCKED: return "database locked";
    case ORM_TDB_ERR_READONLY: return "database is read-only";
    case ORM_TDB_ERR_BUSY: return "database is busy";
    case ORM_TDB_ERR_PRECONDITION: return "precondition failed";
    default: return "unknown TidesDB error";
  }
}

static orm_status_t orm_tidesdb_map_status(int code,
                                           orm_status_t fallback) {
  switch (code) {
    case ORM_TDB_ERR_MEMORY: return ORM_STATUS_OUT_OF_MEMORY;
    case ORM_TDB_ERR_INVALID_ARGS: return ORM_STATUS_INVALID_ARGUMENT;
    case ORM_TDB_ERR_READONLY: return ORM_STATUS_INVALID_STATE;
    case ORM_TDB_ERR_CONFLICT:
    case ORM_TDB_ERR_LOCKED:
    case ORM_TDB_ERR_BUSY:
    case ORM_TDB_ERR_PRECONDITION: return ORM_STATUS_BUSY;
    case ORM_TDB_ERR_TOO_LARGE:
    case ORM_TDB_ERR_MEMORY_LIMIT: return ORM_STATUS_LIMIT_EXCEEDED;
    case ORM_TDB_ERR_CORRUPTION:
    case ORM_TDB_ERR_INVALID_DB: return ORM_STATUS_DATASTORE_ERROR;
    default: return fallback;
  }
}

static orm_status_t orm_tidesdb_native_error(orm_error_t *error, int code,
                                             orm_status_t fallback,
                                             const char *operation) {
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
  const orm_status_t status = orm_tidesdb_map_status(code, fallback);
  (void)snprintf(message, sizeof(message), "%s: %s (%d)", operation,
                 orm_tidesdb_error_name(code), code);
  return orm_tidesdb_fail(error, status, message);
}

static int orm_tidesdb_identifier(vstr value) {
  size_t index;
  if (!orm_view_valid(value, false) ||
      value.len > ORM_TIDESDB_IDENTIFIER_MAX_BYTES)
    return 0;
  for (index = 0u; index < value.len; ++index) {
    const unsigned char next = (unsigned char)value.data[index];
    const int start = (next >= (unsigned char)'a' &&
                       next <= (unsigned char)'z') ||
                      (next >= (unsigned char)'A' &&
                       next <= (unsigned char)'Z') ||
                      next == (unsigned char)'_';
    if ((index == 0u && !start) ||
        (index != 0u && !start &&
         !(next >= (unsigned char)'0' && next <= (unsigned char)'9')))
      return 0;
  }
  return 1;
}

static int orm_tidesdb_parse_u64(vstr input, uint64_t *out) {
  uint64_t value = 0u;
  size_t index;
  if (!orm_view_valid(input, false))
    return 0;
  for (index = 0u; index < input.len; ++index) {
    const unsigned char digit = (unsigned char)input.data[index];
    if (digit < (unsigned char)'0' || digit > (unsigned char)'9')
      return 0;
    if (value > (UINT64_MAX - (uint64_t)(digit - (unsigned char)'0')) / 10u)
      return 0;
    value = value * 10u + (uint64_t)(digit - (unsigned char)'0');
  }
  *out = value;
  return 1;
}

static int orm_tidesdb_parse_bool(vstr input, int *out) {
  if (orm_view_equal_cstr(input, "true") || orm_view_equal_cstr(input, "1")) {
    *out = 1;
    return 1;
  }
  if (orm_view_equal_cstr(input, "false") || orm_view_equal_cstr(input, "0")) {
    *out = 0;
    return 1;
  }
  return 0;
}

static void orm_tidesdb_settings_destroy(orm_tidesdb_settings *settings) {
  if (settings == NULL)
    return;
  tstr_freep(&settings->path);
  tstr_freep(&settings->column_family);
  tstr_freep(&settings->id_column);
  tstr_freep(&settings->key_prefix);
  memset(settings, 0, sizeof(*settings));
}

static orm_status_t orm_tidesdb_option_copy(tstr *destination, vstr input,
                                            size_t max_bytes,
                                            orm_error_t *error,
                                            const char *message) {
  tstr copied;
  if (!orm_view_valid(input, false) || input.len > max_bytes ||
      memchr(input.data, 0, input.len) != NULL)
    return orm_tidesdb_fail(error,
                            input.len > max_bytes ? ORM_STATUS_LIMIT_EXCEEDED
                                                  : ORM_STATUS_INVALID_ARGUMENT,
                            message);
  copied = tstr_from_v(input);
  if (copied == NULL)
    return orm_tidesdb_fail(error, ORM_STATUS_OUT_OF_MEMORY, message);
  tstr_freep(destination);
  *destination = copied;
  return ORM_STATUS_OK;
}

static orm_status_t orm_tidesdb_settings_parse(
    const orm_config_t *config, const orm_limits *limits,
    orm_tidesdb_settings *settings, orm_error_t *error) {
  uint32_t index;
  memset(settings, 0, sizeof(*settings));
  settings->column_family = tstr_dup(orm_tidesdb_default_column_family);
  settings->id_column = tstr_dup(orm_tidesdb_default_id_column);
  settings->key_prefix = tstr_dup(orm_tidesdb_default_key_prefix);
  settings->create_if_missing = 1;
  settings->max_scan_rows = ORM_TIDESDB_DEFAULT_MAX_SCAN_ROWS;
  settings->max_scan_bytes = ORM_TIDESDB_DEFAULT_MAX_SCAN_BYTES;
  if (settings->column_family == NULL || settings->id_column == NULL ||
      settings->key_prefix == NULL) {
    orm_tidesdb_settings_destroy(settings);
    return orm_tidesdb_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                            "initialize TidesDB settings");
  }
  for (index = 0u; index < config->option_count; ++index) {
    const orm_option_t *option = &config->options[index];
    uint32_t previous;
    orm_status_t status = ORM_STATUS_OK;
    for (previous = 0u; previous < index; ++previous) {
      const vstr prior = config->options[previous].keyword;
      if (option->keyword.len == prior.len &&
          (prior.len == 0u ||
           memcmp(option->keyword.data, prior.data, prior.len) == 0)) {
        orm_tidesdb_settings_destroy(settings);
        return orm_tidesdb_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                                "duplicate TidesDB connection option");
      }
    }
    if (orm_view_equal_cstr(option->keyword, "path")) {
      status = orm_tidesdb_option_copy(&settings->path, option->value,
                                       limits->max_query_bytes, error,
                                       "invalid TidesDB path option");
    } else if (orm_view_equal_cstr(option->keyword, "column_family")) {
      if (!orm_tidesdb_identifier(option->value))
        status = orm_tidesdb_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                                  "invalid TidesDB column_family option");
      else
        status = orm_tidesdb_option_copy(&settings->column_family,
                                         option->value,
                                         ORM_TIDESDB_IDENTIFIER_MAX_BYTES,
                                         error,
                                         "copy TidesDB column_family option");
    } else if (orm_view_equal_cstr(option->keyword, "id_column")) {
      if (!orm_tidesdb_identifier(option->value))
        status = orm_tidesdb_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                                  "invalid TidesDB id_column option");
      else
        status = orm_tidesdb_option_copy(&settings->id_column, option->value,
                                         ORM_TIDESDB_IDENTIFIER_MAX_BYTES,
                                         error,
                                         "copy TidesDB id_column option");
    } else if (orm_view_equal_cstr(option->keyword, "key_prefix")) {
      status = orm_tidesdb_option_copy(&settings->key_prefix, option->value,
                                       limits->max_query_bytes, error,
                                       "invalid TidesDB key_prefix option");
    } else if (orm_view_equal_cstr(option->keyword, "create_if_missing")) {
      if (!orm_tidesdb_parse_bool(option->value,
                                  &settings->create_if_missing))
        status = orm_tidesdb_fail(
            error, ORM_STATUS_INVALID_ARGUMENT,
            "TidesDB create_if_missing must be true, false, 1, or 0");
    } else if (orm_view_equal_cstr(option->keyword, "ttl_seconds")) {
      if (!orm_tidesdb_parse_u64(option->value, &settings->ttl_seconds))
        status = orm_tidesdb_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                                  "invalid TidesDB ttl_seconds option");
    } else if (orm_view_equal_cstr(option->keyword, "max_scan_rows")) {
      if (!orm_tidesdb_parse_u64(option->value, &settings->max_scan_rows) ||
          settings->max_scan_rows == 0u)
        status = orm_tidesdb_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                                  "TidesDB max_scan_rows must be positive");
    } else if (orm_view_equal_cstr(option->keyword, "max_scan_bytes")) {
      if (!orm_tidesdb_parse_u64(option->value, &settings->max_scan_bytes) ||
          settings->max_scan_bytes == 0u)
        status = orm_tidesdb_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                                  "TidesDB max_scan_bytes must be positive");
    } else {
      status = orm_tidesdb_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                                "unknown TidesDB connection option");
    }
    if (status != ORM_STATUS_OK) {
      orm_tidesdb_settings_destroy(settings);
      return status;
    }
  }
  if (settings->path == NULL) {
    orm_tidesdb_settings_destroy(settings);
    return orm_tidesdb_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                            "TidesDB path option is required");
  }
  return ORM_STATUS_OK;
}

static orm_tidesdb_isolation_level_t orm_tidesdb_native_isolation(
    orm_isolation_t isolation) {
  return (orm_tidesdb_isolation_level_t)isolation;
}

static orm_status_t orm_tidesdb_begin_native(
    orm_tidesdb_backend_state *state, orm_tidesdb_isolation_level_t isolation,
    orm_tidesdb_transaction_t **out, orm_error_t *error) {
  int native_status;
  *out = NULL;
  native_status = orm_tidesdb_txn_begin_with_isolation(state->database,
                                                       isolation, out);
  if (native_status != ORM_TDB_SUCCESS)
    return orm_tidesdb_native_error(error, native_status,
                                    ORM_STATUS_DATASTORE_ERROR,
                                    "begin TidesDB transaction");
  if (*out == NULL)
    return orm_tidesdb_fail(error, ORM_STATUS_INTERNAL_ERROR,
                            "TidesDB returned a null transaction");
  return ORM_STATUS_OK;
}

static orm_status_t orm_tidesdb_table_prefix(
    const orm_tidesdb_backend_state *state, tstr table, tstr *out,
    orm_error_t *error) {
  const size_t prefix_size = tstr_len(state->settings.key_prefix);
  const size_t table_size = tstr_len(table);
  tstr result;
  if (prefix_size >= state->limits.max_query_bytes ||
      table_size > state->limits.max_query_bytes - prefix_size - 1u)
    return orm_tidesdb_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                            "TidesDB table key prefix exceeds max_query_bytes");
  result = tstr_clone(state->settings.key_prefix);
  if (result != NULL)
    result = tstr_cat_len(result, table, table_size);
  if (result != NULL)
    result = tstr_cat_len(result, "", 1u);
  if (result == NULL)
    return orm_tidesdb_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                            "allocate TidesDB table key prefix");
  *out = result;
  return ORM_STATUS_OK;
}

static orm_status_t orm_tidesdb_entity_key(
    const orm_tidesdb_backend_state *state, tstr table,
    const orm_owned_value *id, tstr *out, orm_error_t *error) {
  orm_tidesdb_cell cell = {0};
  tstr key = NULL;
  const unsigned char kind = id != NULL ? (unsigned char)id->kind : 0u;
  orm_status_t status;
  size_t maximum;
  if (id == NULL || id->kind == ORM_VALUE_NULL)
    return orm_tidesdb_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                            "TidesDB entity id cannot be null");
  if (state->limits.max_query_bytes > SIZE_MAX - state->limits.max_parameter_bytes)
    return orm_tidesdb_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                            "TidesDB entity key limit overflows size_t");
  maximum = state->limits.max_query_bytes + state->limits.max_parameter_bytes;
  status = orm_tidesdb_table_prefix(state, table, &key, error);
  if (status != ORM_STATUS_OK)
    return status;
  status = orm_tidesdb_cell_from_value(&cell, id, error);
  if (status != ORM_STATUS_OK)
    goto fail;
  if (tstr_len(key) >= maximum || tstr_len(cell.bytes) > maximum - tstr_len(key) - 1u) {
    status = orm_tidesdb_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                              "TidesDB entity key exceeds configured bounds");
    goto fail;
  }
  key = tstr_cat_len(key, (const char *)&kind, 1u);
  if (key != NULL)
    key = tstr_cat_len(key, cell.bytes, tstr_len(cell.bytes));
  if (key == NULL) {
    status = orm_tidesdb_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                              "allocate TidesDB entity key");
    goto fail;
  }
  tstr_freep(&cell.bytes);
  *out = key;
  return ORM_STATUS_OK;

fail:
  tstr_freep(&cell.bytes);
  tstr_freep(&key);
  return status;
}

static time_t orm_tidesdb_expiration(const orm_tidesdb_backend_state *state,
                                     orm_status_t *status,
                                     orm_error_t *error) {
  const time_t now = time(NULL);
  uint64_t now_value;
  uint64_t maximum;
  *status = ORM_STATUS_OK;
  if (state->settings.ttl_seconds == 0u)
    return (time_t)0;
  if (now < (time_t)0) {
    *status = orm_tidesdb_fail(error, ORM_STATUS_DATASTORE_ERROR,
                               "read system time for TidesDB TTL failed");
    return (time_t)0;
  }
  now_value = (uint64_t)now;
  maximum = sizeof(time_t) >= sizeof(uint64_t)
                ? (uint64_t)INT64_MAX
                : (uint64_t)INT32_MAX;
  if (now_value > maximum ||
      state->settings.ttl_seconds > maximum - now_value) {
    *status = orm_tidesdb_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                               "TidesDB TTL exceeds time_t range");
    return (time_t)0;
  }
  return (time_t)(now_value + state->settings.ttl_seconds);
}

static orm_status_t orm_tidesdb_unqualified(const orm_query_plan *plan,
                                            tstr column, vstr *out,
                                            orm_error_t *error) {
  const char *separator = strchr(column, '.');
  if (separator == NULL) {
    *out = tstr_to_v(column);
    return ORM_STATUS_OK;
  }
  if (strchr(separator + 1, '.') != NULL ||
      (size_t)(separator - column) != tstr_len(plan->table) ||
      memcmp(column, plan->table, tstr_len(plan->table)) != 0 ||
      separator[1] == '\0')
    return orm_tidesdb_fail(error, ORM_STATUS_UNSUPPORTED,
                            "TidesDB column must belong to the selected table");
  out->data = separator + 1;
  out->len = tstr_len(column) - (size_t)(separator + 1 - column);
  return ORM_STATUS_OK;
}

static const orm_predicate *orm_tidesdb_require_id(
    const orm_tidesdb_backend_state *state, const orm_query_plan *plan,
    orm_error_t *error, orm_status_t *status) {
  const orm_predicate *id = NULL;
  size_t index;
  *status = ORM_STATUS_OK;
  for (index = 0u; index < vec_size(&plan->predicates); ++index) {
    const orm_predicate *predicate =
        (const orm_predicate *)vec_at_const(&plan->predicates, index);
    vstr column;
    if (predicate->comparison != ORM_COMPARE_EQUAL ||
        predicate->value.kind == ORM_VALUE_NULL) {
      *status = orm_tidesdb_fail(
          error, ORM_STATUS_UNSUPPORTED,
          "TidesDB UPDATE/DELETE requires non-null equality predicates");
      return NULL;
    }
    *status = orm_tidesdb_unqualified(plan, predicate->column, &column, error);
    if (*status != ORM_STATUS_OK)
      return NULL;
    if (tstr_eq_v(state->settings.id_column, column)) {
      if (id != NULL) {
        *status = orm_tidesdb_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                                   "duplicate TidesDB id predicate");
        return NULL;
      }
      id = predicate;
    }
  }
  if (id == NULL)
    *status = orm_tidesdb_fail(
        error, ORM_STATUS_UNSUPPORTED,
        "TidesDB UPDATE/DELETE requires an id equality predicate");
  return id;
}

static orm_status_t orm_tidesdb_commit_owned(
    orm_tidesdb_transaction_t *transaction, orm_error_t *error) {
  const int native_status = orm_tidesdb_txn_commit(transaction);
  if (native_status != ORM_TDB_SUCCESS) {
    (void)orm_tidesdb_txn_rollback(transaction);
    return orm_tidesdb_native_error(error, native_status,
                                    ORM_STATUS_DATASTORE_ERROR,
                                    "commit TidesDB command");
  }
  return ORM_STATUS_OK;
}

static orm_status_t orm_tidesdb_insert(
    orm_tidesdb_backend_state *state, const orm_query_plan *plan,
    orm_tidesdb_transaction_t *transaction, uint64_t *affected,
    orm_error_t *error) {
  orm_tidesdb_row row = {0};
  const orm_owned_value *id = NULL;
  orm_tidesdb_transaction_t *owned = NULL;
  tstr key = NULL;
  tstr encoded = NULL;
  size_t index;
  orm_status_t status;
  uint8_t *existing = NULL;
  size_t existing_size = 0u;
  time_t expiration;
  int native_status;
  if (vec_size(&plan->assignments) == 0u)
    return orm_tidesdb_fail(error, ORM_STATUS_INVALID_STATE,
                            "TidesDB INSERT has no assignments");
  status = orm_tidesdb_row_init(&row, state->limits.max_assignments, error);
  if (status != ORM_STATUS_OK)
    return status;
  for (index = 0u; index < vec_size(&plan->assignments); ++index) {
    const orm_assignment *assignment =
        (const orm_assignment *)vec_at_const(&plan->assignments, index);
    orm_tidesdb_cell cell = {0};
    if (tstr_cmp(assignment->column, state->settings.id_column) == 0)
      id = &assignment->value;
    status = orm_tidesdb_cell_from_value(&cell, &assignment->value, error);
    if (status == ORM_STATUS_OK)
      status = orm_tidesdb_row_set(&row, tstr_to_v(assignment->column),
                                   &cell, error);
    tstr_freep(&cell.bytes);
    if (status != ORM_STATUS_OK)
      goto cleanup;
  }
  if (id == NULL || id->kind == ORM_VALUE_NULL) {
    status = orm_tidesdb_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                              "TidesDB INSERT requires a non-null id column");
    goto cleanup;
  }
  status = orm_tidesdb_entity_key(state, plan->table, id, &key, error);
  if (status == ORM_STATUS_OK)
    status = orm_tidesdb_row_encode(&row, state->limits.max_parameter_bytes,
                                    &encoded, error);
  if (status != ORM_STATUS_OK)
    goto cleanup;
  if (transaction == NULL) {
    status = orm_tidesdb_begin_native(
        state, ORM_TDB_ISOLATION_SERIALIZABLE, &owned, error);
    if (status != ORM_STATUS_OK)
      goto cleanup;
    transaction = owned;
  }
  native_status = orm_tidesdb_txn_get(
      transaction, state->column_family, (const uint8_t *)key, tstr_len(key),
      &existing, &existing_size);
  orm_tidesdb_free(existing);
  existing = NULL;
  if (native_status == ORM_TDB_SUCCESS) {
    status = orm_tidesdb_fail(error, ORM_STATUS_DATASTORE_ERROR,
                              "TidesDB INSERT entity already exists");
    goto cleanup;
  }
  if (native_status != ORM_TDB_ERR_NOT_FOUND) {
    status = orm_tidesdb_native_error(error, native_status,
                                      ORM_STATUS_DATASTORE_ERROR,
                                      "read TidesDB entity before INSERT");
    goto cleanup;
  }
  expiration = orm_tidesdb_expiration(state, &status, error);
  if (status != ORM_STATUS_OK)
    goto cleanup;
  native_status = orm_tidesdb_txn_put(
      transaction, state->column_family, (const uint8_t *)key, tstr_len(key),
      (const uint8_t *)encoded, tstr_len(encoded), expiration);
  if (native_status != ORM_TDB_SUCCESS) {
    status = orm_tidesdb_native_error(error, native_status,
                                      ORM_STATUS_DATASTORE_ERROR,
                                      "write TidesDB INSERT");
    goto cleanup;
  }
  if (owned != NULL) {
    status = orm_tidesdb_commit_owned(owned, error);
    if (status != ORM_STATUS_OK)
      goto cleanup;
  }
  *affected = 1u;
  status = ORM_STATUS_OK;

cleanup:
  if (status != ORM_STATUS_OK && owned != NULL)
    (void)orm_tidesdb_txn_rollback(owned);
  orm_tidesdb_txn_free(owned);
  tstr_freep(&encoded);
  tstr_freep(&key);
  orm_tidesdb_row_destroy(&row);
  return status;
}

static orm_status_t orm_tidesdb_update_or_delete(
    orm_tidesdb_backend_state *state, const orm_query_plan *plan,
    orm_tidesdb_transaction_t *transaction, int remove,
    uint64_t *affected, orm_error_t *error) {
  orm_status_t status;
  const orm_predicate *id = orm_tidesdb_require_id(state, plan, error, &status);
  orm_tidesdb_transaction_t *owned = NULL;
  orm_tidesdb_row row = {0};
  tstr key = NULL;
  tstr encoded = NULL;
  uint8_t *raw = NULL;
  size_t raw_size = 0u;
  bool matches = false;
  int native_status;
  size_t index;
  time_t expiration;
  if (status != ORM_STATUS_OK)
    return status;
  if (!remove && vec_size(&plan->assignments) == 0u)
    return orm_tidesdb_fail(error, ORM_STATUS_INVALID_STATE,
                            "TidesDB UPDATE has no assignments");
  status = orm_tidesdb_entity_key(state, plan->table, &id->value, &key,
                                  error);
  if (status != ORM_STATUS_OK)
    goto cleanup;
  if (transaction == NULL) {
    status = orm_tidesdb_begin_native(
        state, ORM_TDB_ISOLATION_SERIALIZABLE, &owned, error);
    if (status != ORM_STATUS_OK)
      goto cleanup;
    transaction = owned;
  }
  native_status = orm_tidesdb_txn_get(
      transaction, state->column_family, (const uint8_t *)key, tstr_len(key),
      &raw, &raw_size);
  if (native_status == ORM_TDB_ERR_NOT_FOUND) {
    status = ORM_STATUS_OK;
    goto commit_empty;
  }
  if (native_status != ORM_TDB_SUCCESS) {
    status = orm_tidesdb_native_error(error, native_status,
                                      ORM_STATUS_DATASTORE_ERROR,
                                      "read TidesDB entity");
    goto cleanup;
  }
  status = orm_tidesdb_row_decode(raw, raw_size,
                                  state->limits.max_parameter_bytes,
                                  state->limits.max_assignments, &row, error);
  orm_tidesdb_free(raw);
  raw = NULL;
  if (status != ORM_STATUS_OK)
    goto cleanup;
  status = orm_tidesdb_row_matches(&row, plan, &matches, error);
  if (status != ORM_STATUS_OK || !matches)
    goto commit_empty;
  if (remove) {
    native_status = orm_tidesdb_txn_delete(
        transaction, state->column_family, (const uint8_t *)key,
        tstr_len(key));
    if (native_status != ORM_TDB_SUCCESS) {
      status = orm_tidesdb_native_error(error, native_status,
                                        ORM_STATUS_DATASTORE_ERROR,
                                        "write TidesDB DELETE");
      goto cleanup;
    }
  } else {
    for (index = 0u; index < vec_size(&plan->assignments); ++index) {
      const orm_assignment *assignment =
          (const orm_assignment *)vec_at_const(&plan->assignments, index);
      orm_tidesdb_cell cell = {0};
      if (tstr_cmp(assignment->column, state->settings.id_column) == 0) {
        status = orm_tidesdb_fail(
            error, ORM_STATUS_UNSUPPORTED,
            "TidesDB UPDATE cannot change the configured id column");
        goto cleanup;
      }
      status = orm_tidesdb_cell_from_value(&cell, &assignment->value, error);
      if (status == ORM_STATUS_OK)
        status = orm_tidesdb_row_set(&row, tstr_to_v(assignment->column),
                                     &cell, error);
      tstr_freep(&cell.bytes);
      if (status != ORM_STATUS_OK)
        goto cleanup;
    }
    status = orm_tidesdb_row_encode(&row, state->limits.max_parameter_bytes,
                                    &encoded, error);
    if (status != ORM_STATUS_OK)
      goto cleanup;
    expiration = orm_tidesdb_expiration(state, &status, error);
    if (status != ORM_STATUS_OK)
      goto cleanup;
    native_status = orm_tidesdb_txn_put(
        transaction, state->column_family, (const uint8_t *)key,
        tstr_len(key), (const uint8_t *)encoded, tstr_len(encoded),
        expiration);
    if (native_status != ORM_TDB_SUCCESS) {
      status = orm_tidesdb_native_error(error, native_status,
                                        ORM_STATUS_DATASTORE_ERROR,
                                        "write TidesDB UPDATE");
      goto cleanup;
    }
  }
  *affected = 1u;

commit_empty:
  if (owned != NULL)
    status = orm_tidesdb_commit_owned(owned, error);

cleanup:
  orm_tidesdb_free(raw);
  if (status != ORM_STATUS_OK && owned != NULL)
    (void)orm_tidesdb_txn_rollback(owned);
  orm_tidesdb_txn_free(owned);
  tstr_freep(&encoded);
  tstr_freep(&key);
  orm_tidesdb_row_destroy(&row);
  return status;
}

static orm_status_t orm_tidesdb_execute_native(
    orm_tidesdb_backend_state *state, const orm_query_plan *plan,
    orm_tidesdb_transaction_t *transaction, uint64_t *affected,
    orm_error_t *error) {
  if (state == NULL || plan == NULL || affected == NULL)
    return orm_tidesdb_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                            "invalid TidesDB command");
  *affected = 0u;
  switch (plan->kind) {
    case ORM_QUERY_INSERT:
      return orm_tidesdb_insert(state, plan, transaction, affected, error);
    case ORM_QUERY_UPDATE:
      return orm_tidesdb_update_or_delete(state, plan, transaction, 0,
                                          affected, error);
    case ORM_QUERY_DELETE:
      return orm_tidesdb_update_or_delete(state, plan, transaction, 1,
                                          affected, error);
    case ORM_QUERY_SELECT:
      return orm_tidesdb_fail(error, ORM_STATUS_UNSUPPORTED,
                              "TidesDB SELECT must be opened as a row Publisher");
    case ORM_QUERY_RAW:
      return orm_tidesdb_fail(error, ORM_STATUS_UNSUPPORTED,
                              "TidesDB does not support raw SQL");
    default:
      return orm_tidesdb_fail(error, ORM_STATUS_INTERNAL_ERROR,
                              "unknown TidesDB query kind");
  }
}

static void orm_tidesdb_transaction_release(
    orm_tidesdb_transaction_state *transaction) {
  if (transaction == NULL)
    return;
  if (--transaction->references != 0u)
    return;
  orm_tidesdb_txn_free(transaction->native);
  free(transaction);
}

static void orm_tidesdb_transaction_finish_deferred(
    orm_tidesdb_transaction_state *transaction) {
  if (transaction == NULL || !transaction->owner_released ||
      transaction->cursors != 0u || !transaction->active)
    return;
  (void)orm_tidesdb_txn_rollback(transaction->native);
  transaction->active = 0;
  transaction->owner->transaction_active = 0;
}

static orm_tidesdb_driver_step orm_tidesdb_flow_error(
    orm_tidesdb_flow_state *state, orm_status_t status,
    const char *message) {
  orm_tidesdb_driver_step step = ORM_TIDESDB_DRIVER_STEP_INIT;
  if (state == NULL) {
    step.kind = ORM_TIDESDB_DRIVER_ERROR;
    step.status = ORM_STATUS_INVALID_ARGUMENT;
    step.message = "invalid TidesDB flow state";
    return step;
  }
  state->done = 1;
  (void)snprintf(state->error_message, sizeof(state->error_message), "%s",
                 message != NULL ? message : orm_status_message(status));
  step.kind = ORM_TIDESDB_DRIVER_ERROR;
  step.status = status;
  step.message = state->error_message;
  return step;
}

static orm_tidesdb_driver_step orm_tidesdb_flow_native_error(
    orm_tidesdb_flow_state *state, int code, const char *operation) {
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message), "%s: %s (%d)", operation,
                 orm_tidesdb_error_name(code), code);
  return orm_tidesdb_flow_error(
      state, orm_tidesdb_map_status(code, ORM_STATUS_DATASTORE_ERROR),
      message);
}

static int orm_tidesdb_has_prefix(const uint8_t *key, size_t key_size,
                                  tstr prefix) {
  return key != NULL && key_size >= tstr_len(prefix) &&
         memcmp(key, prefix, tstr_len(prefix)) == 0;
}

static orm_tidesdb_driver_step orm_tidesdb_flow_next(
    void *context, const unsigned char **out_row, size_t *out_size) {
  orm_tidesdb_flow_state *state = (orm_tidesdb_flow_state *)context;
  orm_tidesdb_driver_step done = ORM_TIDESDB_DRIVER_STEP_INIT;
  if (state == NULL || out_row == NULL || out_size == NULL)
    return orm_tidesdb_flow_error(state, ORM_STATUS_INVALID_ARGUMENT,
                                  "invalid TidesDB flow resume");
  *out_row = NULL;
  *out_size = 0u;
  if (state->done)
    return done;
  if (state->plan->has_limit &&
      state->yielded_rows >= state->plan->limit) {
    state->done = 1;
    return done;
  }
  for (;;) {
    uint8_t *key = NULL;
    size_t key_size = 0u;
    uint8_t *value = NULL;
    size_t value_size = 0u;
    orm_tidesdb_row decoded = {0};
    orm_tidesdb_row projected = {0};
    orm_error_t error;
    orm_status_t status;
    bool matches = false;
    int native_status;
    if (state->needs_advance) {
      state->needs_advance = 0;
      native_status = orm_tidesdb_iter_next(state->iterator);
      if (native_status == ORM_TDB_ERR_NOT_FOUND) {
        state->done = 1;
        return done;
      }
      if (native_status != ORM_TDB_SUCCESS)
        return orm_tidesdb_flow_native_error(state, native_status,
                                             "advance TidesDB iterator");
    }
    if (!orm_tidesdb_iter_valid(state->iterator)) {
      state->done = 1;
      return done;
    }
    native_status = orm_tidesdb_iter_key(state->iterator, &key, &key_size);
    if (native_status != ORM_TDB_SUCCESS)
      return orm_tidesdb_flow_native_error(state, native_status,
                                           "read TidesDB iterator key");
    if (!orm_tidesdb_has_prefix(key, key_size, state->prefix)) {
      state->done = 1;
      return done;
    }
    native_status = orm_tidesdb_iter_value(state->iterator, &value,
                                           &value_size);
    if (native_status != ORM_TDB_SUCCESS)
      return orm_tidesdb_flow_native_error(state, native_status,
                                           "read TidesDB iterator value");
    if (state->scanned_rows >= state->owner->settings.max_scan_rows)
      return orm_tidesdb_flow_error(state, ORM_STATUS_LIMIT_EXCEEDED,
                                    "TidesDB scan exceeds max_scan_rows");
    ++state->scanned_rows;
    if ((uint64_t)key_size >
            state->owner->settings.max_scan_bytes - state->scanned_bytes ||
        (uint64_t)value_size >
            state->owner->settings.max_scan_bytes - state->scanned_bytes -
                (uint64_t)key_size)
      return orm_tidesdb_flow_error(state, ORM_STATUS_LIMIT_EXCEEDED,
                                    "TidesDB scan exceeds max_scan_bytes");
    state->scanned_bytes += (uint64_t)key_size + (uint64_t)value_size;
    state->needs_advance = 1;
    orm_error_init(&error);
    status = orm_tidesdb_row_decode(value, value_size,
                                    state->limits.max_parameter_bytes,
                                    state->limits.max_assignments, &decoded,
                                    &error);
    if (status == ORM_STATUS_OK)
      status = orm_tidesdb_row_matches(&decoded, state->plan, &matches,
                                       &error);
    if (status != ORM_STATUS_OK) {
      orm_tidesdb_row_destroy(&decoded);
      return orm_tidesdb_flow_error(state, status, error.message);
    }
    if (!matches) {
      orm_tidesdb_row_destroy(&decoded);
      continue;
    }
    if (state->matched_rows++ <
        (state->plan->has_offset ? state->plan->offset : 0u)) {
      orm_tidesdb_row_destroy(&decoded);
      continue;
    }
    status = orm_tidesdb_row_project(&decoded, state->plan, &state->limits,
                                     &projected, &error);
    if (status == ORM_STATUS_OK)
      status = orm_tidesdb_row_encode(&projected,
                                      state->limits.max_parameter_bytes,
                                      &state->encoded, &error);
    orm_tidesdb_row_destroy(&projected);
    orm_tidesdb_row_destroy(&decoded);
    if (status != ORM_STATUS_OK)
      return orm_tidesdb_flow_error(state, status, error.message);
    if ((uint64_t)tstr_len(state->encoded) >
        state->limits.max_result_bytes - state->yielded_bytes) {
      tstr_freep(&state->encoded);
      return orm_tidesdb_flow_error(state, ORM_STATUS_LIMIT_EXCEEDED,
                                    "TidesDB results exceed max_result_bytes");
    }
    state->yielded_bytes += (uint64_t)tstr_len(state->encoded);
    *out_row = (const unsigned char *)state->encoded;
    *out_size = tstr_len(state->encoded);
    ++state->yielded_rows;
    done.kind = ORM_TIDESDB_DRIVER_ROW;
    return done;
  }
}

static void orm_tidesdb_flow_release(void *context,
                                     const unsigned char *row) {
  orm_tidesdb_flow_state *state = (orm_tidesdb_flow_state *)context;
  (void)row;
  if (state != NULL)
    tstr_freep(&state->encoded);
}

static void orm_tidesdb_flow_destroy(void *context) {
  orm_tidesdb_flow_state *state = (orm_tidesdb_flow_state *)context;
  if (state == NULL)
    return;
  tstr_freep(&state->encoded);
  tstr_freep(&state->prefix);
  if (state->iterator != NULL)
    orm_tidesdb_iter_free(state->iterator);
  if (state->owns_transaction) {
    (void)orm_tidesdb_txn_rollback(state->native);
    if (state->native != NULL)
      orm_tidesdb_txn_free(state->native);
  } else if (state->explicit_transaction != NULL) {
    --state->explicit_transaction->cursors;
    orm_tidesdb_transaction_finish_deferred(state->explicit_transaction);
    orm_tidesdb_transaction_release(state->explicit_transaction);
  }
  free(state);
}

static const orm_tidesdb_driver_ops orm_tidesdb_flow_ops = {
    sizeof(orm_tidesdb_driver_ops), ORM_TIDESDB_DRIVER_OPS_ABI_VERSION,
    orm_tidesdb_flow_next, orm_tidesdb_flow_release,
    orm_tidesdb_flow_destroy};

static orm_status_t orm_tidesdb_open_native(
    orm_tidesdb_backend_state *state,
    orm_tidesdb_transaction_state *explicit_transaction,
    const orm_query_plan *plan, const orm_limits *limits,
    orm_row_cursor *out_cursor, orm_error_t *error) {
  orm_tidesdb_flow_state *flow = NULL;
  orm_tidesdb_driver driver = {0};
  orm_tidesdb_cursor_config config;
  orm_status_t status;
  int native_status;
  if (state == NULL || plan == NULL || limits == NULL || out_cursor == NULL ||
      out_cursor->ops != NULL || out_cursor->context != NULL)
    return orm_tidesdb_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                            "invalid TidesDB cursor request");
  if (plan->kind != ORM_QUERY_SELECT)
    return orm_tidesdb_fail(error, ORM_STATUS_UNSUPPORTED,
                            "TidesDB commands are not row Publishers");
  if (plan->select_all || vec_size(&plan->columns) == 0u)
    return orm_tidesdb_fail(error, ORM_STATUS_UNSUPPORTED,
                            "TidesDB SELECT requires an explicit projection");
  if (plan->ordering.present)
    return orm_tidesdb_fail(error, ORM_STATUS_UNSUPPORTED,
                            "TidesDB ORDER BY requires a CFlow sort operator");
  if (plan->has_limit && plan->limit > limits->max_result_rows)
    return orm_tidesdb_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                            "TidesDB query limit exceeds max_result_rows");
  if (limits->max_result_rows > (uint64_t)SIZE_MAX)
    return orm_tidesdb_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                            "TidesDB max_result_rows exceeds size_t");
  flow = (orm_tidesdb_flow_state *)calloc(1u, sizeof(*flow));
  if (flow == NULL)
    return orm_tidesdb_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                            "allocate TidesDB flow state");
  flow->owner = state;
  flow->plan = plan;
  flow->limits = *limits;
  status = orm_tidesdb_table_prefix(state, plan->table, &flow->prefix, error);
  if (status != ORM_STATUS_OK)
    goto fail;
  if (explicit_transaction != NULL) {
    if (!explicit_transaction->active) {
      status = orm_tidesdb_fail(error, ORM_STATUS_INVALID_STATE,
                                "TidesDB transaction is not active");
      goto fail;
    }
    flow->native = explicit_transaction->native;
    flow->explicit_transaction = explicit_transaction;
    ++explicit_transaction->references;
    ++explicit_transaction->cursors;
  } else {
    status = orm_tidesdb_begin_native(state, ORM_TDB_ISOLATION_SNAPSHOT,
                                      &flow->native, error);
    if (status != ORM_STATUS_OK)
      goto fail;
    flow->owns_transaction = 1;
  }
  native_status = orm_tidesdb_iter_new(flow->native, state->column_family,
                                       &flow->iterator);
  if (native_status != ORM_TDB_SUCCESS) {
    status = orm_tidesdb_native_error(error, native_status,
                                      ORM_STATUS_DATASTORE_ERROR,
                                      "create TidesDB iterator");
    goto fail;
  }
  native_status = orm_tidesdb_iter_seek(
      flow->iterator, (const uint8_t *)flow->prefix, tstr_len(flow->prefix));
  if (native_status == ORM_TDB_ERR_NOT_FOUND)
    flow->done = 1;
  else if (native_status != ORM_TDB_SUCCESS) {
    status = orm_tidesdb_native_error(error, native_status,
                                      ORM_STATUS_DATASTORE_ERROR,
                                      "seek TidesDB table prefix");
    goto fail;
  }
  driver.ops = &orm_tidesdb_flow_ops;
  driver.context = flow;
  config = (orm_tidesdb_cursor_config)ORM_TIDESDB_CURSOR_CONFIG_INIT(
      (size_t)limits->max_result_rows, limits->max_parameter_bytes,
      limits->max_columns);
  status = orm_tidesdb_cursor_start(out_cursor, &driver, &config, error);
  if (status != ORM_STATUS_OK)
    goto fail;
  return ORM_STATUS_OK;

fail:
  if (driver.ops != NULL)
    driver.ops->destroy(driver.context);
  else
    orm_tidesdb_flow_destroy(flow);
  return status;
}

static orm_status_t orm_tidesdb_backend_open(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    orm_row_cursor *out_cursor, orm_error_t *error) {
  orm_tidesdb_backend_state *state = (orm_tidesdb_backend_state *)context;
  if (state != NULL && state->transaction_active)
    return orm_tidesdb_fail(
        error, ORM_STATUS_INVALID_STATE,
        "execute TidesDB transaction queries through the transaction handle");
  return orm_tidesdb_open_native(state, NULL, plan, limits, out_cursor, error);
}

static orm_status_t orm_tidesdb_backend_execute(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    uint64_t *affected, orm_error_t *error) {
  orm_tidesdb_backend_state *state = (orm_tidesdb_backend_state *)context;
  (void)limits;
  if (state != NULL && state->transaction_active)
    return orm_tidesdb_fail(
        error, ORM_STATUS_INVALID_STATE,
        "execute TidesDB transaction queries through the transaction handle");
  return orm_tidesdb_execute_native(state, plan, NULL, affected, error);
}

static void orm_tidesdb_transaction_destroy(void *context) {
  orm_tidesdb_transaction_state *transaction =
      (orm_tidesdb_transaction_state *)context;
  if (transaction == NULL || transaction->owner_released)
    return;
  transaction->owner_released = 1;
  orm_tidesdb_transaction_finish_deferred(transaction);
  orm_tidesdb_transaction_release(transaction);
}

static orm_status_t orm_tidesdb_transaction_open(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    orm_row_cursor *out_cursor, orm_error_t *error) {
  orm_tidesdb_transaction_state *transaction =
      (orm_tidesdb_transaction_state *)context;
  if (transaction == NULL || !transaction->active)
    return orm_tidesdb_fail(error, ORM_STATUS_INVALID_STATE,
                            "TidesDB transaction is not active");
  return orm_tidesdb_open_native(transaction->owner, transaction, plan,
                                 limits, out_cursor, error);
}

static orm_status_t orm_tidesdb_transaction_execute(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    uint64_t *affected, orm_error_t *error) {
  orm_tidesdb_transaction_state *transaction =
      (orm_tidesdb_transaction_state *)context;
  (void)limits;
  if (transaction == NULL || !transaction->active)
    return orm_tidesdb_fail(error, ORM_STATUS_INVALID_STATE,
                            "TidesDB transaction is not active");
  return orm_tidesdb_execute_native(transaction->owner, plan,
                                    transaction->native, affected, error);
}

static orm_status_t orm_tidesdb_transaction_finish(
    orm_tidesdb_transaction_state *transaction, int commit,
    orm_error_t *error) {
  int native_status;
  if (transaction == NULL || !transaction->active)
    return orm_tidesdb_fail(error, ORM_STATUS_INVALID_STATE,
                            "TidesDB transaction is not active");
  if (transaction->cursors != 0u)
    return orm_tidesdb_fail(error, ORM_STATUS_BUSY,
                            "close TidesDB row Publishers before transaction finish");
  native_status = commit ? orm_tidesdb_txn_commit(transaction->native)
                         : orm_tidesdb_txn_rollback(transaction->native);
  if (native_status != ORM_TDB_SUCCESS)
    return orm_tidesdb_native_error(
        error, native_status, ORM_STATUS_DATASTORE_ERROR,
        commit ? "commit TidesDB transaction" : "roll back TidesDB transaction");
  transaction->active = 0;
  transaction->owner->transaction_active = 0;
  return ORM_STATUS_OK;
}

static orm_status_t orm_tidesdb_transaction_commit(void *context,
                                                   orm_error_t *error) {
  return orm_tidesdb_transaction_finish(
      (orm_tidesdb_transaction_state *)context, 1, error);
}

static orm_status_t orm_tidesdb_transaction_rollback(void *context,
                                                     orm_error_t *error) {
  return orm_tidesdb_transaction_finish(
      (orm_tidesdb_transaction_state *)context, 0, error);
}

static orm_status_t orm_tidesdb_savepoint_call(void *context, vstr name,
                                               orm_error_t *error,
                                               int operation) {
  orm_tidesdb_transaction_state *transaction =
      (orm_tidesdb_transaction_state *)context;
  tstr owned;
  int native_status;
  if (transaction == NULL || !transaction->active ||
      !orm_view_valid(name, false) || memchr(name.data, 0, name.len) != NULL)
    return orm_tidesdb_fail(error, ORM_STATUS_INVALID_STATE,
                            "invalid TidesDB savepoint operation");
  owned = tstr_from_v(name);
  if (owned == NULL)
    return orm_tidesdb_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                            "copy TidesDB savepoint name");
  native_status = operation == 0
                      ? orm_tidesdb_txn_savepoint(transaction->native, owned)
                  : operation == 1
                      ? orm_tidesdb_txn_rollback_to_savepoint(
                            transaction->native, owned)
                      : orm_tidesdb_txn_release_savepoint(transaction->native,
                                                          owned);
  tstr_free(owned);
  if (native_status != ORM_TDB_SUCCESS)
    return orm_tidesdb_native_error(error, native_status,
                                    ORM_STATUS_DATASTORE_ERROR,
                                    "perform TidesDB savepoint operation");
  return ORM_STATUS_OK;
}

static orm_status_t orm_tidesdb_transaction_savepoint(void *context,
                                                      vstr name,
                                                      orm_error_t *error) {
  return orm_tidesdb_savepoint_call(context, name, error, 0);
}

static orm_status_t orm_tidesdb_transaction_rollback_savepoint(
    void *context, vstr name, orm_error_t *error) {
  return orm_tidesdb_savepoint_call(context, name, error, 1);
}

static orm_status_t orm_tidesdb_transaction_release_savepoint(
    void *context, vstr name, orm_error_t *error) {
  return orm_tidesdb_savepoint_call(context, name, error, 2);
}

static const orm_transaction_backend_ops orm_tidesdb_transaction_ops = {
    sizeof(orm_transaction_backend_ops),
    ORM_TRANSACTION_BACKEND_OPS_ABI_VERSION,
    orm_tidesdb_transaction_destroy,
    orm_tidesdb_transaction_open,
    orm_tidesdb_transaction_execute,
    orm_tidesdb_transaction_commit,
    orm_tidesdb_transaction_rollback,
    orm_tidesdb_transaction_savepoint,
    orm_tidesdb_transaction_rollback_savepoint,
    orm_tidesdb_transaction_release_savepoint};

static orm_status_t orm_tidesdb_backend_begin(
    void *context, orm_isolation_t isolation,
    orm_transaction_backend *out_transaction, orm_error_t *error) {
  orm_tidesdb_backend_state *state = (orm_tidesdb_backend_state *)context;
  orm_tidesdb_transaction_state *transaction;
  orm_status_t status;
  if (state == NULL || out_transaction == NULL || state->transaction_active)
    return orm_tidesdb_fail(error, ORM_STATUS_INVALID_STATE,
                            "TidesDB connection already has an active transaction");
  memset(out_transaction, 0, sizeof(*out_transaction));
  transaction = (orm_tidesdb_transaction_state *)calloc(1u,
                                                         sizeof(*transaction));
  if (transaction == NULL)
    return orm_tidesdb_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                            "allocate TidesDB transaction state");
  transaction->owner = state;
  transaction->references = 1u;
  status = orm_tidesdb_begin_native(state, orm_tidesdb_native_isolation(isolation),
                                    &transaction->native, error);
  if (status != ORM_STATUS_OK) {
    free(transaction);
    return status;
  }
  transaction->active = 1;
  state->transaction_active = 1;
  out_transaction->ops = &orm_tidesdb_transaction_ops;
  out_transaction->context = transaction;
  return ORM_STATUS_OK;
}

static void orm_tidesdb_backend_destroy(void *context) {
  orm_tidesdb_backend_state *state = (orm_tidesdb_backend_state *)context;
  if (state == NULL)
    return;
  if (state->database != NULL)
    (void)orm_tidesdb_close(state->database);
  orm_tidesdb_settings_destroy(&state->settings);
  free(state);
}

static const orm_backend_ops orm_tidesdb_backend_ops = {
    sizeof(orm_backend_ops), ORM_BACKEND_OPS_ABI_VERSION,
    orm_tidesdb_backend_destroy, orm_tidesdb_backend_open,
    orm_tidesdb_backend_execute, orm_tidesdb_backend_begin};

orm_status_t orm_tidesdb_backend_create(const orm_config_t *config,
                                        const orm_limits *limits,
                                        orm_backend *out_backend,
                                        orm_error_t *error) {
  orm_tidesdb_backend_state *state;
  orm_tidesdb_config_t native_config;
  orm_tidesdb_column_family_config_t family_config;
  orm_status_t status;
  int native_status;
  if (config == NULL || limits == NULL || out_backend == NULL)
    return orm_tidesdb_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                            "invalid TidesDB backend request");
  memset(out_backend, 0, sizeof(*out_backend));
  state = (orm_tidesdb_backend_state *)calloc(1u, sizeof(*state));
  if (state == NULL)
    return orm_tidesdb_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                            "allocate TidesDB backend");
  state->limits = *limits;
  status = orm_tidesdb_settings_parse(config, limits, &state->settings,
                                      error);
  if (status != ORM_STATUS_OK)
    goto fail;
  native_config = orm_tidesdb_default_config();
  native_config.db_path = state->settings.path;
  native_status = orm_tidesdb_open(&native_config, &state->database);
  if (native_status != ORM_TDB_SUCCESS) {
    status = orm_tidesdb_native_error(error, native_status,
                                      ORM_STATUS_CONNECTION_ERROR,
                                      "open TidesDB database");
    goto fail;
  }
  state->column_family = orm_tidesdb_get_column_family(
      state->database, state->settings.column_family);
  if (state->column_family == NULL && state->settings.create_if_missing) {
    family_config = orm_tidesdb_default_column_family_config();
    native_status = orm_tidesdb_create_column_family(
        state->database, state->settings.column_family, &family_config);
    if (native_status != ORM_TDB_SUCCESS &&
        native_status != ORM_TDB_ERR_EXISTS) {
      status = orm_tidesdb_native_error(error, native_status,
                                        ORM_STATUS_CONNECTION_ERROR,
                                        "create TidesDB column family");
      goto fail;
    }
    state->column_family = orm_tidesdb_get_column_family(
        state->database, state->settings.column_family);
  }
  if (state->column_family == NULL) {
    status = orm_tidesdb_fail(
        error, ORM_STATUS_CONNECTION_ERROR,
        "TidesDB column family does not exist and creation is disabled");
    goto fail;
  }
  out_backend->ops = &orm_tidesdb_backend_ops;
  out_backend->context = state;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;

fail:
  orm_tidesdb_backend_destroy(state);
  return status;
}

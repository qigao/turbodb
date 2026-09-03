#include "orm_internal.h"
#include "orm_redis_cursor.h"
#include "orm_redis_lib.h"
#include "query.h"

#include <redis_cflow.h>
#include <salts/clock.h>
#include <salts_error.h>

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  ORM_REDIS_DEFAULT_PORT = 6379u,
  ORM_REDIS_DEFAULT_TIMEOUT_MS = 5000u,
  ORM_REDIS_IO_SOURCE_CAPACITY = 1u,
  ORM_REDIS_IO_COMPLETION_BATCH_CAPACITY = 1u,
  ORM_REDIS_ADDRESS_CAPACITY = 8u,
  ORM_REDIS_INITIAL_BUFFER_BYTES = 4096u,
  ORM_REDIS_RECEIVE_CHUNK_BYTES = 16384u,
  ORM_REDIS_CONTROL_BUFFER_BYTES = 65536u,
  ORM_REDIS_DATABASE_MAX = 15u,
  ORM_REDIS_OPTION_VALUE_MAX = 4096u,
  ORM_REDIS_REPLY_MAX_DEPTH = 128u
};

static const char orm_redis_default_host[] = "127.0.0.1";
static const char orm_redis_default_id_column[] = "id";
static const char orm_redis_default_key_prefix[] = "orm:";
static const char orm_redis_default_index_prefix[] = "idx:";
static const char orm_redis_insert_script[] =
    "if redis.call('EXISTS', KEYS[1]) ~= 0 then "
    "return redis.error_reply('ORM_DUPLICATE_KEY') end "
    "for i = 2, #ARGV, 2 do redis.call('HSET', KEYS[1], ARGV[i], ARGV[i + 1]) end "
    "if tonumber(ARGV[1]) > 0 then redis.call('EXPIRE', KEYS[1], ARGV[1]) end "
    "return 1";
static const char orm_redis_update_script[] =
    "if redis.call('EXISTS', KEYS[1]) == 0 then return 0 end "
    "for i = 2, #ARGV, 3 do "
    "if ARGV[i] == 'set' then redis.call('HSET', KEYS[1], ARGV[i + 1], ARGV[i + 2]) "
    "else redis.call('HDEL', KEYS[1], ARGV[i + 1]) end end "
    "if tonumber(ARGV[1]) > 0 then redis.call('EXPIRE', KEYS[1], ARGV[1]) end "
    "return 1";

typedef struct orm_redis_settings {
  tstr host;
  uint16_t port;
  tstr username;
  tstr password;
  int database;
  uint32_t timeout_ms;
  uint32_t command_timeout_ms;
  tstr id_column;
  tstr key_prefix;
  tstr index_prefix;
  uint64_t ttl_seconds;
} orm_redis_settings;

typedef struct orm_redis_stream_driver orm_redis_stream_driver;

typedef struct orm_redis_backend_state {
  redis_io_runtime runtime;
  redis_cflow_connection connection;
  orm_redis_settings settings;
  orm_redis_stream_driver *active_driver;
  int cursor_active;
} orm_redis_backend_state;

typedef enum orm_redis_stream_phase {
  ORM_REDIS_STREAM_TOTAL = 0,
  ORM_REDIS_STREAM_ID,
  ORM_REDIS_STREAM_ROW
} orm_redis_stream_phase;

struct orm_redis_stream_driver {
  orm_redis_backend_state *owner;
  redis_cflow_stream stream;
  size_t max_result_bytes;
  size_t result_bytes;
  orm_redis_stream_phase phase;
  int cleanup_pending;
  char error_message[ORM_C_ERROR_MESSAGE_CAPACITY];
};

typedef struct orm_redis_command_result {
  int status;
  redis_command_outcome_t outcome;
  redis_server_error_t server_error;
  redis_reply_t *reply;
} orm_redis_command_result;

#define ORM_REDIS_COMMAND_RESULT_INIT                                                              \
  {SALTS_OK, REDIS_COMMAND_NOT_SENT, REDIS_SERVER_ERROR_NONE, NULL}

typedef struct orm_redis_arguments {
  vec_t values;
} orm_redis_arguments;

static orm_status_t orm_redis_fail(orm_error_t *error, orm_status_t status, const char *message) {
  orm_error_set(error, status, message);
  return status;
}

static void orm_redis_settings_destroy(orm_redis_settings *settings) {
  if (settings == NULL) return;
  tstr_freep(&settings->host);
  tstr_freep(&settings->username);
  tstr_freep(&settings->password);
  tstr_freep(&settings->id_column);
  tstr_freep(&settings->key_prefix);
  tstr_freep(&settings->index_prefix);
  memset(settings, 0, sizeof(*settings));
}

static int orm_redis_parse_u64(vstr input, uint64_t maximum, uint64_t *out) {
  uint64_t value = 0u;
  size_t index;
  if (!orm_view_valid(input, false)) return 0;
  for (index = 0u; index < input.len; ++index) {
    const unsigned char digit = (unsigned char)input.data[index];
    if (digit < '0' || digit > '9') return 0;
    if (value > (maximum - (uint64_t)(digit - '0')) / 10u) return 0;
    value = value * 10u + (uint64_t)(digit - '0');
  }
  *out = value;
  return 1;
}

static orm_status_t orm_redis_copy_option(tstr *out, vstr value, int allow_empty,
                                          orm_error_t *error, const char *message) {
  if (!orm_view_valid(value, allow_empty) || value.len > ORM_REDIS_OPTION_VALUE_MAX ||
      memchr(value.data, 0, value.len) != NULL)
    return orm_redis_fail(error,
                          value.len > ORM_REDIS_OPTION_VALUE_MAX ? ORM_STATUS_LIMIT_EXCEEDED
                                                                 : ORM_STATUS_INVALID_ARGUMENT,
                          message);
  tstr_freep(out);
  *out = tstr_from_v(value);
  return *out != NULL ? ORM_STATUS_OK : orm_redis_fail(error, ORM_STATUS_OUT_OF_MEMORY, message);
}

static orm_status_t orm_redis_settings_parse(const orm_config_t *config,
                                             orm_redis_settings *settings, orm_error_t *error) {
  uint32_t index;
  memset(settings, 0, sizeof(*settings));
  settings->host = tstr_dup(orm_redis_default_host);
  settings->id_column = tstr_dup(orm_redis_default_id_column);
  settings->key_prefix = tstr_dup(orm_redis_default_key_prefix);
  settings->index_prefix = tstr_dup(orm_redis_default_index_prefix);
  settings->port = ORM_REDIS_DEFAULT_PORT;
  settings->timeout_ms = ORM_REDIS_DEFAULT_TIMEOUT_MS;
  settings->command_timeout_ms = ORM_REDIS_DEFAULT_TIMEOUT_MS;
  if (settings->host == NULL || settings->id_column == NULL || settings->key_prefix == NULL ||
      settings->index_prefix == NULL) {
    orm_redis_settings_destroy(settings);
    return orm_redis_fail(error, ORM_STATUS_OUT_OF_MEMORY, "initialize Redis settings");
  }
  for (index = 0u; index < config->option_count; ++index) {
    const orm_option_t *option = &config->options[index];
    uint32_t prior;
    uint64_t parsed;
    orm_status_t status = ORM_STATUS_OK;
    for (prior = 0u; prior < index; ++prior) {
      if (option->keyword.len == config->options[prior].keyword.len &&
          memcmp(option->keyword.data, config->options[prior].keyword.data, option->keyword.len) ==
              0) {
        orm_redis_settings_destroy(settings);
        return orm_redis_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                              "duplicate Redis connection option");
      }
    }
    if (orm_view_equal_cstr(option->keyword, "host")) {
      status = orm_redis_copy_option(&settings->host, option->value, 0, error,
                                     "invalid Redis host option");
    } else if (orm_view_equal_cstr(option->keyword, "username")) {
      status = orm_redis_copy_option(&settings->username, option->value, 1, error,
                                     "invalid Redis username option");
    } else if (orm_view_equal_cstr(option->keyword, "password")) {
      status = orm_redis_copy_option(&settings->password, option->value, 1, error,
                                     "invalid Redis password option");
    } else if (orm_view_equal_cstr(option->keyword, "id_column")) {
      status = orm_redis_copy_option(&settings->id_column, option->value, 0, error,
                                     "invalid Redis id_column option");
    } else if (orm_view_equal_cstr(option->keyword, "key_prefix")) {
      status = orm_redis_copy_option(&settings->key_prefix, option->value, 0, error,
                                     "invalid Redis key_prefix option");
    } else if (orm_view_equal_cstr(option->keyword, "index_prefix")) {
      status = orm_redis_copy_option(&settings->index_prefix, option->value, 0, error,
                                     "invalid Redis index_prefix option");
    } else if (orm_view_equal_cstr(option->keyword, "port")) {
      if (!orm_redis_parse_u64(option->value, UINT16_MAX, &parsed) || parsed == 0u)
        status = orm_redis_fail(error, ORM_STATUS_INVALID_ARGUMENT, "Redis port is out of range");
      else settings->port = (uint16_t)parsed;
    } else if (orm_view_equal_cstr(option->keyword, "database")) {
      if (!orm_redis_parse_u64(option->value, ORM_REDIS_DATABASE_MAX, &parsed))
        status =
            orm_redis_fail(error, ORM_STATUS_INVALID_ARGUMENT, "Redis database must be in [0, 15]");
      else settings->database = (int)parsed;
    } else if (orm_view_equal_cstr(option->keyword, "timeout_ms")) {
      if (!orm_redis_parse_u64(option->value, UINT32_MAX, &parsed) || parsed == 0u)
        status =
            orm_redis_fail(error, ORM_STATUS_INVALID_ARGUMENT, "Redis timeout_ms must be positive");
      else settings->timeout_ms = (uint32_t)parsed;
    } else if (orm_view_equal_cstr(option->keyword, "command_timeout_ms")) {
      if (!orm_redis_parse_u64(option->value, UINT32_MAX, &parsed) || parsed == 0u)
        status = orm_redis_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                                "Redis command_timeout_ms must be positive");
      else settings->command_timeout_ms = (uint32_t)parsed;
    } else if (orm_view_equal_cstr(option->keyword, "ttl_seconds")) {
      if (!orm_redis_parse_u64(option->value, UINT64_MAX, &parsed))
        status = orm_redis_fail(error, ORM_STATUS_INVALID_ARGUMENT, "Redis ttl_seconds is invalid");
      else settings->ttl_seconds = parsed;
    } else {
      status =
          orm_redis_fail(error, ORM_STATUS_INVALID_ARGUMENT, "unknown Redis connection option");
    }
    if (status != ORM_STATUS_OK) {
      orm_redis_settings_destroy(settings);
      return status;
    }
  }
  if (settings->username != NULL && tstr_len(settings->username) != 0u &&
      (settings->password == NULL || tstr_len(settings->password) == 0u)) {
    orm_redis_settings_destroy(settings);
    return orm_redis_fail(error, ORM_STATUS_INVALID_ARGUMENT, "Redis username requires a password");
  }
  return ORM_STATUS_OK;
}

static orm_status_t orm_redis_result_error(orm_redis_command_result *result, int mutation,
                                           orm_error_t *error, const char *operation) {
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
  orm_status_t status;
  if (result->outcome != REDIS_COMMAND_REPLIED) {
    status = ORM_STATUS_CONNECTION_ERROR;
    (void)snprintf(message, sizeof(message), "%s failed with transport status %d%s", operation,
                   result->status,
                   mutation && (result->outcome == REDIS_COMMAND_SEND_UNCERTAIN ||
                                result->outcome == REDIS_COMMAND_REPLY_UNKNOWN)
                       ? "; mutation outcome is unknown and must not be retried blindly"
                       : "");
  } else {
    status = ORM_STATUS_DATASTORE_ERROR;
    (void)snprintf(message, sizeof(message), "%s: %.*s", operation,
                   result->reply != NULL ? (int)result->reply->len : 0,
                   result->reply != NULL && result->reply->str != NULL ? result->reply->str : "");
  }
  redis_reply_free(result->reply);
  *result = (orm_redis_command_result)ORM_REDIS_COMMAND_RESULT_INIT;
  return orm_redis_fail(error, status, message);
}

static uint64_t orm_redis_timeout_ns(uint32_t timeout_ms) {
  return (uint64_t)timeout_ms * UINT64_C(1000000);
}

static int orm_redis_command_run(orm_redis_backend_state *state, int count, const char **argv,
                                 const size_t *lengths, size_t max_reply_bytes,
                                 orm_redis_command_result *out) {
  redis_cflow_stream stream = {0};
  redis_cflow_stream_step step;
  uint64_t timeout_ns = orm_redis_timeout_ns(state->settings.command_timeout_ms);
  uint64_t started = salts_hrtime();
  uint64_t deadline = timeout_ns > UINT64_MAX - started ? UINT64_MAX : started + timeout_ns;
  int status;
  *out = (orm_redis_command_result)ORM_REDIS_COMMAND_RESULT_INIT;
  status =
      redis_cflow_command_open(&state->connection, count, argv, lengths, max_reply_bytes, &stream);
  if (status != SALTS_OK) {
    out->status = status;
    return status;
  }
  for (;;) {
    step = redis_cflow_stream_next(&stream);
    if (step.kind == REDIS_CFLOW_STREAM_WAIT) {
      uint64_t now = salts_hrtime();
      status = now >= deadline ? SALTS_ETIMEDOUT
                               : redis_io_runtime_wait_idle(&state->runtime, deadline - now);
      if (status == SALTS_OK) continue;
      out->status = status;
      out->outcome = step.outcome;
      (void)redis_cflow_stream_cancel(&stream);
      (void)redis_cflow_stream_destroy(&stream);
      return status;
    }
    if (step.kind == REDIS_CFLOW_STREAM_ERROR) {
      out->status = step.status;
      out->outcome = step.outcome;
      out->server_error = step.server_error;
      out->reply = step.item;
      (void)redis_cflow_stream_destroy(&stream);
      return out->status;
    }
    if (step.kind == REDIS_CFLOW_STREAM_ITEM) {
      if (out->reply != NULL) {
        redis_reply_free(step.item);
        out->status = SALTS_EPROTO;
        out->outcome = REDIS_COMMAND_REPLY_UNKNOWN;
        (void)redis_cflow_stream_destroy(&stream);
        return out->status;
      }
      out->reply = step.item;
      out->outcome = step.outcome;
      out->server_error = redis_server_error_classify(step.item);
      continue;
    }
    out->status = out->reply != NULL ? SALTS_OK : SALTS_EPROTO;
    out->outcome = out->reply != NULL ? REDIS_COMMAND_REPLIED : REDIS_COMMAND_REPLY_UNKNOWN;
    status = redis_cflow_stream_destroy(&stream);
    if (status != SALTS_OK && out->status == SALTS_OK) out->status = status;
    return out->status;
  }
}

static orm_status_t orm_redis_command(orm_redis_backend_state *state, size_t count,
                                      const tstr *arguments, int mutation,
                                      orm_redis_command_result *out, orm_error_t *error,
                                      const char *operation) {
  const char **argv;
  size_t *lengths;
  size_t index;
  int native_status;
  if (count == 0u || count > (size_t)INT_MAX)
    return orm_redis_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                          "Redis command argument count exceeds int");
  argv = (const char **)calloc(count, sizeof(*argv));
  lengths = (size_t *)calloc(count, sizeof(*lengths));
  if (argv == NULL || lengths == NULL) {
    free(lengths);
    free(argv);
    return orm_redis_fail(error, ORM_STATUS_OUT_OF_MEMORY, "allocate Redis command views");
  }
  for (index = 0u; index < count; ++index) {
    argv[index] = arguments[index];
    lengths[index] = tstr_len(arguments[index]);
  }
  native_status =
      orm_redis_command_run(state, (int)count, argv, lengths, ORM_REDIS_CONTROL_BUFFER_BYTES, out);
  free(lengths);
  free(argv);
  (void)native_status;
  if (out->outcome != REDIS_COMMAND_REPLIED || out->server_error != REDIS_SERVER_ERROR_NONE)
    return orm_redis_result_error(out, mutation, error, operation);
  return ORM_STATUS_OK;
}

static void orm_redis_arguments_destroy(orm_redis_arguments *arguments) {
  size_t index;
  if (arguments == NULL) return;
  for (index = 0u; index < vec_size(&arguments->values); ++index)
    tstr_freep((tstr *)vec_at(&arguments->values, index));
  vec_destroy(&arguments->values);
}

static orm_status_t orm_redis_arguments_init(orm_redis_arguments *arguments, size_t maximum,
                                             orm_error_t *error) {
  memset(arguments, 0, sizeof(*arguments));
  if (vec_init_bytes(&arguments->values, sizeof(tstr), _Alignof(tstr), maximum) != STL_OK)
    return orm_redis_fail(error, ORM_STATUS_OUT_OF_MEMORY, "initialize Redis arguments");
  return ORM_STATUS_OK;
}

static orm_status_t orm_redis_argument_owned(orm_redis_arguments *arguments, tstr value,
                                             orm_error_t *error) {
  stl_status status;
  if (value == NULL)
    return orm_redis_fail(error, ORM_STATUS_OUT_OF_MEMORY, "allocate Redis argument");
  status = vec_push(&arguments->values, &value);
  if (status != STL_OK) {
    tstr_free(value);
    return orm_redis_fail(error,
                          status == STL_CAPACITY_EXCEEDED ? ORM_STATUS_LIMIT_EXCEEDED
                                                          : ORM_STATUS_OUT_OF_MEMORY,
                          "append Redis argument");
  }
  return ORM_STATUS_OK;
}

static orm_status_t orm_redis_argument_cstr(orm_redis_arguments *arguments, const char *value,
                                            orm_error_t *error) {
  return orm_redis_argument_owned(arguments, tstr_dup(value), error);
}

static orm_status_t orm_redis_entity_key(const orm_redis_backend_state *state, tstr table,
                                         const orm_owned_value *id, tstr *out, orm_error_t *error) {
  tstr encoded = NULL;
  tstr key;
  orm_status_t status = orm_redis_value_text(id, &encoded, error);
  if (status != ORM_STATUS_OK) return status;
  if (encoded == NULL || tstr_len(encoded) == 0u) {
    tstr_free(encoded);
    return orm_redis_fail(error, ORM_STATUS_INVALID_ARGUMENT, "Redis entity id is empty");
  }
  key = tstr_clone(state->settings.key_prefix);
  if (key != NULL) key = tstr_cat(key, "{");
  if (key != NULL) key = tstr_cat(key, table);
  if (key != NULL) key = tstr_cat(key, "}:");
  if (key != NULL) key = tstr_cat_len(key, encoded, tstr_len(encoded));
  tstr_free(encoded);
  if (key == NULL) return orm_redis_fail(error, ORM_STATUS_OUT_OF_MEMORY, "build Redis entity key");
  *out = key;
  return ORM_STATUS_OK;
}

static const orm_predicate *orm_redis_require_id(const orm_redis_backend_state *state,
                                                 const orm_query_plan *plan, orm_error_t *error) {
  const orm_predicate *id;
  if (vec_size(&plan->predicates) != 1u) {
    orm_redis_fail(error, ORM_STATUS_UNSUPPORTED,
                   "Redis UPDATE/DELETE requires exactly one id equality predicate");
    return NULL;
  }
  id = (const orm_predicate *)vec_at_const(&plan->predicates, 0u);
  if (id == NULL || tstr_cmp(id->column, state->settings.id_column) != 0 ||
      id->comparison != ORM_COMPARE_EQUAL || id->value.kind == ORM_VALUE_NULL) {
    orm_redis_fail(error, ORM_STATUS_UNSUPPORTED,
                   "Redis UPDATE/DELETE requires a non-null id equality predicate");
    return NULL;
  }
  return id;
}

static orm_status_t orm_redis_integer_reply(orm_redis_command_result *result, uint64_t *affected,
                                            orm_error_t *error, const char *operation) {
  if (result->reply == NULL || result->reply->type != REDIS_REPLY_INTEGER ||
      result->reply->integer < 0) {
    redis_reply_free(result->reply);
    *result = (orm_redis_command_result)ORM_REDIS_COMMAND_RESULT_INIT;
    return orm_redis_fail(error, ORM_STATUS_DATASTORE_ERROR, operation);
  }
  *affected = (uint64_t)result->reply->integer;
  redis_reply_free(result->reply);
  *result = (orm_redis_command_result)ORM_REDIS_COMMAND_RESULT_INIT;
  return ORM_STATUS_OK;
}

static orm_status_t orm_redis_execute_insert(orm_redis_backend_state *state,
                                             const orm_query_plan *plan, uint64_t *affected,
                                             orm_error_t *error) {
  orm_redis_arguments args;
  const orm_assignment *id = NULL;
  orm_redis_command_result result = ORM_REDIS_COMMAND_RESULT_INIT;
  size_t index;
  char ttl[32];
  orm_status_t status;
  if (vec_size(&plan->assignments) == 0u)
    return orm_redis_fail(error, ORM_STATUS_INVALID_STATE, "Redis INSERT has no values");
  status = orm_redis_arguments_init(&args, 5u + vec_size(&plan->assignments) * 2u, error);
  if (status != ORM_STATUS_OK) return status;
  for (index = 0u; index < vec_size(&plan->assignments); ++index) {
    const orm_assignment *value = (const orm_assignment *)vec_at_const(&plan->assignments, index);
    if (tstr_cmp(value->column, state->settings.id_column) == 0) id = value;
  }
  if (id == NULL || id->value.kind == ORM_VALUE_NULL) {
    status = orm_redis_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                            "Redis INSERT requires the configured id column");
    goto cleanup;
  }
  status = orm_redis_argument_cstr(&args, "EVAL", error);
  if (status == ORM_STATUS_OK)
    status = orm_redis_argument_cstr(&args, orm_redis_insert_script, error);
  if (status == ORM_STATUS_OK) status = orm_redis_argument_cstr(&args, "1", error);
  if (status == ORM_STATUS_OK) {
    tstr key = NULL;
    status = orm_redis_entity_key(state, plan->table, &id->value, &key, error);
    if (status == ORM_STATUS_OK) status = orm_redis_argument_owned(&args, key, error);
  }
  (void)snprintf(ttl, sizeof(ttl), "%" PRIu64, state->settings.ttl_seconds);
  if (status == ORM_STATUS_OK) status = orm_redis_argument_cstr(&args, ttl, error);
  for (index = 0u; status == ORM_STATUS_OK && index < vec_size(&plan->assignments); ++index) {
    const orm_assignment *value = (const orm_assignment *)vec_at_const(&plan->assignments, index);
    tstr encoded = NULL;
    if (value->value.kind == ORM_VALUE_NULL) continue;
    status = orm_redis_argument_owned(&args, tstr_clone(value->column), error);
    if (status == ORM_STATUS_OK) status = orm_redis_value_text(&value->value, &encoded, error);
    if (status == ORM_STATUS_OK) status = orm_redis_argument_owned(&args, encoded, error);
    else tstr_free(encoded);
  }
  if (status == ORM_STATUS_OK)
    status = orm_redis_command(state, vec_size(&args.values), (const tstr *)vec_data(&args.values),
                               1, &result, error, "Redis INSERT script");
  if (status == ORM_STATUS_OK)
    status = orm_redis_integer_reply(&result, affected, error,
                                     "Redis INSERT returned an invalid result");
cleanup:
  orm_redis_arguments_destroy(&args);
  return status;
}

static orm_status_t orm_redis_execute_update(orm_redis_backend_state *state,
                                             const orm_query_plan *plan, uint64_t *affected,
                                             orm_error_t *error) {
  orm_redis_arguments args;
  const orm_predicate *id = orm_redis_require_id(state, plan, error);
  orm_redis_command_result result = ORM_REDIS_COMMAND_RESULT_INIT;
  size_t index;
  char ttl[32];
  orm_status_t status;
  if (id == NULL) return error->status;
  if (vec_size(&plan->assignments) == 0u)
    return orm_redis_fail(error, ORM_STATUS_INVALID_STATE, "Redis UPDATE has no assignments");
  status = orm_redis_arguments_init(&args, 5u + vec_size(&plan->assignments) * 3u, error);
  if (status != ORM_STATUS_OK) return status;
  status = orm_redis_argument_cstr(&args, "EVAL", error);
  if (status == ORM_STATUS_OK)
    status = orm_redis_argument_cstr(&args, orm_redis_update_script, error);
  if (status == ORM_STATUS_OK) status = orm_redis_argument_cstr(&args, "1", error);
  if (status == ORM_STATUS_OK) {
    tstr key = NULL;
    status = orm_redis_entity_key(state, plan->table, &id->value, &key, error);
    if (status == ORM_STATUS_OK) status = orm_redis_argument_owned(&args, key, error);
  }
  (void)snprintf(ttl, sizeof(ttl), "%" PRIu64, state->settings.ttl_seconds);
  if (status == ORM_STATUS_OK) status = orm_redis_argument_cstr(&args, ttl, error);
  for (index = 0u; status == ORM_STATUS_OK && index < vec_size(&plan->assignments); ++index) {
    const orm_assignment *value = (const orm_assignment *)vec_at_const(&plan->assignments, index);
    tstr encoded = NULL;
    if (tstr_cmp(value->column, state->settings.id_column) == 0) {
      status = orm_redis_fail(error, ORM_STATUS_UNSUPPORTED,
                              "Redis UPDATE cannot change the configured id column");
      break;
    }
    status = orm_redis_argument_cstr(&args, value->value.kind == ORM_VALUE_NULL ? "delete" : "set",
                                     error);
    if (status == ORM_STATUS_OK)
      status = orm_redis_argument_owned(&args, tstr_clone(value->column), error);
    if (status == ORM_STATUS_OK && value->value.kind != ORM_VALUE_NULL)
      status = orm_redis_value_text(&value->value, &encoded, error);
    if (status == ORM_STATUS_OK)
      status = orm_redis_argument_owned(&args, encoded != NULL ? encoded : tstr_dup(""), error);
    else tstr_free(encoded);
  }
  if (status == ORM_STATUS_OK)
    status = orm_redis_command(state, vec_size(&args.values), (const tstr *)vec_data(&args.values),
                               1, &result, error, "Redis UPDATE script");
  if (status == ORM_STATUS_OK)
    status = orm_redis_integer_reply(&result, affected, error,
                                     "Redis UPDATE returned an invalid result");
  orm_redis_arguments_destroy(&args);
  return status;
}

static orm_status_t orm_redis_execute_delete(orm_redis_backend_state *state,
                                             const orm_query_plan *plan, uint64_t *affected,
                                             orm_error_t *error) {
  const orm_predicate *id = orm_redis_require_id(state, plan, error);
  tstr arguments[2] = {NULL, NULL};
  orm_redis_command_result result = ORM_REDIS_COMMAND_RESULT_INIT;
  orm_status_t status;
  if (id == NULL) return error->status;
  arguments[0] = tstr_dup("DEL");
  status = orm_redis_entity_key(state, plan->table, &id->value, &arguments[1], error);
  if (arguments[0] == NULL && status == ORM_STATUS_OK)
    status = orm_redis_fail(error, ORM_STATUS_OUT_OF_MEMORY, "allocate Redis DEL command");
  if (status == ORM_STATUS_OK)
    status = orm_redis_command(state, 2u, arguments, 1, &result, error, "Redis DELETE");
  if (status == ORM_STATUS_OK)
    status = orm_redis_integer_reply(&result, affected, error,
                                     "Redis DELETE returned an invalid result");
  tstr_free(arguments[1]);
  tstr_free(arguments[0]);
  return status;
}

static orm_redis_driver_step orm_redis_stream_failure(orm_redis_stream_driver *driver,
                                                      redis_cflow_stream_step native) {
  orm_redis_driver_step step = ORM_REDIS_DRIVER_STEP_INIT;
  step.kind = ORM_REDIS_DRIVER_ERROR;
  step.status = native.status == SALTS_ENOBUFS            ? ORM_STATUS_LIMIT_EXCEEDED
                : native.outcome == REDIS_COMMAND_REPLIED ? ORM_STATUS_DATASTORE_ERROR
                                                          : ORM_STATUS_CONNECTION_ERROR;
  if (native.item != NULL && native.item->str != NULL)
    (void)snprintf(driver->error_message, sizeof(driver->error_message),
                   "Redis Query Engine error: %.*s", (int)native.item->len, native.item->str);
  else
    (void)snprintf(driver->error_message, sizeof(driver->error_message),
                   "Redis RESP stream failed with status %d", native.status);
  redis_reply_free(native.item);
  step.message = driver->error_message;
  return step;
}

static int orm_redis_reply_payload_size(const redis_reply_t *reply, size_t depth, size_t *out) {
  size_t total = 0u;
  size_t index;
  if (reply == NULL || out == NULL || depth > ORM_REDIS_REPLY_MAX_DEPTH) return 0;
  if (reply->type == REDIS_REPLY_STRING || reply->type == REDIS_REPLY_ERROR ||
      reply->type == REDIS_REPLY_BULK_STRING) {
    *out = reply->len;
    return 1;
  }
  if (reply->type == REDIS_REPLY_INTEGER) {
    *out = sizeof(reply->integer);
    return 1;
  }
  if (reply->type == REDIS_REPLY_NULL) {
    *out = 0u;
    return 1;
  }
  if (reply->type != REDIS_REPLY_ARRAY || (reply->element_count != 0u && reply->elements == NULL))
    return 0;
  for (index = 0u; index < reply->element_count; ++index) {
    size_t child_size;
    if (!orm_redis_reply_payload_size(reply->elements[index], depth + 1u, &child_size) ||
        child_size > SIZE_MAX - total)
      return 0;
    total += child_size;
  }
  *out = total;
  return 1;
}

static int orm_redis_stream_account(orm_redis_stream_driver *driver, const redis_reply_t *reply) {
  size_t payload;
  if (!orm_redis_reply_payload_size(reply, 0u, &payload) ||
      driver->result_bytes > driver->max_result_bytes ||
      payload > driver->max_result_bytes - driver->result_bytes)
    return 0;
  driver->result_bytes += payload;
  return 1;
}

static orm_redis_driver_step orm_redis_stream_next(void *context, void **row) {
  orm_redis_stream_driver *driver = (orm_redis_stream_driver *)context;
  orm_redis_driver_step step = ORM_REDIS_DRIVER_STEP_INIT;
  redis_cflow_stream_step native;
  *row = NULL;
  for (;;) {
    native = redis_cflow_stream_next(&driver->stream);
    if (native.kind == REDIS_CFLOW_STREAM_WAIT) {
      step.kind = ORM_REDIS_DRIVER_WAIT;
      step.waitable = native.waitable;
      return step;
    }
    if (native.kind == REDIS_CFLOW_STREAM_ERROR) return orm_redis_stream_failure(driver, native);
    if (native.kind == REDIS_CFLOW_STREAM_DONE) {
      if (driver->phase == ORM_REDIS_STREAM_ID) return step;
      step.kind = ORM_REDIS_DRIVER_ERROR;
      step.status = ORM_STATUS_DATASTORE_ERROR;
      step.message = driver->phase == ORM_REDIS_STREAM_TOTAL
                         ? "Redis Query Engine reply omitted the total"
                         : "Redis Query Engine reply ended before a row";
      return step;
    }
    if (native.kind != REDIS_CFLOW_STREAM_ITEM || native.item == NULL) {
      redis_reply_free(native.item);
      step.kind = ORM_REDIS_DRIVER_ERROR;
      step.status = ORM_STATUS_DATASTORE_ERROR;
      step.message = "Redis Query Engine returned an invalid stream step";
      return step;
    }
    if (driver->phase == ORM_REDIS_STREAM_TOTAL) {
      if (native.item->type != REDIS_REPLY_INTEGER || native.item->integer < 0) {
        redis_reply_free(native.item);
        step.kind = ORM_REDIS_DRIVER_ERROR;
        step.status = ORM_STATUS_DATASTORE_ERROR;
        step.message = "Redis Query Engine returned an invalid total";
        return step;
      }
      redis_reply_free(native.item);
      driver->phase = ORM_REDIS_STREAM_ID;
      continue;
    }
    if (driver->phase == ORM_REDIS_STREAM_ID) {
      if (native.item->type != REDIS_REPLY_STRING && native.item->type != REDIS_REPLY_BULK_STRING) {
        redis_reply_free(native.item);
        step.kind = ORM_REDIS_DRIVER_ERROR;
        step.status = ORM_STATUS_DATASTORE_ERROR;
        step.message = "Redis Query Engine returned an invalid document id";
        return step;
      }
      if (!orm_redis_stream_account(driver, native.item)) {
        redis_reply_free(native.item);
        step.kind = ORM_REDIS_DRIVER_ERROR;
        step.status = ORM_STATUS_LIMIT_EXCEEDED;
        step.message = "Redis result exceeds max_result_bytes";
        return step;
      }
      redis_reply_free(native.item);
      driver->phase = ORM_REDIS_STREAM_ROW;
      continue;
    }
    if (!orm_redis_stream_account(driver, native.item)) {
      redis_reply_free(native.item);
      step.kind = ORM_REDIS_DRIVER_ERROR;
      step.status = ORM_STATUS_LIMIT_EXCEEDED;
      step.message = "Redis result exceeds max_result_bytes";
      return step;
    }
    driver->phase = ORM_REDIS_STREAM_ID;
    *row = native.item;
    step.kind = ORM_REDIS_DRIVER_ROW;
    return step;
  }
}

static void orm_redis_stream_release(void *context, void *row) {
  (void)context;
  redis_reply_free((redis_reply_t *)row);
}

static void orm_redis_stream_cancel(void *context) {
  orm_redis_stream_driver *driver = (orm_redis_stream_driver *)context;
  if (driver != NULL && redis_cflow_stream_cancel(&driver->stream) != SALTS_OK)
    driver->cleanup_pending = 1;
}

static int orm_redis_stream_cleanup(orm_redis_stream_driver *driver) {
  orm_redis_backend_state *owner;
  int status;
  if (driver == NULL) return SALTS_EINVAL;
  owner = driver->owner;
  if (driver->stream.impl != NULL) {
    status = redis_cflow_stream_destroy(&driver->stream);
    if (status != SALTS_OK) {
      driver->cleanup_pending = 1;
      return status;
    }
  }
  if (owner != NULL && owner->active_driver == driver) {
    owner->active_driver = NULL;
    owner->cursor_active = 0;
  }
  free(driver);
  return SALTS_OK;
}

static int orm_redis_cleanup_deferred_stream(orm_redis_backend_state *state) {
  if (state == NULL || state->active_driver == NULL || !state->active_driver->cleanup_pending)
    return SALTS_OK;
  return orm_redis_stream_cleanup(state->active_driver);
}

static void orm_redis_stream_destroy(void *context) {
  orm_redis_stream_driver *driver = (orm_redis_stream_driver *)context;
  if (driver != NULL) (void)orm_redis_stream_cleanup(driver);
}

static const orm_redis_row_driver_ops orm_redis_stream_ops = {sizeof(orm_redis_row_driver_ops),
                                                              ORM_REDIS_ROW_DRIVER_OPS_ABI_VERSION,
                                                              orm_redis_stream_next,
                                                              orm_redis_stream_cancel,
                                                              orm_redis_stream_release,
                                                              orm_redis_stream_destroy};

static orm_status_t orm_redis_backend_open(void *context, const orm_query_plan *plan,
                                           const orm_limits *limits, orm_row_cursor *out_cursor,
                                           orm_error_t *error) {
  orm_redis_backend_state *state = (orm_redis_backend_state *)context;
  orm_redis_query query;
  orm_redis_stream_driver *stream_driver = NULL;
  orm_redis_row_driver driver = {0};
  orm_redis_field_view *fields = NULL;
  const char **argv = NULL;
  size_t *lengths = NULL;
  size_t field_count;
  size_t index;
  size_t max_items;
  orm_redis_cursor_config cursor_config;
  orm_status_t status;
  int native_status;
  if (state == NULL || plan == NULL || limits == NULL || out_cursor == NULL)
    return orm_redis_fail(error, ORM_STATUS_INVALID_ARGUMENT, "invalid Redis cursor request");
  native_status = orm_redis_cleanup_deferred_stream(state);
  if (native_status != SALTS_OK)
    return orm_redis_fail(error, ORM_STATUS_BUSY, "Redis row Publisher cleanup is still pending");
  if (state->cursor_active)
    return orm_redis_fail(error, ORM_STATUS_BUSY,
                          "Redis connection already has an active row Publisher");
  if (limits->max_result_rows > (uint64_t)((SIZE_MAX - 1u) / 2u) ||
      limits->max_result_bytes > (uint64_t)SIZE_MAX)
    return orm_redis_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                          "Redis result limits exceed the platform range");
  status =
      orm_redis_query_build(plan, limits, tstr_to_v(state->settings.index_prefix), &query, error);
  if (status != ORM_STATUS_OK) return status;
  field_count = vec_size(&query.output_columns);
  fields = (orm_redis_field_view *)calloc(field_count, sizeof(*fields));
  argv = (const char **)calloc(vec_size(&query.arguments), sizeof(*argv));
  lengths = (size_t *)calloc(vec_size(&query.arguments), sizeof(*lengths));
  stream_driver = (orm_redis_stream_driver *)calloc(1u, sizeof(*stream_driver));
  if (fields == NULL || argv == NULL || lengths == NULL || stream_driver == NULL) {
    status = orm_redis_fail(error, ORM_STATUS_OUT_OF_MEMORY, "allocate Redis stream state");
    goto cleanup;
  }
  for (index = 0u; index < field_count; ++index) {
    const tstr *column = (const tstr *)vec_at_const(&query.output_columns, index);
    fields[index].data = (const unsigned char *)*column;
    fields[index].size = tstr_len(*column);
  }
  for (index = 0u; index < vec_size(&query.arguments); ++index) {
    const tstr *argument = (const tstr *)vec_at_const(&query.arguments, index);
    argv[index] = *argument;
    lengths[index] = tstr_len(*argument);
  }
  max_items = 1u + (size_t)limits->max_result_rows * 2u;
  native_status =
      redis_cflow_stream_open(&state->connection, (int)vec_size(&query.arguments), argv, lengths,
                              (size_t)limits->max_result_bytes, max_items, &stream_driver->stream);
  if (native_status != SALTS_OK) {
    status = orm_redis_fail(
        error, native_status == SALTS_EBUSY ? ORM_STATUS_BUSY : ORM_STATUS_CONNECTION_ERROR,
        "open Redis RESP stream failed");
    goto cleanup;
  }
  stream_driver->owner = state;
  state->active_driver = stream_driver;
  state->cursor_active = 1;
  stream_driver->max_result_bytes = (size_t)limits->max_result_bytes;
  driver.ops = &orm_redis_stream_ops;
  driver.reply_ops = orm_redis_lib_reply_ops();
  driver.context = stream_driver;
  cursor_config = (orm_redis_cursor_config)ORM_REDIS_CURSOR_CONFIG_INIT(
      (size_t)limits->max_result_rows, (size_t)limits->max_result_bytes,
      orm_redis_timeout_ns(state->settings.command_timeout_ms));
  status = orm_redis_cursor_start(out_cursor, &driver, fields, field_count, &cursor_config, error);
  if (status != ORM_STATUS_OK) goto cleanup;
  stream_driver = NULL;
cleanup:
  if (stream_driver != NULL) orm_redis_stream_destroy(stream_driver);
  free(lengths);
  free(argv);
  free(fields);
  orm_redis_query_destroy(&query);
  return status;
}

static orm_status_t orm_redis_backend_execute(void *context, const orm_query_plan *plan,
                                              const orm_limits *limits, uint64_t *affected,
                                              orm_error_t *error) {
  orm_redis_backend_state *state = (orm_redis_backend_state *)context;
  (void)limits;
  if (state == NULL || plan == NULL || affected == NULL)
    return orm_redis_fail(error, ORM_STATUS_INVALID_ARGUMENT, "invalid Redis command request");
  {
    int cleanup_status = orm_redis_cleanup_deferred_stream(state);
    if (cleanup_status != SALTS_OK)
      return orm_redis_fail(error, ORM_STATUS_BUSY, "Redis row Publisher cleanup is still pending");
  }
  if (state->cursor_active)
    return orm_redis_fail(error, ORM_STATUS_BUSY,
                          "close the active Redis row Publisher before a command");
  switch (plan->kind) {
  case ORM_QUERY_INSERT:
    return orm_redis_execute_insert(state, plan, affected, error);
  case ORM_QUERY_UPDATE:
    return orm_redis_execute_update(state, plan, affected, error);
  case ORM_QUERY_DELETE:
    return orm_redis_execute_delete(state, plan, affected, error);
  case ORM_QUERY_SELECT:
    return orm_redis_fail(error, ORM_STATUS_UNSUPPORTED,
                          "Redis SELECT must be opened as a row Publisher");
  case ORM_QUERY_RAW:
    return orm_redis_fail(error, ORM_STATUS_UNSUPPORTED, "raw SQL is not supported by Redis");
  default:
    return orm_redis_fail(error, ORM_STATUS_INTERNAL_ERROR, "unknown Redis query kind");
  }
}

static orm_status_t orm_redis_backend_begin(void *context, orm_isolation_t isolation,
                                            orm_transaction_backend *out_transaction,
                                            orm_error_t *error) {
  (void)context;
  (void)isolation;
  if (out_transaction != NULL) memset(out_transaction, 0, sizeof(*out_transaction));
  return orm_redis_fail(error, ORM_STATUS_UNSUPPORTED, "Redis ORM transactions are unsupported");
}

static void orm_redis_backend_destroy(void *context) {
  orm_redis_backend_state *state = (orm_redis_backend_state *)context;
  if (state == NULL) return;
  if (state->active_driver != NULL && orm_redis_stream_cleanup(state->active_driver) != SALTS_OK)
    return;
  if (redis_cflow_connection_valid(&state->connection) &&
      redis_cflow_connection_destroy(&state->connection) != SALTS_OK)
    return;
  if (redis_io_runtime_valid(&state->runtime)) {
    if (redis_io_runtime_close(&state->runtime) != SALTS_OK) return;
    if (redis_io_runtime_destroy(&state->runtime) != SALTS_OK) return;
  }
  orm_redis_settings_destroy(&state->settings);
  free(state);
}

static orm_status_t orm_redis_verify_query_engine(orm_redis_backend_state *state,
                                                  const orm_limits *limits, orm_error_t *error) {
  static const char *arguments[] = {"COMMAND", "INFO", "FT.SEARCH"};
  orm_redis_command_result result = ORM_REDIS_COMMAND_RESULT_INIT;
  int native_status;
  if (limits->max_result_bytes > (uint64_t)SIZE_MAX)
    return orm_redis_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                          "Redis max_result_bytes exceeds size_t");
  native_status =
      orm_redis_command_run(state, 3, arguments, NULL, (size_t)limits->max_result_bytes, &result);
  if (native_status != SALTS_OK || result.server_error != REDIS_SERVER_ERROR_NONE) {
    redis_reply_free(result.reply);
    return orm_redis_fail(error,
                          result.outcome == REDIS_COMMAND_REPLIED ? ORM_STATUS_UNSUPPORTED
                                                                  : ORM_STATUS_CONNECTION_ERROR,
                          "Redis Query Engine is required but FT.SEARCH is unavailable");
  }
  if (result.reply == NULL || result.reply->type != REDIS_REPLY_ARRAY ||
      result.reply->element_count != 1u || result.reply->elements == NULL ||
      result.reply->elements[0] == NULL || result.reply->elements[0]->type != REDIS_REPLY_ARRAY) {
    redis_reply_free(result.reply);
    return orm_redis_fail(error, ORM_STATUS_UNSUPPORTED,
                          "Redis Query Engine is required but FT.SEARCH is unavailable");
  }
  redis_reply_free(result.reply);
  return ORM_STATUS_OK;
}

static const orm_backend_ops orm_redis_backend_ops = {
    sizeof(orm_backend_ops), ORM_BACKEND_OPS_ABI_VERSION, orm_redis_backend_destroy,
    orm_redis_backend_open,  orm_redis_backend_execute,   orm_redis_backend_begin};

static orm_status_t orm_redis_expect_ok(orm_redis_backend_state *state, int count,
                                        const char **arguments, orm_error_t *error,
                                        const char *operation) {
  orm_redis_command_result result = ORM_REDIS_COMMAND_RESULT_INIT;
  int status =
      orm_redis_command_run(state, count, arguments, NULL, ORM_REDIS_CONTROL_BUFFER_BYTES, &result);
  if (status != SALTS_OK || result.server_error != REDIS_SERVER_ERROR_NONE) {
    redis_reply_free(result.reply);
    return orm_redis_fail(error,
                          result.outcome == REDIS_COMMAND_REPLIED ? ORM_STATUS_DATASTORE_ERROR
                                                                  : ORM_STATUS_CONNECTION_ERROR,
                          operation);
  }
  if (result.reply == NULL || result.reply->type != REDIS_REPLY_STRING || result.reply->len != 2u ||
      memcmp(result.reply->str, "OK", 2u) != 0) {
    redis_reply_free(result.reply);
    return orm_redis_fail(error, ORM_STATUS_DATASTORE_ERROR, operation);
  }
  redis_reply_free(result.reply);
  return ORM_STATUS_OK;
}

static orm_status_t orm_redis_prepare_connection(orm_redis_backend_state *state,
                                                 orm_error_t *error) {
  orm_status_t status;
  if (state->settings.password != NULL && tstr_len(state->settings.password) != 0u) {
    const char *auth[3] = {"AUTH", state->settings.username, state->settings.password};
    int count =
        state->settings.username != NULL && tstr_len(state->settings.username) != 0u ? 3 : 2;
    if (count == 2) auth[1] = state->settings.password;
    status = orm_redis_expect_ok(state, count, auth, error, "Redis AUTH failed");
    if (status != ORM_STATUS_OK) return status;
  }
  if (state->settings.database != 0) {
    char database[16];
    const char *select_arguments[2] = {"SELECT", database};
    (void)snprintf(database, sizeof(database), "%d", state->settings.database);
    status = orm_redis_expect_ok(state, 2, select_arguments, error, "Redis SELECT failed");
    if (status != ORM_STATUS_OK) return status;
  }
  return ORM_STATUS_OK;
}

orm_status_t orm_redis_backend_create(const orm_config_t *config, const orm_limits *limits,
                                      orm_backend *out_backend, orm_error_t *error) {
  orm_redis_backend_state *state;
  redis_io_runtime_config runtime_config;
  redis_cflow_open_config open_config;
  redis_cflow_connect_step connect_step;
  size_t max_command_bytes;
  size_t max_buffer_bytes;
  size_t initial_buffer_bytes;
  size_t receive_chunk_bytes;
  int native_status;
  orm_status_t status;
  if (config == NULL || limits == NULL || out_backend == NULL)
    return orm_redis_fail(error, ORM_STATUS_INVALID_ARGUMENT, "invalid Redis backend request");
  memset(out_backend, 0, sizeof(*out_backend));
  state = (orm_redis_backend_state *)calloc(1u, sizeof(*state));
  if (state == NULL)
    return orm_redis_fail(error, ORM_STATUS_OUT_OF_MEMORY, "allocate Redis backend");
  status = orm_redis_settings_parse(config, &state->settings, error);
  if (status != ORM_STATUS_OK) goto fail;
  if (limits->max_result_bytes > (uint64_t)SIZE_MAX) {
    status =
        orm_redis_fail(error, ORM_STATUS_LIMIT_EXCEEDED, "Redis max_result_bytes exceeds size_t");
    goto fail;
  }
  if (limits->max_query_bytes > SIZE_MAX - limits->max_parameter_bytes ||
      limits->max_query_bytes + limits->max_parameter_bytes >
          SIZE_MAX - ORM_REDIS_CONTROL_BUFFER_BYTES) {
    status = orm_redis_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                            "Redis command byte limit overflows size_t");
    goto fail;
  }
  max_command_bytes =
      limits->max_query_bytes + limits->max_parameter_bytes + ORM_REDIS_CONTROL_BUFFER_BYTES;
  max_buffer_bytes = (size_t)limits->max_result_bytes;
  if (max_buffer_bytes < ORM_REDIS_CONTROL_BUFFER_BYTES)
    max_buffer_bytes = ORM_REDIS_CONTROL_BUFFER_BYTES;
  initial_buffer_bytes = max_buffer_bytes < ORM_REDIS_INITIAL_BUFFER_BYTES
                             ? max_buffer_bytes
                             : ORM_REDIS_INITIAL_BUFFER_BYTES;
  receive_chunk_bytes = max_buffer_bytes < ORM_REDIS_RECEIVE_CHUNK_BYTES
                            ? max_buffer_bytes
                            : ORM_REDIS_RECEIVE_CHUNK_BYTES;
  runtime_config =
      (redis_io_runtime_config){redis_io_default_backend_kind(), ORM_REDIS_IO_SOURCE_CAPACITY,
                                ORM_REDIS_IO_COMPLETION_BATCH_CAPACITY};
  native_status = redis_io_runtime_init(&state->runtime, &runtime_config);
  if (native_status != SALTS_OK) {
    status = orm_redis_fail(error, ORM_STATUS_CONNECTION_ERROR,
                            "initialize Redis CFlow I/O runtime failed");
    goto fail;
  }
  open_config = (redis_cflow_open_config){&state->runtime,
                                          state->settings.host,
                                          state->settings.port,
                                          ORM_REDIS_ADDRESS_CAPACITY,
                                          max_command_bytes,
                                          initial_buffer_bytes,
                                          max_buffer_bytes,
                                          receive_chunk_bytes,
                                          orm_redis_timeout_ns(state->settings.command_timeout_ms)};
  native_status = redis_cflow_connection_open(&state->connection, &open_config);
  if (native_status != SALTS_OK) {
    status = orm_redis_fail(error, ORM_STATUS_CONNECTION_ERROR, "resolve Redis endpoint failed");
    goto fail;
  }
  for (;;) {
    connect_step = redis_cflow_connection_connect_next(&state->connection);
    if (connect_step.kind == REDIS_CFLOW_CONNECT_DONE) break;
    if (connect_step.kind == REDIS_CFLOW_CONNECT_ERROR) {
      status =
          orm_redis_fail(error, ORM_STATUS_CONNECTION_ERROR, "connect Redis through CFlow failed");
      goto fail;
    }
    native_status = redis_io_runtime_wait_idle(&state->runtime,
                                               orm_redis_timeout_ns(state->settings.timeout_ms));
    if (native_status != SALTS_OK) {
      status = orm_redis_fail(error, ORM_STATUS_CONNECTION_ERROR, "connect Redis timed out");
      goto fail;
    }
  }
  status = orm_redis_prepare_connection(state, error);
  if (status != ORM_STATUS_OK) goto fail;
  status = orm_redis_verify_query_engine(state, limits, error);
  if (status != ORM_STATUS_OK) goto fail;
  out_backend->ops = &orm_redis_backend_ops;
  out_backend->context = state;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
fail:
  orm_redis_backend_destroy(state);
  return status;
}

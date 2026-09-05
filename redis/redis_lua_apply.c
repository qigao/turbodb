#include "redis_lua_apply.h"

#include "salts_error.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REDIS_LUA_APPLY_ARGUMENT_COUNT 11
#define REDIS_LUA_APPLY_MAX_REPLY_BYTES 512u
#define REDIS_LUA_APPLY_U64_TEXT_BYTES 21u

static const char redis_lua_apply_script[] =
    "local function decimal_compare(left, right)\n"
    "  if #left ~= #right then return #left < #right and -1 or 1 end\n"
    "  if left == right then return 0 end\n"
    "  return left < right and -1 or 1\n"
    "end\n"
    "local function decimal_increment(value)\n"
    "  local chars = {}\n"
    "  local carry = 1\n"
    "  for index = #value, 1, -1 do\n"
    "    local digit = string.byte(value, index) - 48 + carry\n"
    "    if digit == 10 then chars[index] = '0' else chars[index] = string.char(48 + digit); carry = 0 end\n"
    "  end\n"
    "  if carry == 1 then table.insert(chars, 1, '1') end\n"
    "  return table.concat(chars)\n"
    "end\n"
    "local applied = redis.call('HGET', KEYS[1], 'applied_index') or '0'\n"
    "local requested = ARGV[1]\n"
    "if not string.match(applied, '^0$') and not string.match(applied, '^[1-9][0-9]*$') then\n"
    "  error('invalid applied_index metadata')\n"
    "end\n"
    "local ordering = decimal_compare(requested, applied)\n"
    "if ordering < 0 then return {'GAP', applied} end\n"
    "if ordering == 0 then\n"
    "  local applied_term = redis.call('HGET', KEYS[1], 'term')\n"
    "  local applied_command_id = redis.call('HGET', KEYS[1], 'command_id')\n"
    "  if applied_term ~= ARGV[2] or applied_command_id ~= ARGV[3] then\n"
    "    return {'CONFLICT', tostring(applied)}\n"
    "  end\n"
    "  return {'REPLAYED', applied}\n"
    "end\n"
    "if requested ~= decimal_increment(applied) then return {'GAP', applied} end\n"
    "redis.call('HSET', KEYS[2], ARGV[4], ARGV[5])\n"
    "redis.call('HSET', KEYS[1], 'applied_index', ARGV[1], 'term', ARGV[2], "
    "'command_id', ARGV[3])\n"
    "redis.call('XADD', KEYS[3], '*', 'index', ARGV[1], 'term', ARGV[2], "
    "'command_id', ARGV[3], 'field', ARGV[4], 'value', ARGV[5])\n"
    "return {'APPLIED', ARGV[1]}\n";

typedef struct redis_lua_apply_impl {
  redis_cflow_stream stream;
  int terminal;
  redis_lua_apply_step terminal_step;
} redis_lua_apply_impl;

static redis_lua_apply_impl *redis_lua_apply_impl_get(
    const redis_lua_apply *operation) {
  return operation != NULL ? (redis_lua_apply_impl *)operation->impl : NULL;
}

static int redis_lua_apply_tag(const char *key, size_t key_length,
                               const char **out_tag, size_t *out_tag_length) {
  size_t start;
  size_t index;
  if (key == NULL || key_length == 0u || out_tag == NULL || out_tag_length == NULL)
    return SALTS_EINVAL;
  for (start = 0u; start < key_length; ++start) {
    if (key[start] != '{') continue;
    for (index = start + 1u; index < key_length; ++index) {
      if (key[index] != '}') continue;
      if (index == start + 1u) return SALTS_EINVAL;
      *out_tag = key + start + 1u;
      *out_tag_length = index - start - 1u;
      return SALTS_OK;
    }
    return SALTS_EINVAL;
  }
  return SALTS_EINVAL;
}

static int redis_lua_apply_validate(const redis_lua_apply_request *request) {
  const char *metadata_tag;
  const char *state_tag;
  const char *outbox_tag;
  size_t metadata_tag_length;
  size_t state_tag_length;
  size_t outbox_tag_length;
  int status;
  if (request == NULL || request->index == 0u || request->term == 0u ||
      request->command_id == NULL || request->command_id_length == 0u ||
      request->field == NULL || request->field_length == 0u ||
      request->value == NULL)
    return SALTS_EINVAL;
  status = redis_lua_apply_tag(request->metadata_key, request->metadata_key_length,
                               &metadata_tag, &metadata_tag_length);
  if (status != SALTS_OK) return status;
  status = redis_lua_apply_tag(request->state_key, request->state_key_length,
                               &state_tag, &state_tag_length);
  if (status != SALTS_OK) return status;
  status = redis_lua_apply_tag(request->outbox_key, request->outbox_key_length,
                               &outbox_tag, &outbox_tag_length);
  if (status != SALTS_OK) return status;
  if (metadata_tag_length != state_tag_length || metadata_tag_length != outbox_tag_length ||
      memcmp(metadata_tag, state_tag, metadata_tag_length) != 0 ||
      memcmp(metadata_tag, outbox_tag, metadata_tag_length) != 0)
    return SALTS_EINVAL;
  return SALTS_OK;
}

static int redis_lua_apply_parse_u64(const redis_reply_t *reply, uint64_t *out_value) {
  const char *text;
  size_t length;
  size_t index;
  uint64_t value = 0u;
  if (reply == NULL || out_value == NULL) return SALTS_EINVAL;
  if (reply->type == REDIS_REPLY_INTEGER) {
    if (reply->integer < 0) return SALTS_EPROTO;
    *out_value = (uint64_t)reply->integer;
    return SALTS_OK;
  }
  if (reply->type != REDIS_REPLY_STRING && reply->type != REDIS_REPLY_BULK_STRING)
    return SALTS_EPROTO;
  text = reply->str;
  length = reply->len;
  if (text == NULL || length == 0u) return SALTS_EPROTO;
  for (index = 0u; index < length; ++index) {
    uint64_t digit;
    if (text[index] < '0' || text[index] > '9') return SALTS_EPROTO;
    digit = (uint64_t)(text[index] - '0');
    if (value > (UINT64_MAX - digit) / 10u) return SALTS_EPROTO;
    value = value * 10u + digit;
  }
  *out_value = value;
  return SALTS_OK;
}

static int redis_lua_apply_parse_kind(const redis_reply_t *reply,
                                      redis_lua_apply_receipt_kind *out_kind) {
  if (reply == NULL || out_kind == NULL ||
      (reply->type != REDIS_REPLY_STRING && reply->type != REDIS_REPLY_BULK_STRING) ||
      reply->str == NULL)
    return SALTS_EPROTO;
  if (reply->len == 7u && memcmp(reply->str, "APPLIED", 7u) == 0)
    *out_kind = REDIS_LUA_APPLY_APPLIED;
  else if (reply->len == 8u && memcmp(reply->str, "REPLAYED", 8u) == 0)
    *out_kind = REDIS_LUA_APPLY_REPLAYED;
  else if (reply->len == 3u && memcmp(reply->str, "GAP", 3u) == 0)
    *out_kind = REDIS_LUA_APPLY_GAP;
  else if (reply->len == 8u && memcmp(reply->str, "CONFLICT", 8u) == 0)
    *out_kind = REDIS_LUA_APPLY_CONFLICT;
  else
    return SALTS_EPROTO;
  return SALTS_OK;
}

static int redis_lua_apply_parse_receipt(const redis_reply_t *reply,
                                         redis_lua_apply_receipt *out_receipt) {
  int status;
  if (reply == NULL || out_receipt == NULL || reply->type != REDIS_REPLY_ARRAY ||
      reply->element_count != 2u || reply->elements == NULL)
    return SALTS_EPROTO;
  status = redis_lua_apply_parse_kind(reply->elements[0], &out_receipt->kind);
  if (status != SALTS_OK) return status;
  return redis_lua_apply_parse_u64(reply->elements[1], &out_receipt->applied_index);
}

static redis_lua_apply_step redis_lua_apply_finish(redis_lua_apply *operation,
                                                   redis_lua_apply_step step) {
  redis_lua_apply_impl *impl = redis_lua_apply_impl_get(operation);
  if (impl == NULL) return step;
  impl->terminal = 1;
  impl->terminal_step = step;
  return step;
}

int redis_lua_apply_open(redis_cflow_connection *connection,
                         const redis_lua_apply_request *request,
                         redis_lua_apply *out_operation) {
  static const char eval_command[] = "EVAL";
  static const char key_count[] = "3";
  const char *arguments[REDIS_LUA_APPLY_ARGUMENT_COUNT];
  size_t lengths[REDIS_LUA_APPLY_ARGUMENT_COUNT];
  char index_text[REDIS_LUA_APPLY_U64_TEXT_BYTES];
  char term_text[REDIS_LUA_APPLY_U64_TEXT_BYTES];
  redis_lua_apply_impl *impl;
  int status;
  int written;
  if (connection == NULL || out_operation == NULL || out_operation->impl != NULL)
    return SALTS_EINVAL;
  status = redis_lua_apply_validate(request);
  if (status != SALTS_OK) return status;
  written = snprintf(index_text, sizeof(index_text), "%" PRIu64 "", request->index);
  if (written < 0 || (size_t)written >= sizeof(index_text)) return SALTS_EINVAL;
  written = snprintf(term_text, sizeof(term_text), "%" PRIu64 "", request->term);
  if (written < 0 || (size_t)written >= sizeof(term_text)) return SALTS_EINVAL;
  arguments[0] = eval_command;
  lengths[0] = sizeof(eval_command) - 1u;
  arguments[1] = redis_lua_apply_script;
  lengths[1] = sizeof(redis_lua_apply_script) - 1u;
  arguments[2] = key_count;
  lengths[2] = sizeof(key_count) - 1u;
  arguments[3] = request->metadata_key;
  lengths[3] = request->metadata_key_length;
  arguments[4] = request->state_key;
  lengths[4] = request->state_key_length;
  arguments[5] = request->outbox_key;
  lengths[5] = request->outbox_key_length;
  arguments[6] = index_text;
  lengths[6] = strlen(index_text);
  arguments[7] = term_text;
  lengths[7] = strlen(term_text);
  arguments[8] = request->command_id;
  lengths[8] = request->command_id_length;
  arguments[9] = request->field;
  lengths[9] = request->field_length;
  arguments[10] = request->value;
  lengths[10] = request->value_length;
  impl = (redis_lua_apply_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  status = redis_cflow_command_open(connection, REDIS_LUA_APPLY_ARGUMENT_COUNT,
                                    arguments, lengths, REDIS_LUA_APPLY_MAX_REPLY_BYTES,
                                    &impl->stream);
  if (status != SALTS_OK) {
    free(impl);
    return status;
  }
  out_operation->impl = impl;
  return SALTS_OK;
}

redis_lua_apply_step redis_lua_apply_next(redis_lua_apply *operation) {
  redis_lua_apply_step step = REDIS_LUA_APPLY_STEP_INIT;
  redis_lua_apply_impl *impl = redis_lua_apply_impl_get(operation);
  redis_cflow_stream_step native;
  if (impl == NULL) {
    step.receipt.status = SALTS_EINVAL;
    return step;
  }
  if (impl->terminal) return impl->terminal_step;
  native = redis_cflow_stream_next(&impl->stream);
  if (native.kind == REDIS_CFLOW_STREAM_WAIT) {
    step.kind = REDIS_LUA_APPLY_WAIT;
    step.waitable = native.waitable;
    step.receipt.outcome = native.outcome;
    return step;
  }
  step.receipt.status = native.status;
  step.receipt.outcome = native.outcome;
  step.receipt.server_error = native.server_error;
  if (native.kind == REDIS_CFLOW_STREAM_ERROR) {
    if (native.outcome == REDIS_COMMAND_SEND_UNCERTAIN ||
        native.outcome == REDIS_COMMAND_REPLY_UNKNOWN) {
      step.kind = REDIS_LUA_APPLY_DONE;
      step.receipt.kind = REDIS_LUA_APPLY_COMMIT_UNKNOWN;
    } else {
      step.kind = REDIS_LUA_APPLY_STEP_ERROR;
      step.receipt.kind = REDIS_LUA_APPLY_ERROR;
    }
    redis_reply_free(native.item);
    return redis_lua_apply_finish(operation, step);
  }
  if (native.kind != REDIS_CFLOW_STREAM_ITEM ||
      redis_lua_apply_parse_receipt(native.item, &step.receipt) != SALTS_OK) {
    redis_reply_free(native.item);
    step.kind = REDIS_LUA_APPLY_STEP_ERROR;
    step.receipt.kind = REDIS_LUA_APPLY_ERROR;
    step.receipt.status = SALTS_EPROTO;
    (void)redis_cflow_stream_cancel(&impl->stream);
    return redis_lua_apply_finish(operation, step);
  }
  redis_reply_free(native.item);
  native = redis_cflow_stream_next(&impl->stream);
  if (native.kind != REDIS_CFLOW_STREAM_DONE) {
    step.kind = REDIS_LUA_APPLY_STEP_ERROR;
    step.receipt.kind = REDIS_LUA_APPLY_ERROR;
    step.receipt.status = native.status != SALTS_OK ? native.status : SALTS_EPROTO;
    step.receipt.outcome = native.outcome;
    step.receipt.server_error = native.server_error;
    redis_reply_free(native.item);
    (void)redis_cflow_stream_cancel(&impl->stream);
    return redis_lua_apply_finish(operation, step);
  }
  step.kind = REDIS_LUA_APPLY_DONE;
  step.receipt.status = SALTS_OK;
  step.receipt.outcome = REDIS_COMMAND_REPLIED;
  return redis_lua_apply_finish(operation, step);
}

int redis_lua_apply_cancel(redis_lua_apply *operation) {
  redis_lua_apply_impl *impl = redis_lua_apply_impl_get(operation);
  int status;
  if (impl == NULL) return SALTS_EINVAL;
  status = redis_cflow_stream_cancel(&impl->stream);
  if (status != SALTS_OK) return status;
  status = redis_cflow_stream_destroy(&impl->stream);
  if (status != SALTS_OK) return status;
  free(impl);
  operation->impl = NULL;
  return SALTS_OK;
}

int redis_lua_apply_destroy(redis_lua_apply *operation) {
  return redis_lua_apply_cancel(operation);
}

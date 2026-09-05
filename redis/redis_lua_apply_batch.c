#include "redis_lua_apply_batch.h"

#include "salts_error.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REDIS_LUA_APPLY_BATCH_KEY_COUNT 4
#define REDIS_LUA_APPLY_BATCH_COMMAND_PREFIX_ARGUMENTS 7u
#define REDIS_LUA_APPLY_BATCH_RECORD_ARGUMENTS 4u
#define REDIS_LUA_APPLY_BATCH_MAX_REPLY_BYTES 512u
#define REDIS_LUA_APPLY_BATCH_U64_TEXT_BYTES 21u

/*
 * Redis does not roll back writes that precede a script runtime error. The
 * script therefore performs every type and range check before its first write,
 * writes metadata last as the commit marker, and callers treat an unexpected
 * server error as commit-unknown for explicit reconciliation.
 */
static const char redis_lua_apply_batch_script[] =
    "local MAX_U64 = '18446744073709551615'\n"
    "local function decimal_compare(left, right)\n"
    "  if #left ~= #right then return #left < #right and -1 or 1 end\n"
    "  if left == right then return 0 end\n"
    "  return left < right and -1 or 1\n"
    "end\n"
    "local function valid_u64(value)\n"
    "  if not string.match(value, '^0$') and not string.match(value, '^[1-9][0-9]*$') then return false end\n"
    "  return #value < #MAX_U64 or (#value == #MAX_U64 and decimal_compare(value, MAX_U64) <= 0)\n"
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
    "local function key_type_is(key, expected)\n"
    "  local kind = redis.call('TYPE', key)['ok']\n"
    "  return kind == 'none' or kind == expected\n"
    "end\n"
    "if (#ARGV == 0) or (#ARGV % 4 ~= 0) then return {'INVALID', 'arguments'} end\n"
    "if not key_type_is(KEYS[1], 'hash') or not key_type_is(KEYS[2], 'hash') or\n"
    "   not key_type_is(KEYS[3], 'hash') or not key_type_is(KEYS[4], 'stream') then\n"
    "  return {'INVALID', 'key_type'}\n"
    "end\n"
    "local applied = redis.call('HGET', KEYS[1], 'applied_index') or '0'\n"
    "if not valid_u64(applied) then return {'INVALID', 'applied_index'} end\n"
    "local previous = nil\n"
    "for offset = 1, #ARGV, 4 do\n"
    "  local index = ARGV[offset]\n"
    "  local term = ARGV[offset + 1]\n"
    "  local command_id = ARGV[offset + 2]\n"
    "  if not valid_u64(index) or index == '0' or not valid_u64(term) or term == '0' or #command_id == 0 then\n"
    "    return {'INVALID', 'record'}\n"
    "  end\n"
    "  if previous ~= nil and index ~= decimal_increment(previous) then\n"
    "    return {'INVALID', 'range'}\n"
    "  end\n"
    "  previous = index\n"
    "end\n"
    "local cursor = applied\n"
    "local applied_any = false\n"
    "local final_term = nil\n"
    "local final_command_id = nil\n"
    "for offset = 1, #ARGV, 4 do\n"
    "  local index = ARGV[offset]\n"
    "  local term = ARGV[offset + 1]\n"
    "  local command_id = ARGV[offset + 2]\n"
    "  local payload = ARGV[offset + 3]\n"
    "  if decimal_compare(index, applied) <= 0 then\n"
    "    local identity = redis.call('HGET', KEYS[3], index)\n"
    "    local stored_payload = redis.call('HGET', KEYS[2], index)\n"
    "    if identity ~= term .. string.char(0) .. command_id or stored_payload ~= payload then\n"
    "      return {'CONFLICT', applied}\n"
    "    end\n"
    "  else\n"
    "    if index ~= decimal_increment(cursor) then return {'GAP', applied} end\n"
    "    redis.call('HSET', KEYS[2], index, payload)\n"
    "    redis.call('HSET', KEYS[3], index, term .. string.char(0) .. command_id)\n"
    "    redis.call('XADD', KEYS[4], '*', 'index', index, 'term', term, 'command_id', command_id, 'payload', payload)\n"
    "    cursor = index\n"
    "    final_term = term\n"
    "    final_command_id = command_id\n"
    "    applied_any = true\n"
    "  end\n"
    "end\n"
    "if applied_any then\n"
    "  redis.call('HSET', KEYS[1], 'applied_index', cursor, 'term', final_term, 'command_id', final_command_id)\n"
    "  return {'APPLIED', cursor}\n"
    "end\n"
    "return {'REPLAYED', applied}\n";

typedef struct redis_lua_apply_batch_impl {
  redis_cflow_stream stream;
  int terminal;
  redis_lua_apply_batch_step terminal_step;
} redis_lua_apply_batch_impl;

static redis_lua_apply_batch_impl *redis_lua_apply_batch_impl_get(
    const redis_lua_apply_batch *operation) {
  return operation != NULL ? (redis_lua_apply_batch_impl *)operation->impl : NULL;
}

static int redis_lua_apply_batch_tag(const char *key, size_t key_length,
                                     const char **out_tag,
                                     size_t *out_tag_length) {
  size_t start;
  size_t index;
  if (key == NULL || key_length == 0u || out_tag == NULL ||
      out_tag_length == NULL)
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

static int redis_lua_apply_batch_validate(
    const redis_lua_apply_batch_request *request) {
  const char *metadata_tag;
  const char *journal_tag;
  const char *identity_tag;
  const char *outbox_tag;
  size_t metadata_tag_length;
  size_t journal_tag_length;
  size_t identity_tag_length;
  size_t outbox_tag_length;
  size_t index;
  int status;
  if (request == NULL || request->records == NULL || request->record_count == 0u ||
      request->record_count > REDIS_LUA_APPLY_BATCH_MAX_RECORDS)
    return SALTS_EINVAL;
  status = redis_lua_apply_batch_tag(request->metadata_key,
                                     request->metadata_key_length, &metadata_tag,
                                     &metadata_tag_length);
  if (status != SALTS_OK) return status;
  status = redis_lua_apply_batch_tag(request->journal_key,
                                     request->journal_key_length, &journal_tag,
                                     &journal_tag_length);
  if (status != SALTS_OK) return status;
  status = redis_lua_apply_batch_tag(request->identity_key,
                                     request->identity_key_length, &identity_tag,
                                     &identity_tag_length);
  if (status != SALTS_OK) return status;
  status = redis_lua_apply_batch_tag(request->outbox_key,
                                     request->outbox_key_length, &outbox_tag,
                                     &outbox_tag_length);
  if (status != SALTS_OK) return status;
  if (metadata_tag_length != journal_tag_length ||
      metadata_tag_length != identity_tag_length ||
      metadata_tag_length != outbox_tag_length ||
      memcmp(metadata_tag, journal_tag, metadata_tag_length) != 0 ||
      memcmp(metadata_tag, identity_tag, metadata_tag_length) != 0 ||
      memcmp(metadata_tag, outbox_tag, metadata_tag_length) != 0)
    return SALTS_EINVAL;
  for (index = 0u; index < request->record_count; ++index) {
    const redis_lua_apply_batch_record *record = &request->records[index];
    if (record->index == 0u || record->term == 0u ||
        record->command_id == NULL || record->command_id_length == 0u ||
        record->payload == NULL)
      return SALTS_EINVAL;
    if (index != 0u &&
        (request->records[index - 1u].index == UINT64_MAX ||
         record->index != request->records[index - 1u].index + UINT64_C(1)))
      return SALTS_EINVAL;
  }
  return SALTS_OK;
}

static int redis_lua_apply_batch_parse_u64(const redis_reply_t *reply,
                                           uint64_t *out_value) {
  const char *text;
  size_t length;
  size_t index;
  uint64_t value = 0u;
  if (reply == NULL || out_value == NULL ||
      (reply->type != REDIS_REPLY_STRING && reply->type != REDIS_REPLY_BULK_STRING))
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

static int redis_lua_apply_batch_parse_kind(
    const redis_reply_t *reply, redis_lua_apply_receipt_kind *out_kind) {
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

static int redis_lua_apply_batch_parse_receipt(
    const redis_reply_t *reply, redis_lua_apply_receipt *out_receipt) {
  int status;
  if (reply == NULL || out_receipt == NULL || reply->type != REDIS_REPLY_ARRAY ||
      reply->element_count != 2u || reply->elements == NULL)
    return SALTS_EPROTO;
  status = redis_lua_apply_batch_parse_kind(reply->elements[0], &out_receipt->kind);
  if (status != SALTS_OK) return status;
  return redis_lua_apply_batch_parse_u64(reply->elements[1],
                                          &out_receipt->applied_index);
}

static redis_lua_apply_batch_step redis_lua_apply_batch_finish(
    redis_lua_apply_batch *operation, redis_lua_apply_batch_step step) {
  redis_lua_apply_batch_impl *impl = redis_lua_apply_batch_impl_get(operation);
  if (impl == NULL) return step;
  impl->terminal = 1;
  impl->terminal_step = step;
  return step;
}

int redis_lua_apply_batch_open(
    redis_cflow_connection *connection,
    const redis_lua_apply_batch_request *request,
    redis_lua_apply_batch *out_operation) {
  static const char eval_command[] = "EVAL";
  static const char key_count[] = "4";
  const char **arguments;
  size_t *lengths;
  char *number_text;
  size_t argument_count;
  size_t record_index;
  redis_lua_apply_batch_impl *impl;
  int status;
  if (connection == NULL || out_operation == NULL || out_operation->impl != NULL)
    return SALTS_EINVAL;
  status = redis_lua_apply_batch_validate(request);
  if (status != SALTS_OK) return status;
  argument_count = REDIS_LUA_APPLY_BATCH_COMMAND_PREFIX_ARGUMENTS +
                   request->record_count * REDIS_LUA_APPLY_BATCH_RECORD_ARGUMENTS;
  if (argument_count > (size_t)INT_MAX) return SALTS_ERANGE;
  arguments = (const char **)calloc(argument_count, sizeof(*arguments));
  lengths = (size_t *)calloc(argument_count, sizeof(*lengths));
  number_text = (char *)calloc(
      request->record_count, 2u * REDIS_LUA_APPLY_BATCH_U64_TEXT_BYTES);
  if (arguments == NULL || lengths == NULL || number_text == NULL) {
    free(number_text);
    free(lengths);
    free(arguments);
    return SALTS_ENOMEM;
  }
  arguments[0] = eval_command;
  lengths[0] = sizeof(eval_command) - 1u;
  arguments[1] = redis_lua_apply_batch_script;
  lengths[1] = sizeof(redis_lua_apply_batch_script) - 1u;
  arguments[2] = key_count;
  lengths[2] = sizeof(key_count) - 1u;
  arguments[3] = request->metadata_key;
  lengths[3] = request->metadata_key_length;
  arguments[4] = request->journal_key;
  lengths[4] = request->journal_key_length;
  arguments[5] = request->identity_key;
  lengths[5] = request->identity_key_length;
  arguments[6] = request->outbox_key;
  lengths[6] = request->outbox_key_length;
  for (record_index = 0u; record_index < request->record_count; ++record_index) {
    const redis_lua_apply_batch_record *record = &request->records[record_index];
    size_t argument_index =
        REDIS_LUA_APPLY_BATCH_COMMAND_PREFIX_ARGUMENTS +
        record_index * REDIS_LUA_APPLY_BATCH_RECORD_ARGUMENTS;
    char *index_text =
        number_text + record_index * 2u * REDIS_LUA_APPLY_BATCH_U64_TEXT_BYTES;
    char *term_text = index_text + REDIS_LUA_APPLY_BATCH_U64_TEXT_BYTES;
    int index_written = snprintf(index_text,
                                 REDIS_LUA_APPLY_BATCH_U64_TEXT_BYTES,
                                 "%" PRIu64, record->index);
    int term_written;
    if (index_written < 0 ||
        (size_t)index_written >= REDIS_LUA_APPLY_BATCH_U64_TEXT_BYTES) {
      free(number_text);
      free(lengths);
      free(arguments);
      return SALTS_ERANGE;
    }
    term_written = snprintf(term_text, REDIS_LUA_APPLY_BATCH_U64_TEXT_BYTES,
                            "%" PRIu64, record->term);
    if (term_written < 0 ||
        (size_t)term_written >= REDIS_LUA_APPLY_BATCH_U64_TEXT_BYTES) {
      free(number_text);
      free(lengths);
      free(arguments);
      return SALTS_ERANGE;
    }
    arguments[argument_index] = index_text;
    lengths[argument_index] = (size_t)index_written;
    arguments[argument_index + 1u] = term_text;
    lengths[argument_index + 1u] = (size_t)term_written;
    arguments[argument_index + 2u] = record->command_id;
    lengths[argument_index + 2u] = record->command_id_length;
    arguments[argument_index + 3u] = record->payload;
    lengths[argument_index + 3u] = record->payload_length;
  }
  impl = (redis_lua_apply_batch_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) {
    free(number_text);
    free(lengths);
    free(arguments);
    return SALTS_ENOMEM;
  }
  status = redis_cflow_command_open(
      connection, (int)argument_count, arguments, lengths,
      REDIS_LUA_APPLY_BATCH_MAX_REPLY_BYTES, &impl->stream);
  free(number_text);
  free(lengths);
  free(arguments);
  if (status != SALTS_OK) {
    free(impl);
    return status;
  }
  out_operation->impl = impl;
  return SALTS_OK;
}

redis_lua_apply_batch_step redis_lua_apply_batch_next(
    redis_lua_apply_batch *operation) {
  redis_lua_apply_batch_step step = REDIS_LUA_APPLY_BATCH_STEP_INIT;
  redis_lua_apply_batch_impl *impl = redis_lua_apply_batch_impl_get(operation);
  redis_cflow_stream_step native;
  if (impl == NULL) {
    step.receipt.status = SALTS_EINVAL;
    return step;
  }
  if (impl->terminal) return impl->terminal_step;
  native = redis_cflow_stream_next(&impl->stream);
  if (native.kind == REDIS_CFLOW_STREAM_WAIT) {
    step.kind = REDIS_LUA_APPLY_BATCH_WAIT;
    step.waitable = native.waitable;
    step.receipt.outcome = native.outcome;
    return step;
  }
  step.receipt.status = native.status;
  step.receipt.outcome = native.outcome;
  step.receipt.server_error = native.server_error;
  if (native.kind == REDIS_CFLOW_STREAM_ERROR) {
    step.kind = native.outcome == REDIS_COMMAND_NOT_SENT
                    ? REDIS_LUA_APPLY_BATCH_STEP_ERROR
                    : REDIS_LUA_APPLY_BATCH_DONE;
    step.receipt.kind = native.outcome == REDIS_COMMAND_NOT_SENT
                            ? REDIS_LUA_APPLY_ERROR
                            : REDIS_LUA_APPLY_COMMIT_UNKNOWN;
    redis_reply_free(native.item);
    return redis_lua_apply_batch_finish(operation, step);
  }
  if (native.kind != REDIS_CFLOW_STREAM_ITEM ||
      redis_lua_apply_batch_parse_receipt(native.item, &step.receipt) != SALTS_OK) {
    redis_reply_free(native.item);
    step.kind = REDIS_LUA_APPLY_BATCH_DONE;
    step.receipt.kind = REDIS_LUA_APPLY_COMMIT_UNKNOWN;
    step.receipt.status = SALTS_EPROTO;
    (void)redis_cflow_stream_cancel(&impl->stream);
    return redis_lua_apply_batch_finish(operation, step);
  }
  redis_reply_free(native.item);
  native = redis_cflow_stream_next(&impl->stream);
  if (native.kind != REDIS_CFLOW_STREAM_DONE) {
    step.kind = native.outcome == REDIS_COMMAND_NOT_SENT
                    ? REDIS_LUA_APPLY_BATCH_STEP_ERROR
                    : REDIS_LUA_APPLY_BATCH_DONE;
    step.receipt.kind = native.outcome == REDIS_COMMAND_NOT_SENT
                            ? REDIS_LUA_APPLY_ERROR
                            : REDIS_LUA_APPLY_COMMIT_UNKNOWN;
    step.receipt.status = native.status != SALTS_OK ? native.status : SALTS_EPROTO;
    step.receipt.outcome = native.outcome;
    step.receipt.server_error = native.server_error;
    redis_reply_free(native.item);
    (void)redis_cflow_stream_cancel(&impl->stream);
    return redis_lua_apply_batch_finish(operation, step);
  }
  step.kind = REDIS_LUA_APPLY_BATCH_DONE;
  step.receipt.status = SALTS_OK;
  step.receipt.outcome = REDIS_COMMAND_REPLIED;
  return redis_lua_apply_batch_finish(operation, step);
}

int redis_lua_apply_batch_cancel(redis_lua_apply_batch *operation) {
  redis_lua_apply_batch_impl *impl = redis_lua_apply_batch_impl_get(operation);
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

int redis_lua_apply_batch_destroy(redis_lua_apply_batch *operation) {
  return redis_lua_apply_batch_cancel(operation);
}

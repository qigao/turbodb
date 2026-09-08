#include "redis_internal.h"
#include "redis_resp_parser.h"
#include "tstr.h"

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define REDIS_REPLY_OWNER_MAGIC UINT32_C(0x52535032)
#define REDIS_TSTR_ALLOCATION_OVERHEAD (sizeof(size_t) * 2u + 1u)
#define REDIS_RESP_PARSE_OOM (-3)
#define REDIS_RESP_PARSE_LIMIT (-2)

typedef struct redis_reply_string_s {
  tstr value;
  struct redis_reply_string_s *next;
} redis_reply_string_t;

typedef struct {
  uint32_t magic;
  mem_pool_t pool;
  redis_reply_string_t *strings;
  size_t allocation_bytes;
  size_t max_allocation_bytes;
  int failure;
} redis_reply_owner_t;

typedef struct {
  redis_reply_owner_t *owner;
  redis_reply_t reply;
} redis_reply_allocation_t;

typedef struct redis_resp_parse_frame {
  redis_reply_t *array;
  size_t next;
} redis_resp_parse_frame;

typedef struct redis_resp_reply_reader_impl {
  redis_reply_owner_t *owner;
  redis_reply_t *root;
  redis_resp_parse_frame frames[REDIS_RESP_MAX_DEPTH + 1u];
  size_t depth;
  size_t offset;
  size_t line_scan;
  size_t bulk_header_len;
  size_t bulk_len;
  int bulk_pending;
} redis_resp_reply_reader_impl;

static int redis_reply_owner_charge(redis_reply_owner_t *owner, size_t bytes) {
  if (!owner || bytes > owner->max_allocation_bytes - owner->allocation_bytes) {
    if (owner) owner->failure = REDIS_RESP_PARSE_LIMIT;
    return -1;
  }
  owner->allocation_bytes += bytes;
  return 0;
}

static redis_reply_t *redis_reply_alloc(redis_reply_owner_t *owner) {
  redis_reply_allocation_t *allocation;
  if (redis_reply_owner_charge(owner, sizeof(*allocation)) != 0) return NULL;
  allocation = mem_alloc(&owner->pool, sizeof(*allocation));
  if (!allocation) {
    owner->failure = REDIS_RESP_PARSE_OOM;
    return NULL;
  }
  memset(allocation, 0, sizeof(*allocation));
  allocation->owner = owner;
  return &allocation->reply;
}

static tstr redis_reply_string_create(redis_reply_owner_t *owner, vstr value) {
  redis_reply_string_t *tracked;
  tstr string;
  size_t allocation_bytes;
  if (value.len > SIZE_MAX - sizeof(*tracked) - REDIS_TSTR_ALLOCATION_OVERHEAD) {
    owner->failure = -2;
    return NULL;
  }
  allocation_bytes = sizeof(*tracked) + REDIS_TSTR_ALLOCATION_OVERHEAD + value.len;
  if (redis_reply_owner_charge(owner, allocation_bytes) != 0) return NULL;
  string = tstr_from_v(value);
  if (!string) {
    owner->failure = REDIS_RESP_PARSE_OOM;
    return NULL;
  }
  tracked = mem_alloc(&owner->pool, sizeof(*tracked));
  if (!tracked) {
    owner->failure = REDIS_RESP_PARSE_OOM;
    tstr_free(string);
    return NULL;
  }
  tracked->value = string;
  tracked->next = owner->strings;
  owner->strings = tracked;
  return string;
}

static void redis_reply_strings_destroy(redis_reply_owner_t *owner) {
  redis_reply_string_t *tracked;
  if (!owner) return;
  tracked = owner->strings;
  while (tracked) {
    tstr_free(tracked->value);
    tracked = tracked->next;
  }
  owner->strings = NULL;
}

static void redis_reply_owner_destroy(redis_reply_owner_t *owner) {
  if (!owner) return;
  redis_reply_strings_destroy(owner);
  mem_destroy(&owner->pool);
  owner->magic = 0u;
  free(owner);
}

static int redis_resp_reply_reader_fail(redis_resp_reply_reader *reader,
                                        int status) {
  redis_resp_reply_reader_impl *impl;
  if (!reader) return status;
  impl = (redis_resp_reply_reader_impl *)reader->impl;
  if (impl) {
    redis_reply_owner_destroy(impl->owner);
    free(impl);
  }
  reader->impl = NULL;
  return status;
}

static int redis_resp_reply_reader_begin(redis_resp_reply_reader *reader) {
  redis_resp_reply_reader_impl *impl;
  redis_reply_owner_t *owner;
  if (reader->impl) return 0;
  impl = (redis_resp_reply_reader_impl *)calloc(1u, sizeof(*impl));
  owner = (redis_reply_owner_t *)calloc(1u, sizeof(*owner));
  if (!impl || !owner) {
    free(owner);
    free(impl);
    return REDIS_RESP_PARSE_OOM;
  }
  owner->max_allocation_bytes = reader->max_reply_bytes;
  if (redis_reply_owner_charge(owner, sizeof(*owner)) != 0) {
    free(owner);
    free(impl);
    return REDIS_RESP_PARSE_LIMIT;
  }
  if (mem_init(&owner->pool, 0) != 0) {
    free(owner);
    free(impl);
    return REDIS_RESP_PARSE_OOM;
  }
  owner->magic = REDIS_REPLY_OWNER_MAGIC;
  impl->owner = owner;
  reader->impl = impl;
  return 0;
}

static int redis_resp_reply_reader_complete_value(
    redis_resp_reply_reader_impl *impl) {
  for (;;) {
    redis_resp_parse_frame *frame;
    if (impl->depth == 0u) return 1;
    frame = &impl->frames[impl->depth - 1u];
    ++frame->next;
    if (frame->next < frame->array->element_count) return 0;
    --impl->depth;
  }
}

static int redis_resp_reply_reader_attach(
    redis_resp_reply_reader_impl *impl, redis_reply_t *reply,
    int nonempty_array) {
  if (impl->depth == 0u) {
    if (impl->root) return -1;
    impl->root = reply;
  } else {
    redis_resp_parse_frame *parent = &impl->frames[impl->depth - 1u];
    if (parent->next >= parent->array->element_count) return -1;
    parent->array->elements[parent->next] = reply;
  }
  if (nonempty_array) {
    if (impl->depth > REDIS_RESP_MAX_DEPTH) return -1;
    impl->frames[impl->depth].array = reply;
    impl->frames[impl->depth].next = 0u;
    ++impl->depth;
    return 0;
  }
  return redis_resp_reply_reader_complete_value(impl);
}

static int redis_resp_reply_reader_line(
    redis_resp_reply_reader_impl *impl, const redis_resp_buffer *buffer,
    redis_resp_token_t *token) {
  size_t index = impl->line_scan > impl->offset
                     ? impl->line_scan : impl->offset;
  for (; index + 1u < buffer->used; ++index) {
    int parsed;
    if (buffer->data[index] != '\r' || buffer->data[index + 1u] != '\n')
      continue;
    parsed = redis_resp_scan_token(buffer->data + impl->offset,
                                   index + 2u - impl->offset, token);
    return parsed == 1 && token->header_len == index + 2u - impl->offset
               ? 1 : -1;
  }
  impl->line_scan = buffer->used > impl->offset
                        ? buffer->used - 1u : impl->offset;
  return 0;
}

static int redis_resp_reply_reader_finish(
    redis_resp_reply_reader *reader, redis_resp_reply_reader_impl *impl,
    redis_reply_t **reply) {
  int consumed;
  if (impl->offset > (size_t)INT_MAX)
    return redis_resp_reply_reader_fail(reader, -1);
  consumed = (int)impl->offset;
  *reply = impl->root;
  impl->owner = NULL;
  free(impl);
  reader->impl = NULL;
  return consumed;
}

void redis_resp_reply_reader_init(redis_resp_reply_reader *reader,
                                  size_t max_reply_bytes) {
  if (!reader) return;
  memset(reader, 0, sizeof(*reader));
  reader->max_reply_bytes = max_reply_bytes;
}

void redis_resp_reply_reader_destroy(redis_resp_reply_reader *reader) {
  if (!reader) return;
  (void)redis_resp_reply_reader_fail(reader, 0);
  reader->max_reply_bytes = 0u;
}

int redis_resp_reply_reader_next_buffer(redis_resp_buffer *buffer,
                                        redis_resp_reply_reader *reader,
                                        redis_reply_t **reply) {
  redis_resp_reply_reader_impl *impl;
  int status;
  if (!buffer || !buffer->data || !reader || !reply ||
      reader->max_reply_bytes == 0u)
    return -1;
  *reply = NULL;
  if (buffer->used == 0u) return 0;
  status = redis_resp_reply_reader_begin(reader);
  if (status != 0) return status;
  impl = (redis_resp_reply_reader_impl *)reader->impl;
  for (;;) {
    redis_reply_t *parsed;
    redis_resp_token_t token;
    int complete;
    if (impl->depth > REDIS_RESP_MAX_DEPTH)
      return redis_resp_reply_reader_fail(reader, -1);
    if (impl->bulk_pending) {
      size_t total;
      if (impl->bulk_len > SIZE_MAX - impl->bulk_header_len - 2u)
        return redis_resp_reply_reader_fail(reader, -1);
      total = impl->bulk_header_len + impl->bulk_len + 2u;
      if (total > (size_t)INT_MAX || impl->offset > (size_t)INT_MAX - total)
        return redis_resp_reply_reader_fail(reader, -1);
      if (buffer->used - impl->offset < total) return 0;
      if (buffer->data[impl->offset + total - 2u] != '\r' ||
          buffer->data[impl->offset + total - 1u] != '\n')
        return redis_resp_reply_reader_fail(reader, -1);
      parsed = redis_reply_alloc(impl->owner);
      if (!parsed)
        return redis_resp_reply_reader_fail(
            reader, impl->owner->failure != 0
                        ? impl->owner->failure : REDIS_RESP_PARSE_OOM);
      parsed->type = REDIS_REPLY_BULK_STRING;
      parsed->str = redis_reply_string_create(
          impl->owner,
          vstr_from_buf(buffer->data + impl->offset + impl->bulk_header_len,
                        impl->bulk_len));
      if (!parsed->str)
        return redis_resp_reply_reader_fail(
            reader, impl->owner->failure != 0
                        ? impl->owner->failure : REDIS_RESP_PARSE_OOM);
      parsed->len = impl->bulk_len;
      impl->offset += total;
      impl->line_scan = impl->offset;
      impl->bulk_pending = 0;
      complete = redis_resp_reply_reader_attach(impl, parsed, 0);
      if (complete < 0) return redis_resp_reply_reader_fail(reader, -1);
      if (complete > 0)
        return redis_resp_reply_reader_finish(reader, impl, reply);
      continue;
    }
    status = redis_resp_reply_reader_line(impl, buffer, &token);
    if (status <= 0)
      return status == 0 ? 0 : redis_resp_reply_reader_fail(reader, -1);
    if (token.header_len > (size_t)INT_MAX ||
        impl->offset > (size_t)INT_MAX - token.header_len)
      return redis_resp_reply_reader_fail(reader, -1);
    if (token.type == '$' && token.int_value >= 0) {
      if ((uint64_t)token.int_value > SIZE_MAX)
        return redis_resp_reply_reader_fail(reader, -1);
      impl->bulk_pending = 1;
      impl->bulk_header_len = token.header_len;
      impl->bulk_len = (size_t)token.int_value;
      continue;
    }
    parsed = redis_reply_alloc(impl->owner);
    if (!parsed)
      return redis_resp_reply_reader_fail(
          reader, impl->owner->failure != 0
                      ? impl->owner->failure : REDIS_RESP_PARSE_OOM);
    complete = 0;
    switch (token.type) {
      case '+':
      case '-':
        parsed->type = token.type == '+' ? REDIS_REPLY_STRING
                                         : REDIS_REPLY_ERROR;
        parsed->str = redis_reply_string_create(impl->owner, token.value);
        if (!parsed->str)
          return redis_resp_reply_reader_fail(
              reader, impl->owner->failure != 0
                          ? impl->owner->failure : REDIS_RESP_PARSE_OOM);
        parsed->len = token.value.len;
        break;
      case ':':
        parsed->type = REDIS_REPLY_INTEGER;
        parsed->integer = token.int_value;
        break;
      case '$':
        if (token.int_value != -1)
          return redis_resp_reply_reader_fail(reader, -1);
        parsed->type = REDIS_REPLY_NULL;
        break;
      case '*': {
        size_t count;
        if (token.int_value == -1) {
          parsed->type = REDIS_REPLY_NULL;
          break;
        }
        if (token.int_value < 0 || (uint64_t)token.int_value > SIZE_MAX)
          return redis_resp_reply_reader_fail(reader, -1);
        count = (size_t)token.int_value;
        parsed->type = REDIS_REPLY_ARRAY;
        parsed->element_count = count;
        if (count != 0u) {
          size_t bytes;
          if (count > SIZE_MAX / sizeof(*parsed->elements))
            return redis_resp_reply_reader_fail(reader, -1);
          bytes = count * sizeof(*parsed->elements);
          if (redis_reply_owner_charge(impl->owner, bytes) != 0)
            return redis_resp_reply_reader_fail(reader,
                                                 REDIS_RESP_PARSE_LIMIT);
          parsed->elements = mem_alloc_array(
              &impl->owner->pool, sizeof(*parsed->elements), count);
          if (!parsed->elements) {
            impl->owner->failure = REDIS_RESP_PARSE_OOM;
            return redis_resp_reply_reader_fail(reader,
                                                 REDIS_RESP_PARSE_OOM);
          }
          memset(parsed->elements, 0, bytes);
          complete = 1;
        }
        break;
      }
      default:
        return redis_resp_reply_reader_fail(reader, -1);
    }
    impl->offset += token.header_len;
    impl->line_scan = impl->offset;
    complete = redis_resp_reply_reader_attach(impl, parsed, complete);
    if (complete < 0) return redis_resp_reply_reader_fail(reader, -1);
    if (complete > 0)
      return redis_resp_reply_reader_finish(reader, impl, reply);
  }
}

int redis_resp_buffer_init(redis_resp_buffer *buffer, size_t initial_capacity) {
  if (!buffer || buffer->data || buffer->capacity || buffer->used ||
      initial_capacity == 0u)
    return -1;
  buffer->data = (char *)malloc(initial_capacity);
  if (!buffer->data) return -1;
  buffer->capacity = initial_capacity;
  return 0;
}

void redis_resp_buffer_destroy(redis_resp_buffer *buffer) {
  if (!buffer) return;
  free(buffer->data);
  memset(buffer, 0, sizeof(*buffer));
}

void redis_resp_buffer_reset(redis_resp_buffer *buffer) {
  if (buffer) buffer->used = 0u;
}

int redis_resp_buffer_append_bounded(redis_resp_buffer *buffer,
                                     const char *data, size_t len,
                                     size_t max_buffer_bytes) {
  char *new_buffer;
  size_t required;
  size_t capacity;
  if (!buffer || !buffer->data || max_buffer_bytes == 0u ||
      (!data && len != 0u))
    return -1;
  if (buffer->used > max_buffer_bytes || len > max_buffer_bytes - buffer->used)
    return -2;
  if (len == 0u) return 0;
  required = buffer->used + len;
  if (required <= buffer->capacity) {
    memcpy(buffer->data + buffer->used, data, len);
    buffer->used = required;
    return 0;
  }
  capacity = buffer->capacity;
  while (capacity < required && capacity <= max_buffer_bytes / 2u)
    capacity *= 2u;
  if (capacity < required) capacity = required;
  if (capacity > max_buffer_bytes) capacity = max_buffer_bytes;
  new_buffer = (char *)realloc(buffer->data, capacity);
  if (!new_buffer) return -1;
  buffer->data = new_buffer;
  buffer->capacity = capacity;
  memcpy(buffer->data + buffer->used, data, len);
  buffer->used = required;
  return 0;
}

int redis_resp_buffer_consume(redis_resp_buffer *buffer, size_t consumed) {
  if (!buffer || !buffer->data || consumed > buffer->used) return -1;
  if (consumed == 0u) return 0;
  memmove(buffer->data, buffer->data + consumed, buffer->used - consumed);
  buffer->used -= consumed;
  return 0;
}

void redis_resp_array_reader_init(redis_resp_array_reader *reader,
                                  size_t max_items,
                                  size_t max_reply_bytes) {
  if (!reader) return;
  memset(reader, 0, sizeof(*reader));
  reader->max_items = max_items;
  reader->max_reply_bytes = max_reply_bytes;
  redis_resp_reply_reader_init(&reader->item_reader, max_reply_bytes);
}

void redis_resp_array_reader_destroy(redis_resp_array_reader *reader) {
  if (!reader) return;
  redis_resp_reply_reader_destroy(&reader->item_reader);
  memset(reader, 0, sizeof(*reader));
}

static int redis_resp_array_header_ready(const redis_resp_buffer *buffer,
                                         redis_resp_array_reader *reader) {
  size_t scan;
  if (buffer->used == 0u) return 0;
  scan = reader->header_scan > 1u ? reader->header_scan : 1u;
  if (scan > buffer->used) scan = buffer->used;
  while (scan < buffer->used) {
    if (buffer->data[scan - 1u] == '\r' && buffer->data[scan] == '\n') {
      reader->header_scan = scan + 1u;
      return reader->header_scan <= reader->max_reply_bytes ? 1 : -1;
    }
    ++scan;
  }
  reader->header_scan = scan;
  return buffer->used < reader->max_reply_bytes ? 0 : -1;
}

redis_resp_array_step redis_resp_array_reader_next_buffer(
    redis_resp_buffer *buffer, redis_resp_array_reader *reader,
    redis_reply_t **item) {
  redis_resp_token_t token;
  int parsed;

  if (item) *item = NULL;
  if (!buffer || !buffer->data || !reader || !item || reader->max_items == 0u ||
      reader->max_reply_bytes == 0u)
    return REDIS_RESP_ARRAY_ERROR;
  if (reader->terminal)
    return REDIS_RESP_ARRAY_DONE;

  if (!reader->header_read) {
    if (buffer->used == 0u) return REDIS_RESP_ARRAY_NEED_MORE;
    if (buffer->data[0] == '-') {
      parsed = redis_resp_reply_reader_next_buffer(
          buffer, &reader->item_reader, item);
      if (parsed <= 0)
        return parsed == 0 ? REDIS_RESP_ARRAY_NEED_MORE :
               parsed == REDIS_RESP_PARSE_OOM ? REDIS_RESP_ARRAY_OOM :
               parsed == REDIS_RESP_PARSE_LIMIT ? REDIS_RESP_ARRAY_LIMIT
                                                : REDIS_RESP_ARRAY_ERROR;
      redis_resp_buffer_consume(buffer, (size_t)parsed);
      reader->terminal = 1;
      return REDIS_RESP_ARRAY_SERVER_ERROR;
    }
    if (buffer->data[0] != '*') return REDIS_RESP_ARRAY_ERROR;
    parsed = redis_resp_array_header_ready(buffer, reader);
    if (parsed <= 0)
      return parsed == 0 ? REDIS_RESP_ARRAY_NEED_MORE
                         : REDIS_RESP_ARRAY_LIMIT;
    parsed = redis_resp_scan_token(buffer->data, buffer->used, &token);
    if (parsed <= 0)
      return parsed == 0 ? REDIS_RESP_ARRAY_NEED_MORE
                         : REDIS_RESP_ARRAY_ERROR;
    if (token.type != '*' || token.int_value < 0 ||
        (uint64_t)token.int_value > reader->max_items)
      return REDIS_RESP_ARRAY_ERROR;
    reader->remaining = (size_t)token.int_value;
    reader->header_read = 1;
    redis_resp_buffer_consume(buffer, token.header_len);
  }

  if (reader->remaining == 0u) {
    reader->terminal = 1;
    return REDIS_RESP_ARRAY_DONE;
  }
  parsed = redis_resp_reply_reader_next_buffer(
      buffer, &reader->item_reader, item);
  if (parsed <= 0)
    return parsed == 0 ? REDIS_RESP_ARRAY_NEED_MORE :
           parsed == REDIS_RESP_PARSE_OOM ? REDIS_RESP_ARRAY_OOM :
           parsed == REDIS_RESP_PARSE_LIMIT ? REDIS_RESP_ARRAY_LIMIT
                                            : REDIS_RESP_ARRAY_ERROR;
  redis_resp_buffer_consume(buffer, (size_t)parsed);
  --reader->remaining;
  return REDIS_RESP_ARRAY_ITEM;
}

void redis_reply_free(redis_reply_t *reply) {
  redis_reply_allocation_t *allocation;
  redis_reply_owner_t *owner;
  if (!reply) return;
  allocation =
      (redis_reply_allocation_t *)((char *)reply - offsetof(redis_reply_allocation_t, reply));
  owner = allocation->owner;
  if (!owner || owner->magic != REDIS_REPLY_OWNER_MAGIC) return;
  redis_reply_strings_destroy(owner);
  owner->magic = 0;
  mem_destroy(&owner->pool);
  free(owner);
}

static int redis_error_token_is(const redis_reply_t *reply,
                                const char *token) {
  size_t token_len;
  if (!reply || reply->type != REDIS_REPLY_ERROR || !reply->str || !token)
    return 0;
  token_len = strlen(token);
  return reply->len >= token_len &&
         memcmp(reply->str, token, token_len) == 0 &&
         (reply->len == token_len || reply->str[token_len] == ' ');
}

redis_server_error_t redis_server_error_classify(const redis_reply_t *reply) {
  if (!reply || reply->type != REDIS_REPLY_ERROR)
    return REDIS_SERVER_ERROR_NONE;
  if (redis_error_token_is(reply, "ERR")) return REDIS_SERVER_ERROR_ERR;
  if (redis_error_token_is(reply, "BUSYGROUP"))
    return REDIS_SERVER_ERROR_BUSY_GROUP;
  if (redis_error_token_is(reply, "NOGROUP"))
    return REDIS_SERVER_ERROR_NO_GROUP;
  if (redis_error_token_is(reply, "WRONGTYPE"))
    return REDIS_SERVER_ERROR_WRONG_TYPE;
  if (redis_error_token_is(reply, "NOAUTH"))
    return REDIS_SERVER_ERROR_NO_AUTH;
  if (redis_error_token_is(reply, "MOVED")) return REDIS_SERVER_ERROR_MOVED;
  if (redis_error_token_is(reply, "ASK")) return REDIS_SERVER_ERROR_ASK;
  if (redis_error_token_is(reply, "TRYAGAIN"))
    return REDIS_SERVER_ERROR_TRY_AGAIN;
  if (redis_error_token_is(reply, "CLUSTERDOWN"))
    return REDIS_SERVER_ERROR_CLUSTER_DOWN;
  if (redis_error_token_is(reply, "READONLY"))
    return REDIS_SERVER_ERROR_READ_ONLY;
  if (redis_error_token_is(reply, "NOSCRIPT"))
    return REDIS_SERVER_ERROR_NO_SCRIPT;
  if (redis_error_token_is(reply, "LOADING"))
    return REDIS_SERVER_ERROR_LOADING;
  if (redis_error_token_is(reply, "OOM")) return REDIS_SERVER_ERROR_OOM;
  if (redis_error_token_is(reply, "EXECABORT"))
    return REDIS_SERVER_ERROR_EXEC_ABORT;
  if (redis_error_token_is(reply, "MASTERDOWN"))
    return REDIS_SERVER_ERROR_MASTER_DOWN;
  if (redis_error_token_is(reply, "MISCONF"))
    return REDIS_SERVER_ERROR_MISCONF;
  return REDIS_SERVER_ERROR_UNKNOWN;
}

int redis_argc_mul_add(int base, int count, int mul, int *argc_out) {
  long long total;
  if (base < 0 || count < 0 || mul < 0 || !argc_out) return -1;
  total = (long long)base + (long long)count * (long long)mul;
  if (total <= 0 || total > INT_MAX) return -1;
  *argc_out = (int)total;
  return 0;
}

const char **redis_argv_alloc(int argc) {
  if (argc <= 0) return NULL;
  return mem_alloc_array(mem_global(), sizeof(char *), (size_t)argc);
}

void redis_argv_free(const char **argv) { mem_free(mem_global(), (void *)argv); }

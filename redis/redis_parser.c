#include "redis_internal.h"
#include "redis_resp_parser.h"
#include "turbo_str.h"

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define REDIS_REPLY_OWNER_MAGIC UINT32_C(0x52535032)
#define REDIS_RECV_BUFFER_INITIAL_SIZE 16384u
#define REDIS_RESP_MIN_VALUE_SIZE 3u

typedef struct redis_reply_string_s {
  tstr_t value;
  struct redis_reply_string_s *next;
} redis_reply_string_t;

typedef struct {
  uint32_t magic;
  mem_pool_t pool;
  redis_reply_string_t *strings;
} redis_reply_owner_t;

typedef struct {
  redis_reply_owner_t *owner;
  redis_reply_t reply;
} redis_reply_allocation_t;

static redis_reply_t *redis_reply_alloc(redis_reply_owner_t *owner) {
  redis_reply_allocation_t *allocation;
  allocation = mem_alloc(&owner->pool, sizeof(*allocation));
  if (!allocation) return NULL;
  memset(allocation, 0, sizeof(*allocation));
  allocation->owner = owner;
  return &allocation->reply;
}

static tstr_t redis_reply_string_create(redis_reply_owner_t *owner, tstr_v value) {
  redis_reply_string_t *tracked;
  tstr_t string = tstr_from_v(value);
  if (!string) return NULL;
  tracked = mem_alloc(&owner->pool, sizeof(*tracked));
  if (!tracked) {
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

static int redis_parse_resp_value(const char *data, size_t len, size_t depth,
                                  redis_reply_owner_t *owner, redis_reply_t **reply) {
  redis_resp_token_t token;
  redis_reply_t *parsed;
  int token_rc;

  if (!reply || !owner || (!data && len != 0)) return -1;
  *reply = NULL;
  if (depth > REDIS_RESP_MAX_DEPTH) return -1;

  token_rc = redis_resp_scan_token(data, len, &token);
  if (token_rc <= 0) return token_rc;
  if (token.header_len > (size_t)INT_MAX) return -1;

  parsed = redis_reply_alloc(owner);
  if (!parsed) return -1;

  switch (token.type) {
  case '+':
  case '-':
    parsed->type = token.type == '+' ? REDIS_REPLY_STRING : REDIS_REPLY_ERROR;
    parsed->str = redis_reply_string_create(owner, token.value);
    if (!parsed->str) return -1;
    parsed->len = token.value.len;
    break;
  case ':':
    parsed->type = REDIS_REPLY_INTEGER;
    parsed->integer = token.int_value;
    break;
  case '$': {
    size_t bulk_len;
    size_t total;
    if (token.int_value == -1) {
      parsed->type = REDIS_REPLY_NULL;
      break;
    }
    if (token.int_value < 0 || (uint64_t)token.int_value > SIZE_MAX) return -1;
    bulk_len = (size_t)token.int_value;
    if (bulk_len > SIZE_MAX - token.header_len - 2u) return -1;
    total = token.header_len + bulk_len + 2u;
    if (total > (size_t)INT_MAX) return -1;
    if (len < total) return 0;
    if (data[total - 2u] != '\r' || data[total - 1u] != '\n') return -1;
    parsed->type = REDIS_REPLY_BULK_STRING;
    parsed->str =
        redis_reply_string_create(owner, tstr_v_from_buf(data + token.header_len, bulk_len));
    if (!parsed->str) return -1;
    parsed->len = bulk_len;
    *reply = parsed;
    return (int)total;
  }
  case '*': {
    size_t count;
    size_t consumed = token.header_len;
    if (token.int_value == -1) {
      parsed->type = REDIS_REPLY_NULL;
      break;
    }
    if (token.int_value < 0 || (uint64_t)token.int_value > SIZE_MAX) return -1;
    count = (size_t)token.int_value;
    parsed->type = REDIS_REPLY_ARRAY;
    parsed->element_count = count;
    if (count > (len - token.header_len) / REDIS_RESP_MIN_VALUE_SIZE) return 0;
    if (count != 0) {
      parsed->elements = mem_alloc_array(&owner->pool, sizeof(*parsed->elements), count);
      if (!parsed->elements) return -1;
      memset(parsed->elements, 0, count * sizeof(*parsed->elements));
    }
    for (size_t i = 0; i < count; ++i) {
      int element_size;
      if (consumed > len) return -1;
      element_size = redis_parse_resp_value(data + consumed, len - consumed, depth + 1u, owner,
                                            &parsed->elements[i]);
      if (element_size <= 0) return element_size;
      if ((size_t)element_size > (size_t)INT_MAX - consumed) return -1;
      consumed += (size_t)element_size;
    }
    *reply = parsed;
    return (int)consumed;
  }
  default:
    return -1;
  }

  *reply = parsed;
  return (int)token.header_len;
}

int redis_recv_buffer_init(redis_client_t *client) {
  if (!client) return -1;
  client->recv_buffer = malloc(REDIS_RECV_BUFFER_INITIAL_SIZE);
  if (!client->recv_buffer) return -1;
  client->recv_buffer_size = REDIS_RECV_BUFFER_INITIAL_SIZE;
  client->recv_buffer_used = 0;
  return 0;
}

void redis_recv_buffer_destroy(redis_client_t *client) {
  if (!client) return;
  free(client->recv_buffer);
  client->recv_buffer = NULL;
  client->recv_buffer_size = 0;
  client->recv_buffer_used = 0;
}

int redis_recv_buffer_append(redis_client_t *client, const char *data, size_t len) {
  char *new_buffer;
  size_t required;
  size_t capacity;
  if (!client || (!data && len != 0)) return -1;
  if (len == 0) return 0;
  if (len > SIZE_MAX - client->recv_buffer_used) return -1;
  required = client->recv_buffer_used + len;
  if (required <= client->recv_buffer_size) {
    memcpy(client->recv_buffer + client->recv_buffer_used, data, len);
    client->recv_buffer_used = required;
    return 0;
  }
  capacity = client->recv_buffer_size ? client->recv_buffer_size : REDIS_RECV_BUFFER_INITIAL_SIZE;
  while (capacity < required && capacity <= SIZE_MAX / 2u)
    capacity *= 2u;
  if (capacity < required) capacity = required;
  new_buffer = realloc(client->recv_buffer, capacity);
  if (!new_buffer) return -1;
  client->recv_buffer = new_buffer;
  client->recv_buffer_size = capacity;
  memcpy(client->recv_buffer + client->recv_buffer_used, data, len);
  client->recv_buffer_used = required;
  return 0;
}

int redis_parse_resp_reply(redis_client_t *client, redis_reply_t **reply) {
  redis_reply_owner_t *owner;
  int consumed;
  if (!client || !reply) return -1;
  *reply = NULL;
  if (client->recv_buffer_used == 0) return 0;
  owner = calloc(1, sizeof(*owner));
  if (!owner) return -1;
  if (mem_init(&owner->pool, 0) != 0) {
    free(owner);
    return -1;
  }
  owner->magic = REDIS_REPLY_OWNER_MAGIC;
  consumed = redis_parse_resp_value(client->recv_buffer, client->recv_buffer_used, 0, owner, reply);
  if (consumed <= 0) {
    redis_reply_strings_destroy(owner);
    mem_destroy(&owner->pool);
    owner->magic = 0;
    free(owner);
    *reply = NULL;
  }
  return consumed;
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

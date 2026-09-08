/**
 * @file redis_internal.h
 * @brief Internal Redis helpers (transport buffer and RESP parsing)
 */

#ifndef REDIS_INTERNAL_H
#define REDIS_INTERNAL_H

#include "redis_export.h"
#include "redis_reply.h"
#include "salts_buffer.h"
#include "tstr.h"

#define REDIS_RESP_MAX_DEPTH 128u

typedef enum redis_resp_array_step {
  REDIS_RESP_ARRAY_OOM = -3,
  REDIS_RESP_ARRAY_LIMIT = -2,
  REDIS_RESP_ARRAY_ERROR = -1,
  REDIS_RESP_ARRAY_NEED_MORE = 0,
  REDIS_RESP_ARRAY_ITEM = 1,
  REDIS_RESP_ARRAY_DONE = 2,
  REDIS_RESP_ARRAY_SERVER_ERROR = 3
} redis_resp_array_step;

typedef struct redis_resp_reply_reader {
  void *impl;
  size_t max_reply_bytes;
} redis_resp_reply_reader;

typedef struct redis_resp_array_reader {
  size_t remaining;
  size_t max_items;
  size_t max_reply_bytes;
  size_t header_scan;
  int header_read;
  int terminal;
  redis_resp_reply_reader item_reader;
} redis_resp_array_reader;

typedef struct redis_resp_buffer {
  char *data;
  size_t capacity;
  size_t used;
} redis_resp_buffer;

REDIS_API int redis_resp_buffer_init(redis_resp_buffer *buffer,
                                     size_t initial_capacity);
REDIS_API void redis_resp_buffer_destroy(redis_resp_buffer *buffer);
REDIS_API void redis_resp_buffer_reset(redis_resp_buffer *buffer);
REDIS_API int redis_resp_buffer_consume(redis_resp_buffer *buffer,
                                        size_t consumed);
REDIS_API int redis_resp_buffer_append_bounded(redis_resp_buffer *buffer,
                                               const char *data, size_t len,
                                               size_t max_buffer_bytes);
REDIS_API void redis_resp_reply_reader_init(redis_resp_reply_reader *reader,
                                            size_t max_reply_bytes);
REDIS_API void redis_resp_reply_reader_destroy(redis_resp_reply_reader *reader);
REDIS_API int redis_resp_reply_reader_next_buffer(
    redis_resp_buffer *buffer, redis_resp_reply_reader *reader,
    redis_reply_t **reply);
REDIS_API redis_resp_array_step redis_resp_array_reader_next_buffer(
    redis_resp_buffer *buffer, redis_resp_array_reader *reader,
    redis_reply_t **item);
REDIS_API int redis_resp_command_build_bounded(
    int argc, const char **argv, const size_t *argvlen,
    size_t max_command_bytes, tstr *out_command);

REDIS_API void redis_resp_array_reader_init(redis_resp_array_reader *reader,
                                            size_t max_items,
                                            size_t max_reply_bytes);
REDIS_API void redis_resp_array_reader_destroy(redis_resp_array_reader *reader);

int redis_argc_mul_add(int base, int count, int mul, int *argc_out);
const char **redis_argv_alloc(int argc);
void redis_argv_free(const char **argv);

#endif /* REDIS_INTERNAL_H */

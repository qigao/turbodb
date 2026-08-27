/**
 * @file redis_internal.h
 * @brief Internal Redis helpers (transport buffer and RESP parsing)
 */

#ifndef REDIS_INTERNAL_H
#define REDIS_INTERNAL_H

#include "redis_client.h"
#include "redis_export.h"
#include "turbo_buffer.h"

#define REDIS_RESP_MAX_DEPTH 128u

typedef enum redis_resp_array_step {
  REDIS_RESP_ARRAY_ERROR = -1,
  REDIS_RESP_ARRAY_NEED_MORE = 0,
  REDIS_RESP_ARRAY_ITEM = 1,
  REDIS_RESP_ARRAY_DONE = 2,
  REDIS_RESP_ARRAY_SERVER_ERROR = 3
} redis_resp_array_step;

typedef struct redis_resp_array_reader {
  size_t remaining;
  size_t max_items;
  int header_read;
  int terminal;
} redis_resp_array_reader;

REDIS_API int redis_recv_buffer_init(redis_client_t *client);
REDIS_API void redis_recv_buffer_destroy(redis_client_t *client);
REDIS_API int redis_recv_buffer_append(redis_client_t *client, const char *data, size_t len);
REDIS_API int redis_recv_buffer_append_bounded(redis_client_t *client,
                                               const char *data, size_t len,
                                               size_t max_buffer_bytes);
REDIS_API int redis_parse_resp_reply(redis_client_t *client, redis_reply_t **reply);
REDIS_API void redis_resp_array_reader_init(redis_resp_array_reader *reader,
                                            size_t max_items);
REDIS_API redis_resp_array_step redis_resp_array_reader_next(
    redis_client_t *client, redis_resp_array_reader *reader,
    redis_reply_t **item);

int redis_argc_mul_add(int base, int count, int mul, int *argc_out);
const char **redis_argv_alloc(int argc);
void redis_argv_free(const char **argv);

#endif /* REDIS_INTERNAL_H */

/**
 * @file redis_internal.h
 * @brief Internal Redis helpers (transport buffer and RESP parsing)
 */

#ifndef REDIS_INTERNAL_H
#define REDIS_INTERNAL_H

#include "redis_client.h"
#include "turbo_buffer.h"

#define REDIS_RESP_MAX_DEPTH 128u

CXX_C_API int redis_recv_buffer_init(redis_client_t *client);
CXX_C_API void redis_recv_buffer_destroy(redis_client_t *client);
CXX_C_API int redis_recv_buffer_append(redis_client_t *client, const char *data, size_t len);
CXX_C_API int redis_parse_resp_reply(redis_client_t *client, redis_reply_t **reply);

int redis_argc_mul_add(int base, int count, int mul, int *argc_out);
const char **redis_argv_alloc(int argc);
void redis_argv_free(const char **argv);

#endif /* REDIS_INTERNAL_H */

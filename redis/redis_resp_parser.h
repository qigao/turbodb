/**
 * @file redis_resp_parser.h
 * @brief Internal RESP token parser interface for the Redis client.
 */

#ifndef REDIS_RESP_PARSER_H
#define REDIS_RESP_PARSER_H

#include "vstr.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
  char type;
  int64_t int_value;
  size_t header_len;
  vstr value;
} redis_resp_token_t;

/** Returns 1 for a complete token header, 0 when more data is required, -1 on error. */
int redis_resp_scan_token(const char *data, size_t len, redis_resp_token_t *token);

#endif /* REDIS_RESP_PARSER_H */

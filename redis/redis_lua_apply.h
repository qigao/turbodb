#ifndef REDIS_LUA_APPLY_H
#define REDIS_LUA_APPLY_H

#include "redis_cflow.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct redis_lua_apply {
  void *impl;
} redis_lua_apply;

/** All byte views are borrowed until redis_lua_apply_open returns. */
typedef struct redis_lua_apply_request {
  const char *metadata_key;
  size_t metadata_key_length;
  const char *state_key;
  size_t state_key_length;
  const char *outbox_key;
  size_t outbox_key_length;
  uint64_t index;
  uint64_t term;
  const char *command_id;
  size_t command_id_length;
  const char *field;
  size_t field_length;
  const char *value;
  size_t value_length;
} redis_lua_apply_request;

#define REDIS_LUA_APPLY_REQUEST_INIT (redis_lua_apply_request){NULL, 0u, NULL, 0u, NULL, 0u, 0u, 0u, NULL, 0u, NULL, 0u, NULL, 0u}

typedef enum redis_lua_apply_receipt_kind {
  REDIS_LUA_APPLY_APPLIED = 0,
  REDIS_LUA_APPLY_REPLAYED,
  REDIS_LUA_APPLY_GAP,
  REDIS_LUA_APPLY_CONFLICT,
  REDIS_LUA_APPLY_COMMIT_UNKNOWN,
  REDIS_LUA_APPLY_ERROR,
  /** A verified prefix is committed; the exact request may be retried. */
  REDIS_LUA_APPLY_PENDING
} redis_lua_apply_receipt_kind;

typedef struct redis_lua_apply_receipt {
  redis_lua_apply_receipt_kind kind;
  int status;
  redis_command_outcome_t outcome;
  redis_server_error_t server_error;
  uint64_t applied_index;
} redis_lua_apply_receipt;

typedef enum redis_lua_apply_step_kind {
  REDIS_LUA_APPLY_WAIT = 0,
  REDIS_LUA_APPLY_DONE,
  REDIS_LUA_APPLY_STEP_ERROR
} redis_lua_apply_step_kind;

typedef struct redis_lua_apply_step {
  redis_lua_apply_step_kind kind;
  cflow_waitable waitable;
  redis_lua_apply_receipt receipt;
} redis_lua_apply_step;

#define REDIS_LUA_APPLY_STEP_INIT (redis_lua_apply_step){REDIS_LUA_APPLY_STEP_ERROR, {0}, {REDIS_LUA_APPLY_ERROR, 0, REDIS_COMMAND_NOT_SENT, REDIS_SERVER_ERROR_NONE, 0u}}

/**
 * Atomically advances the requested applied index, writes one hash field, and
 * appends one Stream outbox event. All three data keys must have one equal,
 * non-empty Redis Cluster hash tag. The command is submitted exactly once.
 */
REDIS_API int redis_lua_apply_open(redis_cflow_connection *connection,
                                   const redis_lua_apply_request *request,
                                   redis_lua_apply *out_operation);
REDIS_API redis_lua_apply_step redis_lua_apply_next(redis_lua_apply *operation);
REDIS_API int redis_lua_apply_cancel(redis_lua_apply *operation);
REDIS_API int redis_lua_apply_destroy(redis_lua_apply *operation);

#ifdef __cplusplus
}
#endif

#endif

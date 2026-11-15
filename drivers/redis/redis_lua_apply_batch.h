#ifndef REDIS_LUA_APPLY_BATCH_H
#define REDIS_LUA_APPLY_BATCH_H

#include "redis_lua_apply.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * This upper bound limits one Lua transaction and the borrowed argument array.
 * Applications must further bound their Raft apply batch before calling open.
 */
#define REDIS_LUA_APPLY_BATCH_MAX_RECORDS 1024u

typedef struct redis_lua_apply_batch {
  void *impl;
} redis_lua_apply_batch;

/** All byte views are borrowed until redis_lua_apply_batch_open returns. */
typedef struct redis_lua_apply_batch_record {
  uint64_t index;
  uint64_t term;
  const char *command_id;
  size_t command_id_length;
  const char *payload;
  size_t payload_length;
} redis_lua_apply_batch_record;

typedef struct redis_lua_apply_batch_request {
  const char *metadata_key;
  size_t metadata_key_length;
  const char *journal_key;
  size_t journal_key_length;
  const char *identity_key;
  size_t identity_key_length;
  const char *outbox_key;
  size_t outbox_key_length;
  const redis_lua_apply_batch_record *records;
  size_t record_count;
} redis_lua_apply_batch_request;

#define REDIS_LUA_APPLY_BATCH_REQUEST_INIT \
  (redis_lua_apply_batch_request){NULL, 0u, NULL, 0u, NULL, 0u, NULL, 0u, NULL, 0u}

typedef enum redis_lua_apply_batch_step_kind {
  REDIS_LUA_APPLY_BATCH_WAIT = 0,
  REDIS_LUA_APPLY_BATCH_DONE,
  REDIS_LUA_APPLY_BATCH_STEP_ERROR
} redis_lua_apply_batch_step_kind;

typedef struct redis_lua_apply_batch_step {
  redis_lua_apply_batch_step_kind kind;
  cflow_waitable waitable;
  redis_lua_apply_receipt receipt;
} redis_lua_apply_batch_step;

#define REDIS_LUA_APPLY_BATCH_STEP_INIT                                                \
  (redis_lua_apply_batch_step){REDIS_LUA_APPLY_BATCH_STEP_ERROR, {0},                 \
                               {REDIS_LUA_APPLY_ERROR, 0, REDIS_COMMAND_NOT_SENT,      \
                                REDIS_SERVER_ERROR_NONE, 0u}}

/**
 * Atomically advances one contiguous Raft range in the metadata commit marker,
 * stores every payload and identity in hashes, and appends a Stream outbox
 * record for each entry. All four keys must use one equal, non-empty Redis
 * Cluster hash tag. The command is submitted exactly once. The owning
 * redis_cflow_connection max_command_bytes must accommodate the Lua script,
 * RESP framing, and all request payload bytes.
 *
 * Redis server errors after a command has been sent are reported as
 * REDIS_LUA_APPLY_COMMIT_UNKNOWN. Callers must reconcile metadata and journal
 * state before retrying.
 */
REDIS_API int redis_lua_apply_batch_open(
    redis_cflow_connection *connection,
    const redis_lua_apply_batch_request *request,
    redis_lua_apply_batch *out_operation);
/**
 * Reads the metadata-backed commit state for request without writing Redis.
 * PENDING means the committed prefix matches and the exact request can be
 * retried through redis_lua_apply_batch_open. COMMIT_UNKNOWN alone never
 * authorizes a retry.
 */
REDIS_API int redis_lua_apply_batch_reconcile_open(
    redis_cflow_connection *connection,
    const redis_lua_apply_batch_request *request,
    redis_lua_apply_batch *out_operation);
REDIS_API redis_lua_apply_batch_step
redis_lua_apply_batch_next(redis_lua_apply_batch *operation);
REDIS_API int redis_lua_apply_batch_cancel(redis_lua_apply_batch *operation);
REDIS_API int redis_lua_apply_batch_destroy(redis_lua_apply_batch *operation);

#ifdef __cplusplus
}
#endif

#endif

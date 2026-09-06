#ifndef REDIS_LUA_APPLY_BATCH_COMPACT_H
#define REDIS_LUA_APPLY_BATCH_COMPACT_H

#include "redis_lua_apply_batch.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** All byte views are borrowed until either open function returns. */
typedef struct redis_lua_apply_batch_compact_request {
  const char *metadata_key;
  size_t metadata_key_length;
  const char *journal_key;
  size_t journal_key_length;
  const char *identity_key;
  size_t identity_key_length;
  const char *outbox_key;
  size_t outbox_key_length;
  uint64_t first_index;
  uint64_t last_index;
  uint64_t snapshot_index;
  uint64_t snapshot_term;
} redis_lua_apply_batch_compact_request;

#define REDIS_LUA_APPLY_BATCH_COMPACT_REQUEST_INIT \
  (redis_lua_apply_batch_compact_request){NULL, 0u, NULL, 0u, NULL, 0u, \
                                          NULL, 0u, 0u, 0u, 0u, 0u}

/**
 * Atomically removes one bounded, contiguous journal and identity range after
 * matching the durable snapshot's term against its identity entry. All four
 * keys require one equal, non-empty Redis Cluster hash tag. The Stream outbox
 * is retained. On APPLIED or REPLAYED, receipt.applied_index is the compacted
 * through index. A COMMIT_UNKNOWN receipt requires reconciliation before a
 * caller can retry the exact request.
 */
REDIS_API int redis_lua_apply_batch_compact_open(
    redis_cflow_connection *connection,
    const redis_lua_apply_batch_compact_request *request,
    redis_lua_apply_batch *out_operation);

/**
 * Reads the durable compaction marker without writing Redis. PENDING permits
 * retrying the exact request through redis_lua_apply_batch_compact_open;
 * COMMIT_UNKNOWN alone never authorizes a retry.
 */
REDIS_API int redis_lua_apply_batch_compact_reconcile_open(
    redis_cflow_connection *connection,
    const redis_lua_apply_batch_compact_request *request,
    redis_lua_apply_batch *out_operation);

#ifdef __cplusplus
}
#endif

#endif

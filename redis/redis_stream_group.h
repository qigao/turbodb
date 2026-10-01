#ifndef REDIS_STREAM_GROUP_H
#define REDIS_STREAM_GROUP_H

#include "redis_cluster.h"
#include "redis_cflow.h"
#include "redis_pool.h"
#include "redis_sentinel.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct redis_stream_group {
  void *impl;
} redis_stream_group;

typedef struct redis_stream_group_receipt redis_stream_group_receipt;

typedef enum redis_stream_group_source_kind {
  REDIS_STREAM_GROUP_SOURCE_CONNECTION = 0,
  REDIS_STREAM_GROUP_SOURCE_POOL,
  REDIS_STREAM_GROUP_SOURCE_CLUSTER,
  REDIS_STREAM_GROUP_SOURCE_SENTINEL
} redis_stream_group_source_kind;

typedef struct redis_stream_group_source {
  redis_stream_group_source_kind kind;
  void *handle;
} redis_stream_group_source;

static inline redis_stream_group_source
redis_stream_group_source_connection(redis_cflow_connection *connection) {
  redis_stream_group_source source = {
      REDIS_STREAM_GROUP_SOURCE_CONNECTION, connection};
  return source;
}

static inline redis_stream_group_source
redis_stream_group_source_pool(redis_pool *pool) {
  redis_stream_group_source source = {REDIS_STREAM_GROUP_SOURCE_POOL, pool};
  return source;
}

static inline redis_stream_group_source
redis_stream_group_source_cluster(redis_cluster *cluster) {
  redis_stream_group_source source = {
      REDIS_STREAM_GROUP_SOURCE_CLUSTER, cluster};
  return source;
}

static inline redis_stream_group_source
redis_stream_group_source_sentinel(redis_sentinel *sentinel) {
  redis_stream_group_source source = {
      REDIS_STREAM_GROUP_SOURCE_SENTINEL, sentinel};
  return source;
}

/*
 * One owner consumes exactly one Stream. All byte views are copied by init.
 *
 * max_* fields are hard admission ceilings for per-operation budgets. V1 uses
 * Redis 7-compatible XREADGROUP COUNT, XAUTOCLAIM COUNT and XACK only.
 */
typedef enum redis_stream_group_retention_policy {
  /*
   * Operational policy must keep every pending entry in the Stream until it
   * is acknowledged. A later DATA_LOSS receipt is therefore a retention
   * contract violation that the caller must surface.
   */
  REDIS_STREAM_GROUP_RETENTION_PRESERVE_PENDING = 1,
  /*
   * The deployment may trim/delete entries that are still pending. V1 never
   * hides this: XAUTOCLAIM deleted IDs are returned as DATA_LOSS receipts.
   */
  REDIS_STREAM_GROUP_RETENTION_ALLOW_PENDING_LOSS = 2
} redis_stream_group_retention_policy;

typedef struct redis_stream_group_config {
  redis_stream_group_source source;
  const char *stream_key;
  size_t stream_key_length;
  const char *group;
  size_t group_length;
  const char *consumer;
  size_t consumer_length;
  uint64_t min_idle_ms;
  uint32_t max_delivery_attempts;
  redis_stream_group_retention_policy retention_policy;
  size_t max_records_per_fetch;
  size_t max_claim_batch;
  size_t max_reply_bytes;
  size_t max_payload_bytes;
  size_t max_fields_per_record;
} redis_stream_group_config;

#define REDIS_STREAM_GROUP_CONFIG_INIT                                      \
  {                                                                         \
    { REDIS_STREAM_GROUP_SOURCE_CONNECTION, NULL }, NULL, 0u, NULL, 0u,    \
        NULL, 0u, UINT64_C(60000), 16u,                                   \
        REDIS_STREAM_GROUP_RETENTION_PRESERVE_PENDING, 64u, 32u,           \
        1024u * 1024u, 512u * 1024u, 32u                                  \
  }

typedef struct redis_stream_group_budget {
  size_t max_records;
  size_t max_reply_bytes;
  size_t max_payload_bytes;
} redis_stream_group_budget;

#define REDIS_STREAM_GROUP_BUDGET_INIT                                      \
  { 1u, 64u * 1024u, 32u * 1024u }

typedef enum redis_stream_group_receipt_kind {
  REDIS_STREAM_GROUP_RECEIPT_RECORD = 0,
  /* XAUTOCLAIM reported a PEL ID whose Stream entry was trimmed/deleted. */
  REDIS_STREAM_GROUP_RECEIPT_DATA_LOSS,
  /* The entry exists but copying its field values would exceed payload budget. */
  REDIS_STREAM_GROUP_RECEIPT_PAYLOAD_LIMIT,
  /* The claimed entry's delivery counter exceeded max_delivery_attempts. */
  REDIS_STREAM_GROUP_RECEIPT_DELIVERY_LIMIT
} redis_stream_group_receipt_kind;

typedef struct redis_stream_group_field_view {
  const char *name;
  size_t name_length;
  const char *value;
  size_t value_length;
} redis_stream_group_field_view;

typedef enum redis_stream_group_step_kind {
  REDIS_STREAM_GROUP_WAIT = 0,
  REDIS_STREAM_GROUP_VALUE,
  REDIS_STREAM_GROUP_DONE,
  REDIS_STREAM_GROUP_ERROR
} redis_stream_group_step_kind;

typedef struct redis_stream_group_step {
  redis_stream_group_step_kind kind;
  cflow_waitable waitable;
  int status;
  redis_command_outcome_t outcome;
  redis_server_error_t server_error;
  redis_stream_group_receipt *receipt;
} redis_stream_group_step;

#define REDIS_STREAM_GROUP_STEP_INIT                                        \
  {                                                                         \
    REDIS_STREAM_GROUP_ERROR, {0}, 0, REDIS_COMMAND_NOT_SENT,              \
        REDIS_SERVER_ERROR_NONE, NULL                                       \
  }

/* Copies all logical identity/configuration and performs no Redis I/O. */
REDIS_API int redis_stream_group_init(
    redis_stream_group *owner, const redis_stream_group_config *config);

/*
 * Replace the current transport after reconnect/failover. No command may be
 * active. Outstanding receipts remain valid because their identity/payload is
 * owner-managed and independent of the transport instance.
 */
REDIS_API int redis_stream_group_rebind(
    redis_stream_group *owner, redis_stream_group_source source);

/*
 * Begin one bounded new-entry read. A zero max_records, max_reply_bytes or
 * max_payload_bytes is a no-I/O operation whose next() result is DONE.
 */
REDIS_API int redis_stream_group_fetch_begin(
    redis_stream_group *owner, const redis_stream_group_budget *budget);

/*
 * Begin one bounded stale-pending scan/claim. The owner keeps the XAUTOCLAIM
 * cursor between calls. A zero budget performs no Redis I/O.
 */
REDIS_API int redis_stream_group_claim_begin(
    redis_stream_group *owner, const redis_stream_group_budget *budget);

/*
 * Begin explicit XACK settlement for exactly this receipt. Re-acking an
 * already acknowledged receipt is an idempotent no-I/O success.
 * DATA_LOSS receipts cannot be acknowledged because Redis already removed the
 * orphaned PEL reference while reporting it.
 */
REDIS_API int redis_stream_group_ack_begin(
    redis_stream_group *owner, redis_stream_group_receipt *receipt);

/*
 * Advance the active fetch/claim/ack state machine. WAIT carries the exact
 * underlying CFlow waitable. VALUE transfers one owned receipt to the caller.
 */
REDIS_API redis_stream_group_step redis_stream_group_next(
    redis_stream_group *owner);

/*
 * Cancel an in-flight Redis command and discard not-yet-delivered local
 * receipts without acknowledging them. Already returned receipts are untouched.
 */
REDIS_API int redis_stream_group_cancel(redis_stream_group *owner);

/*
 * Release a receipt without sending XACK. For RECORD/PAYLOAD_LIMIT/
 * DELIVERY_LIMIT this intentionally leaves the PEL entry pending for retry.
 * DATA_LOSS was already removed from the PEL by Redis XAUTOCLAIM.
 * Returns SALTS_EBUSY while this exact receipt has an active ack_begin().
 */
REDIS_API int redis_stream_group_receipt_release(
    redis_stream_group_receipt *receipt);

/* Alias documenting the at-least-once retry intent. */
REDIS_API int redis_stream_group_receipt_retry(
    redis_stream_group_receipt *receipt);

REDIS_API redis_stream_group_receipt_kind redis_stream_group_receipt_kindof(
    const redis_stream_group_receipt *receipt);

/* Borrowed stable identity view, valid until receipt_release(). */
REDIS_API int redis_stream_group_receipt_id(
    const redis_stream_group_receipt *receipt,
    const char **id, size_t *id_length);

/* Delivery count is 1 for a fresh XREADGROUP record and XPENDING-qualified
 * after XAUTOCLAIM. DATA_LOSS receipts report 0. */
REDIS_API uint64_t redis_stream_group_receipt_delivery_count(
    const redis_stream_group_receipt *receipt);

REDIS_API size_t redis_stream_group_receipt_payload_bytes(
    const redis_stream_group_receipt *receipt);
REDIS_API size_t redis_stream_group_receipt_field_count(
    const redis_stream_group_receipt *receipt);
REDIS_API int redis_stream_group_receipt_field_at(
    const redis_stream_group_receipt *receipt, size_t index,
    redis_stream_group_field_view *field);
REDIS_API int redis_stream_group_receipt_find(
    const redis_stream_group_receipt *receipt,
    const char *name, size_t name_length,
    const char **value, size_t *value_length);
REDIS_API int redis_stream_group_receipt_u64(
    const redis_stream_group_receipt *receipt,
    const char *name, size_t name_length, uint64_t *value);
REDIS_API int redis_stream_group_receipt_acknowledged(
    const redis_stream_group_receipt *receipt);

/*
 * Stop admission and free owner state. An active command is cancelled first,
 * but returned receipts keep the owner alive and cause SALTS_EBUSY until they
 * are acknowledged/retried/released.
 */
REDIS_API int redis_stream_group_destroy(redis_stream_group *owner);

#ifdef __cplusplus
}
#endif

#endif /* REDIS_STREAM_GROUP_H */

#include "../redis_stream_group.h"

#include "salts_error.h"
#include <tinytest.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define REDIS_STREAM_GROUP_TEST_WAIT_NS UINT64_C(5000000000)

static void redis_stream_group_test_sleep_ms(unsigned milliseconds) {
#ifdef _WIN32
  Sleep(milliseconds);
#else
  struct timespec requested;
  requested.tv_sec = (time_t)(milliseconds / 1000u);
  requested.tv_nsec = (long)((milliseconds % 1000u) * 1000000u);
  (void)nanosleep(&requested, NULL);
#endif
}

static redis_reply_t *redis_stream_group_test_command(
    redis_cflow_connection *connection, redis_io_runtime *runtime,
    int argc, const char **argv) {
  redis_cflow_stream stream = {0};
  redis_cflow_stream_step step;
  redis_reply_t *reply = NULL;
  check_equal(redis_cflow_command_open(
                  connection, argc, argv, NULL, 256u * 1024u, &stream),
              SALTS_OK);
  for (;;) {
    step = redis_cflow_stream_next(&stream);
    if (step.kind == REDIS_CFLOW_STREAM_WAIT) {
      check_equal(redis_io_runtime_wait_idle(
                      runtime, REDIS_STREAM_GROUP_TEST_WAIT_NS),
                  SALTS_OK);
      continue;
    }
    if (step.kind == REDIS_CFLOW_STREAM_ITEM) {
      check_null(reply);
      reply = step.item;
      continue;
    }
    check_equal(step.kind, REDIS_CFLOW_STREAM_DONE);
    check_equal(step.status, SALTS_OK);
    break;
  }
  check_equal(redis_cflow_stream_destroy(&stream), SALTS_OK);
  return reply;
}

static redis_stream_group_step redis_stream_group_test_drive(
    redis_stream_group *group, redis_io_runtime *runtime) {
  redis_stream_group_step step;
  for (;;) {
    step = redis_stream_group_next(group);
    if (step.kind != REDIS_STREAM_GROUP_WAIT) return step;
    check_equal(redis_io_runtime_wait_idle(
                    runtime, REDIS_STREAM_GROUP_TEST_WAIT_NS),
                SALTS_OK);
  }
}

static redis_stream_group_receipt *redis_stream_group_test_one_value(
    redis_stream_group *group, redis_io_runtime *runtime) {
  redis_stream_group_step step =
      redis_stream_group_test_drive(group, runtime);
  redis_stream_group_receipt *receipt;
  check_equal(step.kind, REDIS_STREAM_GROUP_VALUE);
  check_not_null(step.receipt);
  receipt = step.receipt;
  step = redis_stream_group_test_drive(group, runtime);
  check_equal(step.kind, REDIS_STREAM_GROUP_DONE);
  return receipt;
}

static uint16_t redis_stream_group_test_port(void) {
  const char *text = getenv("TURBODB_REDIS_TEST_PORT");
  char *end = NULL;
  unsigned long value;
  if (text == NULL || text[0] == '\0') return 0u;
  value = strtoul(text, &end, 10);
  if (end == text || *end != '\0' || value == 0u || value > UINT16_MAX)
    return 0u;
  return (uint16_t)value;
}

static void redis_stream_group_test_connect(
    redis_io_runtime *runtime, redis_cflow_connection *connection,
    uint16_t port) {
  redis_cflow_open_config open = {
      runtime, "127.0.0.1", port, 1u, 256u * 1024u, 256u,
      256u * 1024u, 4096u, REDIS_STREAM_GROUP_TEST_WAIT_NS};
  redis_cflow_connect_step step;

  check_equal(redis_cflow_connection_open(connection, &open), SALTS_OK);
  for (;;) {
    step = redis_cflow_connection_connect_next(connection);
    if (step.kind == REDIS_CFLOW_CONNECT_WAIT) {
      check_equal(redis_io_runtime_wait_idle(
                      runtime, REDIS_STREAM_GROUP_TEST_WAIT_NS),
                  SALTS_OK);
      continue;
    }
    check_equal(step.kind, REDIS_CFLOW_CONNECT_DONE);
    check_equal(step.status, SALTS_OK);
    break;
  }
}

static void redis_stream_group_test_copy_receipt_id(
    const redis_stream_group_receipt *receipt,
    char *out, size_t out_size) {
  const char *id = NULL;
  size_t length = 0u;
  check_equal(redis_stream_group_receipt_id(receipt, &id, &length), SALTS_OK);
  check_true(length + 1u <= out_size);
  memcpy(out, id, length);
  out[length] = '\0';
}

static void redis_stream_group_test_ack(
    redis_stream_group *group, redis_io_runtime *runtime,
    redis_stream_group_receipt *receipt) {
  redis_stream_group_step step;
  check_equal(redis_stream_group_ack_begin(group, receipt), SALTS_OK);
  step = redis_stream_group_test_drive(group, runtime);
  check_equal(step.kind, REDIS_STREAM_GROUP_DONE);
  check_true(redis_stream_group_receipt_acknowledged(receipt));
}

static void redis_stream_group_test_reset_stream(
    redis_cflow_connection *connection, redis_io_runtime *runtime,
    const char *stream, const char *group) {
  const char *delete_command[] = {"DEL", stream};
  const char *create_command[] = {
      "XGROUP", "CREATE", stream, group, "0", "MKSTREAM"};
  redis_reply_t *reply =
      redis_stream_group_test_command(connection, runtime, 2, delete_command);
  check_not_null(reply);
  check_equal(reply->type, REDIS_REPLY_INTEGER);
  redis_reply_free(reply);
  reply = redis_stream_group_test_command(
      connection, runtime, 6, create_command);
  check_not_null(reply);
  check_true(reply->type == REDIS_REPLY_STRING ||
             reply->type == REDIS_REPLY_BULK_STRING);
  redis_reply_free(reply);
}

static void redis_stream_group_test_xadd(
    redis_cflow_connection *connection, redis_io_runtime *runtime,
    const char *stream, const char *index, const char *payload) {
  const char *command[] = {
      "XADD", stream, "*",
      "index", index,
      "term", "7",
      "command_id", "command",
      "payload", payload};
  redis_reply_t *reply =
      redis_stream_group_test_command(connection, runtime, 11, command);
  check_not_null(reply);
  check_true(reply->type == REDIS_REPLY_STRING ||
             reply->type == REDIS_REPLY_BULK_STRING);
  redis_reply_free(reply);
}

static redis_stream_group_config redis_stream_group_test_config(
    redis_cflow_connection *connection,
    const char *stream, const char *group, const char *consumer) {
  redis_stream_group_config config = REDIS_STREAM_GROUP_CONFIG_INIT;
  config.source = redis_stream_group_source_connection(connection);
  config.stream_key = stream;
  config.stream_key_length = strlen(stream);
  config.group = group;
  config.group_length = strlen(group);
  config.consumer = consumer;
  config.consumer_length = strlen(consumer);
  config.min_idle_ms = 5u;
  config.max_delivery_attempts = 1u;
  config.max_records_per_fetch = 8u;
  config.max_claim_batch = 4u;
  config.max_reply_bytes = 64u * 1024u;
  config.max_payload_bytes = 32u * 1024u;
  config.max_fields_per_record = 16u;
  return config;
}

spec("Redis Stream consumer-group typed receipts") {
  it("rejects an unspecified retention policy before any Redis command") {
    redis_cflow_connection fake = {0};
    redis_stream_group owner = {0};
    redis_stream_group_config config =
        redis_stream_group_test_config(
            &fake, "raft:{retention}:outbox", "projection", "worker");

    config.retention_policy = (redis_stream_group_retention_policy)0;
    check_equal(redis_stream_group_init(&owner, &config), SALTS_EINVAL);
    check_null(owner.impl);
  }

  it("zero fetch/claim budgets perform no transport I/O") {
    redis_cflow_connection fake = {0};
    redis_stream_group owner = {0};
    redis_stream_group_config config =
        redis_stream_group_test_config(
            &fake, "raft:{zero}:outbox", "projection", "worker");
    redis_stream_group_budget zero = {0u, 0u, 0u};
    redis_stream_group_step step;

    check_equal(redis_stream_group_init(&owner, &config), SALTS_OK);
    check_equal(redis_stream_group_fetch_begin(&owner, &zero), SALTS_OK);
    step = redis_stream_group_next(&owner);
    check_equal(step.kind, REDIS_STREAM_GROUP_DONE);

    check_equal(redis_stream_group_claim_begin(&owner, &zero), SALTS_OK);
    step = redis_stream_group_next(&owner);
    check_equal(step.kind, REDIS_STREAM_GROUP_DONE);
    check_equal(redis_stream_group_destroy(&owner), SALTS_OK);
  }

  it("reads, settles, reclaims, detects trimmed PEL entries, and rebinds") {
    const uint16_t port = redis_stream_group_test_port();
    const char *stream = "raft:{stream-group}:outbox";
    const char *group_name = "projection";
    const char *consumer = "worker-a";
    redis_io_runtime runtime = {0};
    redis_io_runtime_config runtime_config = {
        redis_io_default_backend_kind(), 2u, 4u};
    redis_cflow_connection first = {0};
    redis_cflow_connection second = {0};
    redis_stream_group owner = {0};
    redis_stream_group_config config;
    redis_stream_group_budget one = {1u, 64u * 1024u, 32u * 1024u};
    redis_stream_group_budget tiny_payload = {1u, 64u * 1024u, 2u};
    redis_stream_group_receipt *receipt;
    redis_stream_group_step step;
    const char *value;
    size_t value_length;
    uint64_t index;
    char pending_id[64];
    char stable_id[64];

    if (port == 0u) return;

    check_equal(redis_io_runtime_init(&runtime, &runtime_config), SALTS_OK);
    redis_stream_group_test_connect(&runtime, &first, port);
    redis_stream_group_test_reset_stream(
        &first, &runtime, stream, group_name);

    config = redis_stream_group_test_config(
        &first, stream, group_name, consumer);
    check_equal(redis_stream_group_init(&owner, &config), SALTS_OK);

    /* Open a real XREADGROUP and cancel it before delivery. The owner must
     * remain reusable and cancellation must not synthesize an ACK. */
    check_equal(redis_stream_group_fetch_begin(&owner, &one), SALTS_OK);
    check_equal(redis_stream_group_cancel(&owner), SALTS_OK);

    redis_stream_group_test_xadd(&first, &runtime, stream, "1", "one");
    check_equal(redis_stream_group_fetch_begin(&owner, &one), SALTS_OK);
    receipt = redis_stream_group_test_one_value(&owner, &runtime);
    check_equal(redis_stream_group_receipt_kindof(receipt),
                REDIS_STREAM_GROUP_RECEIPT_RECORD);
    check_equal(redis_stream_group_receipt_delivery_count(receipt),
                UINT64_C(1));
    check_equal(redis_stream_group_receipt_u64(
                    receipt, "index", 5u, &index), SALTS_OK);
    check_equal(index, UINT64_C(1));
    check_equal(redis_stream_group_receipt_find(
                    receipt, "payload", 7u, &value, &value_length), SALTS_OK);
    check_equal(value, "one", 3u);
    redis_stream_group_test_ack(&owner, &runtime, receipt);

    check_equal(redis_stream_group_ack_begin(&owner, receipt), SALTS_OK);
    step = redis_stream_group_next(&owner);
    check_equal(step.kind, REDIS_STREAM_GROUP_DONE);
    check_equal(redis_stream_group_receipt_release(receipt), SALTS_OK);

    redis_stream_group_test_xadd(&first, &runtime, stream, "2", "two");
    check_equal(redis_stream_group_fetch_begin(&owner, &one), SALTS_OK);
    receipt = redis_stream_group_test_one_value(&owner, &runtime);
    redis_stream_group_test_copy_receipt_id(
        receipt, pending_id, sizeof(pending_id));
    check_equal(redis_stream_group_receipt_retry(receipt), SALTS_OK);

    redis_stream_group_test_sleep_ms(10u);
    check_equal(redis_stream_group_claim_begin(&owner, &one), SALTS_OK);
    receipt = redis_stream_group_test_one_value(&owner, &runtime);
    check_equal(redis_stream_group_receipt_kindof(receipt),
                REDIS_STREAM_GROUP_RECEIPT_DELIVERY_LIMIT);
    check_true(redis_stream_group_receipt_delivery_count(receipt) >=
               UINT64_C(2));
    redis_stream_group_test_ack(&owner, &runtime, receipt);
    check_equal(redis_stream_group_receipt_release(receipt), SALTS_OK);

    redis_stream_group_test_xadd(&first, &runtime, stream, "3", "three");
    check_equal(redis_stream_group_fetch_begin(&owner, &one), SALTS_OK);
    receipt = redis_stream_group_test_one_value(&owner, &runtime);
    redis_stream_group_test_copy_receipt_id(
        receipt, pending_id, sizeof(pending_id));
    check_equal(redis_stream_group_receipt_retry(receipt), SALTS_OK);
    {
      const char *delete_entry[] = {"XDEL", stream, pending_id};
      redis_reply_t *reply = redis_stream_group_test_command(
          &first, &runtime, 3, delete_entry);
      check_not_null(reply);
      check_equal(reply->type, REDIS_REPLY_INTEGER);
      check_equal(reply->integer, 1);
      redis_reply_free(reply);
    }

    redis_stream_group_test_sleep_ms(10u);
    check_equal(redis_stream_group_claim_begin(&owner, &one), SALTS_OK);
    receipt = redis_stream_group_test_one_value(&owner, &runtime);
    check_equal(redis_stream_group_receipt_kindof(receipt),
                REDIS_STREAM_GROUP_RECEIPT_DATA_LOSS);
    check_equal(redis_stream_group_receipt_delivery_count(receipt),
                UINT64_C(0));
    check_equal(redis_stream_group_ack_begin(&owner, receipt), SALTS_ENOENT);
    check_equal(redis_stream_group_receipt_release(receipt), SALTS_OK);

    redis_stream_group_test_xadd(
        &first, &runtime, stream, "4", "payload-too-large");
    check_equal(redis_stream_group_fetch_begin(
                    &owner, &tiny_payload), SALTS_OK);
    receipt = redis_stream_group_test_one_value(&owner, &runtime);
    check_equal(redis_stream_group_receipt_kindof(receipt),
                REDIS_STREAM_GROUP_RECEIPT_PAYLOAD_LIMIT);
    check_equal(redis_stream_group_receipt_field_count(receipt), (size_t)0u);
    check_true(redis_stream_group_receipt_payload_bytes(receipt) >
               tiny_payload.max_payload_bytes);
    redis_stream_group_test_ack(&owner, &runtime, receipt);
    check_equal(redis_stream_group_receipt_release(receipt), SALTS_OK);

    redis_stream_group_test_xadd(&first, &runtime, stream, "5", "stable");
    check_equal(redis_stream_group_fetch_begin(&owner, &one), SALTS_OK);
    receipt = redis_stream_group_test_one_value(&owner, &runtime);
    redis_stream_group_test_copy_receipt_id(
        receipt, stable_id, sizeof(stable_id));

    redis_stream_group_test_connect(&runtime, &second, port);
    {
      /* Receipt identity is owner-managed, not transport-managed. Crossing a
       * Sentinel rebind boundary cannot rewrite it; the actual rediscovery
       * lifecycle remains owned by redis_sentinel. */
      redis_sentinel sentinel_boundary = {(void *)(uintptr_t)1u};
      const char *before = NULL;
      const char *after = NULL;
      size_t before_length = 0u;
      size_t after_length = 0u;

      check_equal(redis_stream_group_receipt_id(
                      receipt, &before, &before_length), SALTS_OK);
      check_equal(redis_stream_group_rebind(
                      &owner,
                      redis_stream_group_source_sentinel(&sentinel_boundary)),
                  SALTS_OK);
      check_equal(redis_stream_group_receipt_id(
                      receipt, &after, &after_length), SALTS_OK);
      check_equal(after_length, before_length);
      check_equal(memcmp(after, before, before_length), 0);
    }
    check_equal(redis_stream_group_rebind(
                    &owner,
                    redis_stream_group_source_connection(&second)),
                SALTS_OK);
    redis_stream_group_test_ack(&owner, &runtime, receipt);
    {
      const char *id;
      size_t id_length;
      check_equal(redis_stream_group_receipt_id(
                      receipt, &id, &id_length), SALTS_OK);
      check_equal(id_length, strlen(stable_id));
      check_equal(memcmp(id, stable_id, id_length), 0);
    }

    check_equal(redis_stream_group_destroy(&owner), SALTS_EBUSY);
    check_equal(redis_stream_group_receipt_release(receipt), SALTS_OK);
    check_equal(redis_stream_group_destroy(&owner), SALTS_OK);

    check_equal(redis_cflow_connection_destroy(&second), SALTS_OK);
    check_equal(redis_cflow_connection_destroy(&first), SALTS_OK);
    check_equal(redis_io_runtime_close(&runtime), SALTS_OK);
    check_equal(redis_io_runtime_destroy(&runtime), SALTS_OK);
  }
}

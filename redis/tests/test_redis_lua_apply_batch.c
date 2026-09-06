#include "../redis_lua_apply_batch.h"

#include "tinytest.h"
#include "salts_error.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define REDIS_LUA_APPLY_BATCH_TEST_WAIT_TIMEOUT_NS UINT64_C(5000000000)
#define REDIS_LUA_APPLY_BATCH_TEST_MAX_STEPS 16u
#define REDIS_LUA_APPLY_BATCH_TEST_MAX_COMMAND_BYTES 8192u

static cflow_io_native_backend_kind redis_lua_apply_batch_test_backend(void) {
#if defined(_WIN32)
  return CFLOW_IO_NATIVE_IOCP;
#elif defined(__linux__)
  return CFLOW_IO_NATIVE_EPOLL;
#elif defined(__APPLE__)
  return CFLOW_IO_NATIVE_KQUEUE;
#else
  return CFLOW_IO_NATIVE_POLL;
#endif
}

static redis_lua_apply_batch_request redis_lua_apply_batch_test_request(
    const redis_lua_apply_batch_record *records, size_t record_count) {
  redis_lua_apply_batch_request request = REDIS_LUA_APPLY_BATCH_REQUEST_INIT;
  request.metadata_key = "raft:{orders}:meta";
  request.metadata_key_length = strlen(request.metadata_key);
  request.journal_key = "raft:{orders}:journal";
  request.journal_key_length = strlen(request.journal_key);
  request.identity_key = "raft:{orders}:identity";
  request.identity_key_length = strlen(request.identity_key);
  request.outbox_key = "raft:{orders}:outbox";
  request.outbox_key_length = strlen(request.outbox_key);
  request.records = records;
  request.record_count = record_count;
  return request;
}

static redis_lua_apply_batch_step redis_lua_apply_batch_test_complete(
    redis_lua_apply_batch *operation, redis_io_runtime *runtime) {
  redis_lua_apply_batch_step step = REDIS_LUA_APPLY_BATCH_STEP_INIT;
  size_t attempt;
  for (attempt = 0u; attempt < REDIS_LUA_APPLY_BATCH_TEST_MAX_STEPS; ++attempt) {
    step = redis_lua_apply_batch_next(operation);
    if (step.kind != REDIS_LUA_APPLY_BATCH_WAIT) return step;
    if (redis_io_runtime_wait_idle(
            runtime, REDIS_LUA_APPLY_BATCH_TEST_WAIT_TIMEOUT_NS) != SALTS_OK) {
      step.kind = REDIS_LUA_APPLY_BATCH_STEP_ERROR;
      step.receipt.kind = REDIS_LUA_APPLY_ERROR;
      step.receipt.status = SALTS_ETIMEDOUT;
      return step;
    }
  }
  step.kind = REDIS_LUA_APPLY_BATCH_STEP_ERROR;
  step.receipt.kind = REDIS_LUA_APPLY_ERROR;
  step.receipt.status = SALTS_ETIMEDOUT;
  return step;
}

static redis_reply_t *redis_lua_apply_batch_test_command(
    redis_cflow_connection *connection, redis_io_runtime *runtime, int argc,
    const char **argv) {
  redis_cflow_stream stream = {0};
  redis_cflow_stream_step step;
  size_t attempt;
  if (redis_cflow_command_open(connection, argc, argv, NULL, 4096u, &stream) !=
      SALTS_OK)
    return NULL;
  for (attempt = 0u; attempt < REDIS_LUA_APPLY_BATCH_TEST_MAX_STEPS; ++attempt) {
    step = redis_cflow_stream_next(&stream);
    if (step.kind == REDIS_CFLOW_STREAM_WAIT) {
      if (redis_io_runtime_wait_idle(
              runtime, REDIS_LUA_APPLY_BATCH_TEST_WAIT_TIMEOUT_NS) != SALTS_OK)
        break;
      continue;
    }
    if (step.kind == REDIS_CFLOW_STREAM_ITEM) {
      redis_reply_t *reply = step.item;
      step = redis_cflow_stream_next(&stream);
      if (step.kind == REDIS_CFLOW_STREAM_DONE) {
        (void)redis_cflow_stream_destroy(&stream);
        return reply;
      }
      redis_reply_free(reply);
      redis_reply_free(step.item);
      break;
    }
    redis_reply_free(step.item);
    break;
  }
  (void)redis_cflow_stream_destroy(&stream);
  return NULL;
}

spec("redis_lua_apply_batch") {
  it("rejects an empty batch before opening an I/O command") {
    redis_cflow_connection connection = {0};
    redis_lua_apply_batch_request request =
        redis_lua_apply_batch_test_request(NULL, 0u);
    redis_lua_apply_batch operation = {0};

    check_equal(redis_lua_apply_batch_open(&connection, &request, &operation),
                SALTS_EINVAL);
    check_null(operation.impl);
  }

  it("rejects records that are not a contiguous Raft range") {
    static const redis_lua_apply_batch_record records[] = {
        {UINT64_C(42), UINT64_C(7), "command-42", 10u, "first", 5u},
        {UINT64_C(44), UINT64_C(7), "command-44", 10u, "second", 6u},
    };
    redis_cflow_connection connection = {0};
    redis_lua_apply_batch_request request =
        redis_lua_apply_batch_test_request(records, 2u);
    redis_lua_apply_batch operation = {0};

    check_equal(redis_lua_apply_batch_open(&connection, &request, &operation),
                SALTS_EINVAL);
    check_null(operation.impl);
  }

  it("rejects Redis keys without one equal Cluster hash tag") {
    static const redis_lua_apply_batch_record records[] = {
        {UINT64_C(42), UINT64_C(7), "command-42", 10u, "first", 5u},
    };
    redis_cflow_connection connection = {0};
    redis_lua_apply_batch_request request =
        redis_lua_apply_batch_test_request(records, 1u);
    redis_lua_apply_batch operation = {0};

    request.identity_key = "raft:{other}:identity";
    request.identity_key_length = strlen(request.identity_key);
    check_equal(redis_lua_apply_batch_open(&connection, &request, &operation),
                SALTS_EINVAL);
    check_null(operation.impl);
  }

  it("writes a complete two-record journal batch against configured Redis") {
    const char *port_text = getenv("TURBODB_REDIS_TEST_PORT");
    static const redis_lua_apply_batch_record records[] = {
        {UINT64_C(42), UINT64_C(123456), "command-42", 10u, "first", 5u},
        {UINT64_C(43), UINT64_C(123456), "command-43", 10u, "second", 6u},
    };
    static const redis_lua_apply_batch_record conflicting_records[] = {
        {UINT64_C(42), UINT64_C(123456), "command-42", 10u, "other", 5u},
        {UINT64_C(43), UINT64_C(123456), "command-43", 10u, "second", 6u},
    };
    static const redis_lua_apply_batch_record high_records[] = {
        {UINT64_MAX - UINT64_C(1), UINT64_C(8), "command-high-1", 14u,
         "high-first", 10u},
        {UINT64_MAX, UINT64_C(8), "command-high-2", 14u, "high-last", 9u},
    };
    static const char *delete_command[] = {
        "DEL", "raft:{orders}:meta", "raft:{orders}:journal",
        "raft:{orders}:identity", "raft:{orders}:outbox"};
    static const char *seed_command[] = {
        "HSET", "raft:{orders}:meta", "applied_index", "41", "term", "7",
        "command_id", "seed-41"};
    static const char *prepared_stream_command[] = {
        "XADD", "raft:{orders}:outbox", "42-0", "index", "42", "term",
        "123456", "command_id", "command-42", "payload", "first"};
    static const char *high_seed_command[] = {
        "HSET", "raft:{orders}:meta", "applied_index", "18446744073709551613",
        "term", "8", "command_id", "seed-high"};
    static const char *meta_command[] = {
        "HGET", "raft:{orders}:meta", "applied_index"};
    static const char *outbox_command[] = {
        "XLEN", "raft:{orders}:outbox"};
    redis_io_runtime runtime = {0};
    redis_io_runtime_config runtime_config = {
        redis_lua_apply_batch_test_backend(), 1u, 1u};
    redis_cflow_connection connection = {0};
    redis_cflow_open_config connection_config;
    redis_cflow_connect_step connect_step;
    redis_lua_apply_batch_request request =
        redis_lua_apply_batch_test_request(records, 2u);
    redis_lua_apply_batch operation = {0};
    redis_lua_apply_batch_step step;
    redis_reply_t *reply;
    char *port_end = NULL;
    unsigned long port;

    if (port_text == NULL || port_text[0] == '\0') return;
    port = strtoul(port_text, &port_end, 10);
    check_true(port_end != port_text && *port_end == '\0' && port > 0u &&
               port <= UINT16_MAX);
    check_equal(redis_io_runtime_init(&runtime, &runtime_config), SALTS_OK);
    connection_config = (redis_cflow_open_config){
        &runtime, "127.0.0.1", (uint16_t)port, 1u,
        REDIS_LUA_APPLY_BATCH_TEST_MAX_COMMAND_BYTES, 64u, 4096u, 64u,
        REDIS_LUA_APPLY_BATCH_TEST_WAIT_TIMEOUT_NS};
    check_equal(redis_cflow_connection_open(&connection, &connection_config),
                SALTS_OK);
    connect_step = redis_cflow_connection_connect_next(&connection);
    check_equal(connect_step.kind, REDIS_CFLOW_CONNECT_WAIT);
    check_equal(redis_io_runtime_wait_idle(
                    &runtime, REDIS_LUA_APPLY_BATCH_TEST_WAIT_TIMEOUT_NS),
                SALTS_OK);
    connect_step = redis_cflow_connection_connect_next(&connection);
    check_equal(connect_step.kind, REDIS_CFLOW_CONNECT_DONE);

    reply = redis_lua_apply_batch_test_command(
        &connection, &runtime, 5, delete_command);
    check_not_null(reply);
    redis_reply_free(reply);
    reply = redis_lua_apply_batch_test_command(
        &connection, &runtime, 8, seed_command);
    check_not_null(reply);
    redis_reply_free(reply);
    reply = redis_lua_apply_batch_test_command(
        &connection, &runtime, 11, prepared_stream_command);
    check_not_null(reply);
    redis_reply_free(reply);

    check_equal(redis_lua_apply_batch_reconcile_open(&connection, &request,
                                                      &operation), SALTS_OK);
    step = redis_lua_apply_batch_test_complete(&operation, &runtime);
    check_equal(step.kind, REDIS_LUA_APPLY_BATCH_DONE);
    check_equal(step.receipt.kind, REDIS_LUA_APPLY_PENDING);
    check_equal(step.receipt.applied_index, UINT64_C(41));
    check_equal(redis_lua_apply_batch_destroy(&operation), SALTS_OK);

    check_equal(redis_lua_apply_batch_open(&connection, &request, &operation),
                SALTS_OK);
    step = redis_lua_apply_batch_test_complete(&operation, &runtime);
    check_equal(step.kind, REDIS_LUA_APPLY_BATCH_DONE);
    check_equal(step.receipt.kind, REDIS_LUA_APPLY_APPLIED);
    check_equal(step.receipt.applied_index, UINT64_C(43));
    check_equal(redis_lua_apply_batch_destroy(&operation), SALTS_OK);

    reply = redis_lua_apply_batch_test_command(&connection, &runtime, 3,
                                               meta_command);
    check_not_null(reply);
    check_equal(reply->type, REDIS_REPLY_BULK_STRING);
    check_equal(reply->str, "43", 2u);
    redis_reply_free(reply);
    reply = redis_lua_apply_batch_test_command(&connection, &runtime, 2,
                                               outbox_command);
    check_not_null(reply);
    check_equal(reply->type, REDIS_REPLY_INTEGER);
    check_equal(reply->integer, 2);
    redis_reply_free(reply);

    check_equal(redis_lua_apply_batch_open(&connection, &request, &operation),
                SALTS_OK);
    step = redis_lua_apply_batch_test_complete(&operation, &runtime);
    check_equal(step.kind, REDIS_LUA_APPLY_BATCH_DONE);
    check_equal(step.receipt.kind, REDIS_LUA_APPLY_REPLAYED);
    check_equal(step.receipt.applied_index, UINT64_C(43));
    check_equal(redis_lua_apply_batch_destroy(&operation), SALTS_OK);
    check_equal(redis_lua_apply_batch_reconcile_open(&connection, &request,
                                                      &operation), SALTS_OK);
    step = redis_lua_apply_batch_test_complete(&operation, &runtime);
    check_equal(step.kind, REDIS_LUA_APPLY_BATCH_DONE);
    check_equal(step.receipt.kind, REDIS_LUA_APPLY_REPLAYED);
    check_equal(step.receipt.applied_index, UINT64_C(43));
    check_equal(redis_lua_apply_batch_destroy(&operation), SALTS_OK);
    reply = redis_lua_apply_batch_test_command(&connection, &runtime, 2,
                                               outbox_command);
    check_not_null(reply);
    check_equal(reply->type, REDIS_REPLY_INTEGER);
    check_equal(reply->integer, 2);
    redis_reply_free(reply);

    request.records = conflicting_records;
    check_equal(redis_lua_apply_batch_open(&connection, &request, &operation),
                SALTS_OK);
    step = redis_lua_apply_batch_test_complete(&operation, &runtime);
    check_equal(step.kind, REDIS_LUA_APPLY_BATCH_DONE);
    check_equal(step.receipt.kind, REDIS_LUA_APPLY_CONFLICT);
    check_equal(step.receipt.applied_index, UINT64_C(43));
    check_equal(redis_lua_apply_batch_destroy(&operation), SALTS_OK);
    check_equal(redis_lua_apply_batch_reconcile_open(&connection, &request,
                                                      &operation), SALTS_OK);
    step = redis_lua_apply_batch_test_complete(&operation, &runtime);
    check_equal(step.kind, REDIS_LUA_APPLY_BATCH_DONE);
    check_equal(step.receipt.kind, REDIS_LUA_APPLY_CONFLICT);
    check_equal(step.receipt.applied_index, UINT64_C(43));
    check_equal(redis_lua_apply_batch_destroy(&operation), SALTS_OK);
    reply = redis_lua_apply_batch_test_command(&connection, &runtime, 2,
                                               outbox_command);
    check_not_null(reply);
    check_equal(reply->type, REDIS_REPLY_INTEGER);
    check_equal(reply->integer, 2);
    redis_reply_free(reply);

    reply = redis_lua_apply_batch_test_command(
        &connection, &runtime, 5, delete_command);
    check_not_null(reply);
    redis_reply_free(reply);
    reply = redis_lua_apply_batch_test_command(
        &connection, &runtime, 8, high_seed_command);
    check_not_null(reply);
    redis_reply_free(reply);
    request.records = high_records;
    check_equal(redis_lua_apply_batch_open(&connection, &request, &operation),
                SALTS_OK);
    step = redis_lua_apply_batch_test_complete(&operation, &runtime);
    check_equal(step.kind, REDIS_LUA_APPLY_BATCH_DONE);
    check_equal(step.receipt.kind, REDIS_LUA_APPLY_APPLIED);
    check_equal(step.receipt.applied_index, UINT64_MAX);
    check_equal(redis_lua_apply_batch_destroy(&operation), SALTS_OK);
    reply = redis_lua_apply_batch_test_command(&connection, &runtime, 3,
                                               meta_command);
    check_not_null(reply);
    check_equal(reply->type, REDIS_REPLY_BULK_STRING);
    check_equal(reply->str, "18446744073709551615", 20u);
    redis_reply_free(reply);

    check_equal(redis_cflow_connection_destroy(&connection), SALTS_OK);
    check_equal(redis_io_runtime_close(&runtime), SALTS_OK);
    check_equal(redis_io_runtime_destroy(&runtime), SALTS_OK);
  }
}

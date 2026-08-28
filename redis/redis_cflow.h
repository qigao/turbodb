#ifndef REDIS_CFLOW_H
#define REDIS_CFLOW_H

#include "redis_io.h"
#include "redis_reply.h"

#include <cflow/runtime.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct redis_cflow_connection {
  void *impl;
} redis_cflow_connection;

typedef struct redis_cflow_stream {
  void *impl;
} redis_cflow_stream;

typedef struct redis_cflow_connection_config {
  redis_io_runtime *runtime;
  uintptr_t socket;
  size_t max_command_bytes;
  size_t initial_buffer_bytes;
  /** Hard limit for incremental reply buffering. */
  size_t max_buffer_bytes;
  size_t receive_chunk_bytes;
  uint64_t cancel_timeout_ns;
  int take_socket_ownership;
} redis_cflow_connection_config;

typedef struct redis_cflow_open_config {
  redis_io_runtime *runtime;
  const char *host;
  uint16_t port;
  size_t address_capacity;
  size_t max_command_bytes;
  size_t initial_buffer_bytes;
  /** Hard limit for incremental reply buffering. */
  size_t max_buffer_bytes;
  size_t receive_chunk_bytes;
  uint64_t cancel_timeout_ns;
} redis_cflow_open_config;

typedef enum redis_cflow_connect_step_kind {
  REDIS_CFLOW_CONNECT_WAIT = 0,
  REDIS_CFLOW_CONNECT_DONE,
  REDIS_CFLOW_CONNECT_ERROR
} redis_cflow_connect_step_kind;

typedef struct redis_cflow_connect_step {
  redis_cflow_connect_step_kind kind;
  cflow_waitable waitable;
  int status;
} redis_cflow_connect_step;

typedef enum redis_cflow_stream_step_kind {
  REDIS_CFLOW_STREAM_WAIT = 0,
  REDIS_CFLOW_STREAM_ITEM,
  REDIS_CFLOW_STREAM_DONE,
  REDIS_CFLOW_STREAM_ERROR
} redis_cflow_stream_step_kind;

typedef struct redis_cflow_stream_step {
  redis_cflow_stream_step_kind kind;
  cflow_waitable waitable;
  int status;
  redis_command_outcome_t outcome;
  redis_server_error_t server_error;
  redis_reply_t *item;
} redis_cflow_stream_step;

#define REDIS_CFLOW_STREAM_STEP_INIT                                      \
  { REDIS_CFLOW_STREAM_DONE, {0}, 0, REDIS_COMMAND_NOT_SENT,             \
    REDIS_SERVER_ERROR_NONE, NULL }

REDIS_API int redis_cflow_connection_init_attached(
    redis_cflow_connection *connection,
    const redis_cflow_connection_config *config);
REDIS_API int redis_cflow_connection_open(
    redis_cflow_connection *connection, const redis_cflow_open_config *config);
/* TURBO_EBUSY is retryable after the current wake/driver callback returns. */
REDIS_API redis_cflow_connect_step redis_cflow_connection_connect_next(
    redis_cflow_connection *connection);
REDIS_API int redis_cflow_connection_valid(
    const redis_cflow_connection *connection);
REDIS_API int redis_cflow_connection_usable(
    const redis_cflow_connection *connection);
REDIS_API int redis_cflow_connection_close(redis_cflow_connection *connection);
REDIS_API int redis_cflow_connection_destroy(
    redis_cflow_connection *connection);

REDIS_API int redis_cflow_stream_open(
    redis_cflow_connection *connection, int argc, const char **argv,
    const size_t *argvlen, size_t max_reply_bytes, size_t max_items,
    redis_cflow_stream *out_stream);
REDIS_API int redis_cflow_command_open(
    redis_cflow_connection *connection, int argc, const char **argv,
    const size_t *argvlen, size_t max_reply_bytes,
    redis_cflow_stream *out_stream);
REDIS_API redis_cflow_stream_step redis_cflow_stream_next(
    redis_cflow_stream *stream);
REDIS_API int redis_cflow_stream_cancel(redis_cflow_stream *stream);
REDIS_API int redis_cflow_stream_destroy(redis_cflow_stream *stream);

#ifdef __cplusplus
}
#endif

#endif

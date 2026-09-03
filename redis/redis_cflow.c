#include "redis_cflow.h"

#include "redis_internal.h"
#include "redis_io_flow.h"
#include "redis_socket.h"
#include "salts_error.h"

#include <stdlib.h>
#include <string.h>

typedef enum redis_cflow_phase {
  REDIS_CFLOW_SENDING = 0,
  REDIS_CFLOW_RECEIVING,
  REDIS_CFLOW_TERMINAL
} redis_cflow_phase;

typedef enum redis_cflow_mode {
  REDIS_CFLOW_ARRAY_ITEMS = 0,
  REDIS_CFLOW_SINGLE_REPLY
} redis_cflow_mode;

typedef struct redis_cflow_connection_impl {
  redis_io_runtime *runtime;
  uintptr_t socket;
  redis_resp_buffer buffer;
  redis_socket_address *addresses;
  size_t address_count;
  size_t next_address;
  redis_io_flow io;
  int connect_status;
  int socket_cleanup_pending;
  int socket_retired;
  uintptr_t closed_socket_to_forget;
  int socket_close_status;
  unsigned char *receive_chunk;
  size_t max_command_bytes;
  size_t max_buffer_bytes;
  size_t receive_chunk_bytes;
  uint64_t cancel_timeout_ns;
  tstr command;
  void *active_stream;
  int owns_socket;
  int usable;
} redis_cflow_connection_impl;

typedef struct redis_cflow_stream_impl {
  redis_cflow_connection_impl *connection;
  redis_resp_array_reader reader;
  size_t send_offset;
  redis_cflow_phase phase;
  redis_cflow_mode mode;
  int single_reply_read;
  redis_command_outcome_t outcome;
  redis_cflow_stream_step terminal;
} redis_cflow_stream_impl;

static int redis_cflow_connection_allocate(
    redis_cflow_connection *connection, redis_io_runtime *runtime,
    uintptr_t socket_value, size_t max_command_bytes, size_t initial_buffer_bytes,
    size_t max_buffer_bytes,
    size_t receive_chunk_bytes, uint64_t cancel_timeout_ns,
    int owns_socket, int usable) {
  redis_cflow_connection_impl *impl;
  int status;
  if (!connection || connection->impl || !redis_io_runtime_valid(runtime) ||
      initial_buffer_bytes == 0u ||
      max_command_bytes == 0u ||
      max_buffer_bytes == 0u || initial_buffer_bytes > max_buffer_bytes ||
      receive_chunk_bytes == 0u || receive_chunk_bytes > max_buffer_bytes ||
      cancel_timeout_ns == 0u)
    return SALTS_EINVAL;
  impl = (redis_cflow_connection_impl *)calloc(1u, sizeof(*impl));
  if (!impl) return SALTS_ENOMEM;
  impl->receive_chunk = (unsigned char *)malloc(receive_chunk_bytes);
  if (!impl->receive_chunk ||
      redis_resp_buffer_init(&impl->buffer, initial_buffer_bytes) != 0) {
    free(impl->receive_chunk);
    free(impl);
    return SALTS_ENOMEM;
  }
  status = redis_io_flow_init(&impl->io, runtime, cancel_timeout_ns);
  if (status != SALTS_OK) {
    redis_resp_buffer_destroy(&impl->buffer);
    free(impl->receive_chunk);
    free(impl);
    return status;
  }
  impl->runtime = runtime;
  impl->socket = socket_value;
  impl->closed_socket_to_forget = REDIS_SOCKET_INVALID;
  impl->max_command_bytes = max_command_bytes;
  impl->max_buffer_bytes = max_buffer_bytes;
  impl->receive_chunk_bytes = receive_chunk_bytes;
  impl->cancel_timeout_ns = cancel_timeout_ns;
  impl->owns_socket = owns_socket;
  impl->usable = usable;
  connection->impl = impl;
  return SALTS_OK;
}

static redis_cflow_connection_impl *redis_cflow_connection_get(
    const redis_cflow_connection *connection) {
  return connection ? (redis_cflow_connection_impl *)connection->impl : NULL;
}

static redis_cflow_stream_impl *redis_cflow_stream_get(
    const redis_cflow_stream *stream) {
  return stream ? (redis_cflow_stream_impl *)stream->impl : NULL;
}

static int redis_cflow_cleanup_socket(
    redis_cflow_connection_impl *connection) {
  uintptr_t closed_socket;
  int close_status;
  int consumed = 0;
  int status;
  if (connection->closed_socket_to_forget != REDIS_SOCKET_INVALID) {
    status = redis_io_runtime_forget_socket_wait(
        connection->runtime, connection->closed_socket_to_forget,
        connection->cancel_timeout_ns);
    if (status != SALTS_OK) return status;
    connection->closed_socket_to_forget = REDIS_SOCKET_INVALID;
    status = connection->socket_close_status;
    connection->socket_close_status = SALTS_OK;
    return status;
  }
  if (!connection->socket_cleanup_pending) return SALTS_OK;
  if (connection->socket == REDIS_SOCKET_INVALID) return SALTS_EINVAL;
  if (!connection->socket_retired) {
    status = redis_io_runtime_retire_socket(connection->runtime,
                                            connection->socket);
    if (status != SALTS_OK) return status;
    connection->socket_retired = 1;
  }
  closed_socket = connection->socket;
  close_status = redis_socket_close_once(closed_socket, &consumed);
  if (!consumed) return SALTS_EBUSY;
  connection->socket = REDIS_SOCKET_INVALID;
  connection->socket_cleanup_pending = 0;
  connection->socket_retired = 0;
  connection->closed_socket_to_forget = closed_socket;
  connection->socket_close_status = close_status;
  status = redis_io_runtime_forget_socket_wait(
      connection->runtime, closed_socket, connection->cancel_timeout_ns);
  if (status != SALTS_OK) return status;
  connection->closed_socket_to_forget = REDIS_SOCKET_INVALID;
  connection->socket_close_status = SALTS_OK;
  return close_status;
}

static redis_cflow_stream_step redis_cflow_wait(
    redis_cflow_stream_impl *stream) {
  redis_cflow_stream_step step = REDIS_CFLOW_STREAM_STEP_INIT;
  step.kind = REDIS_CFLOW_STREAM_WAIT;
  step.waitable = redis_io_flow_waitable(&stream->connection->io);
  step.outcome = stream->outcome;
  if (!cflow_waitable_valid(&step.waitable)) {
    step.kind = REDIS_CFLOW_STREAM_ERROR;
    step.status = SALTS_EINVAL;
  }
  return step;
}

static void redis_cflow_release_active(redis_cflow_stream_impl *stream) {
  redis_cflow_connection_impl *connection = stream->connection;
  if (connection && connection->active_stream == stream)
    connection->active_stream = NULL;
}

static redis_cflow_stream_step redis_cflow_fail(
    redis_cflow_stream_impl *stream, int status,
    redis_command_outcome_t outcome, redis_reply_t *item,
    int connection_reusable) {
  redis_cflow_stream_step step = REDIS_CFLOW_STREAM_STEP_INIT;
  step.kind = REDIS_CFLOW_STREAM_ERROR;
  step.status = status;
  step.outcome = outcome;
  step.server_error = redis_server_error_classify(item);
  step.item = item;
  stream->phase = REDIS_CFLOW_TERMINAL;
  stream->terminal = step;
  stream->terminal.item = NULL;
  if (!connection_reusable) {
    stream->connection->usable = 0;
    redis_resp_buffer_reset(&stream->connection->buffer);
  }
  redis_cflow_release_active(stream);
  return step;
}

static int redis_cflow_submit_send(redis_cflow_stream_impl *stream) {
  redis_cflow_connection_impl *connection = stream->connection;
  size_t command_size = tstr_len(connection->command);
  cflow_io_native_operation operation = {
      .kind = CFLOW_IO_NATIVE_TCP_SEND,
      .socket = connection->socket,
      .buffer = connection->command + stream->send_offset,
      .length = command_size - stream->send_offset};
  int submitted = redis_io_flow_submit(&connection->io, &operation);
  if (submitted != SALTS_OK) return submitted;
  stream->outcome = REDIS_COMMAND_SEND_UNCERTAIN;
  return SALTS_OK;
}

static int redis_cflow_submit_receive(redis_cflow_stream_impl *stream) {
  redis_cflow_connection_impl *connection = stream->connection;
  cflow_io_native_operation operation = {
      .kind = CFLOW_IO_NATIVE_TCP_RECV,
      .socket = connection->socket,
      .buffer = connection->receive_chunk,
      .length = connection->receive_chunk_bytes};
  return redis_io_flow_submit(&connection->io, &operation);
}

int redis_cflow_connection_init_attached(
    redis_cflow_connection *connection,
    const redis_cflow_connection_config *config) {
  if (!connection || connection->impl || !config ||
      !redis_io_runtime_valid(config->runtime) ||
      config->socket == REDIS_SOCKET_INVALID)
    return SALTS_EINVAL;
  return redis_cflow_connection_allocate(
      connection, config->runtime, config->socket,
      config->max_command_bytes, config->initial_buffer_bytes,
      config->max_buffer_bytes,
      config->receive_chunk_bytes, config->cancel_timeout_ns,
      config->take_socket_ownership != 0, 1);
}

int redis_cflow_connection_open(redis_cflow_connection *connection,
                                const redis_cflow_open_config *config) {
  redis_cflow_connection_impl *impl;
  int status;
  if (!config || !config->host || !config->host[0] || config->port == 0u ||
      config->address_capacity == 0u ||
      config->address_capacity > SIZE_MAX / sizeof(redis_socket_address))
    return SALTS_EINVAL;
  status = redis_cflow_connection_allocate(
      connection, config->runtime, REDIS_SOCKET_INVALID,
      config->max_command_bytes, config->initial_buffer_bytes,
      config->max_buffer_bytes,
      config->receive_chunk_bytes, config->cancel_timeout_ns, 1, 0);
  if (status != SALTS_OK) return status;
  impl = redis_cflow_connection_get(connection);
  impl->addresses = (redis_socket_address *)calloc(
      config->address_capacity, sizeof(*impl->addresses));
  if (!impl->addresses) {
    (void)redis_cflow_connection_destroy(connection);
    return SALTS_ENOMEM;
  }
  status = redis_socket_resolve(config->host, config->port, impl->addresses,
                                config->address_capacity,
                                &impl->address_count);
  if (status != SALTS_OK) {
    (void)redis_cflow_connection_destroy(connection);
    return status;
  }
  return SALTS_OK;
}

static redis_cflow_connect_step redis_cflow_connect_wait(
    redis_cflow_connection_impl *impl) {
  redis_cflow_connect_step step = {REDIS_CFLOW_CONNECT_WAIT, {0}, SALTS_OK};
  step.waitable = redis_io_flow_waitable(&impl->io);
  if (!cflow_waitable_valid(&step.waitable)) {
    step.kind = REDIS_CFLOW_CONNECT_ERROR;
    step.status = SALTS_EINVAL;
  }
  return step;
}

redis_cflow_connect_step redis_cflow_connection_connect_next(
    redis_cflow_connection *connection) {
  redis_cflow_connection_impl *impl = redis_cflow_connection_get(connection);
  redis_cflow_connect_step step = {
      REDIS_CFLOW_CONNECT_ERROR, {0}, SALTS_EINVAL};
  if (!impl || impl->active_stream) return step;
  step.status = redis_cflow_cleanup_socket(impl);
  if (step.status != SALTS_OK) return step;
  if (impl->usable) {
    step.kind = REDIS_CFLOW_CONNECT_DONE;
    step.status = SALTS_OK;
    return step;
  }
  if (!impl->addresses) return step;
  for (;;) {
    if (redis_io_flow_active(&impl->io)) {
      redis_io_flow_step io = redis_io_flow_next(&impl->io);
      cflow_io_completion completion;
      if (io.kind == REDIS_IO_FLOW_WAIT)
        return redis_cflow_connect_wait(impl);
      if (io.kind != REDIS_IO_FLOW_VALUE) {
        step.status = io.status;
        return step;
      }
      completion = io.completion;
      if (completion.kind == CFLOW_IO_COMPLETION_OK) {
        impl->usable = 1;
        step.kind = REDIS_CFLOW_CONNECT_DONE;
        step.status = SALTS_OK;
        return step;
      }
      impl->connect_status = completion.error != 0
                                 ? completion.error : SALTS_ENOTCONN;
      if (impl->socket != REDIS_SOCKET_INVALID) {
        int cleanup_status;
        impl->socket_cleanup_pending = 1;
        cleanup_status = redis_cflow_cleanup_socket(impl);
        if (cleanup_status != SALTS_OK) {
          step.status = cleanup_status;
          return step;
        }
      }
    }
    if (impl->next_address == impl->address_count) {
      step.status = impl->connect_status != 0
                        ? impl->connect_status : SALTS_EHOSTUNREACH;
      return step;
    }
    {
      redis_socket_address *address = &impl->addresses[impl->next_address++];
      cflow_io_native_operation operation;
      int status = redis_socket_open(address, &impl->socket);
      if (status != SALTS_OK) {
        impl->connect_status = status;
        continue;
      }
      memset(&operation, 0, sizeof(operation));
      operation.kind = CFLOW_IO_NATIVE_TCP_CONNECT;
      operation.socket = impl->socket;
      operation.address = address->storage.bytes;
      operation.address_capacity = address->length;
      operation.address_length = address->length;
      status = redis_io_flow_submit(&impl->io, &operation);
      if (status != SALTS_OK) {
        int cleanup_status;
        impl->socket_cleanup_pending = 1;
        cleanup_status = redis_cflow_cleanup_socket(impl);
        if (cleanup_status != SALTS_OK) {
          step.status = cleanup_status;
          return step;
        }
        step.status = status;
        return step;
      }
      return redis_cflow_connect_wait(impl);
    }
  }
}

int redis_cflow_connection_valid(const redis_cflow_connection *connection) {
  return redis_cflow_connection_get(connection) != NULL;
}

int redis_cflow_connection_usable(const redis_cflow_connection *connection) {
  redis_cflow_connection_impl *impl = redis_cflow_connection_get(connection);
  return impl != NULL && impl->usable;
}

int redis_cflow_connection_close(redis_cflow_connection *connection) {
  redis_cflow_connection_impl *impl = redis_cflow_connection_get(connection);
  int status = SALTS_OK;
  if (!impl) return SALTS_EINVAL;
  if (impl->active_stream) return SALTS_EBUSY;
  impl->usable = 0;
  if (redis_io_flow_active(&impl->io)) {
    status = redis_io_flow_cancel(&impl->io);
    if (status != SALTS_OK) return status;
  }
  status = redis_cflow_cleanup_socket(impl);
  if (status != SALTS_OK) return status;
  if (impl->socket == REDIS_SOCKET_INVALID) return SALTS_OK;
  if (impl->owns_socket) {
    impl->socket_cleanup_pending = 1;
    status = redis_cflow_cleanup_socket(impl);
    if (status != SALTS_OK) return status;
  } else {
    impl->socket = REDIS_SOCKET_INVALID;
  }
  return status;
}

int redis_cflow_connection_destroy(redis_cflow_connection *connection) {
  redis_cflow_connection_impl *impl = redis_cflow_connection_get(connection);
  int status;
  if (!impl) return SALTS_EINVAL;
  if (impl->active_stream) return SALTS_EBUSY;
  status = redis_cflow_connection_close(connection);
  if (status != SALTS_OK) return status;
  status = redis_io_flow_destroy(&impl->io);
  if (status != SALTS_OK) return status;
  tstr_free(impl->command);
  free(impl->addresses);
  redis_resp_buffer_destroy(&impl->buffer);
  free(impl->receive_chunk);
  free(impl);
  connection->impl = NULL;
  return SALTS_OK;
}

static int redis_cflow_stream_open_mode(
    redis_cflow_connection *connection, int argc, const char **argv,
    const size_t *argvlen, size_t max_reply_bytes, size_t max_items,
    redis_cflow_mode mode, redis_cflow_stream *out_stream) {
  redis_cflow_connection_impl *owner = redis_cflow_connection_get(connection);
  redis_cflow_stream_impl *stream;
  tstr command = NULL;
  int status;
  if (!owner || !out_stream || out_stream->impl || argc <= 0 || !argv ||
      max_reply_bytes == 0u ||
      (mode == REDIS_CFLOW_ARRAY_ITEMS && max_items == 0u))
    return SALTS_EINVAL;
  if (!owner->usable) return SALTS_ENOTCONN;
  if (owner->active_stream) return SALTS_EBUSY;
  status = redis_resp_command_build_bounded(
      argc, argv, argvlen, owner->max_command_bytes, &command);
  if (status != SALTS_OK) return status;
  stream = (redis_cflow_stream_impl *)calloc(1u, sizeof(*stream));
  if (!stream) {
    tstr_free(command);
    return SALTS_ENOMEM;
  }
  tstr_free(owner->command);
  owner->command = command;
  stream->connection = owner;
  stream->phase = REDIS_CFLOW_SENDING;
  stream->mode = mode;
  stream->outcome = REDIS_COMMAND_NOT_SENT;
  stream->terminal = (redis_cflow_stream_step)REDIS_CFLOW_STREAM_STEP_INIT;
  redis_resp_array_reader_init(&stream->reader, max_items, max_reply_bytes);
  owner->active_stream = stream;
  out_stream->impl = stream;
  return SALTS_OK;
}

int redis_cflow_stream_open(
    redis_cflow_connection *connection, int argc, const char **argv,
    const size_t *argvlen, size_t max_reply_bytes, size_t max_items,
    redis_cflow_stream *out_stream) {
  return redis_cflow_stream_open_mode(
      connection, argc, argv, argvlen, max_reply_bytes, max_items,
      REDIS_CFLOW_ARRAY_ITEMS, out_stream);
}

int redis_cflow_command_open(
    redis_cflow_connection *connection, int argc, const char **argv,
    const size_t *argvlen, size_t max_reply_bytes,
    redis_cflow_stream *out_stream) {
  return redis_cflow_stream_open_mode(
      connection, argc, argv, argvlen, max_reply_bytes, 0u,
      REDIS_CFLOW_SINGLE_REPLY, out_stream);
}

redis_cflow_stream_step redis_cflow_stream_next(redis_cflow_stream *stream_) {
  redis_cflow_stream_impl *stream = redis_cflow_stream_get(stream_);
  redis_cflow_connection_impl *connection;
  cflow_io_completion completion;
  if (!stream) {
    redis_cflow_stream_step invalid = REDIS_CFLOW_STREAM_STEP_INIT;
    invalid.kind = REDIS_CFLOW_STREAM_ERROR;
    invalid.status = SALTS_EINVAL;
    return invalid;
  }
  if (stream->phase == REDIS_CFLOW_TERMINAL) {
    redis_cflow_stream_step terminal = stream->terminal;
    stream->terminal.item = NULL;
    return terminal;
  }
  connection = stream->connection;
  if (!connection->usable)
    return redis_cflow_fail(stream, SALTS_ENOTCONN, stream->outcome, NULL, 0);

  if (redis_io_flow_active(&connection->io)) {
    redis_io_flow_step io = redis_io_flow_next(&connection->io);
    if (io.kind == REDIS_IO_FLOW_WAIT) return redis_cflow_wait(stream);
    if (io.kind != REDIS_IO_FLOW_VALUE)
      return redis_cflow_fail(stream, io.status, stream->outcome, NULL, 0);
    completion = io.completion;
    if (completion.kind != CFLOW_IO_COMPLETION_OK || completion.bytes == 0u) {
      int status = completion.kind == CFLOW_IO_COMPLETION_EOF
                       ? SALTS_ENOTCONN
                       : completion.error != 0 ? completion.error
                                               : SALTS_ECANCELED;
      return redis_cflow_fail(stream, status, stream->outcome, NULL, 0);
    }
    if (stream->phase == REDIS_CFLOW_SENDING) {
      size_t remaining = tstr_len(connection->command) - stream->send_offset;
      if (completion.bytes > remaining)
        return redis_cflow_fail(stream, SALTS_EPROTO,
                                REDIS_COMMAND_SEND_UNCERTAIN, NULL, 0);
      stream->send_offset += completion.bytes;
      stream->outcome = REDIS_COMMAND_SEND_UNCERTAIN;
    } else {
      int appended = redis_resp_buffer_append_bounded(
          &connection->buffer, (const char *)connection->receive_chunk,
          completion.bytes, connection->max_buffer_bytes);
      if (appended != 0)
        return redis_cflow_fail(
            stream, appended == -2 ? SALTS_ENOBUFS : SALTS_ENOMEM,
            REDIS_COMMAND_REPLY_UNKNOWN, NULL, 0);
    }
  }

  if (stream->phase == REDIS_CFLOW_SENDING) {
    if (stream->send_offset < tstr_len(connection->command)) {
      int status = redis_cflow_submit_send(stream);
      if (status != SALTS_OK)
        return redis_cflow_fail(stream, status,
                                stream->send_offset == 0u
                                    ? REDIS_COMMAND_NOT_SENT
                                    : REDIS_COMMAND_SEND_UNCERTAIN,
                                NULL, 0);
      return redis_cflow_wait(stream);
    }
    stream->phase = REDIS_CFLOW_RECEIVING;
    stream->outcome = REDIS_COMMAND_REPLY_UNKNOWN;
  }

  for (;;) {
    redis_reply_t *item = NULL;
    redis_resp_array_step parsed;
    if (stream->mode == REDIS_CFLOW_SINGLE_REPLY) {
      int consumed;
      if (stream->single_reply_read) {
        stream->phase = REDIS_CFLOW_TERMINAL;
        stream->terminal =
            (redis_cflow_stream_step)REDIS_CFLOW_STREAM_STEP_INIT;
        stream->terminal.outcome = REDIS_COMMAND_REPLIED;
        redis_cflow_release_active(stream);
        return stream->terminal;
      }
      consumed = redis_resp_reply_reader_next_buffer(
          &connection->buffer, &stream->reader.item_reader, &item);
      if (consumed > 0) {
        if (redis_resp_buffer_consume(&connection->buffer,
                                      (size_t)consumed) != 0) {
          redis_reply_free(item);
          return redis_cflow_fail(stream, SALTS_EPROTO,
                                  REDIS_COMMAND_REPLY_UNKNOWN, NULL, 0);
        }
        stream->single_reply_read = 1;
        if (item->type == REDIS_REPLY_ERROR)
          return redis_cflow_fail(stream, SALTS_EIO, REDIS_COMMAND_REPLIED,
                                  item, 1);
        {
          redis_cflow_stream_step step = REDIS_CFLOW_STREAM_STEP_INIT;
          step.kind = REDIS_CFLOW_STREAM_ITEM;
          step.outcome = REDIS_COMMAND_REPLIED;
          step.item = item;
          return step;
        }
      }
      if (consumed < 0)
        return redis_cflow_fail(
            stream, consumed == -3 ? SALTS_ENOMEM
                                    : consumed == -2 ? SALTS_ENOBUFS
                                                     : SALTS_EPROTO,
            REDIS_COMMAND_REPLY_UNKNOWN, NULL, 0);
      parsed = REDIS_RESP_ARRAY_NEED_MORE;
    } else {
      parsed = redis_resp_array_reader_next_buffer(
          &connection->buffer, &stream->reader, &item);
    }
    if (parsed == REDIS_RESP_ARRAY_ITEM) {
      redis_cflow_stream_step step = REDIS_CFLOW_STREAM_STEP_INIT;
      step.kind = REDIS_CFLOW_STREAM_ITEM;
      step.outcome = REDIS_COMMAND_REPLIED;
      step.item = item;
      return step;
    }
    if (parsed == REDIS_RESP_ARRAY_DONE) {
      stream->phase = REDIS_CFLOW_TERMINAL;
      stream->terminal = (redis_cflow_stream_step)REDIS_CFLOW_STREAM_STEP_INIT;
      stream->terminal.outcome = REDIS_COMMAND_REPLIED;
      redis_cflow_release_active(stream);
      return stream->terminal;
    }
    if (parsed == REDIS_RESP_ARRAY_SERVER_ERROR)
      return redis_cflow_fail(stream, SALTS_EIO, REDIS_COMMAND_REPLIED,
                              item, 1);
    if (parsed == REDIS_RESP_ARRAY_LIMIT)
      return redis_cflow_fail(stream, SALTS_ENOBUFS,
                              REDIS_COMMAND_REPLY_UNKNOWN, NULL, 0);
    if (parsed == REDIS_RESP_ARRAY_OOM)
      return redis_cflow_fail(stream, SALTS_ENOMEM,
                              REDIS_COMMAND_REPLY_UNKNOWN, NULL, 0);
    if (parsed == REDIS_RESP_ARRAY_ERROR)
      return redis_cflow_fail(stream, SALTS_EPROTO,
                              REDIS_COMMAND_REPLY_UNKNOWN, NULL, 0);
    {
      int status = redis_cflow_submit_receive(stream);
      if (status != SALTS_OK)
        return redis_cflow_fail(stream, status,
                                REDIS_COMMAND_REPLY_UNKNOWN, NULL, 0);
      return redis_cflow_wait(stream);
    }
  }
}

int redis_cflow_stream_cancel(redis_cflow_stream *stream_) {
  redis_cflow_stream_impl *stream = redis_cflow_stream_get(stream_);
  int status;
  if (!stream) return SALTS_EINVAL;
  if (stream->phase == REDIS_CFLOW_TERMINAL) return SALTS_OK;
  stream->connection->usable = 0;
  redis_resp_buffer_reset(&stream->connection->buffer);
  if (!redis_io_flow_active(&stream->connection->io)) {
    stream->phase = REDIS_CFLOW_TERMINAL;
    redis_cflow_release_active(stream);
    return SALTS_OK;
  }
  status = redis_io_flow_cancel(&stream->connection->io);
  if (status != SALTS_OK) return status;
  stream->phase = REDIS_CFLOW_TERMINAL;
  redis_cflow_release_active(stream);
  return SALTS_OK;
}

int redis_cflow_stream_destroy(redis_cflow_stream *stream_) {
  redis_cflow_stream_impl *stream = redis_cflow_stream_get(stream_);
  int status;
  if (!stream) return SALTS_EINVAL;
  status = redis_cflow_stream_cancel(stream_);
  if (status != SALTS_OK) return status;
  stream->phase = REDIS_CFLOW_TERMINAL;
  redis_cflow_release_active(stream);
  redis_resp_array_reader_destroy(&stream->reader);
  free(stream);
  stream_->impl = NULL;
  return SALTS_OK;
}

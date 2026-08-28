#include "redis_pool.h"

#include "turbo_error.h"
#include "turbo_str.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum redis_pool_slot_state {
  REDIS_POOL_SLOT_CLOSED = 0,
  REDIS_POOL_SLOT_CONNECTING,
  REDIS_POOL_SLOT_PREPARING,
  REDIS_POOL_SLOT_IDLE,
  REDIS_POOL_SLOT_BORROWED,
  REDIS_POOL_SLOT_INVALID
} redis_pool_slot_state;

typedef enum redis_pool_phase {
  REDIS_POOL_PHASE_CONNECTING = 0,
  REDIS_POOL_PHASE_READY,
  REDIS_POOL_PHASE_FAILED,
  REDIS_POOL_PHASE_CLOSING,
  REDIS_POOL_PHASE_CLOSED
} redis_pool_phase;

typedef enum redis_pool_prepare_stage {
  REDIS_POOL_PREPARE_AUTH = 0,
  REDIS_POOL_PREPARE_SELECT,
  REDIS_POOL_PREPARE_READONLY,
  REDIS_POOL_PREPARE_DONE
} redis_pool_prepare_stage;

typedef struct redis_pool_slot {
  redis_cflow_connection connection;
  redis_pool_slot_state state;
  size_t stream_refs;
} redis_pool_slot;

typedef struct redis_pool_impl {
  redis_io_runtime *runtime;
  tstr host;
  uint16_t port;
  tstr username;
  tstr password;
  int database;
  int readonly;
  size_t capacity;
  size_t address_capacity;
  size_t max_command_bytes;
  size_t initial_buffer_bytes;
  size_t max_buffer_bytes;
  size_t receive_chunk_bytes;
  size_t prepare_reply_bytes;
  uint64_t cancel_timeout_ns;
  redis_pool_slot *slots;
  redis_pool_phase phase;
  size_t connect_index;
  redis_pool_prepare_stage prepare_stage;
  redis_cflow_stream prepare_stream;
  int prepare_item_seen;
  int repairing;
  redis_pool_stats stats;
} redis_pool_impl;

typedef struct redis_pool_stream_impl {
  redis_pool_impl *pool;
  redis_pool_slot *slot;
  redis_cflow_stream inner;
  int terminal;
  int released;
} redis_pool_stream_impl;

static redis_pool_impl *redis_pool_get(redis_pool *pool) {
  return pool ? (redis_pool_impl *)pool->impl : NULL;
}

static const redis_pool_impl *redis_pool_get_const(const redis_pool *pool) {
  return pool ? (const redis_pool_impl *)pool->impl : NULL;
}

static redis_pool_stream_impl *redis_pool_stream_get(
    redis_pool_stream *stream) {
  return stream ? (redis_pool_stream_impl *)stream->impl : NULL;
}

static tstr redis_pool_copy_string(const char *value) {
  return value ? tstr_new_len(value, strlen(value)) : NULL;
}

static redis_pool_connect_step redis_pool_connect_result(
    redis_pool_impl *impl, redis_pool_connect_step_kind kind, int status,
    cflow_waitable waitable) {
  redis_pool_connect_step step;
  memset(&step, 0, sizeof(step));
  step.kind = kind;
  step.status = status;
  step.waitable = waitable;
  step.connected_connections = impl ? impl->connect_index : 0u;
  return step;
}

static redis_pool_prepare_stage redis_pool_first_prepare(
    const redis_pool_impl *impl) {
  if (impl->password && tstr_len(impl->password) != 0u)
    return REDIS_POOL_PREPARE_AUTH;
  if (impl->database != 0) return REDIS_POOL_PREPARE_SELECT;
  if (impl->readonly) return REDIS_POOL_PREPARE_READONLY;
  return REDIS_POOL_PREPARE_DONE;
}

static redis_pool_prepare_stage redis_pool_next_prepare(
    const redis_pool_impl *impl, redis_pool_prepare_stage current) {
  if (current < REDIS_POOL_PREPARE_SELECT && impl->database != 0)
    return REDIS_POOL_PREPARE_SELECT;
  if (current < REDIS_POOL_PREPARE_READONLY && impl->readonly)
    return REDIS_POOL_PREPARE_READONLY;
  return REDIS_POOL_PREPARE_DONE;
}

static void redis_pool_mark_connect_failure(redis_pool_impl *impl,
                                            redis_pool_slot *slot) {
  slot->state = REDIS_POOL_SLOT_INVALID;
  impl->phase = impl->repairing ? REDIS_POOL_PHASE_READY
                                : REDIS_POOL_PHASE_FAILED;
}

static int redis_pool_open_prepare(redis_pool_impl *impl,
                                   redis_pool_slot *slot) {
  const char *arguments[3];
  size_t lengths[3];
  int count = 0;
  char database[32];
  if (impl->prepare_stage == REDIS_POOL_PREPARE_AUTH) {
    arguments[count] = "AUTH";
    lengths[count++] = 4u;
    if (impl->username && tstr_len(impl->username) != 0u) {
      arguments[count] = impl->username;
      lengths[count++] = tstr_len(impl->username);
    }
    arguments[count] = impl->password;
    lengths[count++] = tstr_len(impl->password);
  } else if (impl->prepare_stage == REDIS_POOL_PREPARE_SELECT) {
    int written = snprintf(database, sizeof(database), "%d", impl->database);
    if (written <= 0 || (size_t)written >= sizeof(database))
      return TURBO_EINVAL;
    arguments[0] = "SELECT";
    lengths[0] = 6u;
    arguments[1] = database;
    lengths[1] = (size_t)written;
    count = 2;
  } else if (impl->prepare_stage == REDIS_POOL_PREPARE_READONLY) {
    arguments[0] = "READONLY";
    lengths[0] = 8u;
    count = 1;
  } else {
    return TURBO_EINVAL;
  }
  return redis_cflow_command_open(&slot->connection, count, arguments, lengths,
                                  impl->prepare_reply_bytes,
                                  &impl->prepare_stream);
}

static int redis_pool_destroy_slot(redis_pool_slot *slot) {
  int status;
  if (!slot || !slot->connection.impl) {
    if (slot) slot->state = REDIS_POOL_SLOT_CLOSED;
    return TURBO_OK;
  }
  status = redis_cflow_connection_destroy(&slot->connection);
  if (status == TURBO_OK) slot->state = REDIS_POOL_SLOT_CLOSED;
  return status;
}

static void redis_pool_release_stream(redis_pool_stream_impl *stream,
                                      int failed) {
  redis_pool_impl *pool;
  if (!stream || stream->released) return;
  pool = stream->pool;
  if (failed)
    pool->stats.failed_commands++;
  else
    pool->stats.completed_commands++;
  if (pool->phase == REDIS_POOL_PHASE_READY &&
      redis_cflow_connection_usable(&stream->slot->connection)) {
    stream->slot->state = REDIS_POOL_SLOT_IDLE;
  } else {
    stream->slot->state = REDIS_POOL_SLOT_INVALID;
    if (pool->phase == REDIS_POOL_PHASE_CLOSING)
      (void)redis_pool_destroy_slot(stream->slot);
  }
  stream->released = 1;
}

int redis_pool_init(redis_pool *pool, const redis_pool_config *config) {
  redis_pool_impl *impl;
  size_t slots_bytes;
  if (!pool || pool->impl || !config ||
      !redis_io_runtime_valid(config->runtime) || !config->host ||
      config->host[0] == '\0' || config->port == 0u ||
      config->connection_capacity == 0u ||
      config->address_capacity == 0u || config->max_command_bytes == 0u ||
      config->initial_buffer_bytes == 0u ||
      config->max_buffer_bytes < config->initial_buffer_bytes ||
      config->receive_chunk_bytes == 0u || config->prepare_reply_bytes == 0u ||
      config->cancel_timeout_ns == 0u || config->database < 0 ||
      (config->username && config->username[0] != '\0' &&
       (!config->password || config->password[0] == '\0')) ||
      config->connection_capacity > SIZE_MAX / sizeof(redis_pool_slot))
    return TURBO_EINVAL;
  slots_bytes = config->connection_capacity * sizeof(redis_pool_slot);
  impl = (redis_pool_impl *)calloc(1, sizeof(*impl));
  if (!impl) return TURBO_ENOMEM;
  impl->slots = (redis_pool_slot *)calloc(1, slots_bytes);
  impl->host = redis_pool_copy_string(config->host);
  impl->username = redis_pool_copy_string(config->username);
  impl->password = redis_pool_copy_string(config->password);
  if (!impl->slots || !impl->host ||
      (config->username && !impl->username) ||
      (config->password && !impl->password)) {
    tstr_free(impl->password);
    tstr_free(impl->username);
    tstr_free(impl->host);
    free(impl->slots);
    free(impl);
    return TURBO_ENOMEM;
  }
  impl->runtime = config->runtime;
  impl->port = config->port;
  impl->database = config->database;
  impl->readonly = config->readonly != 0;
  impl->capacity = config->connection_capacity;
  impl->address_capacity = config->address_capacity;
  impl->max_command_bytes = config->max_command_bytes;
  impl->initial_buffer_bytes = config->initial_buffer_bytes;
  impl->max_buffer_bytes = config->max_buffer_bytes;
  impl->receive_chunk_bytes = config->receive_chunk_bytes;
  impl->prepare_reply_bytes = config->prepare_reply_bytes;
  impl->cancel_timeout_ns = config->cancel_timeout_ns;
  impl->phase = REDIS_POOL_PHASE_CONNECTING;
  impl->stats.connection_capacity = impl->capacity;
  pool->impl = impl;
  return TURBO_OK;
}

redis_pool_connect_step redis_pool_connect_next(redis_pool *pool) {
  redis_pool_impl *impl = redis_pool_get(pool);
  cflow_waitable empty_waitable;
  memset(&empty_waitable, 0, sizeof(empty_waitable));
  if (!impl)
    return redis_pool_connect_result(NULL, REDIS_POOL_CONNECT_ERROR,
                                     TURBO_EINVAL, empty_waitable);
  if (impl->phase == REDIS_POOL_PHASE_READY) {
    size_t index;
    for (index = 0u; index < impl->capacity; ++index) {
      if (impl->slots[index].state == REDIS_POOL_SLOT_INVALID) {
        if (impl->slots[index].stream_refs != 0u)
          return redis_pool_connect_result(
              impl, REDIS_POOL_CONNECT_ERROR, TURBO_EBUSY, empty_waitable);
        impl->phase = REDIS_POOL_PHASE_CONNECTING;
        impl->connect_index = index;
        impl->repairing = 1;
        break;
      }
    }
    if (impl->phase == REDIS_POOL_PHASE_READY)
      return redis_pool_connect_result(impl, REDIS_POOL_CONNECT_DONE,
                                       TURBO_OK, empty_waitable);
  }
  if (impl->phase != REDIS_POOL_PHASE_CONNECTING)
    return redis_pool_connect_result(impl, REDIS_POOL_CONNECT_ERROR,
                                     TURBO_ESHUTDOWN, empty_waitable);

  while (impl->connect_index < impl->capacity) {
    redis_pool_slot *slot = &impl->slots[impl->connect_index];
    if (slot->state == REDIS_POOL_SLOT_IDLE ||
        slot->state == REDIS_POOL_SLOT_BORROWED) {
      impl->connect_index++;
      continue;
    }
    if (slot->state == REDIS_POOL_SLOT_INVALID) {
      int status;
      if (slot->stream_refs != 0u)
        return redis_pool_connect_result(impl, REDIS_POOL_CONNECT_ERROR,
                                         TURBO_EBUSY, empty_waitable);
      status = redis_pool_destroy_slot(slot);
      if (status != TURBO_OK) {
        impl->phase = impl->repairing ? REDIS_POOL_PHASE_READY
                                      : REDIS_POOL_PHASE_FAILED;
        return redis_pool_connect_result(impl, REDIS_POOL_CONNECT_ERROR,
                                         status, empty_waitable);
      }
    }
    if (slot->state == REDIS_POOL_SLOT_CLOSED) {
      redis_cflow_open_config open_config = {
          impl->runtime,
          impl->host,
          impl->port,
          impl->address_capacity,
          impl->max_command_bytes,
          impl->initial_buffer_bytes,
          impl->max_buffer_bytes,
          impl->receive_chunk_bytes,
          impl->cancel_timeout_ns};
      int status = redis_cflow_connection_open(&slot->connection, &open_config);
      if (status != TURBO_OK) {
        redis_pool_mark_connect_failure(impl, slot);
        return redis_pool_connect_result(impl, REDIS_POOL_CONNECT_ERROR,
                                         status, empty_waitable);
      }
      slot->state = REDIS_POOL_SLOT_CONNECTING;
    }
    if (slot->state == REDIS_POOL_SLOT_CONNECTING) {
      redis_cflow_connect_step connected =
          redis_cflow_connection_connect_next(&slot->connection);
      if (connected.kind == REDIS_CFLOW_CONNECT_WAIT)
        return redis_pool_connect_result(impl, REDIS_POOL_CONNECT_WAIT,
                                         TURBO_OK, connected.waitable);
      if (connected.kind == REDIS_CFLOW_CONNECT_ERROR) {
        if (connected.status == TURBO_EBUSY)
          return redis_pool_connect_result(impl, REDIS_POOL_CONNECT_ERROR,
                                           TURBO_EBUSY, empty_waitable);
        redis_pool_mark_connect_failure(impl, slot);
        return redis_pool_connect_result(impl, REDIS_POOL_CONNECT_ERROR,
                                         connected.status, empty_waitable);
      }
      slot->state = REDIS_POOL_SLOT_PREPARING;
      impl->prepare_stage = redis_pool_first_prepare(impl);
    }
    while (slot->state == REDIS_POOL_SLOT_PREPARING) {
      redis_cflow_stream_step prepared;
      if (impl->prepare_stage == REDIS_POOL_PREPARE_DONE) {
        slot->state = REDIS_POOL_SLOT_IDLE;
        impl->connect_index++;
        break;
      }
      if (!impl->prepare_stream.impl) {
        int status = redis_pool_open_prepare(impl, slot);
        if (status != TURBO_OK) {
          redis_pool_mark_connect_failure(impl, slot);
          return redis_pool_connect_result(impl, REDIS_POOL_CONNECT_ERROR,
                                           status, empty_waitable);
        }
        impl->prepare_item_seen = 0;
      }
      prepared = redis_cflow_stream_next(&impl->prepare_stream);
      if (prepared.kind == REDIS_CFLOW_STREAM_WAIT)
        return redis_pool_connect_result(impl, REDIS_POOL_CONNECT_WAIT,
                                         TURBO_OK, prepared.waitable);
      if (prepared.kind == REDIS_CFLOW_STREAM_ITEM) {
        impl->prepare_item_seen = 1;
        redis_reply_free(prepared.item);
        continue;
      }
      if (prepared.kind == REDIS_CFLOW_STREAM_ERROR) {
        int status = prepared.status;
        redis_reply_free(prepared.item);
        (void)redis_cflow_stream_destroy(&impl->prepare_stream);
        redis_pool_mark_connect_failure(impl, slot);
        return redis_pool_connect_result(impl, REDIS_POOL_CONNECT_ERROR,
                                         status, empty_waitable);
      }
      if (!impl->prepare_item_seen) {
        (void)redis_cflow_stream_destroy(&impl->prepare_stream);
        redis_pool_mark_connect_failure(impl, slot);
        return redis_pool_connect_result(impl, REDIS_POOL_CONNECT_ERROR,
                                         TURBO_EPROTO, empty_waitable);
      }
      if (redis_cflow_stream_destroy(&impl->prepare_stream) != TURBO_OK) {
        redis_pool_mark_connect_failure(impl, slot);
        return redis_pool_connect_result(impl, REDIS_POOL_CONNECT_ERROR,
                                         TURBO_EBUSY, empty_waitable);
      }
      impl->prepare_stage =
          redis_pool_next_prepare(impl, impl->prepare_stage);
    }
  }
  impl->repairing = 0;
  impl->phase = REDIS_POOL_PHASE_READY;
  return redis_pool_connect_result(impl, REDIS_POOL_CONNECT_DONE, TURBO_OK,
                                   empty_waitable);
}

int redis_pool_ready(const redis_pool *pool) {
  const redis_pool_impl *impl = redis_pool_get_const(pool);
  return impl && impl->phase == REDIS_POOL_PHASE_READY;
}

int redis_pool_command_open(redis_pool *pool, int argc, const char **argv,
                            const size_t *argvlen, size_t max_reply_bytes,
                            redis_pool_stream *out_stream) {
  redis_pool_impl *impl = redis_pool_get(pool);
  redis_pool_stream_impl *stream;
  redis_pool_slot *slot = NULL;
  size_t index;
  int status;
  if (!impl || impl->phase != REDIS_POOL_PHASE_READY || argc <= 0 || !argv ||
      max_reply_bytes == 0u || !out_stream || out_stream->impl)
    return impl && impl->phase != REDIS_POOL_PHASE_READY ? TURBO_ESHUTDOWN
                                                         : TURBO_EINVAL;
  for (index = 0; index < impl->capacity; ++index) {
    if (impl->slots[index].state == REDIS_POOL_SLOT_IDLE) {
      slot = &impl->slots[index];
      break;
    }
  }
  if (!slot) {
    impl->stats.rejected_commands++;
    return TURBO_ENOBUFS;
  }
  stream = (redis_pool_stream_impl *)calloc(1, sizeof(*stream));
  if (!stream) return TURBO_ENOMEM;
  slot->state = REDIS_POOL_SLOT_BORROWED;
  stream->pool = impl;
  stream->slot = slot;
  status = redis_cflow_command_open(&slot->connection, argc, argv, argvlen,
                                    max_reply_bytes, &stream->inner);
  if (status != TURBO_OK) {
    slot->state = redis_cflow_connection_usable(&slot->connection)
                      ? REDIS_POOL_SLOT_IDLE
                      : REDIS_POOL_SLOT_INVALID;
    free(stream);
    return status;
  }
  impl->stats.admitted_commands++;
  slot->stream_refs++;
  out_stream->impl = stream;
  return TURBO_OK;
}

redis_cflow_stream_step redis_pool_stream_next(redis_pool_stream *stream_) {
  redis_pool_stream_impl *stream = redis_pool_stream_get(stream_);
  redis_cflow_stream_step step = REDIS_CFLOW_STREAM_STEP_INIT;
  if (!stream) {
    step.kind = REDIS_CFLOW_STREAM_ERROR;
    step.status = TURBO_EINVAL;
    return step;
  }
  step = redis_cflow_stream_next(&stream->inner);
  if (!stream->terminal &&
      (step.kind == REDIS_CFLOW_STREAM_DONE ||
       step.kind == REDIS_CFLOW_STREAM_ERROR)) {
    stream->terminal = 1;
    redis_pool_release_stream(stream,
                              step.kind == REDIS_CFLOW_STREAM_ERROR);
  }
  return step;
}

int redis_pool_stream_cancel(redis_pool_stream *stream_) {
  redis_pool_stream_impl *stream = redis_pool_stream_get(stream_);
  int status;
  if (!stream) return TURBO_EINVAL;
  if (stream->terminal) return TURBO_OK;
  status = redis_cflow_stream_cancel(&stream->inner);
  if (status == TURBO_OK) {
    stream->terminal = 1;
    stream->pool->stats.cancelled_commands++;
    redis_pool_release_stream(stream, 1);
  }
  return status;
}

int redis_pool_stream_destroy(redis_pool_stream *stream_) {
  redis_pool_stream_impl *stream = redis_pool_stream_get(stream_);
  int status;
  if (!stream) return TURBO_EINVAL;
  if (!stream->terminal) {
    status = redis_pool_stream_cancel(stream_);
    if (status != TURBO_OK) return status;
  }
  status = redis_cflow_stream_destroy(&stream->inner);
  if (status != TURBO_OK) return status;
  if (stream->slot->stream_refs == 0u) return TURBO_EINVAL;
  stream->slot->stream_refs--;
  redis_pool_release_stream(stream, 0);
  free(stream);
  stream_->impl = NULL;
  return TURBO_OK;
}

int redis_pool_close(redis_pool *pool) {
  redis_pool_impl *impl = redis_pool_get(pool);
  size_t index;
  int result = TURBO_OK;
  if (!impl) return TURBO_EINVAL;
  if (impl->phase == REDIS_POOL_PHASE_CLOSED) return TURBO_OK;
  for (index = 0; index < impl->capacity; ++index) {
    if (impl->slots[index].state == REDIS_POOL_SLOT_BORROWED ||
        impl->slots[index].stream_refs != 0u)
      return TURBO_EBUSY;
  }
  impl->phase = REDIS_POOL_PHASE_CLOSING;
  if (impl->prepare_stream.impl) {
    result = redis_cflow_stream_destroy(&impl->prepare_stream);
    if (result != TURBO_OK) return result;
  }
  for (index = 0; index < impl->capacity; ++index) {
    int status = redis_pool_destroy_slot(&impl->slots[index]);
    if (status != TURBO_OK && result == TURBO_OK) result = status;
  }
  if (result == TURBO_OK) impl->phase = REDIS_POOL_PHASE_CLOSED;
  return result;
}

int redis_pool_destroy(redis_pool *pool) {
  redis_pool_impl *impl = redis_pool_get(pool);
  int status;
  if (!impl) return TURBO_EINVAL;
  status = redis_pool_close(pool);
  if (status != TURBO_OK) return status;
  tstr_free(impl->password);
  tstr_free(impl->username);
  tstr_free(impl->host);
  free(impl->slots);
  free(impl);
  pool->impl = NULL;
  return TURBO_OK;
}

void redis_pool_get_stats(const redis_pool *pool, redis_pool_stats *stats) {
  const redis_pool_impl *impl = redis_pool_get_const(pool);
  size_t index;
  if (!stats) return;
  memset(stats, 0, sizeof(*stats));
  if (!impl) return;
  *stats = impl->stats;
  for (index = 0; index < impl->capacity; ++index) {
    if (impl->slots[index].state == REDIS_POOL_SLOT_IDLE)
      stats->idle_connections++;
    else if (impl->slots[index].state == REDIS_POOL_SLOT_BORROWED)
      stats->borrowed_connections++;
    else if (impl->slots[index].state == REDIS_POOL_SLOT_INVALID)
      stats->invalid_connections++;
  }
}

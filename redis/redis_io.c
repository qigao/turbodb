#include "redis_io.h"

#include "turbo_error.h"
#include "turbo_thread.h"
#include <turbo/clock.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct redis_io_runtime_impl redis_io_runtime_impl;
typedef struct redis_io_slot redis_io_slot;

typedef struct redis_io_retiring_socket {
  uintptr_t identity;
  bool active;
} redis_io_retiring_socket;

typedef struct redis_io_wait_token {
  redis_io_runtime *runtime;
  redis_io_slot *slot;
  uint64_t generation;
} redis_io_wait_token;

struct redis_io_slot {
  cflow_io_native_operation operation;
  redis_io_runtime_impl *owner;
  cflow_io_request_id request_id;
  uint64_t generation;
  cflow_io_completion completion;
  cflow_waker waker;
  bool in_use;
  bool delivered;
  bool abandoned;
  bool wake_inflight;
  uint64_t wake_generation;
  bool release_pending;
  redis_io_wait_token wait_tokens[2];
};

struct redis_io_runtime_impl {
  cflow_io_native_backend backend;
  cflow_executor executor;
  cflow_io_actor actor;
  redis_io_slot *slots;
  redis_io_retiring_socket *retiring_sockets;
  size_t slot_capacity;
  size_t drive_batch_capacity;
  turbo_mutex_t gate;
  turbo_cond_t changed;
  bool driver_active;
  bool drive_pending;
  bool closing;
  bool destroying;
};

static TURBO_THREAD_LOCAL redis_io_slot *redis_io_current_wake_slot;
static TURBO_THREAD_LOCAL unsigned redis_io_driver_depth;

cflow_io_native_backend_kind redis_io_default_backend_kind(void) {
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

static redis_io_runtime_impl *redis_io_impl(redis_io_runtime *runtime) {
  return runtime != NULL ? (redis_io_runtime_impl *)runtime->impl : NULL;
}

static const redis_io_runtime_impl *redis_io_const_impl(
    const redis_io_runtime *runtime) {
  return runtime != NULL ? (const redis_io_runtime_impl *)runtime->impl : NULL;
}

static redis_io_slot *redis_io_slot_lock(
    const redis_io_request *request, redis_io_runtime_impl **out_impl) {
  redis_io_runtime_impl *impl;
  redis_io_slot *slot;
  if (request == NULL || request->runtime == NULL || out_impl == NULL)
    return NULL;
  impl = redis_io_impl(request->runtime);
  if (impl == NULL || request->slot >= impl->slot_capacity) return NULL;
  turbo_mutex_lock(&impl->gate);
  slot = &impl->slots[request->slot];
  if (!slot->in_use || slot->generation != request->generation) {
    turbo_mutex_unlock(&impl->gate);
    return NULL;
  }
  *out_impl = impl;
  return slot;
}

static bool redis_io_slot_matches_locked(const redis_io_slot *slot,
                                         const redis_io_request *request) {
  return slot->in_use && slot->generation == request->generation;
}

static redis_io_submit_status redis_io_map_submit(
    cflow_io_submit_status status) {
  switch (status) {
    case CFLOW_IO_SUBMIT_ACCEPTED:
      return REDIS_IO_SUBMIT_ACCEPTED;
    case CFLOW_IO_SUBMIT_FULL:
      return REDIS_IO_SUBMIT_FULL;
    case CFLOW_IO_SUBMIT_CLOSED:
      return REDIS_IO_SUBMIT_CLOSED;
    case CFLOW_IO_SUBMIT_LEASE_IN_USE:
      return REDIS_IO_SUBMIT_LEASE_IN_USE;
    case CFLOW_IO_SUBMIT_ID_EXHAUSTED:
      return REDIS_IO_SUBMIT_ID_EXHAUSTED;
    case CFLOW_IO_SUBMIT_INVALID_ARGUMENT:
    default:
      return REDIS_IO_SUBMIT_INVALID_ARGUMENT;
  }
}

static void redis_io_slot_reset_locked(redis_io_slot *slot) {
  redis_io_runtime_impl *impl = slot->owner;
  memset(&slot->operation, 0, sizeof(slot->operation));
  slot->request_id = 0u;
  slot->completion = (cflow_io_completion){0};
  slot->waker = (cflow_waker){0};
  slot->in_use = false;
  slot->delivered = false;
  slot->abandoned = false;
  slot->wake_inflight = false;
  slot->wake_generation = 0u;
  slot->release_pending = false;
  turbo_cond_broadcast(&impl->changed);
}

static void redis_io_slot_release(void *operation_user) {
  redis_io_slot *slot = (redis_io_slot *)operation_user;
  redis_io_runtime_impl *impl;
  if (slot == NULL || slot->owner == NULL) return;
  impl = slot->owner;
  turbo_mutex_lock(&impl->gate);
  if (slot->wake_inflight)
    slot->release_pending = true;
  else
    redis_io_slot_reset_locked(slot);
  turbo_mutex_unlock(&impl->gate);
}

static void redis_io_slot_rollback_submit(redis_io_slot *slot,
                                          uint64_t generation) {
  redis_io_runtime_impl *impl;
  bool wake_inflight;
  uint64_t wake_generation;
  if (slot == NULL || slot->owner == NULL) return;
  impl = slot->owner;
  turbo_mutex_lock(&impl->gate);
  if (!slot->in_use || slot->generation != generation) {
    turbo_mutex_unlock(&impl->gate);
    return;
  }
  wake_inflight = slot->wake_inflight;
  wake_generation = slot->wake_generation;
  redis_io_slot_reset_locked(slot);
  slot->wake_inflight = wake_inflight;
  slot->wake_generation = wake_generation;
  turbo_mutex_unlock(&impl->gate);
}

static void redis_io_actor_completion(
    void *user, cflow_io_request_id request_id, cflow_io_lease_id lease_id,
    void *operation_user, const cflow_io_completion *completion) {
  redis_io_runtime_impl *impl = (redis_io_runtime_impl *)user;
  redis_io_slot *slot = (redis_io_slot *)operation_user;
  (void)lease_id;
  if (impl == NULL || slot == NULL || completion == NULL) return;
  turbo_mutex_lock(&impl->gate);
  if (slot->in_use &&
      (slot->request_id == 0u || slot->request_id == request_id)) {
    slot->request_id = request_id;
    slot->completion = *completion;
    slot->delivered = true;
    turbo_cond_broadcast(&impl->changed);
  }
  turbo_mutex_unlock(&impl->gate);
}

static bool redis_io_run_one_waker(redis_io_runtime_impl *impl) {
  redis_io_slot *slot = NULL;
  cflow_waker waker = {0};
  uint64_t generation = 0u;
  size_t index;
  turbo_mutex_lock(&impl->gate);
  for (index = 0u; index < impl->slot_capacity; ++index) {
    redis_io_slot *candidate = &impl->slots[index];
    if (candidate->in_use && candidate->delivered &&
        !candidate->wake_inflight && candidate->waker.wake != NULL) {
      slot = candidate;
      waker = candidate->waker;
      generation = candidate->generation;
      candidate->waker = (cflow_waker){0};
      candidate->wake_inflight = true;
      candidate->wake_generation = generation;
      break;
    }
  }
  turbo_mutex_unlock(&impl->gate);
  if (slot == NULL) return false;
  redis_io_current_wake_slot = slot;
  waker.wake(waker.user);
  redis_io_current_wake_slot = NULL;
  turbo_mutex_lock(&impl->gate);
  if (slot->wake_inflight && slot->wake_generation == generation) {
    slot->wake_inflight = false;
    slot->wake_generation = 0u;
    if (slot->generation == generation && slot->release_pending)
      redis_io_slot_reset_locked(slot);
  }
  turbo_cond_broadcast(&impl->changed);
  turbo_mutex_unlock(&impl->gate);
  return true;
}

static cflow_io_request_id redis_io_abandoned_delivered(
    redis_io_runtime_impl *impl) {
  cflow_io_request_id request_id = 0u;
  size_t index;
  turbo_mutex_lock(&impl->gate);
  for (index = 0u; index < impl->slot_capacity; ++index) {
    redis_io_slot *slot = &impl->slots[index];
    if (slot->in_use && slot->abandoned && slot->delivered) {
      request_id = slot->request_id;
      break;
    }
  }
  turbo_mutex_unlock(&impl->gate);
  return request_id;
}

int redis_io_runtime_run_ready(redis_io_runtime *runtime, size_t max_steps,
                               size_t *progressed) {
  redis_io_runtime_impl *impl = redis_io_impl(runtime);
  size_t count = 0u;
  bool repeat = true;
  if (impl == NULL || max_steps == 0u || progressed == NULL)
    return TURBO_EINVAL;
  turbo_mutex_lock(&impl->gate);
  if (impl->destroying) {
    turbo_mutex_unlock(&impl->gate);
    return TURBO_EBUSY;
  }
  if (impl->driver_active) {
    impl->drive_pending = true;
    turbo_mutex_unlock(&impl->gate);
    *progressed = 0u;
    return TURBO_EBUSY;
  }
  impl->driver_active = true;
  impl->drive_pending = false;
  turbo_mutex_unlock(&impl->gate);
  ++redis_io_driver_depth;

  while (repeat && count < max_steps) {
    cflow_io_request_id abandoned = redis_io_abandoned_delivered(impl);
    cflow_io_run_result actor_result;
    if (abandoned != 0u) {
      if (cflow_io_actor_acknowledge(&impl->actor, abandoned) ==
          CFLOW_IO_ACK_RELEASED) {
        ++count;
        continue;
      }
    }
    actor_result = cflow_io_actor_run_one(&impl->actor);
    if (actor_result.status == CFLOW_IO_RUN_PROGRESSED) {
      ++count;
      continue;
    }
    if (cflow_executor_run_one(&impl->executor)) {
      ++count;
      continue;
    }
    if (redis_io_run_one_waker(impl)) {
      ++count;
      continue;
    }
    turbo_mutex_lock(&impl->gate);
    repeat = impl->drive_pending;
    impl->drive_pending = false;
    turbo_mutex_unlock(&impl->gate);
  }

  turbo_mutex_lock(&impl->gate);
  impl->driver_active = false;
  repeat = impl->drive_pending;
  turbo_cond_broadcast(&impl->changed);
  turbo_mutex_unlock(&impl->gate);
  --redis_io_driver_depth;
  *progressed = count;
  if (repeat && count < max_steps) {
    size_t additional = 0u;
    int status = redis_io_runtime_run_ready(
        runtime, max_steps - count, &additional);
    *progressed += additional;
    return status == TURBO_EBUSY ? TURBO_OK : status;
  }
  return TURBO_OK;
}

static void redis_io_actor_wake(void *user) {
  redis_io_runtime *runtime = (redis_io_runtime *)user;
  redis_io_runtime_impl *impl = redis_io_impl(runtime);
  size_t progressed = 0u;
  if (impl == NULL) return;
  (void)redis_io_runtime_run_ready(runtime, impl->drive_batch_capacity,
                                   &progressed);
}

static void redis_io_cleanup_init(redis_io_runtime *runtime,
                                  redis_io_runtime_impl *impl,
                                  bool backend_initialized,
                                  bool executor_initialized,
                                  bool actor_initialized) {
  if (impl == NULL) return;
  if (actor_initialized) {
    (void)cflow_io_actor_close(&impl->actor);
    (void)cflow_io_actor_destroy(&impl->actor);
  }
  if (backend_initialized) {
    int status = cflow_io_native_backend_shutdown(&impl->backend);
    if (status == TURBO_OK || status == TURBO_EALREADY)
      (void)cflow_io_native_backend_destroy(&impl->backend);
  }
  if (executor_initialized) {
    (void)cflow_executor_shutdown(&impl->executor);
    cflow_executor_destroy(&impl->executor);
  }
  turbo_cond_destroy(&impl->changed);
  turbo_mutex_destroy(&impl->gate);
  free(impl->slots);
  free(impl->retiring_sockets);
  free(impl);
  if (runtime != NULL) runtime->impl = NULL;
}

int redis_io_runtime_init(redis_io_runtime *runtime,
                          const redis_io_runtime_config *config) {
  redis_io_runtime_impl *impl;
  cflow_io_native_backend_config backend_config;
  cflow_io_actor_config actor_config;
  bool backend_initialized = false;
  bool executor_initialized = false;
  bool actor_initialized = false;
  size_t index;
  int status;
  if (runtime == NULL || runtime->impl != NULL || config == NULL ||
      config->request_capacity == 0u || config->command_capacity == 0u ||
      config->completion_batch_capacity == 0u ||
      config->request_capacity > SIZE_MAX / sizeof(redis_io_slot) ||
      config->request_capacity >
          SIZE_MAX / sizeof(redis_io_retiring_socket))
    return TURBO_EINVAL;
  impl = (redis_io_runtime_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return TURBO_ENOMEM;
  impl->slots = (redis_io_slot *)calloc(config->request_capacity,
                                        sizeof(*impl->slots));
  impl->retiring_sockets = (redis_io_retiring_socket *)calloc(
      config->request_capacity, sizeof(*impl->retiring_sockets));
  if (impl->slots == NULL || impl->retiring_sockets == NULL) {
    free(impl->retiring_sockets);
    free(impl->slots);
    free(impl);
    return TURBO_ENOMEM;
  }
  impl->slot_capacity = config->request_capacity;
  if (config->request_capacity >
      (SIZE_MAX - config->command_capacity) / 3u) {
    free(impl->retiring_sockets);
    free(impl->slots);
    free(impl);
    return TURBO_EINVAL;
  }
  impl->drive_batch_capacity = config->command_capacity +
                               config->request_capacity * 3u;
  turbo_mutex_init(&impl->gate);
  turbo_cond_init(&impl->changed);
  for (index = 0u; index < impl->slot_capacity; ++index) {
    impl->slots[index].owner = impl;
    impl->slots[index].wait_tokens[0].runtime = runtime;
    impl->slots[index].wait_tokens[1].runtime = runtime;
    impl->slots[index].wait_tokens[0].slot = &impl->slots[index];
    impl->slots[index].wait_tokens[1].slot = &impl->slots[index];
  }

  backend_config = (cflow_io_native_backend_config){
      config->backend_kind, config->request_capacity,
      config->completion_batch_capacity};
  status = cflow_io_native_backend_init(&impl->backend, &backend_config);
  if (status != TURBO_OK) goto failed;
  backend_initialized = true;
  if (!cflow_executor_manual_init_with_capacity(&impl->executor,
                                                 config->request_capacity)) {
    status = TURBO_ENOMEM;
    goto failed;
  }
  executor_initialized = true;
  memset(&actor_config, 0, sizeof(actor_config));
  actor_config.request_capacity = config->request_capacity;
  actor_config.command_capacity = config->command_capacity;
  actor_config.executor = &impl->executor;
  actor_config.backend = cflow_io_native_backend_actor_ops();
  actor_config.backend_user = &impl->backend;
  actor_config.completion = redis_io_actor_completion;
  actor_config.completion_user = impl;
  actor_config.wake = redis_io_actor_wake;
  actor_config.wake_user = runtime;
  status = cflow_io_actor_init(&impl->actor, &actor_config);
  if (status != TURBO_OK) goto failed;
  actor_initialized = true;
  runtime->impl = impl;
  return TURBO_OK;

failed:
  redis_io_cleanup_init(runtime, impl, backend_initialized,
                        executor_initialized, actor_initialized);
  return status;
}

int redis_io_runtime_valid(const redis_io_runtime *runtime) {
  return redis_io_const_impl(runtime) != NULL;
}

redis_io_submit_status redis_io_runtime_try_submit(
    redis_io_runtime *runtime, cflow_io_lease_id lease_id,
    const cflow_io_native_operation *operation, redis_io_request *out_request) {
  redis_io_runtime_impl *impl = redis_io_impl(runtime);
  redis_io_slot *slot = NULL;
  cflow_io_operation actor_operation;
  cflow_io_submit_result submitted;
  uint64_t generation = 0u;
  size_t index;
  if (impl == NULL || operation == NULL || out_request == NULL ||
      out_request->runtime != NULL || lease_id == 0u)
    return REDIS_IO_SUBMIT_INVALID_ARGUMENT;
  turbo_mutex_lock(&impl->gate);
  if (impl->closing) {
    turbo_mutex_unlock(&impl->gate);
    return REDIS_IO_SUBMIT_CLOSED;
  }
  for (index = 0u; index < impl->slot_capacity; ++index) {
    if (impl->retiring_sockets[index].active &&
        impl->retiring_sockets[index].identity == operation->socket) {
      turbo_mutex_unlock(&impl->gate);
      return REDIS_IO_SUBMIT_LEASE_IN_USE;
    }
  }
  for (index = 0u; index < impl->slot_capacity; ++index) {
    if (impl->slots[index].in_use && impl->slots[index].release_pending &&
        impl->slots[index].wake_inflight &&
        redis_io_current_wake_slot == &impl->slots[index]) {
      uint64_t wake_generation = impl->slots[index].wake_generation;
      redis_io_slot_reset_locked(&impl->slots[index]);
      impl->slots[index].wake_inflight = true;
      impl->slots[index].wake_generation = wake_generation;
    }
    if (!impl->slots[index].in_use) {
      slot = &impl->slots[index];
      slot->operation = *operation;
      ++slot->generation;
      if (slot->generation == 0u) ++slot->generation;
      generation = slot->generation;
      slot->wait_tokens[generation & 1u].generation = generation;
      slot->request_id = 0u;
      slot->completion = (cflow_io_completion){0};
      slot->waker = (cflow_waker){0};
      slot->in_use = true;
      slot->delivered = false;
      slot->abandoned = false;
      slot->release_pending = false;
      break;
    }
  }
  turbo_mutex_unlock(&impl->gate);
  if (slot == NULL) return REDIS_IO_SUBMIT_FULL;
  actor_operation = (cflow_io_operation){slot, redis_io_slot_release};
  submitted = cflow_io_actor_try_submit(&impl->actor, lease_id,
                                         &actor_operation);
  if (submitted.status != CFLOW_IO_SUBMIT_ACCEPTED) {
    redis_io_slot_rollback_submit(slot, generation);
    return redis_io_map_submit(submitted.status);
  }
  turbo_mutex_lock(&impl->gate);
  slot->request_id = submitted.request_id;
  out_request->runtime = runtime;
  out_request->slot = (size_t)(slot - impl->slots);
  out_request->generation = slot->generation;
  turbo_mutex_unlock(&impl->gate);
  return REDIS_IO_SUBMIT_ACCEPTED;
}

int redis_io_request_valid(const redis_io_request *request) {
  redis_io_runtime_impl *impl;
  redis_io_slot *slot = redis_io_slot_lock(request, &impl);
  if (slot == NULL) return 0;
  turbo_mutex_unlock(&impl->gate);
  return 1;
}

static bool redis_io_waitable_arm(void *state, cflow_waker waker) {
  redis_io_wait_token *token = (redis_io_wait_token *)state;
  redis_io_slot *slot;
  redis_io_runtime_impl *impl;
  uint64_t expected_generation;
  bool ready;
  if (token == NULL || token->slot == NULL || waker.wake == NULL) return false;
  slot = token->slot;
  impl = slot->owner;
  if (impl == NULL) return false;
  turbo_mutex_lock(&impl->gate);
  expected_generation = token->generation;
  if (!slot->in_use || slot->generation != expected_generation) {
    turbo_mutex_unlock(&impl->gate);
    return false;
  }
  if (slot->waker.wake != NULL) {
    turbo_mutex_unlock(&impl->gate);
    return false;
  }
  ready = slot->delivered;
  slot->waker = waker;
  turbo_mutex_unlock(&impl->gate);
  if (ready) redis_io_actor_wake(token->runtime);
  return true;
}

static void redis_io_waitable_cancel(void *state) {
  redis_io_wait_token *token = (redis_io_wait_token *)state;
  redis_io_slot *slot;
  redis_io_runtime_impl *impl;
  uint64_t expected_generation;
  if (token == NULL || token->slot == NULL) return;
  slot = token->slot;
  impl = slot->owner;
  if (impl == NULL) return;
  turbo_mutex_lock(&impl->gate);
  expected_generation = token->generation;
  if (slot->in_use && slot->generation == expected_generation)
    slot->waker = (cflow_waker){0};
  while (slot->wake_inflight &&
         slot->wake_generation == expected_generation &&
         redis_io_current_wake_slot != slot)
    turbo_cond_wait(&impl->changed, &impl->gate);
  turbo_mutex_unlock(&impl->gate);
}

CMETA_IMPLEMENTS(cflow_waitable, redis_io_waitable, 0,
    .arm = redis_io_waitable_arm,
    .cancel = redis_io_waitable_cancel
);

cflow_waitable redis_io_request_waitable(redis_io_request *request) {
  redis_io_runtime_impl *impl;
  redis_io_slot *slot = redis_io_slot_lock(request, &impl);
  redis_io_wait_token *token;
  if (slot == NULL) return (cflow_waitable){0};
  token = &slot->wait_tokens[slot->generation & 1u];
  turbo_mutex_unlock(&impl->gate);
  return redis_io_waitable_as_cflow_waitable(token);
}

redis_io_request_poll_status redis_io_request_poll(
    const redis_io_request *request, cflow_io_completion *completion) {
  redis_io_runtime_impl *impl;
  redis_io_slot *slot;
  bool delivered;
  if (completion == NULL) return REDIS_IO_REQUEST_INVALID;
  slot = redis_io_slot_lock(request, &impl);
  if (slot == NULL) return REDIS_IO_REQUEST_INVALID;
  delivered = slot->delivered;
  if (delivered) *completion = slot->completion;
  turbo_mutex_unlock(&impl->gate);
  return delivered ? REDIS_IO_REQUEST_COMPLETED : REDIS_IO_REQUEST_PENDING;
}

cflow_io_cancel_status redis_io_request_cancel(redis_io_request *request) {
  const redis_io_request expected =
      request != NULL ? *request : (redis_io_request){0};
  redis_io_runtime_impl *impl;
  redis_io_slot *slot = redis_io_slot_lock(&expected, &impl);
  cflow_io_request_id request_id;
  if (slot == NULL) return CFLOW_IO_CANCEL_INVALID_ARGUMENT;
  slot->waker = (cflow_waker){0};
  while (slot->wake_inflight &&
         slot->wake_generation == expected.generation &&
         redis_io_current_wake_slot != slot)
    turbo_cond_wait(&impl->changed, &impl->gate);
  if (!redis_io_slot_matches_locked(slot, &expected)) {
    turbo_mutex_unlock(&impl->gate);
    return CFLOW_IO_CANCEL_INVALID_ARGUMENT;
  }
  request_id = slot->request_id;
  turbo_mutex_unlock(&impl->gate);
  return cflow_io_actor_try_cancel(&impl->actor, request_id);
}

void redis_io_request_abandon(redis_io_request *request) {
  redis_io_request expected;
  redis_io_runtime_impl *impl;
  redis_io_slot *slot;
  redis_io_runtime *runtime;
  cflow_io_request_id request_id;
  if (request == NULL) return;
  expected = *request;
  slot = redis_io_slot_lock(&expected, &impl);
  if (slot == NULL) return;
  runtime = request->runtime;
  slot->waker = (cflow_waker){0};
  while (slot->wake_inflight &&
         slot->wake_generation == expected.generation &&
         redis_io_current_wake_slot != slot)
    turbo_cond_wait(&impl->changed, &impl->gate);
  if (!redis_io_slot_matches_locked(slot, &expected)) {
    turbo_mutex_unlock(&impl->gate);
    return;
  }
  slot->abandoned = true;
  request_id = slot->request_id;
  turbo_mutex_unlock(&impl->gate);
  (void)cflow_io_actor_try_cancel(&impl->actor, request_id);
  *request = (redis_io_request){0};
  redis_io_actor_wake(runtime);
}

int redis_io_request_acknowledge(redis_io_request *request) {
  redis_io_runtime_impl *impl;
  redis_io_slot *slot = redis_io_slot_lock(request, &impl);
  cflow_io_request_id request_id;
  cflow_io_ack_status status;
  if (slot == NULL) return TURBO_EINVAL;
  request_id = slot->request_id;
  turbo_mutex_unlock(&impl->gate);
  status = cflow_io_actor_acknowledge(&impl->actor, request_id);
  if (status == CFLOW_IO_ACK_RELEASED) {
    *request = (redis_io_request){0};
    return TURBO_OK;
  }
  return status == CFLOW_IO_ACK_BUSY ? TURBO_EBUSY : TURBO_EINVAL;
}

int redis_io_runtime_wait_idle(redis_io_runtime *runtime,
                               uint64_t timeout_ns) {
  redis_io_runtime_impl *impl = redis_io_impl(runtime);
  uint64_t started;
  if (impl == NULL || timeout_ns == 0u) return TURBO_EINVAL;
  if (redis_io_driver_depth != 0u) return TURBO_EBUSY;
  started = turbo_hrtime();
  for (;;) {
    bool pending;
    size_t index;
    uint64_t now;
    uint64_t remaining;
    size_t progressed = 0u;
    (void)redis_io_runtime_run_ready(runtime, impl->drive_batch_capacity,
                                     &progressed);
    turbo_mutex_lock(&impl->gate);
    pending = impl->driver_active || impl->drive_pending;
    for (index = 0u; index < impl->slot_capacity; ++index) {
      if (impl->slots[index].in_use &&
          (!impl->slots[index].delivered ||
           impl->slots[index].waker.wake != NULL ||
           impl->slots[index].wake_inflight)) {
        pending = true;
        break;
      }
    }
    if (!pending) {
      turbo_mutex_unlock(&impl->gate);
      return TURBO_OK;
    }
    now = turbo_hrtime();
    if (now - started >= timeout_ns) {
      turbo_mutex_unlock(&impl->gate);
      return TURBO_ETIMEDOUT;
    }
    remaining = timeout_ns - (now - started);
    (void)turbo_cond_timedwait(&impl->changed, &impl->gate, remaining);
    turbo_mutex_unlock(&impl->gate);
  }
}

int redis_io_runtime_forget_socket(redis_io_runtime *runtime,
                                   uintptr_t closed_socket) {
  redis_io_runtime_impl *impl = redis_io_impl(runtime);
  int status;
  size_t index;
  if (impl == NULL) return TURBO_EINVAL;
  status = cflow_io_native_backend_forget_socket(&impl->backend,
                                                  closed_socket);
  if (status != TURBO_OK && status != TURBO_ENOENT) return status;
  turbo_mutex_lock(&impl->gate);
  for (index = 0u; index < impl->slot_capacity; ++index) {
    if (impl->retiring_sockets[index].active &&
        impl->retiring_sockets[index].identity == closed_socket) {
      impl->retiring_sockets[index].active = false;
      break;
    }
  }
  turbo_mutex_unlock(&impl->gate);
  return TURBO_OK;
}

int redis_io_runtime_retire_socket(redis_io_runtime *runtime,
                                   uintptr_t socket_identity) {
  redis_io_runtime_impl *impl = redis_io_impl(runtime);
  size_t available = SIZE_MAX;
  size_t index;
  if (impl == NULL) return TURBO_EINVAL;
  turbo_mutex_lock(&impl->gate);
  for (index = 0u; index < impl->slot_capacity; ++index) {
    if (impl->retiring_sockets[index].active) {
      if (impl->retiring_sockets[index].identity == socket_identity) {
        turbo_mutex_unlock(&impl->gate);
        return TURBO_EBUSY;
      }
    } else if (available == SIZE_MAX) {
      available = index;
    }
    if (impl->slots[index].in_use &&
        impl->slots[index].operation.socket == socket_identity) {
      turbo_mutex_unlock(&impl->gate);
      return TURBO_EBUSY;
    }
  }
  if (available == SIZE_MAX) {
    turbo_mutex_unlock(&impl->gate);
    return TURBO_ENOBUFS;
  }
  impl->retiring_sockets[available].identity = socket_identity;
  impl->retiring_sockets[available].active = true;
  turbo_mutex_unlock(&impl->gate);
  return TURBO_OK;
}

int redis_io_runtime_forget_socket_wait(redis_io_runtime *runtime,
                                        uintptr_t closed_socket,
                                        uint64_t timeout_ns) {
  uint64_t started;
  if (redis_io_impl(runtime) == NULL || timeout_ns == 0u) return TURBO_EINVAL;
  started = turbo_hrtime();
  for (;;) {
    int status = redis_io_runtime_forget_socket(runtime, closed_socket);
    if (status != TURBO_EBUSY) return status;
    if (redis_io_driver_depth != 0u) return TURBO_EBUSY;
    if (turbo_hrtime() - started >= timeout_ns) return TURBO_ETIMEDOUT;
    turbo_thread_yield();
  }
}

int redis_io_runtime_close(redis_io_runtime *runtime) {
  redis_io_runtime_impl *impl = redis_io_impl(runtime);
  size_t index;
  int status;
  if (impl == NULL) return TURBO_EINVAL;
  turbo_mutex_lock(&impl->gate);
  impl->closing = true;
  for (index = 0u; index < impl->slot_capacity; ++index) {
    impl->slots[index].abandoned = impl->slots[index].in_use;
    impl->slots[index].waker = (cflow_waker){0};
    while (impl->slots[index].wake_inflight &&
           redis_io_current_wake_slot != &impl->slots[index])
      turbo_cond_wait(&impl->changed, &impl->gate);
  }
  turbo_mutex_unlock(&impl->gate);
  status = cflow_io_actor_close(&impl->actor);
  redis_io_actor_wake(runtime);
  return status == TURBO_EALREADY ? TURBO_OK : status;
}

int redis_io_runtime_destroy(redis_io_runtime *runtime) {
  redis_io_runtime_impl *impl = redis_io_impl(runtime);
  size_t index;
  bool busy;
  int status;
  if (impl == NULL) return TURBO_EINVAL;
  if (!impl->closing || !cflow_io_actor_is_quiescent(&impl->actor))
    return TURBO_EBUSY;
  turbo_mutex_lock(&impl->gate);
  busy = impl->driver_active;
  for (index = 0u; !busy && index < impl->slot_capacity; ++index)
    busy = impl->slots[index].wake_inflight;
  if (busy || impl->destroying) {
    turbo_mutex_unlock(&impl->gate);
    return TURBO_EBUSY;
  }
  impl->destroying = true;
  turbo_mutex_unlock(&impl->gate);
  status = cflow_io_native_backend_shutdown(&impl->backend);
  if (status != TURBO_OK && status != TURBO_EALREADY) goto fail;
  status = cflow_io_actor_destroy(&impl->actor);
  if (status != TURBO_OK) goto fail;
  status = cflow_io_native_backend_destroy(&impl->backend);
  if (status != TURBO_OK) return status;
  if (!cflow_executor_shutdown(&impl->executor)) return TURBO_EBUSY;
  cflow_executor_destroy(&impl->executor);
  turbo_cond_destroy(&impl->changed);
  turbo_mutex_destroy(&impl->gate);
  free(impl->retiring_sockets);
  free(impl->slots);
  free(impl);
  runtime->impl = NULL;
  return TURBO_OK;

fail:
  turbo_mutex_lock(&impl->gate);
  impl->destroying = false;
  turbo_mutex_unlock(&impl->gate);
  return status;
}

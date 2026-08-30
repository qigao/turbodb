#include "redis_io.h"

#include "redis_io_internal.h"
#include "turbo_error.h"
#include "turbo_thread.h"

#include <cflow/executor.h>
#include <turbo/clock.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

typedef struct redis_io_runtime_impl redis_io_runtime_impl;
typedef struct redis_io_publisher_slot redis_io_publisher_slot;

typedef struct redis_io_bridge_operation {
  cflow_io_native_operation native;
  redis_io_publisher_slot *publisher;
} redis_io_bridge_operation;

_Static_assert(offsetof(redis_io_bridge_operation, native) == 0u,
               "the native backend consumes bridge operation_user as a native operation");

typedef struct redis_io_retiring_socket {
  uintptr_t identity;
  bool active;
} redis_io_retiring_socket;

struct redis_io_publisher_slot {
  redis_io_runtime_impl *runtime;
  uint64_t generation;
  cflow_io_lease_id bridge_lease_id;
  redis_io_runtime_drive_fn drive;
  void *drive_user;
  redis_io_bridge_operation operation;
  cflow_io_actor *target_actor;
  cflow_io_request_id target_request_id;
  cflow_io_request_id bridge_request_id;
  cflow_io_completion target_completion;
  int schedule_error;
  bool attached;
  bool scheduled;
  bool running;
  bool operation_owned;
  bool acknowledge_pending;
};

struct redis_io_runtime_impl {
  cflow_io_native_backend backend;
  cflow_executor driver;
  cflow_io_actor bridge;
  redis_io_publisher_slot *publishers;
  redis_io_retiring_socket *retiring_sockets;
  size_t publisher_capacity;
  size_t attached_publishers;
  size_t active_publisher_operations;
  int bridge_error;
  turbo_mutex_t gate;
  turbo_cond_t changed;
  bool bridge_live;
  bool backend_live;
  bool driver_live;
  bool bridge_scheduled;
  bool bridge_running;
  bool closing;
  bool destroying;
};

enum {
  REDIS_IO_BRIDGE_STEPS = 64u,
  REDIS_IO_DRIVER_CAPACITY_FACTOR = 3u,
  REDIS_IO_DRIVER_CAPACITY_RESERVE = 4u
};

static TURBO_THREAD_LOCAL unsigned redis_io_callback_depth;

static void redis_io_publisher_driver_task(void *user);
static void redis_io_publisher_driver_cancel(void *user);
static void redis_io_bridge_driver_task(void *user);
static void redis_io_bridge_driver_cancel(void *user);

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

static const redis_io_runtime_impl *redis_io_const_impl(const redis_io_runtime *runtime) {
  return runtime != NULL ? (const redis_io_runtime_impl *)runtime->impl : NULL;
}

static bool redis_io_publisher_matches(const redis_io_runtime_publisher *publisher,
                                    const redis_io_publisher_slot *slot) {
  return publisher != NULL && slot != NULL && publisher->slot == slot && publisher->generation != 0u &&
         publisher->generation == slot->generation && slot->attached;
}

static bool redis_io_runtime_idle_locked(const redis_io_runtime_impl *impl) {
  size_t index;
  if (impl->active_publisher_operations != 0u || impl->bridge_scheduled || impl->bridge_running)
    return false;
  for (index = 0u; index < impl->publisher_capacity; ++index) {
    const redis_io_publisher_slot *slot = &impl->publishers[index];
    if (slot->scheduled || slot->running || slot->operation_owned || slot->acknowledge_pending)
      return false;
  }
  return true;
}

static int redis_io_admission_error(cflow_admission_status status) {
  switch (status) {
  case CFLOW_ADMISSION_FULL:
    return TURBO_ENOBUFS;
  case CFLOW_ADMISSION_CLOSED:
    return TURBO_ECANCELED;
  case CFLOW_ADMISSION_ALLOCATION_FAILED:
    return TURBO_ENOMEM;
  case CFLOW_ADMISSION_INVALID_ARGUMENT:
  default:
    return TURBO_EINVAL;
  }
}

static int redis_io_post_publisher(redis_io_publisher_slot *slot) {
  const cflow_executor_task task = {redis_io_publisher_driver_task, redis_io_publisher_driver_cancel,
                                    NULL, slot};
  cflow_admission_status admission;
  if (slot == NULL || slot->runtime == NULL) return TURBO_EINVAL;
  admission = cflow_executor_try_post_task(&slot->runtime->driver, &task);
  return admission == CFLOW_ADMISSION_ACCEPTED ? TURBO_OK : redis_io_admission_error(admission);
}

static int redis_io_post_bridge(redis_io_runtime_impl *impl) {
  const cflow_executor_task task = {redis_io_bridge_driver_task, redis_io_bridge_driver_cancel,
                                    NULL, impl};
  cflow_admission_status admission;
  if (impl == NULL) return TURBO_EINVAL;
  admission = cflow_executor_try_post_task(&impl->driver, &task);
  return admission == CFLOW_ADMISSION_ACCEPTED ? TURBO_OK : redis_io_admission_error(admission);
}

static int redis_io_schedule_bridge(redis_io_runtime_impl *impl) {
  bool post = false;
  int status;
  if (impl == NULL) return TURBO_EINVAL;
  turbo_mutex_lock(&impl->gate);
  if (!impl->driver_live || !impl->bridge_live) {
    turbo_mutex_unlock(&impl->gate);
    return TURBO_ECANCELED;
  }
  if (!impl->bridge_scheduled) {
    impl->bridge_scheduled = true;
    post = !impl->bridge_running;
  }
  turbo_mutex_unlock(&impl->gate);
  if (!post) return TURBO_OK;
  status = redis_io_post_bridge(impl);
  if (status == TURBO_OK) return TURBO_OK;
  turbo_mutex_lock(&impl->gate);
  impl->bridge_scheduled = false;
  impl->bridge_error = status;
  turbo_cond_broadcast(&impl->changed);
  turbo_mutex_unlock(&impl->gate);
  return impl->bridge_error;
}

static void redis_io_bridge_wake(void *user) {
  redis_io_runtime_impl *impl = (redis_io_runtime_impl *)user;
  (void)redis_io_schedule_bridge(impl);
}

static void redis_io_bridge_operation_release(void *user) {
  redis_io_bridge_operation *operation = (redis_io_bridge_operation *)user;
  redis_io_publisher_slot *slot = operation != NULL ? operation->publisher : NULL;
  redis_io_runtime_impl *impl = slot != NULL ? slot->runtime : NULL;
  if (impl == NULL) return;
  turbo_mutex_lock(&impl->gate);
  slot->operation_owned = false;
  slot->acknowledge_pending = false;
  slot->target_actor = NULL;
  slot->target_request_id = 0u;
  slot->bridge_request_id = 0u;
  turbo_cond_broadcast(&impl->changed);
  turbo_mutex_unlock(&impl->gate);
}

static void redis_io_bridge_completion(void *user, cflow_io_request_id request_id,
                                       cflow_io_lease_id lease_id, void *operation_user,
                                       const cflow_io_completion *completion) {
  redis_io_runtime_impl *impl = (redis_io_runtime_impl *)user;
  redis_io_bridge_operation *operation = (redis_io_bridge_operation *)operation_user;
  redis_io_publisher_slot *slot = operation != NULL ? operation->publisher : NULL;
  (void)lease_id;
  if (impl == NULL || slot == NULL || completion == NULL) return;
  turbo_mutex_lock(&impl->gate);
  if (slot->runtime == impl && slot->operation_owned) {
    slot->bridge_request_id = request_id;
    slot->target_completion = *completion;
    slot->acknowledge_pending = true;
  }
  turbo_mutex_unlock(&impl->gate);
  (void)redis_io_schedule_bridge(impl);
}

static int redis_io_bridge_submit(void *backend_user, cflow_io_actor *actor,
                                  cflow_io_request_id request_id, cflow_io_lease_id lease_id,
                                  void *operation_user) {
  redis_io_publisher_slot *slot = (redis_io_publisher_slot *)backend_user;
  redis_io_runtime_impl *impl = slot != NULL ? slot->runtime : NULL;
  cflow_io_native_operation *native = (cflow_io_native_operation *)operation_user;
  cflow_io_operation operation;
  cflow_io_submit_result submitted;
  size_t index;
  (void)lease_id;
  if (impl == NULL || actor == NULL || request_id == 0u || native == NULL) return TURBO_EINVAL;
  turbo_mutex_lock(&impl->gate);
  if (!slot->attached || impl->closing || impl->destroying || slot->operation_owned) {
    int status = slot->operation_owned ? TURBO_EBUSY : TURBO_ECANCELED;
    turbo_mutex_unlock(&impl->gate);
    return status;
  }
  for (index = 0u; index < impl->publisher_capacity; ++index) {
    if (impl->retiring_sockets[index].active &&
        impl->retiring_sockets[index].identity == native->socket) {
      turbo_mutex_unlock(&impl->gate);
      return TURBO_EBUSY;
    }
  }
  slot->operation.native = *native;
  slot->operation.publisher = slot;
  slot->target_actor = actor;
  slot->target_request_id = request_id;
  slot->bridge_request_id = 0u;
  slot->operation_owned = true;
  turbo_mutex_unlock(&impl->gate);
  operation = (cflow_io_operation){&slot->operation, redis_io_bridge_operation_release};
  submitted = cflow_io_actor_try_submit(&impl->bridge, slot->bridge_lease_id, &operation);
  if (submitted.status != CFLOW_IO_SUBMIT_ACCEPTED) {
    turbo_mutex_lock(&impl->gate);
    slot->operation_owned = false;
    slot->target_actor = NULL;
    slot->target_request_id = 0u;
    turbo_cond_broadcast(&impl->changed);
    turbo_mutex_unlock(&impl->gate);
    switch (submitted.status) {
    case CFLOW_IO_SUBMIT_FULL:
    case CFLOW_IO_SUBMIT_LEASE_IN_USE:
      return TURBO_ENOBUFS;
    case CFLOW_IO_SUBMIT_CLOSED:
      return TURBO_ECANCELED;
    default:
      return TURBO_EINVAL;
    }
  }
  turbo_mutex_lock(&impl->gate);
  slot->bridge_request_id = submitted.request_id;
  turbo_mutex_unlock(&impl->gate);
  return TURBO_OK;
}

static int redis_io_bridge_cancel(void *backend_user, cflow_io_request_id request_id) {
  redis_io_publisher_slot *slot = (redis_io_publisher_slot *)backend_user;
  redis_io_runtime_impl *impl = slot != NULL ? slot->runtime : NULL;
  cflow_io_request_id bridge_request_id;
  cflow_io_cancel_status cancelled;
  if (impl == NULL || request_id == 0u) return TURBO_EINVAL;
  turbo_mutex_lock(&impl->gate);
  if (!slot->attached || !slot->operation_owned || slot->target_request_id != request_id) {
    turbo_mutex_unlock(&impl->gate);
    return TURBO_ENOENT;
  }
  bridge_request_id = slot->bridge_request_id;
  turbo_mutex_unlock(&impl->gate);
  if (bridge_request_id == 0u) return TURBO_EBUSY;
  cancelled = cflow_io_actor_try_cancel(&impl->bridge, bridge_request_id);
  switch (cancelled) {
  case CFLOW_IO_CANCEL_ACCEPTED:
  case CFLOW_IO_CANCEL_NOT_FOUND:
    return TURBO_OK;
  case CFLOW_IO_CANCEL_FULL:
    return TURBO_ENOBUFS;
  case CFLOW_IO_CANCEL_CLOSED:
    return TURBO_ECANCELED;
  default:
    return TURBO_EINVAL;
  }
}

static void redis_io_bridge_driver_task(void *user) {
  redis_io_runtime_impl *impl = (redis_io_runtime_impl *)user;
  cflow_io_run_result ran = {CFLOW_IO_RUN_INVALID_ARGUMENT, 0u};
  bool repost = false;
  size_t index;
  if (impl == NULL) return;
  turbo_mutex_lock(&impl->gate);
  impl->bridge_scheduled = false;
  impl->bridge_running = true;
  turbo_mutex_unlock(&impl->gate);
  redis_io_runtime_enter_callback();
  for (index = 0u; index < impl->publisher_capacity; ++index) {
    cflow_io_request_id request_id = 0u;
    cflow_io_request_id target_request_id = 0u;
    cflow_io_actor *target_actor = NULL;
    cflow_io_completion target_completion = {0};
    turbo_mutex_lock(&impl->gate);
    if (impl->publishers[index].acknowledge_pending) {
      request_id = impl->publishers[index].bridge_request_id;
      target_request_id = impl->publishers[index].target_request_id;
      target_actor = impl->publishers[index].target_actor;
      target_completion = impl->publishers[index].target_completion;
      impl->publishers[index].acknowledge_pending = false;
    }
    turbo_mutex_unlock(&impl->gate);
    if (request_id != 0u) {
      cflow_io_ack_status acknowledged = cflow_io_actor_acknowledge(&impl->bridge, request_id);
      if (acknowledged == CFLOW_IO_ACK_BUSY) {
        turbo_mutex_lock(&impl->gate);
        impl->publishers[index].acknowledge_pending = true;
        impl->bridge_scheduled = true;
        turbo_mutex_unlock(&impl->gate);
      } else if (acknowledged == CFLOW_IO_ACK_RELEASED) {
        cflow_io_complete_status completed =
            cflow_io_actor_complete(target_actor, target_request_id, &target_completion);
        if (completed != CFLOW_IO_COMPLETE_ACCEPTED) {
          turbo_mutex_lock(&impl->gate);
          if (impl->bridge_error == TURBO_OK) impl->bridge_error = TURBO_EPROTO;
          turbo_mutex_unlock(&impl->gate);
        }
      } else {
        turbo_mutex_lock(&impl->gate);
        if (impl->bridge_error == TURBO_OK) impl->bridge_error = TURBO_EPROTO;
        turbo_mutex_unlock(&impl->gate);
      }
    }
  }
  ran = cflow_io_actor_run_ready(&impl->bridge, REDIS_IO_BRIDGE_STEPS);
  redis_io_runtime_leave_callback();
  turbo_mutex_lock(&impl->gate);
  if (ran.status == CFLOW_IO_RUN_INVALID_ARGUMENT) impl->bridge_error = TURBO_EINVAL;
  else if (ran.status == CFLOW_IO_RUN_BUSY) impl->bridge_scheduled = true;
  if (ran.progressed == REDIS_IO_BRIDGE_STEPS) impl->bridge_scheduled = true;
  impl->bridge_running = false;
  repost = impl->bridge_scheduled && impl->bridge_live;
  turbo_cond_broadcast(&impl->changed);
  turbo_mutex_unlock(&impl->gate);
  if (repost) {
    int status = redis_io_post_bridge(impl);
    if (status != TURBO_OK) {
      turbo_mutex_lock(&impl->gate);
      impl->bridge_scheduled = false;
      impl->bridge_error = status;
      turbo_cond_broadcast(&impl->changed);
      turbo_mutex_unlock(&impl->gate);
    }
  }
}

static void redis_io_bridge_driver_cancel(void *user) {
  redis_io_runtime_impl *impl = (redis_io_runtime_impl *)user;
  if (impl == NULL) return;
  turbo_mutex_lock(&impl->gate);
  impl->bridge_scheduled = false;
  impl->bridge_running = false;
  impl->bridge_error = TURBO_ECANCELED;
  turbo_cond_broadcast(&impl->changed);
  turbo_mutex_unlock(&impl->gate);
}

static void redis_io_publisher_driver_task(void *user) {
  redis_io_publisher_slot *slot = (redis_io_publisher_slot *)user;
  redis_io_runtime_impl *impl = slot != NULL ? slot->runtime : NULL;
  redis_io_runtime_drive_fn drive = NULL;
  void *drive_user = NULL;
  bool repost = false;
  if (impl == NULL) return;
  turbo_mutex_lock(&impl->gate);
  if (slot->attached) {
    slot->scheduled = false;
    slot->running = true;
    drive = slot->drive;
    drive_user = slot->drive_user;
  }
  turbo_mutex_unlock(&impl->gate);
  if (drive != NULL) {
    redis_io_runtime_enter_callback();
    drive(drive_user);
    redis_io_runtime_leave_callback();
  }
  turbo_mutex_lock(&impl->gate);
  slot->running = false;
  repost = slot->attached && slot->scheduled && !impl->destroying;
  turbo_cond_broadcast(&impl->changed);
  turbo_mutex_unlock(&impl->gate);
  if (repost) {
    int status = redis_io_post_publisher(slot);
    if (status != TURBO_OK) {
      turbo_mutex_lock(&impl->gate);
      slot->scheduled = false;
      slot->schedule_error = status;
      turbo_cond_broadcast(&impl->changed);
      turbo_mutex_unlock(&impl->gate);
    }
  }
}

static void redis_io_publisher_driver_cancel(void *user) {
  redis_io_publisher_slot *slot = (redis_io_publisher_slot *)user;
  redis_io_runtime_impl *impl = slot != NULL ? slot->runtime : NULL;
  if (impl == NULL) return;
  turbo_mutex_lock(&impl->gate);
  slot->scheduled = false;
  slot->running = false;
  slot->schedule_error = TURBO_ECANCELED;
  turbo_cond_broadcast(&impl->changed);
  turbo_mutex_unlock(&impl->gate);
}

int redis_io_runtime_init(redis_io_runtime *runtime, const redis_io_runtime_config *config) {
  redis_io_runtime_impl *impl;
  cflow_io_native_backend_config backend_config;
  cflow_io_actor_config bridge_config = {0};
  size_t driver_capacity;
  int status;
  if (runtime == NULL || runtime->impl != NULL || config == NULL || config->publisher_capacity == 0u ||
      config->completion_batch_capacity == 0u ||
      config->publisher_capacity > SIZE_MAX / sizeof(redis_io_publisher_slot) ||
      config->publisher_capacity > SIZE_MAX / sizeof(redis_io_retiring_socket) ||
      config->publisher_capacity >
          (SIZE_MAX - REDIS_IO_DRIVER_CAPACITY_RESERVE) / REDIS_IO_DRIVER_CAPACITY_FACTOR)
    return TURBO_EINVAL;
  driver_capacity =
      config->publisher_capacity * REDIS_IO_DRIVER_CAPACITY_FACTOR + REDIS_IO_DRIVER_CAPACITY_RESERVE;
  impl = (redis_io_runtime_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return TURBO_ENOMEM;
  impl->publishers = (redis_io_publisher_slot *)calloc(config->publisher_capacity, sizeof(*impl->publishers));
  impl->retiring_sockets =
      (redis_io_retiring_socket *)calloc(config->publisher_capacity, sizeof(*impl->retiring_sockets));
  if (impl->publishers == NULL || impl->retiring_sockets == NULL) {
    free(impl->retiring_sockets);
    free(impl->publishers);
    free(impl);
    return TURBO_ENOMEM;
  }
  impl->publisher_capacity = config->publisher_capacity;
  turbo_mutex_init(&impl->gate);
  turbo_cond_init(&impl->changed);
  if (!cflow_executor_serial_init_with_capacity(&impl->driver, driver_capacity)) {
    status = TURBO_ENOMEM;
    goto failed;
  }
  impl->driver_live = true;
  backend_config = (cflow_io_native_backend_config){config->backend_kind, config->publisher_capacity,
                                                    config->completion_batch_capacity};
  status = cflow_io_native_backend_init(&impl->backend, &backend_config);
  if (status != TURBO_OK) goto failed;
  impl->backend_live = true;
  bridge_config.request_capacity = config->publisher_capacity;
  bridge_config.command_capacity = config->publisher_capacity;
  bridge_config.executor = &impl->driver;
  bridge_config.backend = cflow_io_native_backend_actor_ops();
  bridge_config.backend_user = &impl->backend;
  bridge_config.completion = redis_io_bridge_completion;
  bridge_config.completion_user = impl;
  bridge_config.wake = redis_io_bridge_wake;
  bridge_config.wake_user = impl;
  status = cflow_io_actor_init(&impl->bridge, &bridge_config);
  if (status != TURBO_OK) goto failed;
  impl->bridge_live = true;
  runtime->impl = impl;
  return TURBO_OK;
failed:
  if (impl->backend_live) {
    (void)cflow_io_native_backend_shutdown(&impl->backend);
    (void)cflow_io_native_backend_destroy(&impl->backend);
  }
  if (impl->driver_live) {
    (void)cflow_executor_shutdown(&impl->driver);
    cflow_executor_destroy(&impl->driver);
  }
  turbo_cond_destroy(&impl->changed);
  turbo_mutex_destroy(&impl->gate);
  free(impl->retiring_sockets);
  free(impl->publishers);
  free(impl);
  return status;
}

int redis_io_runtime_valid(const redis_io_runtime *runtime) {
  return redis_io_const_impl(runtime) != NULL;
}

int redis_io_runtime_attach_publisher(redis_io_runtime *runtime, redis_io_runtime_publisher *publisher,
                                   redis_io_runtime_drive_fn drive, void *drive_user,
                                   cflow_io_backend_ops *backend, void **backend_user) {
  redis_io_runtime_impl *impl = redis_io_impl(runtime);
  size_t index;
  if (impl == NULL || publisher == NULL || publisher->slot != NULL || publisher->generation != 0u ||
      drive == NULL || backend == NULL || backend_user == NULL)
    return TURBO_EINVAL;
  turbo_mutex_lock(&impl->gate);
  if (impl->closing || impl->destroying) {
    turbo_mutex_unlock(&impl->gate);
    return TURBO_ECANCELED;
  }
  for (index = 0u; index < impl->publisher_capacity; ++index)
    if (!impl->publishers[index].attached) break;
  if (index == impl->publisher_capacity) {
    turbo_mutex_unlock(&impl->gate);
    return TURBO_ENOBUFS;
  }
  {
    redis_io_publisher_slot *slot = &impl->publishers[index];
    if (slot->generation == UINT64_MAX) slot->generation = 1u;
    else ++slot->generation;
    if (slot->generation == 0u) slot->generation = 1u;
    slot->runtime = impl;
    slot->bridge_lease_id = (cflow_io_lease_id)index + 1u;
    slot->drive = drive;
    slot->drive_user = drive_user;
    slot->schedule_error = TURBO_OK;
    slot->attached = true;
    ++impl->attached_publishers;
    publisher->slot = slot;
    publisher->generation = slot->generation;
    *backend = (cflow_io_backend_ops){redis_io_bridge_submit, redis_io_bridge_cancel};
    *backend_user = slot;
  }
  turbo_mutex_unlock(&impl->gate);
  return TURBO_OK;
}

int redis_io_runtime_schedule_publisher(redis_io_runtime_publisher *publisher) {
  redis_io_publisher_slot *slot = publisher != NULL ? (redis_io_publisher_slot *)publisher->slot : NULL;
  redis_io_runtime_impl *impl = slot != NULL ? slot->runtime : NULL;
  bool post = false;
  int status;
  if (impl == NULL) return TURBO_EINVAL;
  turbo_mutex_lock(&impl->gate);
  if (!redis_io_publisher_matches(publisher, slot) || impl->closing || impl->destroying) {
    turbo_mutex_unlock(&impl->gate);
    return TURBO_ECANCELED;
  }
  if (!slot->scheduled) {
    slot->scheduled = true;
    post = !slot->running;
  }
  turbo_mutex_unlock(&impl->gate);
  if (!post) return TURBO_OK;
  status = redis_io_post_publisher(slot);
  if (status == TURBO_OK) return TURBO_OK;
  turbo_mutex_lock(&impl->gate);
  slot->scheduled = false;
  slot->schedule_error = status;
  turbo_cond_broadcast(&impl->changed);
  turbo_mutex_unlock(&impl->gate);
  return status;
}

int redis_io_runtime_wait_publisher_idle(redis_io_runtime_publisher *publisher, uint64_t timeout_ns) {
  redis_io_publisher_slot *slot = publisher != NULL ? (redis_io_publisher_slot *)publisher->slot : NULL;
  redis_io_runtime_impl *impl = slot != NULL ? slot->runtime : NULL;
  uint64_t started;
  if (impl == NULL || timeout_ns == 0u) return TURBO_EINVAL;
  if (redis_io_runtime_in_callback()) return TURBO_EBUSY;
  started = turbo_hrtime();
  turbo_mutex_lock(&impl->gate);
  while (redis_io_publisher_matches(publisher, slot) && (slot->scheduled || slot->running)) {
    uint64_t now = turbo_hrtime();
    if (now - started >= timeout_ns) {
      turbo_mutex_unlock(&impl->gate);
      return TURBO_ETIMEDOUT;
    }
    (void)turbo_cond_timedwait(&impl->changed, &impl->gate, timeout_ns - (now - started));
  }
  if (!redis_io_publisher_matches(publisher, slot)) {
    turbo_mutex_unlock(&impl->gate);
    return TURBO_EINVAL;
  }
  {
    int status = slot->schedule_error;
    turbo_mutex_unlock(&impl->gate);
    return status;
  }
}

int redis_io_runtime_detach_publisher(redis_io_runtime_publisher *publisher) {
  redis_io_publisher_slot *slot = publisher != NULL ? (redis_io_publisher_slot *)publisher->slot : NULL;
  redis_io_runtime_impl *impl = slot != NULL ? slot->runtime : NULL;
  if (impl == NULL) return TURBO_EINVAL;
  turbo_mutex_lock(&impl->gate);
  if (!redis_io_publisher_matches(publisher, slot)) {
    turbo_mutex_unlock(&impl->gate);
    return TURBO_EINVAL;
  }
  if (slot->scheduled || slot->running || slot->operation_owned) {
    turbo_mutex_unlock(&impl->gate);
    return TURBO_EBUSY;
  }
  slot->attached = false;
  slot->drive = NULL;
  slot->drive_user = NULL;
  slot->schedule_error = TURBO_OK;
  if (impl->attached_publishers != 0u) --impl->attached_publishers;
  publisher->slot = NULL;
  publisher->generation = 0u;
  turbo_cond_broadcast(&impl->changed);
  turbo_mutex_unlock(&impl->gate);
  return TURBO_OK;
}

int redis_io_runtime_publisher_started(redis_io_runtime *runtime) {
  redis_io_runtime_impl *impl = redis_io_impl(runtime);
  int status;
  if (impl == NULL) return TURBO_EINVAL;
  turbo_mutex_lock(&impl->gate);
  if (impl->closing || impl->active_publisher_operations == impl->publisher_capacity) {
    status =
        impl->active_publisher_operations == impl->publisher_capacity ? TURBO_ENOBUFS : TURBO_ECANCELED;
    turbo_mutex_unlock(&impl->gate);
    return status;
  }
  ++impl->active_publisher_operations;
  turbo_mutex_unlock(&impl->gate);
  return TURBO_OK;
}

void redis_io_runtime_publisher_finished(redis_io_runtime *runtime) {
  redis_io_runtime_impl *impl = redis_io_impl(runtime);
  if (impl == NULL) return;
  turbo_mutex_lock(&impl->gate);
  if (impl->active_publisher_operations != 0u) --impl->active_publisher_operations;
  turbo_cond_broadcast(&impl->changed);
  turbo_mutex_unlock(&impl->gate);
}

void redis_io_runtime_enter_callback(void) { ++redis_io_callback_depth; }

void redis_io_runtime_leave_callback(void) {
  if (redis_io_callback_depth != 0u) --redis_io_callback_depth;
}

int redis_io_runtime_in_callback(void) { return redis_io_callback_depth != 0u; }

int redis_io_runtime_wait_idle(redis_io_runtime *runtime, uint64_t timeout_ns) {
  redis_io_runtime_impl *impl = redis_io_impl(runtime);
  uint64_t started;
  if (impl == NULL || timeout_ns == 0u) return TURBO_EINVAL;
  if (redis_io_runtime_in_callback()) return TURBO_EBUSY;
  started = turbo_hrtime();
  for (;;) {
    uint64_t now = turbo_hrtime();
    int status;
    if (now - started >= timeout_ns) return TURBO_ETIMEDOUT;
    if (!cflow_executor_wait_idle(&impl->driver)) return TURBO_EIO;
    turbo_mutex_lock(&impl->gate);
    if (redis_io_runtime_idle_locked(impl)) {
      status = impl->bridge_error;
      turbo_mutex_unlock(&impl->gate);
      return status;
    }
    now = turbo_hrtime();
    if (now - started >= timeout_ns) {
      turbo_mutex_unlock(&impl->gate);
      return TURBO_ETIMEDOUT;
    }
    (void)turbo_cond_timedwait(&impl->changed, &impl->gate, timeout_ns - (now - started));
    turbo_mutex_unlock(&impl->gate);
  }
}

int redis_io_runtime_forget_socket(redis_io_runtime *runtime, uintptr_t closed_socket) {
  redis_io_runtime_impl *impl = redis_io_impl(runtime);
  size_t index;
  int status;
  if (impl == NULL) return TURBO_EINVAL;
  status = cflow_io_native_backend_forget_socket(&impl->backend, closed_socket);
  if (status != TURBO_OK && status != TURBO_ENOENT) return status;
  turbo_mutex_lock(&impl->gate);
  for (index = 0u; index < impl->publisher_capacity; ++index) {
    if (impl->retiring_sockets[index].active &&
        impl->retiring_sockets[index].identity == closed_socket) {
      impl->retiring_sockets[index].active = false;
      break;
    }
  }
  turbo_mutex_unlock(&impl->gate);
  return TURBO_OK;
}

int redis_io_runtime_retire_socket(redis_io_runtime *runtime, uintptr_t socket_identity) {
  redis_io_runtime_impl *impl = redis_io_impl(runtime);
  size_t available = SIZE_MAX;
  size_t index;
  if (impl == NULL) return TURBO_EINVAL;
  turbo_mutex_lock(&impl->gate);
  for (index = 0u; index < impl->publisher_capacity; ++index) {
    if (impl->publishers[index].operation_owned &&
        impl->publishers[index].operation.native.socket == socket_identity) {
      turbo_mutex_unlock(&impl->gate);
      return TURBO_EBUSY;
    }
    if (impl->retiring_sockets[index].active) {
      if (impl->retiring_sockets[index].identity == socket_identity) {
        turbo_mutex_unlock(&impl->gate);
        return TURBO_EBUSY;
      }
    } else if (available == SIZE_MAX) {
      available = index;
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

int redis_io_runtime_forget_socket_wait(redis_io_runtime *runtime, uintptr_t closed_socket,
                                        uint64_t timeout_ns) {
  uint64_t started;
  if (redis_io_impl(runtime) == NULL || timeout_ns == 0u) return TURBO_EINVAL;
  if (redis_io_runtime_in_callback()) return TURBO_EBUSY;
  started = turbo_hrtime();
  for (;;) {
    int status = redis_io_runtime_forget_socket(runtime, closed_socket);
    if (status != TURBO_EBUSY) return status;
    if (turbo_hrtime() - started >= timeout_ns) return TURBO_ETIMEDOUT;
    turbo_thread_yield();
  }
}

int redis_io_runtime_close(redis_io_runtime *runtime) {
  redis_io_runtime_impl *impl = redis_io_impl(runtime);
  if (impl == NULL) return TURBO_EINVAL;
  turbo_mutex_lock(&impl->gate);
  if (impl->attached_publishers != 0u || impl->active_publisher_operations != 0u) {
    turbo_mutex_unlock(&impl->gate);
    return TURBO_EBUSY;
  }
  impl->closing = true;
  turbo_mutex_unlock(&impl->gate);
  return TURBO_OK;
}

int redis_io_runtime_destroy(redis_io_runtime *runtime) {
  redis_io_runtime_impl *impl = redis_io_impl(runtime);
  int status;
  if (impl == NULL) return TURBO_EINVAL;
  if (redis_io_runtime_in_callback()) return TURBO_EBUSY;
  turbo_mutex_lock(&impl->gate);
  if (!impl->closing || impl->destroying || impl->attached_publishers != 0u ||
      impl->active_publisher_operations != 0u) {
    turbo_mutex_unlock(&impl->gate);
    return TURBO_EBUSY;
  }
  impl->destroying = true;
  turbo_mutex_unlock(&impl->gate);
  if (impl->bridge_live) {
    status = cflow_io_actor_close(&impl->bridge);
    if (status != TURBO_OK && status != TURBO_EALREADY) goto failed;
    status = redis_io_schedule_bridge(impl);
    if (status != TURBO_OK || !cflow_executor_wait_idle(&impl->driver)) {
      if (status == TURBO_OK) status = TURBO_EBUSY;
      goto failed;
    }
    if (!cflow_io_actor_is_quiescent(&impl->bridge)) {
      status = TURBO_EBUSY;
      goto failed;
    }
    status = cflow_io_actor_destroy(&impl->bridge);
    if (status != TURBO_OK) goto failed;
    impl->bridge_live = false;
  }
  if (impl->backend_live) {
    status = cflow_io_native_backend_shutdown(&impl->backend);
    if (status != TURBO_OK && status != TURBO_EALREADY) goto failed;
    status = cflow_io_native_backend_destroy(&impl->backend);
    if (status != TURBO_OK) goto failed;
    impl->backend_live = false;
  }
  if (impl->driver_live) {
    if (!cflow_executor_shutdown(&impl->driver)) {
      status = TURBO_EBUSY;
      goto failed;
    }
    cflow_executor_destroy(&impl->driver);
    impl->driver_live = false;
  }
  turbo_cond_destroy(&impl->changed);
  turbo_mutex_destroy(&impl->gate);
  free(impl->retiring_sockets);
  free(impl->publishers);
  free(impl);
  runtime->impl = NULL;
  return TURBO_OK;
failed:
  turbo_mutex_lock(&impl->gate);
  impl->destroying = false;
  turbo_mutex_unlock(&impl->gate);
  return status;
}

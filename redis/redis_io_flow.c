#include "redis_io_flow.h"

#include "redis_io_internal.h"
#include "salts_error.h"
#include "salts_thread.h"

#include <cmeta/cmeta.h>
#include <salts/clock.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

typedef struct redis_io_flow_impl redis_io_flow_impl;
static void redis_io_flow_finish_runtime_pending(redis_io_flow_impl *impl, bool force);

typedef struct redis_io_flow_operation {
  cflow_io_native_operation native;
  redis_io_flow_impl *owner;
} redis_io_flow_operation;

_Static_assert(offsetof(redis_io_flow_operation, native) == 0u,
               "the CFlow native backend consumes operation_user as a native operation");

struct redis_io_flow_impl {
  redis_io_runtime *runtime;
  cflow_publisher publisher;
  cflow_io_publisher_owner owner;
  redis_io_runtime_publisher runtime_publisher;
  redis_io_flow_operation operation;
  cflow_waitable waitable;
  uint64_t close_timeout_ns;
  bool pending_valid;
  bool operation_owned;
  bool active;
  bool publisher_live;
  bool owner_live;
  bool attached;
  bool runtime_pending;
  bool completion_encoded;
  int driver_error;
  salts_mutex_t gate;
};

enum { REDIS_IO_FLOW_DRIVE_STEPS = 32u };

static const cmeta_type_traits redis_io_completion_traits = {.flags = CMETA_TRAIT_TRIVIAL_COPY |
                                                                      CMETA_TRAIT_TRIVIAL_DESTROY};

static const cmeta_type_desc redis_io_completion_type = {.name = "redis_io_completion",
                                                         .size = sizeof(cflow_io_completion),
                                                         .align = _Alignof(cflow_io_completion),
                                                         .kind = CMETA_T_OBJECT,
                                                         .traits = &redis_io_completion_traits};

static void redis_io_flow_operation_release(void *user) {
  redis_io_flow_operation *operation = (redis_io_flow_operation *)user;
  redis_io_flow_impl *impl = operation != NULL ? operation->owner : NULL;
  if (impl == NULL) return;
  salts_mutex_lock(&impl->gate);
  impl->operation_owned = false;
  salts_mutex_unlock(&impl->gate);
  redis_io_flow_finish_runtime_pending(impl, true);
}

static void redis_io_flow_finish_runtime_pending(redis_io_flow_impl *impl, bool force) {
  bool finish = false;
  if (impl == NULL) return;
  salts_mutex_lock(&impl->gate);
  if (impl->runtime_pending && (force || impl->completion_encoded)) {
    impl->runtime_pending = false;
    impl->completion_encoded = false;
    finish = true;
  }
  salts_mutex_unlock(&impl->gate);
  if (finish) redis_io_runtime_publisher_finished(impl->runtime);
}

static cflow_io_publisher_prepare_status
redis_io_flow_prepare(void *user, cflow_io_operation *operation, const char **error) {
  redis_io_flow_impl *impl = (redis_io_flow_impl *)user;
  if (impl == NULL || operation == NULL) {
    if (error != NULL) *error = "Redis I/O Publisher has no pending operation";
    return CFLOW_IO_PUBLISHER_PREPARE_ERROR;
  }
  salts_mutex_lock(&impl->gate);
  if (!impl->pending_valid || impl->operation_owned) {
    salts_mutex_unlock(&impl->gate);
    if (error != NULL) *error = "Redis I/O Publisher has no available operation";
    return CFLOW_IO_PUBLISHER_PREPARE_ERROR;
  }
  impl->pending_valid = false;
  impl->operation_owned = true;
  *operation = (cflow_io_operation){&impl->operation, redis_io_flow_operation_release};
  salts_mutex_unlock(&impl->gate);
  return CFLOW_IO_PUBLISHER_PREPARE_OPERATION;
}

static cflow_read_status redis_io_flow_encode(void *user, cflow_io_request_id request_id,
                                              cflow_io_lease_id lease_id, void *operation_user,
                                              const cflow_io_completion *completion,
                                              void *out_value, const char **error) {
  redis_io_flow_impl *impl = (redis_io_flow_impl *)user;
  (void)request_id;
  (void)lease_id;
  (void)operation_user;
  if (user == NULL || completion == NULL || out_value == NULL) {
    if (error != NULL) *error = "Redis I/O Publisher received an invalid completion";
    return CFLOW_READ_ERROR;
  }
  *(cflow_io_completion *)out_value = *completion;
  salts_mutex_lock(&impl->gate);
  impl->completion_encoded = true;
  salts_mutex_unlock(&impl->gate);
  return CFLOW_READ_VALUE;
}

static void redis_io_flow_run_scheduled(void *user) {
  redis_io_flow_impl *impl = (redis_io_flow_impl *)user;
  size_t progressed = 0u;
  int status = SALTS_OK;
  if (impl == NULL || !impl->owner_live) return;
  do {
    progressed = 0u;
    status = cflow_io_publisher_owner_run_ready(&impl->owner, REDIS_IO_FLOW_DRIVE_STEPS, &progressed);
  } while (status == SALTS_OK && progressed == REDIS_IO_FLOW_DRIVE_STEPS);
  if (status != SALTS_OK && status != SALTS_EBUSY) {
    salts_mutex_lock(&impl->gate);
    if (impl->driver_error == SALTS_OK) impl->driver_error = status;
    salts_mutex_unlock(&impl->gate);
  }
  redis_io_flow_finish_runtime_pending(impl, false);
}

static void redis_io_flow_wake(void *user) {
  redis_io_flow_impl *impl = (redis_io_flow_impl *)user;
  int status;
  if (impl == NULL) return;
  status = redis_io_runtime_schedule_publisher(&impl->runtime_publisher);
  if (status != SALTS_OK) {
    salts_mutex_lock(&impl->gate);
    if (impl->driver_error == SALTS_OK) impl->driver_error = status;
    salts_mutex_unlock(&impl->gate);
  }
}

static int redis_io_flow_close_owner(redis_io_flow_impl *impl) {
  uint64_t started;
  if (impl == NULL || !impl->owner_live) return SALTS_OK;
  if (redis_io_runtime_in_callback()) return SALTS_EBUSY;
  started = salts_hrtime();
  for (;;) {
    uint64_t now;
    int status = redis_io_runtime_schedule_publisher(&impl->runtime_publisher);
    if (status != SALTS_OK) return status;
    now = salts_hrtime();
    if (now - started >= impl->close_timeout_ns) return SALTS_ETIMEDOUT;
    status = redis_io_runtime_wait_publisher_idle(&impl->runtime_publisher,
                                               impl->close_timeout_ns - (now - started));
    if (status != SALTS_OK) return status;
    if (cflow_io_publisher_owner_is_quiescent(&impl->owner)) break;
  }
  {
    int status = cflow_io_publisher_owner_close(&impl->owner);
    if (status != SALTS_OK) return status;
  }
  impl->owner_live = false;
  return SALTS_OK;
}

int redis_io_flow_init(redis_io_flow *flow, redis_io_runtime *runtime, uint64_t close_timeout_ns) {
  redis_io_flow_impl *impl;
  cflow_io_publisher_config config = {0};
  int status;
  if (flow == NULL || flow->impl != NULL || runtime == NULL || close_timeout_ns == 0u)
    return SALTS_EINVAL;
  impl = (redis_io_flow_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  salts_mutex_init(&impl->gate);
  impl->runtime = runtime;
  impl->operation.owner = impl;
  impl->close_timeout_ns = close_timeout_ns;
  status =
      redis_io_runtime_attach_publisher(runtime, &impl->runtime_publisher, redis_io_flow_run_scheduled,
                                     impl, &config.backend, &config.backend_user);
  if (status != SALTS_OK) goto failed;
  impl->attached = true;
  config.name = "redis-native-operation";
  config.type = &redis_io_completion_type;
  config.prepare = redis_io_flow_prepare;
  config.encode = redis_io_flow_encode;
  config.user = impl;
  config.drive = redis_io_flow_wake;
  config.drive_user = impl;
  status = cflow_publisher_from_io_actor(&impl->publisher, &impl->owner, &config);
  if (status != SALTS_OK) goto failed;
  impl->publisher_live = true;
  impl->owner_live = true;
  flow->impl = impl;
  return SALTS_OK;

failed:
  if (impl->attached) (void)redis_io_runtime_detach_publisher(&impl->runtime_publisher);
  salts_mutex_destroy(&impl->gate);
  free(impl);
  return status;
}

int redis_io_flow_submit(redis_io_flow *flow, const cflow_io_native_operation *operation) {
  redis_io_flow_impl *impl = flow != NULL ? (redis_io_flow_impl *)flow->impl : NULL;
  cflow_publish_context context = {0};
  cflow_io_completion ignored = {0};
  cflow_step step;
  int status;
  if (impl == NULL || operation == NULL || !impl->publisher_live || impl->active ||
      impl->pending_valid)
    return SALTS_EINVAL;
  salts_mutex_lock(&impl->gate);
  if (impl->operation_owned) {
    salts_mutex_unlock(&impl->gate);
    return SALTS_EBUSY;
  }
  impl->operation.native = *operation;
  impl->pending_valid = true;
  salts_mutex_unlock(&impl->gate);
  status = redis_io_runtime_publisher_started(impl->runtime);
  if (status != SALTS_OK) {
    salts_mutex_lock(&impl->gate);
    impl->pending_valid = false;
    salts_mutex_unlock(&impl->gate);
    return status;
  }
  salts_mutex_lock(&impl->gate);
  impl->runtime_pending = true;
  impl->driver_error = SALTS_OK;
  salts_mutex_unlock(&impl->gate);
  context.downstream_demand = 1u;
  step = cflow_publisher_resume(&impl->publisher, &context, &ignored);
  if (step.kind != CFLOW_STEP_WAIT) {
    impl->pending_valid = false;
    redis_io_flow_finish_runtime_pending(impl, true);
    return step.kind == CFLOW_STEP_ERROR ? SALTS_EIO : SALTS_EPROTO;
  }
  impl->active = true;
  impl->waitable = step.waitable;
  return SALTS_OK;
}

redis_io_flow_step redis_io_flow_next(redis_io_flow *flow) {
  redis_io_flow_impl *impl = flow != NULL ? (redis_io_flow_impl *)flow->impl : NULL;
  redis_io_flow_step out = {REDIS_IO_FLOW_ERROR, {0}, {0}, SALTS_EINVAL};
  cflow_io_completion completion = {0};
  cflow_publish_context context = {0};
  cflow_step step;
  int status;
  if (impl == NULL || !impl->active || !impl->publisher_live) return out;
  salts_mutex_lock(&impl->gate);
  status = impl->driver_error;
  salts_mutex_unlock(&impl->gate);
  if (status != SALTS_OK) {
    out.status = status;
    return out;
  }
  context.downstream_demand = 1u;
  step = cflow_publisher_resume(&impl->publisher, &context, &completion);
  if (step.kind == CFLOW_STEP_WAIT) {
    impl->waitable = step.waitable;
    out.kind = REDIS_IO_FLOW_WAIT;
    out.waitable = step.waitable;
    out.status = SALTS_OK;
    return out;
  }
  if (step.kind != CFLOW_STEP_VALUE) {
    out.status = step.kind == CFLOW_STEP_ERROR ? SALTS_EIO : SALTS_EPROTO;
    return out;
  }
  impl->active = false;
  impl->waitable = (cflow_waitable){0};
  out.kind = REDIS_IO_FLOW_VALUE;
  out.completion = completion;
  out.status = SALTS_OK;
  return out;
}

cflow_waitable redis_io_flow_waitable(redis_io_flow *flow) {
  redis_io_flow_impl *impl = flow != NULL ? (redis_io_flow_impl *)flow->impl : NULL;
  return impl != NULL && impl->active ? impl->waitable : (cflow_waitable){0};
}

int redis_io_flow_cancel(redis_io_flow *flow) {
  redis_io_flow_impl *impl = flow != NULL ? (redis_io_flow_impl *)flow->impl : NULL;
  int status;
  if (impl == NULL) return SALTS_EINVAL;
  if (impl->publisher_live) {
    cflow_publisher_destroy(&impl->publisher);
    impl->publisher_live = false;
  }
  status = redis_io_flow_close_owner(impl);
  if (status != SALTS_OK) return status;
  impl->active = false;
  impl->pending_valid = false;
  redis_io_flow_finish_runtime_pending(impl, true);
  return SALTS_OK;
}

int redis_io_flow_destroy(redis_io_flow *flow) {
  redis_io_flow_impl *impl = flow != NULL ? (redis_io_flow_impl *)flow->impl : NULL;
  int status;
  if (impl == NULL) return SALTS_EINVAL;
  status = redis_io_flow_cancel(flow);
  if (status != SALTS_OK) return status;
  if (impl->attached) {
    status = redis_io_runtime_detach_publisher(&impl->runtime_publisher);
    if (status != SALTS_OK) return status;
    impl->attached = false;
  }
  salts_mutex_destroy(&impl->gate);
  free(impl);
  flow->impl = NULL;
  return SALTS_OK;
}

int redis_io_flow_active(const redis_io_flow *flow) {
  const redis_io_flow_impl *impl = flow != NULL ? (const redis_io_flow_impl *)flow->impl : NULL;
  return impl != NULL && impl->active;
}

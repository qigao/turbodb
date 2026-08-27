#ifndef REDIS_IO_H
#define REDIS_IO_H

#include "redis_export.h"

#include <cflow/io_native.h>
#include <cflow/runtime.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct redis_io_runtime {
  void *impl;
} redis_io_runtime;

typedef struct redis_io_request {
  redis_io_runtime *runtime;
  size_t slot;
  uint64_t generation;
} redis_io_request;

/* A request object is a single-owner mutable handle and must not be read while
 * another thread acknowledges, abandons, or reuses that same object. A
 * waitable returned by redis_io_request_waitable() carries a runtime-owned
 * immutable generation token and may be cancelled concurrently with its wake
 * callback; cancellation does not return until that callback is quiescent. */

typedef struct redis_io_runtime_config {
  cflow_io_native_backend_kind backend_kind;
  size_t request_capacity;
  size_t command_capacity;
  size_t completion_batch_capacity;
} redis_io_runtime_config;

typedef enum redis_io_submit_status {
  REDIS_IO_SUBMIT_ACCEPTED = 0,
  REDIS_IO_SUBMIT_INVALID_ARGUMENT,
  REDIS_IO_SUBMIT_FULL,
  REDIS_IO_SUBMIT_CLOSED,
  REDIS_IO_SUBMIT_LEASE_IN_USE,
  REDIS_IO_SUBMIT_ID_EXHAUSTED
} redis_io_submit_status;

typedef enum redis_io_request_poll_status {
  REDIS_IO_REQUEST_PENDING = 0,
  REDIS_IO_REQUEST_COMPLETED,
  REDIS_IO_REQUEST_INVALID
} redis_io_request_poll_status;

REDIS_API cflow_io_native_backend_kind redis_io_default_backend_kind(void);

REDIS_API int redis_io_runtime_init(redis_io_runtime *runtime,
                                    const redis_io_runtime_config *config);
REDIS_API int redis_io_runtime_close(redis_io_runtime *runtime);
REDIS_API int redis_io_runtime_destroy(redis_io_runtime *runtime);
/* Returns TURBO_EBUSY when called from an I/O driver or wake callback. */
REDIS_API int redis_io_runtime_wait_idle(redis_io_runtime *runtime,
                                         uint64_t timeout_ns);
/* Reserve a socket identity against admission before the owner closes it. */
REDIS_API int redis_io_runtime_retire_socket(redis_io_runtime *runtime,
                                             uintptr_t socket_identity);
REDIS_API int redis_io_runtime_forget_socket(redis_io_runtime *runtime,
                                             uintptr_t closed_socket);
/*
 * Retry a transient native-backend EBUSY until timeout. Driver/wake callback
 * reentry returns TURBO_EBUSY immediately because that callback must unwind
 * before native readiness can retire the socket identity.
 */
REDIS_API int redis_io_runtime_forget_socket_wait(redis_io_runtime *runtime,
                                                  uintptr_t closed_socket,
                                                  uint64_t timeout_ns);
REDIS_API int redis_io_runtime_run_ready(redis_io_runtime *runtime,
                                         size_t max_steps,
                                         size_t *progressed);
REDIS_API int redis_io_runtime_valid(const redis_io_runtime *runtime);

REDIS_API redis_io_submit_status redis_io_runtime_try_submit(
    redis_io_runtime *runtime, cflow_io_lease_id lease_id,
    const cflow_io_native_operation *operation, redis_io_request *out_request);

REDIS_API int redis_io_request_valid(const redis_io_request *request);
REDIS_API cflow_waitable redis_io_request_waitable(redis_io_request *request);
REDIS_API redis_io_request_poll_status redis_io_request_poll(
    const redis_io_request *request, cflow_io_completion *completion);
REDIS_API cflow_io_cancel_status redis_io_request_cancel(
    redis_io_request *request);
REDIS_API void redis_io_request_abandon(redis_io_request *request);
REDIS_API int redis_io_request_acknowledge(redis_io_request *request);

#ifdef __cplusplus
}
#endif

#endif

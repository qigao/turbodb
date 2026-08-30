#ifndef REDIS_IO_H
#define REDIS_IO_H

#include "redis_export.h"

#include <cflow/io_native.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct redis_io_runtime {
  void *impl;
} redis_io_runtime;

typedef struct redis_io_runtime_config {
  cflow_io_native_backend_kind backend_kind;
  /** Maximum number of attached per-connection CFlow I/O Publishers. */
  size_t publisher_capacity;
  /** Maximum native completions collected by one backend poll. */
  size_t completion_batch_capacity;
} redis_io_runtime_config;

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
REDIS_API int redis_io_runtime_valid(const redis_io_runtime *runtime);

#ifdef __cplusplus
}
#endif

#endif

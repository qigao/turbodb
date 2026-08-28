#ifndef REDIS_IO_INTERNAL_H
#define REDIS_IO_INTERNAL_H

#include "redis_io.h"

#include <cflow/io_actor.h>

typedef struct redis_io_runtime_source {
  void *slot;
  uint64_t generation;
} redis_io_runtime_source;

typedef void (*redis_io_runtime_drive_fn)(void *user);

int redis_io_runtime_attach_source(redis_io_runtime *runtime, redis_io_runtime_source *source,
                                   redis_io_runtime_drive_fn drive, void *drive_user,
                                   cflow_io_backend_ops *backend, void **backend_user);
int redis_io_runtime_schedule_source(redis_io_runtime_source *source);
int redis_io_runtime_wait_source_idle(redis_io_runtime_source *source, uint64_t timeout_ns);
int redis_io_runtime_detach_source(redis_io_runtime_source *source);
int redis_io_runtime_source_started(redis_io_runtime *runtime);
void redis_io_runtime_source_finished(redis_io_runtime *runtime);
void redis_io_runtime_enter_callback(void);
void redis_io_runtime_leave_callback(void);
int redis_io_runtime_in_callback(void);

#endif

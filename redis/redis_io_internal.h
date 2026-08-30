#ifndef REDIS_IO_INTERNAL_H
#define REDIS_IO_INTERNAL_H

#include "redis_io.h"

#include <cflow/io_actor.h>

typedef struct redis_io_runtime_publisher {
  void *slot;
  uint64_t generation;
} redis_io_runtime_publisher;

typedef void (*redis_io_runtime_drive_fn)(void *user);

int redis_io_runtime_attach_publisher(redis_io_runtime *runtime, redis_io_runtime_publisher *publisher,
                                   redis_io_runtime_drive_fn drive, void *drive_user,
                                   cflow_io_backend_ops *backend, void **backend_user);
int redis_io_runtime_schedule_publisher(redis_io_runtime_publisher *publisher);
int redis_io_runtime_wait_publisher_idle(redis_io_runtime_publisher *publisher, uint64_t timeout_ns);
int redis_io_runtime_detach_publisher(redis_io_runtime_publisher *publisher);
int redis_io_runtime_publisher_started(redis_io_runtime *runtime);
void redis_io_runtime_publisher_finished(redis_io_runtime *runtime);
void redis_io_runtime_enter_callback(void);
void redis_io_runtime_leave_callback(void);
int redis_io_runtime_in_callback(void);

#endif

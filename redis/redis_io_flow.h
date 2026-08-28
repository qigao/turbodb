#ifndef REDIS_IO_FLOW_H
#define REDIS_IO_FLOW_H

#include "redis_io.h"

#include <cflow/io_source.h>

typedef struct redis_io_flow {
  void *impl;
} redis_io_flow;

typedef enum redis_io_flow_step_kind {
  REDIS_IO_FLOW_WAIT = 0,
  REDIS_IO_FLOW_VALUE,
  REDIS_IO_FLOW_ERROR
} redis_io_flow_step_kind;

typedef struct redis_io_flow_step {
  redis_io_flow_step_kind kind;
  cflow_waitable waitable;
  cflow_io_completion completion;
  int status;
} redis_io_flow_step;

int redis_io_flow_init(redis_io_flow *flow, redis_io_runtime *runtime, uint64_t close_timeout_ns);
int redis_io_flow_submit(redis_io_flow *flow, const cflow_io_native_operation *operation);
redis_io_flow_step redis_io_flow_next(redis_io_flow *flow);
cflow_waitable redis_io_flow_waitable(redis_io_flow *flow);
int redis_io_flow_cancel(redis_io_flow *flow);
int redis_io_flow_destroy(redis_io_flow *flow);
int redis_io_flow_active(const redis_io_flow *flow);

#endif

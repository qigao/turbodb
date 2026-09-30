#ifndef ORM_ASYNC_WAIT_H
#define ORM_ASYNC_WAIT_H
#include "orm_row_publisher.h"

/* One timer and one deadline per active query; all calls run on the same owner.
 * The Salts timer owns registration/cancellation, including late wake safety. */
typedef struct orm_async_wait {
  cflow_publisher timer;
  orm_async_config_t config;
  uint64_t started;
} orm_async_wait;

static inline int orm_async_wait_init(orm_async_wait *wait,
                                      const orm_async_config_t *config) {
  wait->config = *config;
  wait->started = cflow_scheduler_now(config->scheduler);
  return cflow_publisher_from_timer(&wait->timer, SIZE_MAX, config->poll_interval_ticks);
}
static inline void orm_async_wait_destroy(orm_async_wait *wait) {
  if (cflow_publisher_valid(&wait->timer)) cflow_publisher_destroy(&wait->timer);
  wait->timer = (cflow_publisher){0};
}
static inline int orm_async_wait_expired(const orm_async_wait *wait) {
  return cflow_scheduler_now(wait->config.scheduler) - wait->started >=
         wait->config.timeout_ticks;
}
static inline orm_row_cursor_step orm_async_wait_step(orm_async_wait *wait) {
  orm_row_cursor_step result = ORM_ROW_CURSOR_STEP_INIT;
  if (orm_async_wait_expired(wait)) {
    result.kind = ORM_ROW_CURSOR_ERROR;
    result.status = ORM_STATUS_CONNECTION_ERROR;
    result.message = "async query deadline expired";
    return result;
  }
  cflow_publish_context context = {wait->config.scheduler, 1u};
  uint64_t tick = 0u;
  cflow_step step = cflow_publisher_resume(&wait->timer, &context, &tick);
  if (step.kind == CFLOW_STEP_VALUE)
    step = cflow_publisher_resume(&wait->timer, &context, &tick);
  if (step.kind == CFLOW_STEP_WAIT) {
    result.kind = ORM_ROW_CURSOR_WAIT;
    result.waitable = step.waitable;
  } else {
    result.kind = ORM_ROW_CURSOR_ERROR;
    result.status = ORM_STATUS_INTERNAL_ERROR;
    result.message = step.error ? step.error : "async poll timer failed";
  }
  return result;
}
#endif

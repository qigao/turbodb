#ifndef ORM_ROW_ALLOCATOR_PROBE_H
#define ORM_ROW_ALLOCATOR_PROBE_H

#include <stddef.h>

/* Private test-core injection, scoped to allocations made by the row publisher.
 * One synchronous attempt per thread; disarm before destroying its outputs.
 * No public allocator, process-wide interposition, or production fault switch. */
typedef struct orm_row_allocator_observation {
  size_t calls;
  size_t failures;
  size_t live;
} orm_row_allocator_observation;

void orm_row_allocator_arm(size_t fail_at);
void orm_row_allocator_disarm(void);
orm_row_allocator_observation orm_row_allocator_observe(void);

#endif

/* Compile the unchanged production translation unit in the private test core.
 * Include its headers before the macros: only its own allocation call sites
 * are redirected, never the CRT or a dependency's declarations/implementation. */
#include "orm_row_allocator_probe.h"
#include "orm_row_publisher.h"
#include "orm_owner.h"
#include <data_bind_native.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#define ORM_TEST_THREAD_LOCAL __declspec(thread)
#else
#define ORM_TEST_THREAD_LOCAL _Thread_local
#endif

enum { ORM_ROW_PROBE_SLOTS = 4 };
static ORM_TEST_THREAD_LOCAL struct {
  int armed;
  size_t fail_at;
  orm_row_allocator_observation observation;
  void *allocations[ORM_ROW_PROBE_SLOTS];
} row_probe;

void orm_row_allocator_arm(size_t fail_at) {
  if (row_probe.armed || row_probe.observation.live != 0u) abort();
  memset(&row_probe, 0, sizeof(row_probe));
  row_probe.armed = 1;
  row_probe.fail_at = fail_at;
}

void orm_row_allocator_disarm(void) { row_probe.armed = 0; }

orm_row_allocator_observation orm_row_allocator_observe(void) {
  return row_probe.observation;
}

static int row_allocation_refused(void) {
  if (!row_probe.armed) return 0;
  ++row_probe.observation.calls;
  if (row_probe.observation.calls != row_probe.fail_at) return 0;
  ++row_probe.observation.failures;
  return 1;
}

static void *row_allocation_record(void *memory) {
  if (memory != NULL && row_probe.armed) {
    for (size_t i = 0u; i < ORM_ROW_PROBE_SLOTS; ++i) {
      if (row_probe.allocations[i] == NULL) {
        row_probe.allocations[i] = memory;
        ++row_probe.observation.live;
        return memory;
      }
    }
    abort(); /* A changed preparation layout needs a reviewed probe bound. */
  }
  return memory;
}

static void *row_probe_malloc(size_t bytes) {
  if (row_allocation_refused()) return NULL;
  return row_allocation_record(malloc(bytes));
}

static void *row_probe_calloc(size_t count, size_t bytes) {
  if (row_allocation_refused()) return NULL;
  return row_allocation_record(calloc(count, bytes));
}

static void row_probe_free(void *memory) {
  if (memory != NULL) {
    for (size_t i = 0u; i < ORM_ROW_PROBE_SLOTS; ++i) {
      if (row_probe.allocations[i] == memory) {
        row_probe.allocations[i] = NULL;
        --row_probe.observation.live;
        break;
      }
    }
  }
  free(memory);
}

#define malloc row_probe_malloc
#define calloc row_probe_calloc
#define free row_probe_free
#include "../../src/flow/orm_row_publisher.c"
#undef free
#undef calloc
#undef malloc
#undef ORM_TEST_THREAD_LOCAL

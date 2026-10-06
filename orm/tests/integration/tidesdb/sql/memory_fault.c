#include "memory_fault.h"
#include <errno.h>
#include <stdlib.h>
#include <alloc.h>
/* Native alloc.h redirects the CRT names. The callbacks must call the real
 * allocator, otherwise installing them would recurse into themselves. */
#undef malloc
#undef calloc
#undef realloc
#undef free

#if defined(_MSC_VER)
#define ORM_TDB_MEMORY_TLS __declspec(thread)
#else
#define ORM_TDB_MEMORY_TLS _Thread_local
#endif
static ORM_TDB_MEMORY_TLS struct {
  int armed;
  orm_tdb_memory_phase phase;
  size_t fail_at;
  orm_tdb_memory_observation observed;
} probe;

static int refuse_allocation(void) {
  if (!probe.armed) return 0;
  const int after_wal = probe.observed.wal_frames != 0;
  if (after_wal != (probe.phase == ORM_TDB_MEMORY_AFTER_WAL)) return 0;
  if (++probe.observed.calls != probe.fail_at) return 0;
  ++probe.observed.failures;
  errno = ENOMEM;
  return 1;
}
void *orm_tdb_memory_malloc(size_t bytes) {
  return refuse_allocation() ? NULL : malloc(bytes);
}
void *orm_tdb_memory_calloc(size_t count, size_t bytes) {
  return refuse_allocation() ? NULL : calloc(count, bytes);
}
void *orm_tdb_memory_realloc(void *memory, size_t bytes) {
  return refuse_allocation() ? NULL : realloc(memory, bytes);
}
static void fault_free(void *memory) { free(memory); }

int orm_tdb_memory_fault_init(void) {
  return tidesdb_init(orm_tdb_memory_malloc, orm_tdb_memory_calloc,
      orm_tdb_memory_realloc, fault_free);
}
void orm_tdb_memory_fault_finish(void) { tidesdb_finalize(); }
void orm_tdb_memory_fault_arm(orm_tdb_memory_phase phase, size_t fail_at) {
  if (probe.armed || (phase != ORM_TDB_MEMORY_BEFORE_WAL && phase != ORM_TDB_MEMORY_AFTER_WAL))
    abort();
  probe.armed = 1; probe.phase = phase; probe.fail_at = fail_at;
  probe.observed = (orm_tdb_memory_observation){0};
}
void orm_tdb_memory_fault_disarm(void) { probe.armed = 0; }
orm_tdb_memory_observation orm_tdb_memory_fault_observe(void) { return probe.observed; }
void orm_tdb_memory_fault_wal_written(void) {
  if (probe.armed) ++probe.observed.wal_frames;
}
#undef ORM_TDB_MEMORY_TLS

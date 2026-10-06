#ifndef ORM_TIDESDB_TEST_MEMORY_FAULT_H
#define ORM_TIDESDB_TEST_MEMORY_FAULT_H
#include <stddef.h>

typedef enum orm_tdb_memory_phase {
  ORM_TDB_MEMORY_BEFORE_WAL,
  ORM_TDB_MEMORY_AFTER_WAL
} orm_tdb_memory_phase;
typedef struct orm_tdb_memory_observation {
  size_t calls, failures, wal_frames;
} orm_tdb_memory_observation;

/* Install once before native initialization; finalize only after all native
 * handles and background workers are gone. Callbacks delegate to the CRT;
 * only an armed calling thread refuses its selected allocation. */
int orm_tdb_memory_fault_init(void);
void orm_tdb_memory_fault_finish(void);
void orm_tdb_memory_fault_arm(orm_tdb_memory_phase phase, size_t fail_at);
void orm_tdb_memory_fault_disarm(void);
orm_tdb_memory_observation orm_tdb_memory_fault_observe(void);
/* Private notification from the WAL syscall probe after a full frame write. */
void orm_tdb_memory_fault_wal_written(void);
/* skip_list.c does not include native alloc.h. Its private test TU uses the
 * same callbacks directly; both paths share one TLS observation. */
void *orm_tdb_memory_malloc(size_t bytes);
void *orm_tdb_memory_calloc(size_t count, size_t bytes);
void *orm_tdb_memory_realloc(void *memory, size_t bytes);

enum {
  ORM_TDB_MEMORY_BATCH_KEYS = 16,
  ORM_TDB_MEMORY_KEY_BYTES = 48,
  /* After WAL: dedup hash, used slots, then mandatory batch entries. */
  ORM_TDB_MEMORY_BATCH_ALLOCATION = 3
};
#endif

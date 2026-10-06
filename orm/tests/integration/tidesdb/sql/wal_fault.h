#ifndef ORM_TIDESDB_TEST_WAL_FAULT_H
#define ORM_TIDESDB_TEST_WAL_FAULT_H

#include <stddef.h>

typedef enum orm_tdb_wal_fault {
  ORM_TDB_WAL_FAULT_NONE,
  ORM_TDB_WAL_FAIL_BEFORE,
  ORM_TDB_WAL_FAIL_PARTIAL,
  ORM_TDB_WAL_FAIL_AFTER,
  ORM_TDB_WAL_EXIT_PARTIAL,
  ORM_TDB_WAL_EXIT_AFTER,
  ORM_TDB_WAL_SYNC_FAIL_BEFORE,
  ORM_TDB_WAL_SYNC_FAIL_AFTER,
  ORM_TDB_WAL_SYNC_EXIT_BEFORE,
  ORM_TDB_WAL_SYNC_EXIT_AFTER
} orm_tdb_wal_fault;

/* Private, calling-thread-only probe; arm immediately before one commit. */
void orm_tdb_wal_fault_arm(orm_tdb_wal_fault fault);
size_t orm_tdb_wal_fault_hits(void);
/* Platforms using O_DSYNC have no separate per-append sync call to inject. */
int orm_tdb_wal_uses_explicit_sync(void);

#endif

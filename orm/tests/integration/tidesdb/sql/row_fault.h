#ifndef ORM_TIDESDB_TEST_ROW_FAULT_H
#define ORM_TIDESDB_TEST_ROW_FAULT_H
#include <stddef.h>
typedef enum orm_tdb_row_fault {
  ORM_TDB_ROW_NUMERIC_COPY,
  ORM_TDB_ROW_DECODE_COPY,
  ORM_TDB_ROW_ENCODE_APPEND
} orm_tdb_row_fault;
/* Private single-thread probe; fail_at=0 measures calls without failing. */
void orm_tdb_row_fault_arm(orm_tdb_row_fault site, size_t fail_at);
void orm_tdb_row_fault_disarm(void);
size_t orm_tdb_row_fault_calls(void);
size_t orm_tdb_row_fault_hits(void);
#endif

/* Headers precede macros so only production row.c call sites are replaced. */
#include "row.h"
#include "row_fault.h"
#include <salts/thread.h>
#include <stdlib.h>

static SALTS_THREAD_LOCAL struct {
  int armed;
  orm_tdb_row_fault site;
  size_t fail_at, calls, hits;
} probe;

void orm_tdb_row_fault_arm(orm_tdb_row_fault site, size_t fail_at) {
  if (probe.armed) abort();
  probe.armed = 1; probe.site = site; probe.fail_at = fail_at;
  probe.calls = 0; probe.hits = 0;
}
void orm_tdb_row_fault_disarm(void) { probe.armed = 0; }
size_t orm_tdb_row_fault_calls(void) { return probe.calls; }
size_t orm_tdb_row_fault_hits(void) { return probe.hits; }

static int refused(orm_tdb_row_fault site) {
  if (!probe.armed || probe.site != site) return 0;
  if (++probe.calls != probe.fail_at) return 0;
  ++probe.hits;
  return 1;
}
static tstr numeric_copy(const char *data, size_t size) {
  return refused(ORM_TDB_ROW_NUMERIC_COPY) ? NULL : tstr_dup_len(data, size);
}
static tstr decode_copy(const void *data, size_t size) {
  return refused(ORM_TDB_ROW_DECODE_COPY) ? NULL : tstr_new_len(data, size);
}
static tstr encode_append(tstr text, const char *data, size_t size) {
  return refused(ORM_TDB_ROW_ENCODE_APPEND) ? NULL : tstr_cat_len(text, data, size);
}
#define tstr_dup_len numeric_copy
#define tstr_new_len decode_copy
#define tstr_cat_len encode_append
#include "../../../../../drivers/tidesdb/row.c"
#undef tstr_cat_len
#undef tstr_new_len
#undef tstr_dup_len

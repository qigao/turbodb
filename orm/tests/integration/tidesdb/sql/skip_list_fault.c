#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <skip_list.h>
#include "memory_fault.h"

/* The native skip-list TU uses CRT allocation directly. Intercept only its
 * own call sites; the default and every free still use the same CRT heap. */
#define malloc orm_tdb_memory_malloc
#define calloc orm_tdb_memory_calloc
#define realloc orm_tdb_memory_realloc
#include "../../../../../tidesdb/src/skip_list.c"
#undef realloc
#undef calloc
#undef malloc

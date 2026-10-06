#ifndef ORM_TIDESDB_SQL_WORK_H
#define ORM_TIDESDB_SQL_WORK_H
#include "budget.h"
#include <cstl/vec.h>

/* Private fixed-capacity raw record storage. Zero-initialized vector/reserved
 * required. size must include alignment padding. Reserves capacity + metadata
 * and current Salts raw Vec alignment overhead before allocation. Call release
 * on both success and partial failure; no further growth past count is allowed.
 * The statement budget must outlive storage, with single-owner access. */
turbodb_status_t orm_sql_work_allocate(vec_t *vector, size_t count, size_t size,
    size_t align, size_t metadata, orm_tidesdb_sql_budget *budget,
    size_t *reserved, turbodb_error_t *error);
turbodb_status_t orm_sql_work_release(vec_t *vector, size_t reserved,
    orm_tidesdb_sql_budget *budget, turbodb_error_t *error);
/* Allocate and resize fixed raw records, zeroing live elements. Empty is a
 * no-op. Same partial-failure release requirement as work_allocate. */
turbodb_status_t orm_sql_work_zero(vec_t *vector, size_t count, size_t size, size_t align,
    orm_tidesdb_sql_budget *budget, size_t *reserved, turbodb_error_t *error);
/* Sort a fixed uint64_t span in place. Charges O(count log count) comparison
 * allowance and O(count) CSTL scratch before sorting; refunds only scratch.
 * Empty/singleton spans need no workspace. Caller owns the span throughout. */
turbodb_status_t orm_sql_work_sort_u64(uint64_t *values, size_t count,
    orm_tidesdb_sql_budget *budget, turbodb_error_t *error);
/* Fixed records, size aligned to type->align, with infallible copy/move/compare
 * traits. Borrows base/type through return. comparison_steps is the positive
 * worst-case cost of one comparator call. Charges scratch and comparisons;
 * no partial result may be consumed on failure. */
turbodb_status_t orm_sql_work_sort(void *base, size_t count, const cmeta_type_desc *type,
    size_t comparison_steps, orm_tidesdb_sql_budget *budget, turbodb_error_t *error);
#endif

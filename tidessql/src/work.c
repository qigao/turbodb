#include "work.h"
#include "error.h"
#include <cstl/sort.h>
#include <string.h>

turbodb_status_t orm_sql_work_allocate(vec_t *vector, size_t count, size_t size,
    size_t align, size_t metadata, orm_tidesdb_sql_budget *budget,
    size_t *reserved, turbodb_error_t *error) {
  if (!vector || vector->initialized || !reserved || *reserved || !budget ||
      !size || !align || (align & (align - 1)) || size % align) {
    tdsql_error_set(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid SQL work vector");
    return TURBODB_STATUS_INVALID_ARGUMENT;
  }
  /* Salts sequence_allocate stores a pointer receipt plus alignment slack.
   * CRT heap bookkeeping is not part of the requested-byte budget. */
  if (align - 1 > SIZE_MAX - sizeof(void *) ||
      metadata > SIZE_MAX - (sizeof(void *) + align - 1)) {
    tdsql_error_set(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL work metadata overflow");
    return TURBODB_STATUS_LIMIT_EXCEEDED;
  }
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve_capacity(budget, count,
      size, metadata + sizeof(void *) + align - 1, reserved, error);
  if (status != TURBODB_STATUS_OK) return status;
  stl_status allocated = vec_init_bytes(vector, size, align, count);
  if (allocated == STL_OK) allocated = vec_reserve(vector, count);
  if (allocated == STL_OK) return TURBODB_STATUS_OK;
  status = allocated == STL_OUT_OF_MEMORY ? TURBODB_STATUS_OUT_OF_MEMORY :
      allocated == STL_CAPACITY_EXCEEDED ? TURBODB_STATUS_LIMIT_EXCEEDED : TURBODB_STATUS_INTERNAL_ERROR;
  tdsql_error_set(error, status, "SQL work vector allocation failed");
  return status;
}

turbodb_status_t orm_sql_work_release(vec_t *vector, size_t reserved,
    orm_tidesdb_sql_budget *budget, turbodb_error_t *error) {
  vec_destroy(vector);
  return reserved ? orm_tidesdb_sql_budget_release(budget,
      ORM_SQL_BUDGET_WORK_BYTES, reserved, error) : TURBODB_STATUS_OK;
}

turbodb_status_t orm_sql_work_zero(vec_t *vector, size_t count, size_t size, size_t align,
    orm_tidesdb_sql_budget *budget, size_t *reserved, turbodb_error_t *error) {
  if (!count) return TURBODB_STATUS_OK;
  turbodb_status_t status = orm_sql_work_allocate(vector, count, size, align, 0, budget, reserved, error);
  if (status != TURBODB_STATUS_OK) return status;
  const stl_status resized = vec_resize(vector, count);
  if (resized != STL_OK) {
    status = resized == STL_OUT_OF_MEMORY ? TURBODB_STATUS_OUT_OF_MEMORY :
        resized == STL_CAPACITY_EXCEEDED ? TURBODB_STATUS_LIMIT_EXCEEDED : TURBODB_STATUS_INTERNAL_ERROR;
    tdsql_error_set(error, status, "SQL work vector resize failed"); return status;
  }
  memset(vec_data(vector), 0, count * size); return TURBODB_STATUS_OK;
}

turbodb_status_t orm_sql_work_sort(void *base, size_t count, const cmeta_type_desc *type,
    size_t comparison_steps, orm_tidesdb_sql_budget *budget, turbodb_error_t *error) {
  if (!type || !type->size || !type->align || (type->align & (type->align - 1)) ||
      type->size % type->align || !comparison_steps) {
    tdsql_error_set(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid SQL ordering descriptor");
    return TURBODB_STATUS_INVALID_ARGUMENT;
  }
  if (count < 2) return TURBODB_STATUS_OK;
  if (count > UINT64_MAX / comparison_steps || type->align - 1 > SIZE_MAX - sizeof(void *)) {
    tdsql_error_set(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL ordering capacity overflow");
    return TURBODB_STATUS_LIMIT_EXCEEDED;
  }
  for (size_t width = count; width > 1; width = width / 2 + width % 2) {
    orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = (uint64_t)count * comparison_steps;
    const turbodb_status_t status = orm_tidesdb_sql_budget_reserve(budget, &amount, error);
    if (status != TURBODB_STATUS_OK) return status;
  }
  size_t bytes = 0;
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve_capacity(budget, count, type->size,
      sizeof(void *) + type->align - 1, &bytes, error);
  if (status != TURBODB_STATUS_OK) return status;
  const stl_status sorted = stable_sort(base, count, type, count * type->size);
  if (sorted != STL_OK) {
    status = sorted == STL_OUT_OF_MEMORY ? TURBODB_STATUS_OUT_OF_MEMORY :
        sorted == STL_CAPACITY_EXCEEDED ? TURBODB_STATUS_LIMIT_EXCEEDED : TURBODB_STATUS_INTERNAL_ERROR;
    tdsql_error_set(error, status, "SQL ordering failed");
  }
  const turbodb_status_t released = orm_tidesdb_sql_budget_release(budget, ORM_SQL_BUDGET_WORK_BYTES,
      bytes, status == TURBODB_STATUS_OK ? error : NULL);
  return status == TURBODB_STATUS_OK ? released : status;
}
turbodb_status_t orm_sql_work_sort_u64(uint64_t *values, size_t count,
    orm_tidesdb_sql_budget *budget, turbodb_error_t *error) {
  return orm_sql_work_sort(values, count, &cmeta_type_uint64, 1, budget, error);
}

#include "diagnostics.h"
#include "value.h"

#include <stdio.h>
#include <string.h>

static turbodb_status_t diagnostics_error(turbodb_error_t *error,
    turbodb_status_t status, const char *message) {
  tdsql_error_set(error,status,message); return status;
}

turbodb_status_t orm_sql_evaluation_cast(orm_sql_evaluation evaluation,
    unsigned condition, bool unsigned_target, turbodb_error_t *error) {
  if (evaluation.mode < ORM_SQL_EVALUATION_QUERY || evaluation.mode > ORM_SQL_EVALUATION_IGNORE_WRITE ||
      (condition & ~(ORM_SQL_CAST_TRUNCATED | ORM_SQL_CAST_COMPLEMENT)))
    return diagnostics_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid CAST evaluation policy");
  if ((condition & ORM_SQL_CAST_TRUNCATED) && evaluation.mode == ORM_SQL_EVALUATION_WRITE)
    return diagnostics_error(error,TURBODB_STATUS_SQL_ERROR,"truncated incorrect CAST value");
  turbodb_status_t status=TURBODB_STATUS_OK;
  if (condition & ORM_SQL_CAST_TRUNCATED)
    status=orm_sql_diagnostics_add(evaluation.diagnostics,ORM_SQL_DIAGNOSTIC_CAST_TRUNCATED,
        "Truncated incorrect numeric CAST value",error);
  if (status==TURBODB_STATUS_OK && (condition & ORM_SQL_CAST_COMPLEMENT))
    status=orm_sql_diagnostics_add(evaluation.diagnostics,ORM_SQL_DIAGNOSTIC_CAST_COMPLEMENT,
        unsigned_target ? "Cast to unsigned converted negative integer to its positive complement" :
        "Cast to signed converted positive out-of-range integer to its negative complement",error);
  return status;
}

turbodb_status_t orm_sql_evaluation_division_by_zero(orm_sql_evaluation evaluation,
    turbodb_error_t *error) {
  if (evaluation.mode == ORM_SQL_EVALUATION_WRITE)
    return diagnostics_error(error,TURBODB_STATUS_SQL_ERROR,"division by zero");
  if (evaluation.mode != ORM_SQL_EVALUATION_QUERY && evaluation.mode != ORM_SQL_EVALUATION_IGNORE_WRITE)
    return diagnostics_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SQL evaluation mode");
  return orm_sql_diagnostics_add(evaluation.diagnostics,ORM_SQL_DIAGNOSTIC_DIVISION_BY_ZERO,
      "Division by 0",error);
}

turbodb_status_t orm_sql_diagnostics_init(orm_sql_diagnostics *diagnostics,
    size_t max_records, turbodb_error_t *error) {
  if (!diagnostics || diagnostics->max_records || vec_size(&diagnostics->records))
    return diagnostics_error(error,TURBODB_STATUS_INVALID_ARGUMENT,
        "diagnostics require empty output");
  if (!max_records)
    return diagnostics_error(error,TURBODB_STATUS_INVALID_ARGUMENT,
        "diagnostic record limit must be positive");
  stl_status allocated=vec_init_bytes(&diagnostics->records,
      sizeof(orm_sql_diagnostic),_Alignof(orm_sql_diagnostic),max_records);
  if (allocated==STL_OK) allocated=vec_reserve(&diagnostics->records,max_records);
  if (allocated!=STL_OK) {
    vec_destroy(&diagnostics->records);
    return diagnostics_error(error,TURBODB_STATUS_OUT_OF_MEMORY,
        "allocate SQL diagnostic records");
  }
  diagnostics->max_records=max_records;
  return TURBODB_STATUS_OK;
}

turbodb_status_t orm_sql_diagnostics_reset(orm_sql_diagnostics *diagnostics,
    turbodb_error_t *error) {
  if (!diagnostics || !diagnostics->max_records)
    return diagnostics_error(error,TURBODB_STATUS_INVALID_ARGUMENT,
        "diagnostics are not initialized");
  if (vec_clear(&diagnostics->records)!=STL_OK)
    return diagnostics_error(error,TURBODB_STATUS_INTERNAL_ERROR,
        "clear SQL diagnostic records");
  diagnostics->total=0;
  return TURBODB_STATUS_OK;
}

void orm_sql_diagnostics_destroy(orm_sql_diagnostics *diagnostics) {
  if (!diagnostics) return;
  vec_destroy(&diagnostics->records);
  *diagnostics=(orm_sql_diagnostics){0};
}

turbodb_status_t orm_sql_diagnostics_add(orm_sql_diagnostics *diagnostics,
    uint32_t code, const char *message, turbodb_error_t *error) {
  if (!diagnostics) return TURBODB_STATUS_OK;
  if (!diagnostics->max_records || !code || !message)
    return diagnostics_error(error,TURBODB_STATUS_INVALID_ARGUMENT,
        "invalid SQL diagnostic");
  if (diagnostics->total==UINT64_MAX)
    return diagnostics_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,
        "SQL diagnostic count overflow");
  ++diagnostics->total;
  if (vec_size(&diagnostics->records)==diagnostics->max_records)
    return TURBODB_STATUS_OK;
  orm_sql_diagnostic record={.code=code};
  (void)snprintf(record.message,sizeof(record.message),"%s",message);
  if (vec_push(&diagnostics->records,&record)!=STL_OK)
    return diagnostics_error(error,TURBODB_STATUS_INTERNAL_ERROR,
        "append preallocated SQL diagnostic");
  return TURBODB_STATUS_OK;
}

const orm_sql_diagnostic *orm_sql_diagnostics_at(
    const orm_sql_diagnostics *diagnostics, size_t index) {
  return diagnostics && index<vec_size(&diagnostics->records) ?
      vec_at_const(&diagnostics->records,index) : NULL;
}

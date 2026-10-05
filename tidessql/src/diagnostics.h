#ifndef ORM_TIDESDB_SQL_DIAGNOSTICS_H
#define ORM_TIDESDB_SQL_DIAGNOSTICS_H

#include "error.h"
#include "session.h"
#include <cstl/vec.h>

enum { ORM_SQL_DIAGNOSTIC_MESSAGE_CAPACITY = 256 };

typedef struct orm_sql_diagnostic {
  uint32_t code;
  char message[ORM_SQL_DIAGNOSTIC_MESSAGE_CAPACITY];
} orm_sql_diagnostic;

/* Connection-owned diagnostics for the most recent statement. The record
 * vector is fully reserved during init, so statement execution never allocates
 * while recording warnings. total counts conditions beyond the retained
 * max_records, matching MySQL max_error_count behavior. Single connection
 * owner; readers may borrow records only while no new statement can begin. */
typedef struct orm_sql_diagnostics {
  vec_t records;
  uint64_t total;
  size_t max_records;
} orm_sql_diagnostics;

typedef enum orm_sql_evaluation_mode {
  ORM_SQL_EVALUATION_QUERY, ORM_SQL_EVALUATION_WRITE, ORM_SQL_EVALUATION_IGNORE_WRITE
} orm_sql_evaluation_mode;
/* Immutable statement policy copied into execution owners. diagnostics borrows
 * the connection's preallocated receiver until every consumer closes. No reset
 * while consumers live; session is a by-value admission snapshot derived from
 * connection defaults. Single synchronous owner, no allocation in evaluation. */
typedef struct orm_sql_evaluation {
  orm_sql_diagnostics *diagnostics;
  orm_sql_evaluation_mode mode;
  orm_sql_session_snapshot session;
} orm_sql_evaluation;

enum { ORM_SQL_DIAGNOSTIC_DIVISION_BY_ZERO = 1365,
  ORM_SQL_DIAGNOSTIC_CAST_TRUNCATED = 1292, ORM_SQL_DIAGNOSTIC_CAST_COMPLEMENT = 1105 };
/* Strict writes fail; query/IGNORE record the condition and permit NULL.
 * NULL receiver only omits observation. Failed recording must stop evaluation. */
turbodb_status_t orm_sql_evaluation_division_by_zero(orm_sql_evaluation evaluation,
    turbodb_error_t *error);
/* condition is a bitset of orm_sql_cast_condition. Strict WRITE promotes only
 * truncation; complement remains a warning. Recording errors propagate. */
turbodb_status_t orm_sql_evaluation_cast(orm_sql_evaluation evaluation,
    unsigned condition, bool unsigned_target, turbodb_error_t *error);

turbodb_status_t orm_sql_diagnostics_init(orm_sql_diagnostics *diagnostics,
    size_t max_records, turbodb_error_t *error);
turbodb_status_t orm_sql_diagnostics_reset(orm_sql_diagnostics *diagnostics,
    turbodb_error_t *error);
void orm_sql_diagnostics_destroy(orm_sql_diagnostics *diagnostics);
turbodb_status_t orm_sql_diagnostics_add(orm_sql_diagnostics *diagnostics,
    uint32_t code, const char *message, turbodb_error_t *error);
const orm_sql_diagnostic *orm_sql_diagnostics_at(
    const orm_sql_diagnostics *diagnostics, size_t index);

#endif

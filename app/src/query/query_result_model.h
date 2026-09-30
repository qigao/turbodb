#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <orm.h>

namespace turbodb::app {

enum class QueryOutcomeKind {
  rows,
  command,
  error,
};

struct QueryCell {
  orm_value_kind_t kind = ORM_VALUE_NULL;
  std::int64_t int64_value = 0;
  std::uint64_t uint64_value = 0;
  double double_value = 0.0;
  bool boolean_value = false;
  std::string bytes;
};

struct QueryResultLimits {
  std::uint64_t max_rows = ORM_C_DEFAULT_MAX_RESULT_ROWS;
  std::uint64_t max_columns = ORM_C_DEFAULT_MAX_COLUMNS;
  std::uint64_t max_bytes = ORM_C_DEFAULT_MAX_RESULT_BYTES;
};

struct QueryResultSnapshot {
  std::uint64_t request_id = 0;
  QueryOutcomeKind kind = QueryOutcomeKind::error;
  orm_status_t status = ORM_STATUS_OK;
  std::string message;
  std::uint64_t elapsed_microseconds = 0;
  std::uint64_t affected_rows = 0;
  std::uint64_t payload_bytes = 0;
  bool cancel_supported = false;
  std::vector<std::string> columns;
  std::vector<std::vector<QueryCell>> rows;
};

bool CopyOrmResult(orm_result_t* result, std::uint64_t request_id,
                   std::uint64_t elapsed_microseconds,
                   const QueryResultLimits& limits,
                   QueryResultSnapshot* out, std::string* error);

}  // namespace turbodb::app

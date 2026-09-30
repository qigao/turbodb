#include "query/query_result_model.h"

#include <cstring>
#include <limits>
#include <sstream>

namespace turbodb::app {

namespace {

bool AddBytes(QueryResultSnapshot& result, std::uint64_t bytes,
              const QueryResultLimits& limits, std::string* error) {
  if (result.payload_bytes > limits.max_bytes ||
      bytes > limits.max_bytes - result.payload_bytes) {
    if (error != nullptr) *error = "app result snapshot exceeds byte limit";
    return false;
  }
  result.payload_bytes += bytes;
  return true;
}

std::string ViewToString(orm_string_view_t value) {
  if (value.data == nullptr || value.len == 0u) return {};
  return std::string(static_cast<const char*>(value.data), value.len);
}

bool CopyCell(orm_result_t* result, std::uint64_t row, std::uint64_t column,
              const QueryResultLimits& limits, QueryResultSnapshot& output,
              QueryCell* out_cell, std::string* error) {
  orm_error_t orm_error;
  orm_error_init(&orm_error);
  orm_value_kind_t kind = ORM_VALUE_NULL;
  if (orm_result_value_kind(result, row, column, &kind, &orm_error) !=
      ORM_STATUS_OK) {
    if (error != nullptr) *error = orm_error.message;
    return false;
  }

  QueryCell cell;
  cell.kind = kind;
  switch (kind) {
    case ORM_VALUE_NULL:
      break;
    case ORM_VALUE_INT64:
      if (orm_result_get_int64(result, row, column, &cell.int64_value,
                               &orm_error) != ORM_STATUS_OK)
        goto fail;
      if (!AddBytes(output, sizeof(cell.int64_value), limits, error))
        return false;
      break;
    case ORM_VALUE_UINT64:
      if (orm_result_get_uint64(result, row, column, &cell.uint64_value,
                                &orm_error) != ORM_STATUS_OK)
        goto fail;
      if (!AddBytes(output, sizeof(cell.uint64_value), limits, error))
        return false;
      break;
    case ORM_VALUE_DOUBLE:
      if (orm_result_get_double(result, row, column, &cell.double_value,
                                &orm_error) != ORM_STATUS_OK)
        goto fail;
      if (!AddBytes(output, sizeof(cell.double_value), limits, error))
        return false;
      break;
    case ORM_VALUE_BOOLEAN: {
      std::uint8_t value = 0u;
      if (orm_result_get_boolean(result, row, column, &value, &orm_error) !=
          ORM_STATUS_OK)
        goto fail;
      cell.boolean_value = value != 0u;
      if (!AddBytes(output, sizeof(value), limits, error)) return false;
      break;
    }
    case ORM_VALUE_TEXT: {
      orm_string_view_t value{};
      if (orm_result_get_text(result, row, column, &value, &orm_error) !=
          ORM_STATUS_OK)
        goto fail;
      if (!AddBytes(output, value.len, limits, error)) return false;
      cell.bytes = ViewToString(value);
      break;
    }
    case ORM_VALUE_BLOB: {
      orm_blob_t value{};
      if (orm_result_get_blob(result, row, column, &value, &orm_error) !=
          ORM_STATUS_OK)
        goto fail;
      if (!AddBytes(output, value.size, limits, error)) return false;
      if (value.size != 0u && value.data == nullptr) {
        if (error != nullptr) *error = "ORM returned an invalid BLOB view";
        return false;
      }
      cell.bytes.assign(static_cast<const char*>(value.data), value.size);
      break;
    }
    default:
      if (error != nullptr) *error = "ORM returned an unknown value kind";
      return false;
  }
  *out_cell = std::move(cell);
  return true;

fail:
  if (error != nullptr) *error = orm_error.message;
  return false;
}

}  // namespace

bool CopyOrmResult(orm_result_t* result, std::uint64_t request_id,
                   std::uint64_t elapsed_microseconds,
                   const QueryResultLimits& limits,
                   QueryResultSnapshot* out, std::string* error) {
  if (result == nullptr || out == nullptr || limits.max_columns == 0u ||
      limits.max_rows == 0u || limits.max_bytes == 0u) {
    if (error != nullptr) *error = "invalid app result snapshot request";
    return false;
  }

  orm_error_t orm_error;
  orm_error_init(&orm_error);
  std::uint64_t rows = 0u;
  std::uint64_t columns = 0u;
  std::uint64_t affected = 0u;
  if (orm_result_row_count(result, &rows, &orm_error) != ORM_STATUS_OK ||
      orm_result_column_count(result, &columns, &orm_error) != ORM_STATUS_OK ||
      orm_result_affected_rows(result, &affected, &orm_error) !=
          ORM_STATUS_OK) {
    if (error != nullptr) *error = orm_error.message;
    return false;
  }
  if (rows > limits.max_rows || columns > limits.max_columns ||
      rows > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
      columns > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    if (error != nullptr) *error = "app result snapshot exceeds row/column limit";
    return false;
  }

  QueryResultSnapshot snapshot;
  snapshot.request_id = request_id;
  snapshot.status = ORM_STATUS_OK;
  snapshot.elapsed_microseconds = elapsed_microseconds;
  snapshot.affected_rows = affected;
  snapshot.cancel_supported = false;
  snapshot.kind = columns != 0u || rows != 0u ? QueryOutcomeKind::rows
                                              : QueryOutcomeKind::command;

  if (snapshot.kind == QueryOutcomeKind::rows) {
    snapshot.columns.reserve(static_cast<std::size_t>(columns));
    for (std::uint64_t column = 0u; column < columns; ++column) {
      orm_string_view_t name{};
      const orm_status_t status =
          orm_result_column_name(result, column, &name, &orm_error);
      std::string owned;
      if (status == ORM_STATUS_OK) {
        if (!AddBytes(snapshot, name.len, limits, error)) return false;
        owned = ViewToString(name);
      } else if (status == ORM_STATUS_UNSUPPORTED) {
        owned = "Column " + std::to_string(column + 1u);
        if (!AddBytes(snapshot, owned.size(), limits, error)) return false;
      } else {
        if (error != nullptr) *error = orm_error.message;
        return false;
      }
      snapshot.columns.push_back(std::move(owned));
    }

    snapshot.rows.reserve(static_cast<std::size_t>(rows));
    for (std::uint64_t row = 0u; row < rows; ++row) {
      std::vector<QueryCell> output_row;
      output_row.reserve(static_cast<std::size_t>(columns));
      for (std::uint64_t column = 0u; column < columns; ++column) {
        QueryCell cell;
        if (!CopyCell(result, row, column, limits, snapshot, &cell, error))
          return false;
        output_row.push_back(std::move(cell));
      }
      snapshot.rows.push_back(std::move(output_row));
    }
  }

  *out = std::move(snapshot);
  return true;
}

}  // namespace turbodb::app

#include "row_reader.h"

#include <stdio.h>
#include <string.h>

enum { MYSQL_BINARY_CHARSET = 63u };

static int mysql_row_reader_is_blob_type(uint8_t type) {
  return type == MYSQL_FIELD_TYPE_TINY_BLOB ||
         type == MYSQL_FIELD_TYPE_MEDIUM_BLOB ||
         type == MYSQL_FIELD_TYPE_LONG_BLOB ||
         type == MYSQL_FIELD_TYPE_BLOB;
}

static int mysql_row_reader_is_string_type(uint8_t type) {
  return type == MYSQL_FIELD_TYPE_DECIMAL ||
         type == MYSQL_FIELD_TYPE_VARCHAR ||
         type == MYSQL_FIELD_TYPE_JSON ||
         type == MYSQL_FIELD_TYPE_NEWDECIMAL ||
         type == MYSQL_FIELD_TYPE_ENUM ||
         type == MYSQL_FIELD_TYPE_SET ||
         type == MYSQL_FIELD_TYPE_VAR_STRING ||
         type == MYSQL_FIELD_TYPE_STRING;
}

static cserde_status mysql_row_reader_emit_bytes(
    const mysql_column_definition_t *column,
    const mysql_binary_value_t *value,
    cserde_token *out) {
  if (column == NULL || value == NULL || out == NULL ||
      value->kind != MYSQL_BINARY_VALUE_BYTES ||
      value->data.bytes.is_null ||
      (value->data.bytes.data == NULL &&
       value->data.bytes.length != 0u))
    return CSERDE_SOURCE_ERROR;

  if (column->type == MYSQL_FIELD_TYPE_BIT ||
      column->type == MYSQL_FIELD_TYPE_GEOMETRY ||
      (mysql_row_reader_is_blob_type(column->type) &&
       column->character_set == MYSQL_BINARY_CHARSET)) {
    out->kind = CSERDE_BYTES;
  } else if (mysql_row_reader_is_string_type(column->type) ||
             mysql_row_reader_is_blob_type(column->type)) {
    out->kind = CSERDE_STRING;
  } else {
    return CSERDE_UNSUPPORTED;
  }

  out->value.slice.data = value->data.bytes.data;
  out->value.slice.size = value->data.bytes.length;
  out->value.slice.lifetime = CSERDE_VIEW_TRANSIENT;
  return CSERDE_OK;
}

static cserde_status mysql_row_reader_format_datetime(
    mysql_row_reader_state *state,
    const mysql_binary_value_t *value,
    cserde_token *out) {
  const mysql_binary_datetime_t *time;
  int length;

  if (state == NULL || value == NULL || out == NULL ||
      value->kind != MYSQL_BINARY_VALUE_DATETIME)
    return CSERDE_SOURCE_ERROR;
  time = &value->data.datetime_value;

  if (value->mysql_type == MYSQL_FIELD_TYPE_DATE) {
    length = snprintf(
        (char *)state->temporal, sizeof(state->temporal),
        "%04u-%02u-%02u",
        (unsigned int)time->year,
        (unsigned int)time->month,
        (unsigned int)time->day);
  } else if (value->mysql_type == MYSQL_FIELD_TYPE_DATETIME ||
             value->mysql_type == MYSQL_FIELD_TYPE_TIMESTAMP) {
    if (time->wire_length == 11u) {
      length = snprintf(
          (char *)state->temporal, sizeof(state->temporal),
          "%04u-%02u-%02u %02u:%02u:%02u.%06u",
          (unsigned int)time->year,
          (unsigned int)time->month,
          (unsigned int)time->day,
          (unsigned int)time->hour,
          (unsigned int)time->minute,
          (unsigned int)time->second,
          (unsigned int)time->microsecond);
    } else {
      length = snprintf(
          (char *)state->temporal, sizeof(state->temporal),
          "%04u-%02u-%02u %02u:%02u:%02u",
          (unsigned int)time->year,
          (unsigned int)time->month,
          (unsigned int)time->day,
          (unsigned int)time->hour,
          (unsigned int)time->minute,
          (unsigned int)time->second);
    }
  } else {
    return CSERDE_UNSUPPORTED;
  }

  if (length < 0 || (size_t)length >= sizeof(state->temporal))
    return CSERDE_SOURCE_ERROR;
  state->temporal_size = (size_t)length;
  out->kind = CSERDE_STRING;
  out->value.slice.data = state->temporal;
  out->value.slice.size = state->temporal_size;
  out->value.slice.lifetime = CSERDE_VIEW_TRANSIENT;
  return CSERDE_OK;
}

static cserde_status mysql_row_reader_format_time(
    mysql_row_reader_state *state,
    const mysql_binary_value_t *value,
    cserde_token *out) {
  const mysql_binary_time_t *time;
  unsigned long long hours;
  int length;

  if (state == NULL || value == NULL || out == NULL ||
      value->kind != MYSQL_BINARY_VALUE_TIME ||
      value->mysql_type != MYSQL_FIELD_TYPE_TIME)
    return CSERDE_SOURCE_ERROR;

  time = &value->data.time_value;
  hours = (unsigned long long)time->days * 24ull +
          (unsigned long long)time->hour;

  if (time->wire_length == 12u) {
    length = snprintf(
        (char *)state->temporal, sizeof(state->temporal),
        "%s%02llu:%02u:%02u.%06u",
        time->negative ? "-" : "",
        hours,
        (unsigned int)time->minute,
        (unsigned int)time->second,
        (unsigned int)time->microsecond);
  } else {
    length = snprintf(
        (char *)state->temporal, sizeof(state->temporal),
        "%s%02llu:%02u:%02u",
        time->negative ? "-" : "",
        hours,
        (unsigned int)time->minute,
        (unsigned int)time->second);
  }

  if (length < 0 || (size_t)length >= sizeof(state->temporal))
    return CSERDE_SOURCE_ERROR;
  state->temporal_size = (size_t)length;
  out->kind = CSERDE_STRING;
  out->value.slice.data = state->temporal;
  out->value.slice.size = state->temporal_size;
  out->value.slice.lifetime = CSERDE_VIEW_TRANSIENT;
  return CSERDE_OK;
}

static cserde_status mysql_row_reader_emit_value(
    mysql_row_reader_state *state,
    const mysql_column_definition_t *column,
    const mysql_binary_value_t *value,
    cserde_token *out) {
  if (state == NULL || column == NULL || value == NULL || out == NULL)
    return CSERDE_INVALID_ARGUMENT;

  switch (value->kind) {
    case MYSQL_BINARY_VALUE_NULL:
      out->kind = CSERDE_NULL;
      return CSERDE_OK;
    case MYSQL_BINARY_VALUE_SINT64:
      out->kind = CSERDE_SINT;
      out->value.sint = value->data.sint64_value;
      return CSERDE_OK;
    case MYSQL_BINARY_VALUE_UINT64:
      out->kind = CSERDE_UINT;
      out->value.uint = value->data.uint64_value;
      return CSERDE_OK;
    case MYSQL_BINARY_VALUE_DOUBLE:
      out->kind = CSERDE_FLOAT;
      out->value.floating = value->data.double_value;
      return CSERDE_OK;
    case MYSQL_BINARY_VALUE_BYTES:
      return mysql_row_reader_emit_bytes(column, value, out);
    case MYSQL_BINARY_VALUE_DATETIME:
      return mysql_row_reader_format_datetime(state, value, out);
    case MYSQL_BINARY_VALUE_TIME:
      return mysql_row_reader_format_time(state, value, out);
    default:
      return CSERDE_UNSUPPORTED;
  }
}

static cserde_status mysql_row_reader_next(void *context, cserde_token *out) {
  mysql_row_reader_state *state = (mysql_row_reader_state *)context;

  if (state == NULL || out == NULL)
    return CSERDE_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));

  switch (state->phase) {
    case MYSQL_ROW_READER_MAP_BEGIN:
      out->kind = CSERDE_MAP_BEGIN;
      state->phase = state->column_count == 0u
                         ? MYSQL_ROW_READER_MAP_END
                         : MYSQL_ROW_READER_KEY;
      return CSERDE_OK;

    case MYSQL_ROW_READER_KEY: {
      const mysql_wire_bytes_t name = state->columns[state->column].name;
      if (name.is_null ||
          (name.data == NULL && name.length != 0u))
        return CSERDE_SOURCE_ERROR;
      out->kind = CSERDE_STRING;
      out->value.slice.data = name.data;
      out->value.slice.size = name.length;
      out->value.slice.lifetime = CSERDE_VIEW_TRANSIENT;
      state->phase = MYSQL_ROW_READER_VALUE;
      return CSERDE_OK;
    }

    case MYSQL_ROW_READER_VALUE: {
      const cserde_status status = mysql_row_reader_emit_value(
          state, &state->columns[state->column],
          &state->values[state->column], out);
      if (status != CSERDE_OK)
        return status;
      ++state->column;
      state->phase = state->column == state->column_count
                         ? MYSQL_ROW_READER_MAP_END
                         : MYSQL_ROW_READER_KEY;
      return CSERDE_OK;
    }

    case MYSQL_ROW_READER_MAP_END:
      out->kind = CSERDE_MAP_END;
      state->phase = MYSQL_ROW_READER_DONE;
      return CSERDE_OK;

    case MYSQL_ROW_READER_DONE:
      return CSERDE_DONE;

    default:
      return CSERDE_INVALID_STATE;
  }
}

static const cserde_reader_ops mysql_row_reader_ops = {
    sizeof(cserde_reader_ops),
    CSERDE_READER_OPS_ABI_VERSION,
    mysql_row_reader_next};

cserde_status mysql_row_reader_init(
    mysql_row_reader_state *state,
    const mysql_column_definition_t *columns,
    const mysql_binary_value_t *values,
    size_t column_count,
    cserde_reader *out_reader) {
  if (state == NULL || out_reader == NULL ||
      (columns == NULL && column_count != 0u) ||
      (values == NULL && column_count != 0u))
    return CSERDE_INVALID_ARGUMENT;

  memset(state, 0, sizeof(*state));
  state->columns = columns;
  state->values = values;
  state->column_count = column_count;
  state->phase = MYSQL_ROW_READER_MAP_BEGIN;
  return cserde_reader_init(out_reader, &mysql_row_reader_ops, state);
}

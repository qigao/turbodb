#include "parameters.h"

#include <limits.h>
#include <string.h>

typedef enum mysql_parameter_lex_state_t {
  MYSQL_PARAMETER_LEX_CODE = 0,
  MYSQL_PARAMETER_LEX_SINGLE_QUOTE,
  MYSQL_PARAMETER_LEX_DOUBLE_QUOTE,
  MYSQL_PARAMETER_LEX_BACKTICK,
  MYSQL_PARAMETER_LEX_LINE_DASH,
  MYSQL_PARAMETER_LEX_LINE_HASH,
  MYSQL_PARAMETER_LEX_BLOCK_COMMENT
} mysql_parameter_lex_state_t;

static bool mysql_parameter_is_space(uint8_t value) {
  return value == (uint8_t)' ' || value == (uint8_t)'\t' ||
         value == (uint8_t)'\r' || value == (uint8_t)'\n' ||
         value == (uint8_t)'\f';
}

static bool mysql_sql_is_alpha(uint8_t value) {
  return (value >= (uint8_t)'A' && value <= (uint8_t)'Z') ||
         (value >= (uint8_t)'a' && value <= (uint8_t)'z');
}

static uint8_t mysql_sql_upper(uint8_t value) {
  return value >= (uint8_t)'a' && value <= (uint8_t)'z'
             ? (uint8_t)(value - ((uint8_t)'a' - (uint8_t)'A'))
             : value;
}

static bool mysql_sql_keyword_equal(
    const uint8_t *sql, size_t begin, size_t end,
    const char *keyword) {
  size_t i = 0u;
  if (sql == NULL || keyword == NULL)
    return false;
  while (keyword[i] != '\0' && begin + i < end) {
    if (mysql_sql_upper(sql[begin + i]) !=
        (uint8_t)keyword[i])
      return false;
    ++i;
  }
  return keyword[i] == '\0' && begin + i == end;
}

bool mysql_sql_reject_managed_transaction(
    const uint8_t *sql, size_t sql_size) {
  static const char *const rejected[] = {
      "ALTER", "ANALYZE", "BEGIN", "COMMIT", "CREATE",
      "DROP", "FLUSH", "GRANT", "INSTALL", "LOCK",
      "OPTIMIZE", "RENAME", "REPAIR", "RESET", "REVOKE",
      "ROLLBACK", "SAVEPOINT", "START", "TRUNCATE",
      "UNINSTALL", "UNLOCK", "XA"};
  size_t cursor = 0u;
  size_t begin;
  size_t i;

  if (sql == NULL || sql_size == 0u)
    return false;

  for (;;) {
    while (cursor < sql_size &&
           mysql_parameter_is_space(sql[cursor]))
      ++cursor;
    if (cursor >= sql_size)
      return false;

    if (sql[cursor] == (uint8_t)'#') {
      while (cursor < sql_size &&
             sql[cursor] != (uint8_t)'\n' &&
             sql[cursor] != (uint8_t)'\r')
        ++cursor;
      continue;
    }

    if (cursor + 1u < sql_size &&
        sql[cursor] == (uint8_t)'-' &&
        sql[cursor + 1u] == (uint8_t)'-' &&
        (cursor + 2u == sql_size ||
         mysql_parameter_is_space(sql[cursor + 2u]))) {
      cursor += 2u;
      while (cursor < sql_size &&
             sql[cursor] != (uint8_t)'\n' &&
             sql[cursor] != (uint8_t)'\r')
        ++cursor;
      continue;
    }

    if (cursor + 1u < sql_size &&
        sql[cursor] == (uint8_t)'/' &&
        sql[cursor + 1u] == (uint8_t)'*') {
      size_t end = cursor + 2u;
      if (end < sql_size && sql[end] == (uint8_t)'!')
        return true;
      while (end + 1u < sql_size &&
             !(sql[end] == (uint8_t)'*' &&
               sql[end + 1u] == (uint8_t)'/'))
        ++end;
      if (end + 1u >= sql_size)
        return true;
      cursor = end + 2u;
      continue;
    }
    break;
  }

  begin = cursor;
  while (cursor < sql_size && mysql_sql_is_alpha(sql[cursor]))
    ++cursor;
  if (cursor == begin)
    return false;

  for (i = 0u; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
    if (mysql_sql_keyword_equal(
            sql, begin, cursor, rejected[i]))
      return true;
  }
  return false;
}

static mysql_wire_status_t mysql_parameter_emit(
    uint8_t *out, size_t capacity, size_t *used,
    const uint8_t *data, size_t size) {
  if (out == NULL || used == NULL || (data == NULL && size != 0u))
    return MYSQL_WIRE_STATUS_INVALID;
  if (*used > capacity || capacity - *used < size)
    return MYSQL_WIRE_STATUS_LIMIT;
  if (size != 0u)
    memcpy(out + *used, data, size);
  *used += size;
  return MYSQL_WIRE_STATUS_OK;
}

static mysql_wire_status_t mysql_parameter_record(
    mysql_parameter_style_t style, uint32_t index,
    const mysql_parameter_lower_options_t *options,
    uint32_t *bind_order, size_t bind_order_capacity,
    mysql_parameter_lower_result_t *result) {
  if (options == NULL || bind_order == NULL || result == NULL ||
      options->max_parameters == 0u || index == 0u ||
      index > options->max_parameters)
    return MYSQL_WIRE_STATUS_INVALID;

  if (result->style != MYSQL_PARAMETER_STYLE_NONE &&
      result->style != style)
    return MYSQL_WIRE_STATUS_INVALID;

  if (result->bind_count >= bind_order_capacity ||
      result->bind_count >= (size_t)options->max_parameters)
    return MYSQL_WIRE_STATUS_LIMIT;

  result->style = style;
  bind_order[result->bind_count++] = index;
  return MYSQL_WIRE_STATUS_OK;
}

static mysql_wire_status_t mysql_parameter_parse_portable_index(
    const uint8_t *sql, size_t sql_size, size_t start,
    uint32_t max_parameters, uint32_t *index, size_t *end) {
  size_t cursor = start;
  uint64_t value = 0u;

  if (sql == NULL || index == NULL || end == NULL ||
      start >= sql_size || sql[start] < (uint8_t)'0' ||
      sql[start] > (uint8_t)'9')
    return MYSQL_WIRE_STATUS_INVALID;

  while (cursor < sql_size &&
         sql[cursor] >= (uint8_t)'0' &&
         sql[cursor] <= (uint8_t)'9') {
    const uint32_t digit = (uint32_t)(sql[cursor] - (uint8_t)'0');
    if (value > (UINT64_MAX - digit) / 10u)
      return MYSQL_WIRE_STATUS_LIMIT;
    value = value * 10u + digit;
    ++cursor;
  }

  if (value == 0u || value > UINT32_MAX ||
      value > (uint64_t)max_parameters)
    return MYSQL_WIRE_STATUS_LIMIT;

  *index = (uint32_t)value;
  *end = cursor;
  return MYSQL_WIRE_STATUS_OK;
}

mysql_wire_status_t mysql_parameter_lower(
    const uint8_t *sql, size_t sql_size,
    const mysql_parameter_lower_options_t *options,
    uint8_t *out_sql, size_t out_capacity, size_t *out_size,
    uint32_t *bind_order, size_t bind_order_capacity,
    mysql_parameter_lower_result_t *result) {
  mysql_parameter_lex_state_t state = MYSQL_PARAMETER_LEX_CODE;
  bool executable_comment = false;
  size_t input = 0u;
  size_t output = 0u;

  if (sql == NULL || options == NULL || out_sql == NULL ||
      out_size == NULL || bind_order == NULL || result == NULL ||
      options->max_parameters == 0u)
    return MYSQL_WIRE_STATUS_INVALID;

  *out_size = 0u;
  memset(result, 0, sizeof(*result));

  while (input < sql_size) {
    const uint8_t ch = sql[input];

    if (state == MYSQL_PARAMETER_LEX_LINE_DASH ||
        state == MYSQL_PARAMETER_LEX_LINE_HASH) {
      mysql_wire_status_t status =
          mysql_parameter_emit(out_sql, out_capacity, &output, &ch, 1u);
      if (status != MYSQL_WIRE_STATUS_OK)
        return status;
      ++input;
      if (ch == (uint8_t)'\n' || ch == (uint8_t)'\r')
        state = MYSQL_PARAMETER_LEX_CODE;
      continue;
    }

    if (state == MYSQL_PARAMETER_LEX_BLOCK_COMMENT) {
      if (input + 1u < sql_size &&
          sql[input] == (uint8_t)'*' &&
          sql[input + 1u] == (uint8_t)'/') {
        mysql_wire_status_t status =
            mysql_parameter_emit(out_sql, out_capacity, &output,
                                 sql + input, 2u);
        if (status != MYSQL_WIRE_STATUS_OK)
          return status;
        input += 2u;
        state = MYSQL_PARAMETER_LEX_CODE;
        continue;
      }
      {
        mysql_wire_status_t status =
            mysql_parameter_emit(out_sql, out_capacity, &output, &ch, 1u);
        if (status != MYSQL_WIRE_STATUS_OK)
          return status;
      }
      ++input;
      continue;
    }

    if (state == MYSQL_PARAMETER_LEX_SINGLE_QUOTE ||
        state == MYSQL_PARAMETER_LEX_DOUBLE_QUOTE ||
        state == MYSQL_PARAMETER_LEX_BACKTICK) {
      const uint8_t terminator =
          state == MYSQL_PARAMETER_LEX_SINGLE_QUOTE
              ? (uint8_t)'\''
              : state == MYSQL_PARAMETER_LEX_DOUBLE_QUOTE
                    ? (uint8_t)'"'
                    : (uint8_t)'`';

      if (ch == terminator) {
        if (input + 1u < sql_size && sql[input + 1u] == terminator) {
          mysql_wire_status_t status =
              mysql_parameter_emit(out_sql, out_capacity, &output,
                                   sql + input, 2u);
          if (status != MYSQL_WIRE_STATUS_OK)
            return status;
          input += 2u;
          continue;
        }
        {
          mysql_wire_status_t status =
              mysql_parameter_emit(out_sql, out_capacity, &output, &ch, 1u);
          if (status != MYSQL_WIRE_STATUS_OK)
            return status;
        }
        ++input;
        state = MYSQL_PARAMETER_LEX_CODE;
        continue;
      }

      if (ch == (uint8_t)'\\' &&
          state != MYSQL_PARAMETER_LEX_BACKTICK &&
          !options->no_backslash_escapes) {
        const size_t copy = input + 1u < sql_size ? 2u : 1u;
        mysql_wire_status_t status =
            mysql_parameter_emit(out_sql, out_capacity, &output,
                                 sql + input, copy);
        if (status != MYSQL_WIRE_STATUS_OK)
          return status;
        input += copy;
        continue;
      }

      {
        mysql_wire_status_t status =
            mysql_parameter_emit(out_sql, out_capacity, &output, &ch, 1u);
        if (status != MYSQL_WIRE_STATUS_OK)
          return status;
      }
      ++input;
      continue;
    }

    if (executable_comment && input + 1u < sql_size &&
        sql[input] == (uint8_t)'*' && sql[input + 1u] == (uint8_t)'/') {
      mysql_wire_status_t status =
          mysql_parameter_emit(out_sql, out_capacity, &output,
                               sql + input, 2u);
      if (status != MYSQL_WIRE_STATUS_OK)
        return status;
      input += 2u;
      executable_comment = false;
      continue;
    }

    if (input + 2u < sql_size &&
        sql[input] == (uint8_t)'/' &&
        sql[input + 1u] == (uint8_t)'*' &&
        sql[input + 2u] == (uint8_t)'!') {
      mysql_wire_status_t status =
          mysql_parameter_emit(out_sql, out_capacity, &output,
                               sql + input, 3u);
      if (status != MYSQL_WIRE_STATUS_OK)
        return status;
      input += 3u;
      executable_comment = true;
      continue;
    }

    if (input + 1u < sql_size &&
        sql[input] == (uint8_t)'/' &&
        sql[input + 1u] == (uint8_t)'*') {
      mysql_wire_status_t status =
          mysql_parameter_emit(out_sql, out_capacity, &output,
                               sql + input, 2u);
      if (status != MYSQL_WIRE_STATUS_OK)
        return status;
      input += 2u;
      state = MYSQL_PARAMETER_LEX_BLOCK_COMMENT;
      continue;
    }

    if (ch == (uint8_t)'#') {
      mysql_wire_status_t status =
          mysql_parameter_emit(out_sql, out_capacity, &output, &ch, 1u);
      if (status != MYSQL_WIRE_STATUS_OK)
        return status;
      ++input;
      state = MYSQL_PARAMETER_LEX_LINE_HASH;
      continue;
    }

    if (ch == (uint8_t)'-' && input + 1u < sql_size &&
        sql[input + 1u] == (uint8_t)'-' &&
        (input + 2u == sql_size ||
         mysql_parameter_is_space(sql[input + 2u]))) {
      mysql_wire_status_t status =
          mysql_parameter_emit(out_sql, out_capacity, &output,
                               sql + input, 2u);
      if (status != MYSQL_WIRE_STATUS_OK)
        return status;
      input += 2u;
      state = MYSQL_PARAMETER_LEX_LINE_DASH;
      continue;
    }

    if (ch == (uint8_t)'\'') {
      mysql_wire_status_t status =
          mysql_parameter_emit(out_sql, out_capacity, &output, &ch, 1u);
      if (status != MYSQL_WIRE_STATUS_OK)
        return status;
      ++input;
      state = MYSQL_PARAMETER_LEX_SINGLE_QUOTE;
      continue;
    }

    if (ch == (uint8_t)'"') {
      mysql_wire_status_t status =
          mysql_parameter_emit(out_sql, out_capacity, &output, &ch, 1u);
      if (status != MYSQL_WIRE_STATUS_OK)
        return status;
      ++input;
      state = MYSQL_PARAMETER_LEX_DOUBLE_QUOTE;
      continue;
    }

    if (ch == (uint8_t)'`') {
      mysql_wire_status_t status =
          mysql_parameter_emit(out_sql, out_capacity, &output, &ch, 1u);
      if (status != MYSQL_WIRE_STATUS_OK)
        return status;
      ++input;
      state = MYSQL_PARAMETER_LEX_BACKTICK;
      continue;
    }

    if (ch == (uint8_t)'?') {
      mysql_wire_status_t status;
      uint32_t index;

      if (input + 1u < sql_size &&
          sql[input + 1u] >= (uint8_t)'0' &&
          sql[input + 1u] <= (uint8_t)'9') {
        size_t end = input + 1u;
        status = mysql_parameter_parse_portable_index(
            sql, sql_size, input + 1u, options->max_parameters,
            &index, &end);
        if (status != MYSQL_WIRE_STATUS_OK)
          return status;
        status = mysql_parameter_record(
            MYSQL_PARAMETER_STYLE_PORTABLE, index, options,
            bind_order, bind_order_capacity, result);
        if (status != MYSQL_WIRE_STATUS_OK)
          return status;
        status = mysql_parameter_emit(
            out_sql, out_capacity, &output,
            (const uint8_t *)"?", 1u);
        if (status != MYSQL_WIRE_STATUS_OK)
          return status;
        input = end;
        continue;
      }

      if (result->bind_count >= UINT32_MAX)
        return MYSQL_WIRE_STATUS_LIMIT;
      index = (uint32_t)result->bind_count + 1u;
      status = mysql_parameter_record(
          MYSQL_PARAMETER_STYLE_NATIVE, index, options,
          bind_order, bind_order_capacity, result);
      if (status != MYSQL_WIRE_STATUS_OK)
        return status;
      status = mysql_parameter_emit(
          out_sql, out_capacity, &output, &ch, 1u);
      if (status != MYSQL_WIRE_STATUS_OK)
        return status;
      ++input;
      continue;
    }

    {
      mysql_wire_status_t status =
          mysql_parameter_emit(out_sql, out_capacity, &output, &ch, 1u);
      if (status != MYSQL_WIRE_STATUS_OK)
        return status;
    }
    ++input;
  }

  if (state == MYSQL_PARAMETER_LEX_SINGLE_QUOTE ||
      state == MYSQL_PARAMETER_LEX_DOUBLE_QUOTE ||
      state == MYSQL_PARAMETER_LEX_BACKTICK ||
      state == MYSQL_PARAMETER_LEX_BLOCK_COMMENT ||
      executable_comment)
    return MYSQL_WIRE_STATUS_INVALID;

  *out_size = output;
  return MYSQL_WIRE_STATUS_OK;
}

#include "parameters.h"

#include <core/tstr.h>
#include <simd.h>
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

enum { MYSQL_PARAMETER_SCALAR_SPAN_BYTES = 8u };

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
      "ALTER", "ANALYZE", "BEGIN", "CACHE", "CHANGE",
      "CHECK", "COMMIT", "CREATE", "DROP", "FLUSH",
      "GRANT", "INSTALL", "LOAD", "LOCK", "OPTIMIZE",
      "RENAME", "REPAIR", "RESET", "REVOKE", "ROLLBACK",
      "SAVEPOINT", "SET", "START", "STOP", "TRUNCATE",
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
  /* Removed parameter indices can put output inside the following input span. */
  if (size != 0u)
    memmove(out + *used, data, size);
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

static bool mysql_parameter_code_delimiter(unsigned char ch) {
  return ch == '\'' || ch == '"' || ch == '`' || ch == '#' ||
         ch == '-' || ch == '/' || ch == '*' || ch == '?';
}

/* Count ordinary code bytes in O(n) time/O(1) space. Inputs are borrowed,
 * unaligned and counted; only complete 16-byte blocks reach simd_v128_load.
 * Folding bit 4 lets '/' and '?' share a nibble entry. Any extra candidates
 * (such as 'p' for '`') are resolved by the lexical predicate, never skipped. */
static bool mysql_parameter_code_span(vstr text, size_t *length) {
  static const v128 low_nibble = {{
      15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15}};
  static const v128 fold = {{
      16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16}};
  static const v128 candidates = {{
      0x70, 0xff, 0x32, 0x33, 0xff, 0xff, 0xff, 0x37,
      0xff, 0xff, 0x3a, 0xff, 0xff, 0x3d, 0xff, 0x3f}};
  size_t offset = 0u;

  while (text.len - offset >= sizeof(v128)) {
    v128 bytes, indices, folded, expected, matches;
    uint32_t mask;
    simd_v128_load(&bytes, text.data + offset);
    if (!simd_binary(&simd_u8x16, SALTS_SIMD_BINARY_AND,
                     &indices, &bytes, &low_nibble) ||
        !simd_binary(&simd_u8x16, SALTS_SIMD_BINARY_OR,
                     &folded, &bytes, &fold) ||
        !simd_swizzle_bytes(&expected, &candidates, &indices) ||
        !simd_compare(&simd_u8x16, SALTS_SIMD_COMPARE_EQ,
                      &matches, &folded, &expected) ||
        !simd_reduce(&simd_u8x16, SALTS_SIMD_REDUCE_BITMASK, &matches, &mask))
      return false;
    if (mask != 0u) {
      for (size_t lane = 0u; lane < sizeof(v128); ++lane) {
        if ((mask & (UINT32_C(1) << lane)) != 0u &&
            mysql_parameter_code_delimiter((unsigned char)text.data[offset + lane])) {
          *length = offset + lane;
          return true;
        }
      }
    }
    offset += sizeof(v128);
  }
  while (offset < text.len &&
         !mysql_parameter_code_delimiter((unsigned char)text.data[offset]))
    ++offset;
  *length = offset;
  return true;
}

static size_t mysql_parameter_literal_span(vstr text, char a, char b) {
  size_t prefix = 0u;
  while (prefix < text.len && prefix < MYSQL_PARAMETER_SCALAR_SPAN_BYTES) {
    if (text.data[prefix] == a || text.data[prefix] == b) return prefix;
    ++prefix;
  }
  if (prefix == text.len) return prefix;
  const char delimiters[] = {a, b};
  const vstr remaining = vstr_from_buf(text.data + prefix, text.len - prefix);
  const size_t end = vstr_find_any(remaining, vstr_from_buf(delimiters, 2u));
  return end == VSTR_NPOS ? text.len : prefix + end;
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
    const vstr pending = vstr_from_buf((const char *)sql + input, sql_size - input);

    if (state == MYSQL_PARAMETER_LEX_CODE && !mysql_parameter_code_delimiter(ch)) {
      size_t length = 1u;
      /* Dense markers and short identifiers finish before vector setup. */
      while (length < pending.len && length < MYSQL_PARAMETER_SCALAR_SPAN_BYTES &&
             !mysql_parameter_code_delimiter((unsigned char)pending.data[length]))
        ++length;
      if (length == MYSQL_PARAMETER_SCALAR_SPAN_BYTES &&
          pending.len - length >= sizeof(v128)) {
        size_t rest;
        if (!mysql_parameter_code_span(
                vstr_from_buf(pending.data + length, pending.len - length), &rest))
          return MYSQL_WIRE_STATUS_INVALID;
        length += rest;
      }
      const mysql_wire_status_t status = mysql_parameter_emit(
          out_sql, out_capacity, &output, sql + input, length);
      if (status != MYSQL_WIRE_STATUS_OK) return status;
      input += length;
      continue;
    }

    if (state == MYSQL_PARAMETER_LEX_LINE_DASH ||
        state == MYSQL_PARAMETER_LEX_LINE_HASH) {
      const size_t end = mysql_parameter_literal_span(pending, '\r', '\n');
      const size_t length = end == pending.len ? end : end + 1u;
      mysql_wire_status_t status =
          mysql_parameter_emit(out_sql, out_capacity, &output, sql + input, length);
      if (status != MYSQL_WIRE_STATUS_OK)
        return status;
      input += length;
      if (end != pending.len)
        state = MYSQL_PARAMETER_LEX_CODE;
      continue;
    }

    if (state == MYSQL_PARAMETER_LEX_BLOCK_COMMENT) {
      const size_t length = mysql_parameter_literal_span(pending, '*', '*');
      if (length != 0u) {
        const mysql_wire_status_t status = mysql_parameter_emit(
            out_sql, out_capacity, &output, sql + input, length);
        if (status != MYSQL_WIRE_STATUS_OK) return status;
        input += length;
        continue;
      }
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
      const char escape = state != MYSQL_PARAMETER_LEX_BACKTICK &&
                                  !options->no_backslash_escapes ? '\\' : (char)terminator;
      const size_t length = mysql_parameter_literal_span(pending, (char)terminator, escape);
      if (length != 0u) {
        const mysql_wire_status_t status = mysql_parameter_emit(
            out_sql, out_capacity, &output, sql + input, length);
        if (status != MYSQL_WIRE_STATUS_OK) return status;
        input += length;
        continue;
      }

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

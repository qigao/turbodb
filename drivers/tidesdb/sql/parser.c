#include "sql.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { SQL_IDENTIFIER_BYTES = 63u, SQL_DECIMAL_BASE = 10u };

typedef struct sql_parser {
  const orm_query_plan *raw;
  const orm_limits *limits;
  orm_query_plan *plan;
  orm_error_t *error;
  const char *cursor;
  const char *end;
  orm_tidesdb_sql_token token;
  size_t parameter;
  tstr literal;
} sql_parser;

static orm_status_t sql_error(sql_parser *p, orm_status_t status, const char *reason) {
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message), "TidesDB SQL at byte %zu: %s",
      (size_t)(p->token.text.data - p->raw->raw_sql), reason);
  orm_error_set(p->error, status, message);
  return status;
}

static void advance(sql_parser *p) {
  p->token = orm_tidesdb_sql_next(&p->cursor, p->end);
}

static bool keyword(const sql_parser *p, const char *word) {
  if (p->token.kind != TDB_SQL_WORD || p->token.text.len != strlen(word))
    return false;
  for (size_t i = 0; i < p->token.text.len; ++i) {
    unsigned char c = (unsigned char)p->token.text.data[i];
    if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
    if (c != (unsigned char)word[i]) return false;
  }
  return true;
}

static orm_status_t expect(sql_parser *p, int kind) {
  if (p->token.kind != kind)
    return sql_error(p, ORM_STATUS_SQL_ERROR, "unexpected token");
  advance(p);
  return ORM_STATUS_OK;
}

static orm_status_t expect_word(sql_parser *p, const char *word) {
  if (!keyword(p, word)) return sql_error(p, ORM_STATUS_SQL_ERROR, word);
  advance(p);
  return ORM_STATUS_OK;
}

static orm_status_t identifier(sql_parser *p, vstr *out) {
  static const char *const reserved[] = {
      "select", "insert", "update", "delete", "from", "into", "values", "set",
      "where", "and", "or", "not", "is", "null", "true", "false", "limit",
      "offset", "order", "by", "group", "having", "join", "as", "distinct",
      "returning", "union", "with", "case", "when", "then", "else", "end"};
  if (p->token.kind != TDB_SQL_WORD)
    return sql_error(p, p->token.kind == TDB_SQL_INVALID ? ORM_STATUS_SQL_ERROR : ORM_STATUS_UNSUPPORTED,
                     "expected an unqualified identifier");
  if (p->token.text.len > SQL_IDENTIFIER_BYTES)
    return sql_error(p, ORM_STATUS_LIMIT_EXCEEDED, "identifier is too long");
  for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i)
    if (keyword(p, reserved[i]))
      return sql_error(p, ORM_STATUS_UNSUPPORTED, "keyword cannot be used as an identifier");
  *out = p->token.text;
  advance(p);
  return ORM_STATUS_OK;
}

static orm_status_t table(sql_parser *p) {
  vstr name;
  orm_status_t status = identifier(p, &name);
  if (status != ORM_STATUS_OK) return status;
  tstr owned = tstr_dup_len(name.data, name.len);
  if (owned == NULL) return sql_error(p, ORM_STATUS_OUT_OF_MEMORY, "copy table name");
  tstr_freep(&p->plan->table);
  p->plan->table = owned;
  return ORM_STATUS_OK;
}

static orm_value_t bound_value(const orm_owned_value *value) {
  switch (value->kind) {
    case ORM_VALUE_INT64: return orm_i64(value->data.int64_value);
    case ORM_VALUE_UINT64: return orm_u64(value->data.uint64_value);
    case ORM_VALUE_DOUBLE: return orm_f64(value->data.double_value);
    case ORM_VALUE_BOOLEAN: return orm_bool(value->data.boolean_value);
    case ORM_VALUE_TEXT: return orm_text_v(tstr_to_v(value->bytes));
    case ORM_VALUE_BLOB: return orm_blob(value->bytes, tstr_len(value->bytes));
    default: return orm_null();
  }
}

static orm_status_t integer(sql_parser *p, orm_value_t *out) {
  const vstr text = p->token.text;
  const bool negative = text.data[0] == '-';
  const uint64_t maximum = negative ? (uint64_t)INT64_MAX + 1u : UINT64_MAX;
  uint64_t value = 0;
  for (size_t i = negative ? 1u : 0u; i < text.len; ++i) {
    const unsigned digit = (unsigned)(text.data[i] - '0');
    if (value > (maximum - digit) / SQL_DECIMAL_BASE)
      return sql_error(p, ORM_STATUS_LIMIT_EXCEEDED, "integer is out of range");
    value = value * SQL_DECIMAL_BASE + digit;
  }
  *out = negative ? orm_i64(value == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)value)
                 : value <= INT64_MAX ? orm_i64((int64_t)value) : orm_u64(value);
  return ORM_STATUS_OK;
}

static orm_status_t value(sql_parser *p, orm_value_t *out) {
  orm_status_t status = ORM_STATUS_OK;
  tstr_freep(&p->literal);
  if (p->token.kind == '?') {
    const orm_owned_value *bound = vec_at_const(&p->raw->raw_parameters, p->parameter);
    if (bound == NULL)
      return sql_error(p, ORM_STATUS_INVALID_ARGUMENT, "missing positional parameter");
    if (bound->kind < ORM_VALUE_NULL || bound->kind > ORM_VALUE_BLOB)
      return sql_error(p, ORM_STATUS_INVALID_ARGUMENT, "invalid parameter kind");
    *out = bound_value(bound);
    ++p->parameter;
  } else if (p->token.kind == TDB_SQL_INTEGER) {
    status = integer(p, out);
  } else if (p->token.kind == TDB_SQL_STRING) {
    const vstr text = p->token.text;
    size_t size = 0;
    for (size_t i = 1; i + 1 < text.len; ++i) {
      if (text.data[i] == '\'') ++i;
      ++size;
    }
    if (size > p->limits->max_parameter_bytes - p->plan->parameter_bytes)
      return sql_error(p, ORM_STATUS_LIMIT_EXCEEDED, "string literal exceeds parameter budget");
    p->literal = tstr_new_len(NULL, size);
    if (p->literal == NULL)
      return sql_error(p, ORM_STATUS_OUT_OF_MEMORY, "copy string literal");
    size = 0;
    for (size_t i = 1; i + 1 < text.len; ++i) {
      p->literal[size++] = text.data[i];
      if (text.data[i] == '\'') ++i;
    }
    *out = orm_text_v(tstr_to_v(p->literal));
  } else if (keyword(p, "null")) {
    *out = orm_null();
  } else if (keyword(p, "true") || keyword(p, "false")) {
    *out = orm_bool(keyword(p, "true"));
  } else {
    return sql_error(p, p->token.kind == TDB_SQL_INVALID ? ORM_STATUS_SQL_ERROR : ORM_STATUS_UNSUPPORTED,
                     "expected ?, integer, string, boolean or NULL");
  }
  if (status != ORM_STATUS_OK) return status;
  if (out->kind == ORM_VALUE_DOUBLE && !isfinite(out->data.double_value))
    return sql_error(p, ORM_STATUS_INVALID_ARGUMENT, "non-finite numeric parameter");
  advance(p);
  return ORM_STATUS_OK;
}

static orm_status_t columns(sql_parser *p) {
  for (;;) {
    vstr name;
    orm_status_t status = identifier(p, &name);
    if (status != ORM_STATUS_OK) return status;
    for (size_t i = 0; i < vec_size(&p->plan->columns); ++i) {
      const tstr *existing = vec_at_const(&p->plan->columns, i);
      if (tstr_eq_v(*existing, name))
        return sql_error(p, ORM_STATUS_SQL_ERROR, "duplicate column");
    }
    status = orm_plan_add_column(p->plan, name, p->limits, p->error);
    if (status != ORM_STATUS_OK) return status;
    if (p->token.kind != ',') return ORM_STATUS_OK;
    advance(p);
  }
}

static orm_status_t assignment(sql_parser *p, vstr name) {
  orm_value_t input;
  for (size_t i = 0; i < vec_size(&p->plan->assignments); ++i) {
    const orm_assignment *existing = vec_at_const(&p->plan->assignments, i);
    if (tstr_eq_v(existing->column, name))
      return sql_error(p, ORM_STATUS_SQL_ERROR, "duplicate assignment");
  }
  orm_status_t status = value(p, &input);
  if (status == ORM_STATUS_OK)
    status = orm_plan_add_assignment(p->plan, name, input, p->limits, p->error);
  return status;
}

static orm_status_t insert(sql_parser *p) {
  orm_status_t status = expect_word(p, "into");
  if (status == ORM_STATUS_OK) status = table(p);
  if (status == ORM_STATUS_OK) status = expect(p, '(');
  if (status == ORM_STATUS_OK) status = columns(p);
  if (status == ORM_STATUS_OK) status = expect(p, ')');
  if (status == ORM_STATUS_OK) status = expect_word(p, "values");
  if (status == ORM_STATUS_OK) status = expect(p, '(');
  p->plan->kind = ORM_QUERY_INSERT;
  for (size_t i = 0; status == ORM_STATUS_OK && i < vec_size(&p->plan->columns); ++i) {
    const tstr *name = vec_at_const(&p->plan->columns, i);
    if (i != 0) status = expect(p, ',');
    if (status == ORM_STATUS_OK) status = assignment(p, tstr_to_v(*name));
  }
  if (status == ORM_STATUS_OK) status = expect(p, ')');
  return status;
}

static orm_status_t update(sql_parser *p) {
  orm_status_t status = table(p);
  if (status == ORM_STATUS_OK) status = expect_word(p, "set");
  while (status == ORM_STATUS_OK) {
    vstr name;
    status = identifier(p, &name);
    if (status == ORM_STATUS_OK) status = expect(p, '=');
    if (status == ORM_STATUS_OK) status = assignment(p, name);
    if (status != ORM_STATUS_OK || p->token.kind != ',') break;
    advance(p);
  }
  return status;
}

static orm_status_t predicate(sql_parser *p) {
  vstr name;
  orm_value_t input = orm_null();
  orm_compare_t comparison = ORM_COMPARE_EQUAL;
  orm_status_t status = identifier(p, &name);
  if (status != ORM_STATUS_OK) return status;
  if (keyword(p, "is")) {
    advance(p);
    if (keyword(p, "not")) { comparison = ORM_COMPARE_NOT_EQUAL; advance(p); }
    status = expect_word(p, "null");
  } else {
    switch (p->token.kind) {
      case '=': comparison = ORM_COMPARE_EQUAL; break;
      case TDB_SQL_NE: comparison = ORM_COMPARE_NOT_EQUAL; break;
      case '<': comparison = ORM_COMPARE_LESS; break;
      case TDB_SQL_LE: comparison = ORM_COMPARE_LESS_EQUAL; break;
      case '>': comparison = ORM_COMPARE_GREATER; break;
      case TDB_SQL_GE: comparison = ORM_COMPARE_GREATER_EQUAL; break;
      default: return sql_error(p, ORM_STATUS_UNSUPPORTED, "unsupported comparison");
    }
    advance(p);
    status = value(p, &input);
    if (status == ORM_STATUS_OK && input.kind == ORM_VALUE_NULL)
      return sql_error(p, ORM_STATUS_UNSUPPORTED, "NULL comparisons require IS NULL or IS NOT NULL");
  }
  if (status == ORM_STATUS_OK)
    status = orm_plan_add_predicate(p->plan, name, comparison, input, p->limits, p->error);
  return status;
}

static orm_status_t pagination(sql_parser *p, uint64_t *out) {
  orm_value_t input;
  advance(p);
  orm_status_t status = value(p, &input);
  if (status != ORM_STATUS_OK) return status;
  if (input.kind == ORM_VALUE_UINT64) *out = input.data.uint64_value;
  else if (input.kind == ORM_VALUE_INT64 && input.data.int64_value >= 0)
    *out = (uint64_t)input.data.int64_value;
  else return sql_error(p, ORM_STATUS_INVALID_ARGUMENT, "pagination requires a nonnegative integer");
  return ORM_STATUS_OK;
}

static orm_status_t statement(sql_parser *p) {
  orm_status_t status;
  if (keyword(p, "select")) {
    advance(p);
    status = columns(p);
    if (status == ORM_STATUS_OK) status = expect_word(p, "from");
    if (status == ORM_STATUS_OK) status = table(p);
  } else if (keyword(p, "insert")) {
    advance(p);
    return insert(p);
  } else if (keyword(p, "update")) {
    p->plan->kind = ORM_QUERY_UPDATE;
    advance(p);
    status = update(p);
  } else if (keyword(p, "delete")) {
    p->plan->kind = ORM_QUERY_DELETE;
    advance(p);
    status = expect_word(p, "from");
    if (status == ORM_STATUS_OK) status = table(p);
  } else return sql_error(p, p->token.kind == TDB_SQL_INVALID ? ORM_STATUS_SQL_ERROR : ORM_STATUS_UNSUPPORTED,
                         "expected SELECT, INSERT, UPDATE or DELETE");
  if (status == ORM_STATUS_OK && keyword(p, "where")) {
    do {
      advance(p);
      status = predicate(p);
    } while (status == ORM_STATUS_OK && keyword(p, "and"));
  }
  if (status == ORM_STATUS_OK && p->plan->kind == ORM_QUERY_SELECT && keyword(p, "limit")) {
    status = pagination(p, &p->plan->limit);
    p->plan->has_limit = true;
    if (status == ORM_STATUS_OK && p->plan->limit > p->limits->max_result_rows)
      status = sql_error(p, ORM_STATUS_LIMIT_EXCEEDED, "LIMIT exceeds max_result_rows");
    if (status == ORM_STATUS_OK && keyword(p, "offset")) {
      status = pagination(p, &p->plan->offset);
      p->plan->has_offset = true;
    }
  }
  return status;
}

orm_status_t orm_tidesdb_sql_parse(const orm_query_plan *raw,
    const orm_limits *limits, orm_query_plan *out, orm_error_t *error) {
  if (out == NULL || raw == NULL || raw == out || limits == NULL ||
      raw->kind != ORM_QUERY_RAW || raw->raw_sql == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT, "invalid TidesDB SQL input");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  memset(out, 0, sizeof(*out));
  const size_t size = tstr_len(raw->raw_sql);
  if (size > limits->max_query_bytes || vec_size(&raw->raw_parameters) > limits->max_parameters) {
    orm_error_set(error, ORM_STATUS_LIMIT_EXCEEDED, "TidesDB SQL exceeds query/parameter budget");
    return ORM_STATUS_LIMIT_EXCEEDED;
  }
  if (size == 0 || memchr(raw->raw_sql, 0, size) != NULL) {
    orm_error_set(error, ORM_STATUS_SQL_ERROR, "TidesDB SQL is empty or contains NUL");
    return ORM_STATUS_SQL_ERROR;
  }
  sql_parser parser = {.raw = raw, .limits = limits, .plan = out, .error = error,
      .cursor = raw->raw_sql, .end = raw->raw_sql + size};
  advance(&parser);
  orm_status_t status = orm_plan_init(out, ORM_QUERY_SELECT, orm_view("_sql"), limits, error);
  if (status == ORM_STATUS_OK) status = statement(&parser);
  if (status == ORM_STATUS_OK && parser.token.kind == ';') advance(&parser);
  if (status == ORM_STATUS_OK && parser.token.kind != TDB_SQL_END)
    status = sql_error(&parser, parser.token.kind == TDB_SQL_INVALID ? ORM_STATUS_SQL_ERROR : ORM_STATUS_UNSUPPORTED,
                       "trailing syntax or multiple statements");
  if (status == ORM_STATUS_OK && parser.parameter != vec_size(&raw->raw_parameters))
    status = sql_error(&parser, ORM_STATUS_INVALID_ARGUMENT, "unused positional parameter");
  tstr_freep(&parser.literal);
  if (status != ORM_STATUS_OK) orm_plan_destroy(out);
  return status;
}

#include "value.h"
#include "error.h"

#include <float.h>
#include <math.h>
#include <string.h>

_Static_assert(FLT_RADIX==2 && FLT_MANT_DIG==ORM_SQL_CAST_FLOAT_PRECISION &&
    sizeof(float)==sizeof(uint32_t),"MySQL FLOAT CAST requires binary32 float");

static turbodb_status_t value_error(turbodb_error_t *error, turbodb_status_t status,
                                const char *reason) {
  tdsql_error_set(error, status, reason);
  return status;
}

turbodb_status_t orm_tidesdb_sql_integer_literal(vstr text, bool negative,
    turbodb_value_t *out, turbodb_error_t *error) {
  enum { DECIMAL_BASE = 10 };
  if (!out || !text.data || !text.len)
    return value_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid integer literal");
  const uint64_t maximum = negative ? (uint64_t)INT64_MAX + 1u : UINT64_MAX;
  uint64_t number = 0;
  for (size_t i = 0; i < text.len; ++i) {
    if (text.data[i] < '0' || text.data[i] > '9')
      return value_error(error, TURBODB_STATUS_UNSUPPORTED, "only decimal integer literals are supported");
    const unsigned digit = (unsigned)(text.data[i] - '0');
    if (number > (maximum - digit) / DECIMAL_BASE)
      return value_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "integer is out of range");
    number = number * DECIMAL_BASE + digit;
  }
  *out = negative ? turbodb_i64(number == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)number)
                 : number <= INT64_MAX ? turbodb_i64((int64_t)number) : turbodb_u64(number);
  return TURBODB_STATUS_OK;
}

static void decimal_exponent_add(int64_t *exponent, int64_t delta) {
  if (delta > 0 && *exponent > INT64_MAX - delta)
    *exponent = INT64_MAX;
  else if (delta < 0 && *exponent < INT64_MIN - delta)
    *exponent = INT64_MIN;
  else
    *exponent += delta;
}

static turbodb_status_t decimal_literal(vstr text, bool negative,
    turbodb_value_t *out, turbodb_error_t *error) {
  enum { DECIMAL_BASE = 10, DECIMAL_SIGNIFICANT_DIGITS = 19,
    DECIMAL_EXPONENT_LIMIT = 1000000, DECIMAL_SCALE_SEGMENTS = 2 };
  if (!out || !text.data || !text.len)
    return value_error(error,TURBODB_STATUS_INVALID_ARGUMENT,
        "invalid numeric literal");
  /* The retained digits fit U64; round to floating only after accumulation. */
  uint64_t significand = 0;
  int64_t decimal_exponent = 0;
  size_t stored = 0, i = 0;
  bool point = false, digit = false;
  for (; i < text.len && text.data[i] != 'e' && text.data[i] != 'E'; ++i) {
    const char c = text.data[i];
    if (c == '.' && !point) { point = true; continue; }
    if (c < '0' || c > '9')
      return value_error(error,TURBODB_STATUS_INVALID_ARGUMENT,
          "invalid decimal numeric literal");
    digit = true;
    const unsigned value = (unsigned)(c - '0');
    if (!stored && value == 0) {
      if (point) decimal_exponent_add(&decimal_exponent,-1);
      continue;
    }
    if (stored < DECIMAL_SIGNIFICANT_DIGITS) {
      significand = significand * DECIMAL_BASE + value;
      ++stored;
      if (point) decimal_exponent_add(&decimal_exponent,-1);
    } else if (!point) {
      decimal_exponent_add(&decimal_exponent,1);
    }
  }
  if (!digit)
    return value_error(error,TURBODB_STATUS_INVALID_ARGUMENT,
        "invalid decimal numeric literal");
  if (i < text.len) {
    ++i; bool exponent_negative = false;
    if (i < text.len && (text.data[i] == '+' || text.data[i] == '-'))
      exponent_negative = text.data[i++] == '-';
    if (i == text.len)
      return value_error(error,TURBODB_STATUS_INVALID_ARGUMENT,
          "invalid decimal exponent");
    int64_t exponent = 0;
    for (; i < text.len; ++i) {
      const char c = text.data[i];
      if (c < '0' || c > '9')
        return value_error(error,TURBODB_STATUS_INVALID_ARGUMENT,
            "invalid decimal exponent");
      const unsigned value = (unsigned)(c - '0');
      if (exponent < DECIMAL_EXPONENT_LIMIT)
        exponent = exponent > (DECIMAL_EXPONENT_LIMIT - (int64_t)value) /
            DECIMAL_BASE ? DECIMAL_EXPONENT_LIMIT :
            exponent * DECIMAL_BASE + (int64_t)value;
    }
    decimal_exponent_add(&decimal_exponent,
        exponent_negative ? -exponent : exponent);
  }
  if (significand != 0 &&
      decimal_exponent > LDBL_MAX_10_EXP + DECIMAL_SIGNIFICANT_DIGITS)
    return value_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,
        "floating-point literal is out of range");
  long double scaled = 0.0L;
  if (significand != 0 && decimal_exponent >=
      LDBL_MIN_10_EXP - DECIMAL_SCALE_SEGMENTS * DECIMAL_SIGNIFICANT_DIGITS) {
    scaled = (long double)significand;
    /* A tiny power may underflow before multiplication by the retained digits.
     * Split at a normal power so finite subnormal results survive on MSVC,
     * where long double has the same range as double. The residual is bounded. */
    if (decimal_exponent < LDBL_MIN_10_EXP) {
      scaled *= powl(10.0L, (long double)LDBL_MIN_10_EXP);
      decimal_exponent -= LDBL_MIN_10_EXP;
    }
    scaled *= powl(10.0L, (long double)decimal_exponent);
  }
  const double value = (double)scaled;
  if (!isfinite(value))
    return value_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,
        "floating-point literal is out of range");
  *out = turbodb_f64(negative ? -value : value);
  return TURBODB_STATUS_OK;
}

turbodb_status_t orm_tidesdb_sql_number_literal(vstr text, bool negative,
    turbodb_value_t *out, turbodb_error_t *error) {
  if (!out || !text.data || !text.len)
    return value_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
        "invalid numeric literal");
  for (size_t i = 0; i < text.len; ++i)
    if (text.data[i] == '.' || text.data[i] == 'e' || text.data[i] == 'E')
      return decimal_literal(text, negative, out, error);
  return orm_tidesdb_sql_integer_literal(text, negative, out, error);
}

static bool assignment_numeric(turbodb_value_kind_t kind) {
  return kind == TURBODB_VALUE_INT64 || kind == TURBODB_VALUE_UINT64 ||
      kind == TURBODB_VALUE_DOUBLE;
}

bool orm_tidesdb_sql_assignment_compatible(orm_sql_type target,
    orm_sql_type source) {
  if (!assignment_numeric(target.kind)) return target.kind == source.kind ||
      source.kind == TURBODB_VALUE_NULL;
  return source.kind == TURBODB_VALUE_NULL || assignment_numeric(source.kind) ||
      source.kind == TURBODB_VALUE_BOOLEAN || source.kind == TURBODB_VALUE_TEXT ||
      source.kind == TURBODB_VALUE_BLOB;
}

static bool assignment_space(unsigned char value) {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
      value == '\f' || value == '\v';
}

typedef struct assignment_number {
  uint64_t magnitude;
  size_t end;
  bool negative, digits, overflow, rounded;
} assignment_number;

static size_t assignment_trim_tail(const unsigned char *data, size_t size,
    size_t at) {
  while (at < size && assignment_space(data[at])) ++at;
  return at;
}

static size_t assignment_decimal_prefix(const unsigned char *data, size_t size,
    size_t *start, bool *negative, bool *digits) {
  size_t at = 0;
  while (at < size && assignment_space(data[at])) ++at;
  *negative = false;
  if (at < size && (data[at] == '+' || data[at] == '-'))
    *negative = data[at++] == '-';
  *start = at;
  bool before = false, after = false;
  while (at < size && data[at] >= '0' && data[at] <= '9') {
    before = true; ++at;
  }
  if (at < size && data[at] == '.') {
    ++at;
    while (at < size && data[at] >= '0' && data[at] <= '9') {
      after = true; ++at;
    }
  }
  *digits = before || after;
  if (*digits && at < size && (data[at] == 'e' || data[at] == 'E')) {
    size_t exponent = at + 1;
    if (exponent < size && (data[exponent] == '+' || data[exponent] == '-'))
      ++exponent;
    const size_t first = exponent;
    while (exponent < size && data[exponent] >= '0' &&
        data[exponent] <= '9') ++exponent;
    if (exponent != first) at = exponent;
  }
  return at;
}

/* BIGINT text stores round decimal fractions after exact base-ten scaling.
 * O(bytes) time/O(1) space; a large exponent never causes a large padding loop. */
static assignment_number assignment_integer_number(const unsigned char *data,
    size_t size) {
  enum { DECIMAL_BASE = 10, U64_DECIMAL_DIGITS = 20, ROUND_DIGIT = 5 };
  assignment_number parsed = {0};
  size_t start = 0;
  parsed.end = assignment_decimal_prefix(data, size, &start, &parsed.negative,
      &parsed.digits);
  if (!parsed.digits) return parsed;
  size_t at = start, digits = 0, integer_digits = 0, leading = SIZE_MAX;
  bool point = false;
  for (; at < parsed.end && data[at] != 'e' && data[at] != 'E'; ++at) {
    if (data[at] == '.') { point = true; continue; }
    if (leading == SIZE_MAX && data[at] != '0') leading = digits;
    ++digits;
    if (!point) ++integer_digits;
  }
  const size_t mantissa_end = at;
  if (leading == SIZE_MAX) return parsed;
  size_t exponent = 0; bool exponent_negative = false;
  if (at < parsed.end) {
    ++at;
    if (data[at] == '+' || data[at] == '-')
      exponent_negative = data[at++] == '-';
    for (; at < parsed.end; ++at) {
      const unsigned digit = (unsigned)(data[at] - '0');
      exponent = exponent > (SIZE_MAX - digit) / DECIMAL_BASE ? SIZE_MAX :
          exponent * DECIMAL_BASE + digit;
    }
  }
  if (exponent_negative && exponent > integer_digits) {
    parsed.rounded = true;
    return parsed;
  }
  if (!exponent_negative && exponent > SIZE_MAX - integer_digits) {
    parsed.overflow = true;
    return parsed;
  }
  const size_t cut = exponent_negative ? integer_digits - exponent :
      integer_digits + exponent;
  if (cut > leading && cut - leading > U64_DECIMAL_DIGITS) {
    parsed.overflow = true;
    return parsed;
  }
  size_t ordinal = 0; bool round_up = false;
  for (at = start; at < mantissa_end; ++at) {
    if (data[at] == '.') continue;
    const unsigned digit = (unsigned)(data[at] - '0');
    if (ordinal < cut) {
      if (parsed.magnitude > (UINT64_MAX - digit) / DECIMAL_BASE)
        parsed.overflow = true;
      else if (!parsed.overflow)
        parsed.magnitude = parsed.magnitude * DECIMAL_BASE + digit;
    } else {
      if (ordinal == cut) round_up = digit >= ROUND_DIGIT;
      parsed.rounded = parsed.rounded || digit != 0;
    }
    ++ordinal;
  }
  for (; ordinal < cut && !parsed.overflow; ++ordinal) {
    if (parsed.magnitude > UINT64_MAX / DECIMAL_BASE) parsed.overflow = true;
    else parsed.magnitude *= DECIMAL_BASE;
  }
  if (round_up && !parsed.overflow) {
    if (parsed.magnitude == UINT64_MAX) parsed.overflow = true;
    else ++parsed.magnitude;
  }
  return parsed;
}

static turbodb_status_t assignment_charge(orm_tidesdb_sql_budget *budget,
    uint64_t bytes, turbodb_error_t *error) {
  if (!budget) return value_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
      "column assignment requires a budget");
  if (bytes == UINT64_MAX) return value_error(error,
      TURBODB_STATUS_LIMIT_EXCEEDED, "column assignment work overflow");
  orm_sql_budget_amount amount = {0};
  amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = bytes + 1;
  return orm_tidesdb_sql_budget_reserve(budget, &amount, error);
}

static turbodb_status_t assignment_failure(bool permissive,
    orm_sql_assignment_adjustment reason, turbodb_value_t replacement,
    turbodb_value_t *out, orm_sql_assignment_adjustment *adjustment,
    turbodb_error_t *error) {
  if (permissive) {
    *out = replacement; *adjustment = reason; return TURBODB_STATUS_OK;
  }
  return value_error(error,
      reason == ORM_SQL_ASSIGNMENT_OUT_OF_RANGE ? TURBODB_STATUS_OUT_OF_RANGE :
      reason == ORM_SQL_ASSIGNMENT_NULL_TO_NOT_NULL ? TURBODB_STATUS_CONSTRAINT :
      TURBODB_STATUS_TYPE_ERROR,
      reason == ORM_SQL_ASSIGNMENT_OUT_OF_RANGE ?
        "column assignment value is out of range" :
      reason == ORM_SQL_ASSIGNMENT_NULL_TO_NOT_NULL ?
        "column assignment violates NOT NULL" :
        "column assignment value is invalid");
}

static turbodb_status_t assignment_i64_from_magnitude(assignment_number parsed,
    bool permissive, turbodb_value_t *out,
    orm_sql_assignment_adjustment *adjustment, turbodb_error_t *error) {
  const uint64_t negative_limit = (uint64_t)INT64_MAX + 1u;
  const bool range = parsed.overflow ||
      (parsed.negative ? parsed.magnitude > negative_limit :
                         parsed.magnitude > (uint64_t)INT64_MAX);
  if (range)
    return assignment_failure(permissive, ORM_SQL_ASSIGNMENT_OUT_OF_RANGE,
        turbodb_i64(parsed.negative ? INT64_MIN : INT64_MAX), out, adjustment,
        error);
  *out = turbodb_i64(parsed.negative ?
      (parsed.magnitude == negative_limit ? INT64_MIN :
       -(int64_t)parsed.magnitude) : (int64_t)parsed.magnitude);
  return TURBODB_STATUS_OK;
}

static turbodb_status_t assignment_u64_from_magnitude(assignment_number parsed,
    bool permissive, turbodb_value_t *out,
    orm_sql_assignment_adjustment *adjustment, turbodb_error_t *error) {
  const bool range = parsed.overflow ||
      (parsed.negative && parsed.magnitude != 0);
  if (range)
    return assignment_failure(permissive, ORM_SQL_ASSIGNMENT_OUT_OF_RANGE,
        turbodb_u64(parsed.negative ? 0 : UINT64_MAX), out, adjustment, error);
  *out = turbodb_u64(parsed.magnitude);
  return TURBODB_STATUS_OK;
}

static turbodb_status_t assignment_integer_text(orm_sql_type target,
    const unsigned char *data, size_t size, bool permissive,
    turbodb_value_t *out, orm_sql_assignment_adjustment *adjustment,
    turbodb_error_t *error) {
  assignment_number parsed = assignment_integer_number(data, size);
  if (!parsed.digits)
    return assignment_failure(permissive, ORM_SQL_ASSIGNMENT_INVALID,
        target.kind == TURBODB_VALUE_INT64 ? turbodb_i64(0) : turbodb_u64(0), out,
        adjustment, error);
  const size_t tail = assignment_trim_tail(data, size, parsed.end);
  if (tail != size && !permissive)
    return assignment_failure(false, ORM_SQL_ASSIGNMENT_INVALID, turbodb_null(),
        out, adjustment, error);
  turbodb_status_t status = target.kind == TURBODB_VALUE_INT64 ?
      assignment_i64_from_magnitude(parsed, permissive, out, adjustment,
          error) :
      assignment_u64_from_magnitude(parsed, permissive, out, adjustment,
          error);
  if (status == TURBODB_STATUS_OK && *adjustment == ORM_SQL_ASSIGNMENT_EXACT)
    *adjustment = tail != size ? ORM_SQL_ASSIGNMENT_TRUNCATED :
        parsed.rounded ? ORM_SQL_ASSIGNMENT_ROUNDED : ORM_SQL_ASSIGNMENT_EXACT;
  return status;
}

static turbodb_status_t assignment_double_text(const unsigned char *data,
    size_t size, bool permissive, turbodb_value_t *out,
    orm_sql_assignment_adjustment *adjustment, turbodb_error_t *error) {
  size_t start = 0; bool negative = false, digits = false;
  const size_t end = assignment_decimal_prefix(data, size, &start, &negative,
      &digits);
  if (!digits)
    return assignment_failure(permissive, ORM_SQL_ASSIGNMENT_INVALID,
        turbodb_f64(0.0), out, adjustment, error);
  const size_t tail = assignment_trim_tail(data, size, end);
  if (tail != size && !permissive)
    return assignment_failure(false, ORM_SQL_ASSIGNMENT_INVALID, turbodb_null(),
        out, adjustment, error);
  turbodb_value_t value = turbodb_null();
  turbodb_error_t cause; tdsql_error_init(&cause);
  turbodb_status_t status = decimal_literal(
      (vstr){(const char *)data + start, end - start}, negative, &value, &cause);
  if (status != TURBODB_STATUS_OK) {
    if (status != TURBODB_STATUS_LIMIT_EXCEEDED || !permissive) {
      tdsql_error_set(error, status == TURBODB_STATUS_LIMIT_EXCEEDED ?
          TURBODB_STATUS_OUT_OF_RANGE : status, cause.message);
      return status == TURBODB_STATUS_LIMIT_EXCEEDED ? TURBODB_STATUS_OUT_OF_RANGE :
          status;
    }
    value = turbodb_f64(negative ? -DBL_MAX : DBL_MAX);
    *adjustment = ORM_SQL_ASSIGNMENT_OUT_OF_RANGE;
  } else if (tail != size) {
    *adjustment = ORM_SQL_ASSIGNMENT_TRUNCATED;
  }
  *out = value;
  return TURBODB_STATUS_OK;
}

static turbodb_status_t assignment_from_double(orm_sql_type target, double input,
    bool permissive, turbodb_value_t *out,
    orm_sql_assignment_adjustment *adjustment, turbodb_error_t *error) {
  if (!isfinite(input))
    return assignment_failure(permissive, ORM_SQL_ASSIGNMENT_INVALID,
        target.kind == TURBODB_VALUE_INT64 ? turbodb_i64(0) :
        target.kind == TURBODB_VALUE_UINT64 ? turbodb_u64(0) : turbodb_f64(0.0),
        out, adjustment, error);
  if (target.kind == TURBODB_VALUE_DOUBLE) { *out = turbodb_f64(input); return TURBODB_STATUS_OK; }
  const double rounded = round(input);
  if (target.kind == TURBODB_VALUE_INT64) {
    if (rounded < -9223372036854775808.0 ||
        rounded >= 9223372036854775808.0)
      return assignment_failure(permissive, ORM_SQL_ASSIGNMENT_OUT_OF_RANGE,
          turbodb_i64(rounded < 0.0 ? INT64_MIN : INT64_MAX), out, adjustment,
          error);
    *out = turbodb_i64((int64_t)rounded);
  } else {
    if (rounded < 0.0 || rounded >= 18446744073709551616.0)
      return assignment_failure(permissive, ORM_SQL_ASSIGNMENT_OUT_OF_RANGE,
          turbodb_u64(rounded < 0.0 ? 0 : UINT64_MAX), out, adjustment, error);
    *out = turbodb_u64((uint64_t)rounded);
  }
  if (rounded != input) *adjustment = ORM_SQL_ASSIGNMENT_ROUNDED;
  return TURBODB_STATUS_OK;
}

turbodb_status_t orm_tidesdb_sql_assignment_convert(orm_sql_type target,
    const turbodb_value_t *input, bool permissive,
    orm_tidesdb_sql_budget *budget, turbodb_value_t *out,
    orm_sql_assignment_adjustment *adjustment, turbodb_error_t *error) {
  if (!input || !out || !adjustment || !assignment_numeric(target.kind))
    return value_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
        "invalid column assignment conversion");
  uint64_t bytes = 0;
  if (input->kind == TURBODB_VALUE_TEXT) bytes = input->data.text_value.len;
  else if (input->kind == TURBODB_VALUE_BLOB) bytes = input->data.blob_value.size;
  turbodb_status_t status = assignment_charge(budget, bytes, error);
  if (status != TURBODB_STATUS_OK) return status;
  turbodb_value_t converted = turbodb_null();
  orm_sql_assignment_adjustment changed = ORM_SQL_ASSIGNMENT_EXACT;
  if (input->kind == TURBODB_VALUE_NULL) {
    if (target.nullable) converted = turbodb_null();
    else return assignment_failure(permissive,
        ORM_SQL_ASSIGNMENT_NULL_TO_NOT_NULL,
        target.kind == TURBODB_VALUE_INT64 ? turbodb_i64(0) :
        target.kind == TURBODB_VALUE_UINT64 ? turbodb_u64(0) : turbodb_f64(0.0),
        out, adjustment, error);
  } else if (input->kind == TURBODB_VALUE_TEXT || input->kind == TURBODB_VALUE_BLOB) {
    const unsigned char *data = input->kind == TURBODB_VALUE_TEXT ?
        (const unsigned char *)input->data.text_value.data :
        (const unsigned char *)input->data.blob_value.data;
    const size_t size = input->kind == TURBODB_VALUE_TEXT ?
        input->data.text_value.len : input->data.blob_value.size;
    if (size && !data)
      return value_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
          "column assignment byte input is missing");
    status = target.kind == TURBODB_VALUE_DOUBLE ?
        assignment_double_text(data, size, permissive, &converted, &changed,
            error) :
        assignment_integer_text(target, data, size, permissive, &converted,
            &changed, error);
    if (status != TURBODB_STATUS_OK) return status;
  } else if (input->kind == TURBODB_VALUE_DOUBLE) {
    status = assignment_from_double(target, input->data.double_value,
        permissive, &converted, &changed, error);
    if (status != TURBODB_STATUS_OK) return status;
  } else if (input->kind == TURBODB_VALUE_BOOLEAN) {
    const uint64_t value = input->data.boolean_value != 0;
    converted = target.kind == TURBODB_VALUE_INT64 ? turbodb_i64((int64_t)value) :
        target.kind == TURBODB_VALUE_UINT64 ? turbodb_u64(value) :
        turbodb_f64((double)value);
  } else if (input->kind == TURBODB_VALUE_INT64) {
    const int64_t value = input->data.int64_value;
    if (target.kind == TURBODB_VALUE_INT64) converted = *input;
    else if (target.kind == TURBODB_VALUE_DOUBLE) converted = turbodb_f64((double)value);
    else if (value < 0)
      return assignment_failure(permissive, ORM_SQL_ASSIGNMENT_OUT_OF_RANGE,
          turbodb_u64(0), out, adjustment, error);
    else converted = turbodb_u64((uint64_t)value);
  } else if (input->kind == TURBODB_VALUE_UINT64) {
    const uint64_t value = input->data.uint64_value;
    if (target.kind == TURBODB_VALUE_UINT64) converted = *input;
    else if (target.kind == TURBODB_VALUE_DOUBLE) converted = turbodb_f64((double)value);
    else if (value > (uint64_t)INT64_MAX)
      return assignment_failure(permissive, ORM_SQL_ASSIGNMENT_OUT_OF_RANGE,
          turbodb_i64(INT64_MAX), out, adjustment, error);
    else converted = turbodb_i64((int64_t)value);
  } else {
    return assignment_failure(permissive, ORM_SQL_ASSIGNMENT_INVALID,
        target.kind == TURBODB_VALUE_INT64 ? turbodb_i64(0) :
        target.kind == TURBODB_VALUE_UINT64 ? turbodb_u64(0) : turbodb_f64(0.0),
        out, adjustment, error);
  }
  *out = converted;
  *adjustment = changed;
  return TURBODB_STATUS_OK;
}

static bool unary(orm_sql_predicate_op op) {
  return op == ORM_SQL_IS_NULL || op == ORM_SQL_IS_NOT_NULL || op == ORM_SQL_NOT;
}

static bool valid_type(orm_sql_type type) {
  return type.kind >= TURBODB_VALUE_NULL && type.kind <= TURBODB_VALUE_BLOB &&
         (type.kind != TURBODB_VALUE_NULL || type.nullable);
}

static bool integral(turbodb_value_kind_t kind) {
  return kind == TURBODB_VALUE_INT64 || kind == TURBODB_VALUE_UINT64;
}

static bool boolean_type(turbodb_value_kind_t kind) {
  return kind == TURBODB_VALUE_NULL || kind == TURBODB_VALUE_BOOLEAN;
}

turbodb_status_t orm_tidesdb_sql_predicate_bind(orm_sql_predicate_op op,
    orm_sql_type left, const orm_sql_type *right, orm_sql_predicate *out,
    turbodb_error_t *error) {
  if (op < ORM_SQL_EQUAL || op > ORM_SQL_OR)
    return value_error(error, TURBODB_STATUS_UNSUPPORTED,
                        "TidesDB SQL bind: unsupported scalar predicate");
  if (!out || (unary(op) ? right != NULL : right == NULL))
    return value_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                        "TidesDB SQL bind: invalid predicate output or arity");
  if (!valid_type(left) || (right && !valid_type(*right)))
    return value_error(error, TURBODB_STATUS_TYPE_ERROR,
                        "TidesDB SQL bind: invalid operand type or nullability");

  bool compatible = true;
  if (op == ORM_SQL_NOT || op == ORM_SQL_AND || op == ORM_SQL_OR)
    compatible = boolean_type(left.kind) && (!right || boolean_type(right->kind));
  else if (!unary(op))
    compatible = left.kind == TURBODB_VALUE_NULL || right->kind == TURBODB_VALUE_NULL ||
                 left.kind == right->kind ||
                 (assignment_numeric(left.kind) && assignment_numeric(right->kind));
  if (!compatible)
    return value_error(error, TURBODB_STATUS_UNSUPPORTED,
                        "TidesDB SQL bind: implicit operand conversion is unsupported");

  const bool nullable = op != ORM_SQL_NULL_SAFE_EQUAL &&
      op != ORM_SQL_IS_NULL && op != ORM_SQL_IS_NOT_NULL &&
      (left.nullable || (right && right->nullable));
  const orm_sql_predicate bound = {op, left,
      right ? *right : (orm_sql_type){TURBODB_VALUE_NULL, true},
      {TURBODB_VALUE_BOOLEAN, nullable},
      op <= ORM_SQL_NULL_SAFE_EQUAL && (left.kind == TURBODB_VALUE_DOUBLE ||
          (right && right->kind == TURBODB_VALUE_DOUBLE))};
  *out = bound;
  return TURBODB_STATUS_OK;
}

/* Shape checks never dereference payload; byte scans happen only after budget
 * admission. Byte ownership and accessible length remain the caller's contract. */
static bool valid_value(const turbodb_value_t *value, orm_sql_type type) {
  if (value->reserved) return false;
  if (value->kind == TURBODB_VALUE_NULL) return type.nullable;
  if (value->kind != type.kind) return false;
  switch (value->kind) {
    case TURBODB_VALUE_INT64: case TURBODB_VALUE_UINT64: return true;
    case TURBODB_VALUE_DOUBLE: return isfinite(value->data.double_value) != 0;
    case TURBODB_VALUE_BOOLEAN: return value->data.boolean_value <= 1;
    case TURBODB_VALUE_TEXT:
      return !value->data.text_value.len || value->data.text_value.data != NULL;
    case TURBODB_VALUE_BLOB:
      return !value->data.blob_value.size || value->data.blob_value.data != NULL;
    default: return false;
  }
}

static turbodb_blob_t bytes_of(const turbodb_value_t *value) {
  if (value->kind == TURBODB_VALUE_TEXT)
    return (turbodb_blob_t){value->data.text_value.data, value->data.text_value.len};
  if (value->kind == TURBODB_VALUE_BLOB) return value->data.blob_value;
  return (turbodb_blob_t){0};
}

static bool add_steps(uint64_t *steps, size_t bytes) {
  if ((uint64_t)bytes > UINT64_MAX - *steps) return false;
  *steps += (uint64_t)bytes;
  return true;
}

static turbodb_status_t charge_steps(const orm_sql_predicate *predicate,
    const turbodb_value_t *left, const turbodb_value_t *right,
    orm_tidesdb_sql_budget *budget, turbodb_error_t *error) {
  uint64_t steps = 1;
  const turbodb_blob_t a = bytes_of(left);
  const turbodb_blob_t b = right ? bytes_of(right) : (turbodb_blob_t){0};
  const size_t compared = right && predicate->op <= ORM_SQL_NULL_SAFE_EQUAL
      ? (a.size < b.size ? a.size : b.size) : 0;
  if ((left->kind == TURBODB_VALUE_TEXT && !add_steps(&steps, a.size)) ||
      (right && right->kind == TURBODB_VALUE_TEXT && !add_steps(&steps, b.size)) ||
      !add_steps(&steps, compared))
    return value_error(error, TURBODB_STATUS_LIMIT_EXCEEDED,
                        "TidesDB SQL eval: predicate step count overflows");
  orm_sql_budget_amount charge = {0};
  charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = steps;
  return orm_tidesdb_sql_budget_reserve(budget, &charge, error);
}

static bool valid_text(const turbodb_value_t *value) {
  return value->kind != TURBODB_VALUE_TEXT || !value->data.text_value.len ||
         vstr_utf8_valid(value->data.text_value);
}

turbodb_status_t orm_tidesdb_sql_cast_bind(orm_sql_type source,
    orm_sql_cast_target target, orm_sql_cast *out, turbodb_error_t *error) {
  if (!out) return value_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"CAST requires output");
  if (!valid_type(source)) return value_error(error,TURBODB_STATUS_TYPE_ERROR,"invalid CAST source type");
  if (target<ORM_SQL_CAST_SIGNED || target>ORM_SQL_CAST_FLOAT)
    return value_error(error,TURBODB_STATUS_UNSUPPORTED,"unsupported numeric CAST target");
  const turbodb_value_kind_t kind=target==ORM_SQL_CAST_SIGNED ? TURBODB_VALUE_INT64 :
      target==ORM_SQL_CAST_UNSIGNED ? TURBODB_VALUE_UINT64 : TURBODB_VALUE_DOUBLE;
  *out=(orm_sql_cast){source,{kind,source.nullable},target};
  return TURBODB_STATUS_OK;
}

static turbodb_value_t cast_integer_bits(uint64_t bits, turbodb_value_kind_t target) {
  if (target==TURBODB_VALUE_UINT64) return turbodb_u64(bits);
  /* Every signed conversion is representable, including the minimum value. */
  return turbodb_i64(bits<=(uint64_t)INT64_MAX ? (int64_t)bits :
      -(int64_t)(UINT64_MAX-bits)-1);
}

static uint64_t cast_integer_text(turbodb_blob_t bytes,
    turbodb_value_kind_t target, unsigned *conditions) {
  const unsigned char *data=bytes.data;
  size_t i=0;
  while (i<bytes.size && assignment_space(data[i])) ++i;
  const bool negative=i<bytes.size && data[i]=='-';
  if (i<bytes.size && (negative || data[i]=='+')) ++i;
  const size_t start=i;
  const uint64_t limit=negative ? (uint64_t)INT64_MAX+1u : UINT64_MAX;
  uint64_t magnitude=0; bool overflow=false;
  for (;i<bytes.size && data[i]>='0' && data[i]<='9';++i) {
    const unsigned digit=data[i]-'0';
    if (magnitude>(limit-digit)/10u) overflow=true;
    else if (!overflow) magnitude=magnitude*10u+digit;
  }
  if (start==i || overflow || assignment_trim_tail(data,bytes.size,i)!=bytes.size)
    *conditions|=ORM_SQL_CAST_TRUNCATED;
  if (start==i) return 0;
  if (overflow) return negative ? (uint64_t)INT64_MAX+1u : UINT64_MAX;
  if ((negative && target==TURBODB_VALUE_UINT64) ||
      (!negative && magnitude>(uint64_t)INT64_MAX && target==TURBODB_VALUE_INT64))
    *conditions|=ORM_SQL_CAST_COMPLEMENT;
  return negative ? 0u-magnitude : magnitude;
}

turbodb_status_t orm_tidesdb_sql_cast_eval(const orm_sql_cast *cast,
    const turbodb_value_t *input, orm_tidesdb_sql_budget *budget,
    turbodb_value_t *out, orm_sql_cast_condition *condition, turbodb_error_t *error) {
  if (!cast || !input || !budget || !out || !condition)
    return value_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid CAST evaluation arguments");
  orm_sql_cast checked;
  turbodb_status_t status=orm_tidesdb_sql_cast_bind(cast->source,cast->target,&checked,error);
  if (status!=TURBODB_STATUS_OK) return status;
  if (checked.result.kind!=cast->result.kind || checked.result.nullable!=cast->result.nullable || !valid_value(input,cast->source))
    return value_error(error,TURBODB_STATUS_TYPE_ERROR,"CAST runtime value mismatches bound type");
  const turbodb_blob_t bytes=bytes_of(input);
  status=assignment_charge(budget,(uint64_t)bytes.size,error);
  if (status!=TURBODB_STATUS_OK) return status;
  if (!valid_text(input)) return value_error(error,TURBODB_STATUS_TYPE_ERROR,"CAST text is not valid UTF-8");
  turbodb_value_t result=turbodb_null(); unsigned conditions=ORM_SQL_CAST_EXACT;
  if (input->kind==TURBODB_VALUE_NULL) {
    result=turbodb_null();
  } else if (input->kind==TURBODB_VALUE_TEXT || input->kind==TURBODB_VALUE_BLOB) {
    if (cast->result.kind==TURBODB_VALUE_DOUBLE) {
      orm_sql_assignment_adjustment adjusted=ORM_SQL_ASSIGNMENT_EXACT;
      status=assignment_double_text(bytes.data,bytes.size,true,&result,&adjusted,error);
      if (status!=TURBODB_STATUS_OK) return status;
      if (adjusted==ORM_SQL_ASSIGNMENT_OUT_OF_RANGE)
        return value_error(error,TURBODB_STATUS_OUT_OF_RANGE,"DOUBLE CAST exceeds finite numeric range");
      if (adjusted!=ORM_SQL_ASSIGNMENT_EXACT) conditions|=ORM_SQL_CAST_TRUNCATED;
    } else result=cast_integer_bits(cast_integer_text(bytes,cast->result.kind,&conditions),cast->result.kind);
  } else if (cast->result.kind==TURBODB_VALUE_DOUBLE) {
    result=turbodb_f64(input->kind==TURBODB_VALUE_DOUBLE ? input->data.double_value :
        input->kind==TURBODB_VALUE_INT64 ? (double)input->data.int64_value :
        input->kind==TURBODB_VALUE_UINT64 ? (double)input->data.uint64_value :
        (double)input->data.boolean_value);
  } else {
    uint64_t bits;
    if (input->kind==TURBODB_VALUE_DOUBLE) {
      const double rounded=rint(input->data.double_value);
      const double signed_limit=-(double)INT64_MIN;
      const int64_t integer=rounded<=-signed_limit ? INT64_MIN :
          rounded>=signed_limit ? INT64_MAX : (int64_t)rounded;
      bits=(uint64_t)integer;
    } else bits=input->kind==TURBODB_VALUE_INT64 ? (uint64_t)input->data.int64_value :
        input->kind==TURBODB_VALUE_UINT64 ? input->data.uint64_value : input->data.boolean_value;
    result=cast_integer_bits(bits,cast->result.kind);
  }
  if (cast->target==ORM_SQL_CAST_FLOAT && result.kind!=TURBODB_VALUE_NULL) {
    const double value=result.data.double_value;
    if (value>(double)FLT_MAX || value<-(double)FLT_MAX)
      return value_error(error,TURBODB_STATUS_OUT_OF_RANGE,"FLOAT CAST exceeds single-precision range");
    result=turbodb_f64((double)(float)value);
  }
  *out=result; *condition=(orm_sql_cast_condition)conditions;
  return TURBODB_STATUS_OK;
}

static int compare_u64(uint64_t a, uint64_t b) {
  return a < b ? -1 : (a > b ? 1 : 0);
}

/* Original kinds remain strict even when the bound operation selects MySQL's
 * real lane. Integer peers follow val_real rounding on that lane only. */
static double numeric_real_value(const turbodb_value_t *value) {
  return value->kind == TURBODB_VALUE_DOUBLE ? value->data.double_value :
      value->kind == TURBODB_VALUE_INT64 ? (double)value->data.int64_value :
      (double)value->data.uint64_value;
}

turbodb_status_t orm_tidesdb_sql_real_promote(const turbodb_value_t *input,
    orm_tidesdb_sql_budget *budget, turbodb_value_t *out, turbodb_error_t *error) {
  if (!input || !out)
    return value_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid numeric result promotion");
  if ((!assignment_numeric(input->kind) && input->kind != TURBODB_VALUE_NULL) ||
      !valid_value(input, (orm_sql_type){input->kind, true}))
    return value_error(error, TURBODB_STATUS_TYPE_ERROR, "invalid numeric result value");
  orm_sql_budget_amount amount = {0};
  amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1;
  const turbodb_status_t status = orm_tidesdb_sql_budget_reserve(budget, &amount, error);
  if (status == TURBODB_STATUS_OK)
    *out = input->kind == TURBODB_VALUE_NULL ? turbodb_null() : turbodb_f64(numeric_real_value(input));
  return status;
}

/* bind + runtime validation establish non-NULL compatible operands. Pure
 * integer comparisons retain adjacent values above 2^53. */
static int compare_values(const turbodb_value_t *left, const turbodb_value_t *right,
    bool real_comparison) {
  if (real_comparison) {
    const double a = numeric_real_value(left), b = numeric_real_value(right);
    return a < b ? -1 : (a > b ? 1 : 0);
  }
  if (integral(left->kind) && integral(right->kind)) {
    if (left->kind == TURBODB_VALUE_INT64 && right->kind == TURBODB_VALUE_INT64) {
      const int64_t a = left->data.int64_value, b = right->data.int64_value;
      return a < b ? -1 : (a > b ? 1 : 0);
    }
    if (left->kind == TURBODB_VALUE_UINT64 && right->kind == TURBODB_VALUE_UINT64)
      return compare_u64(left->data.uint64_value, right->data.uint64_value);
    const bool signed_left = left->kind == TURBODB_VALUE_INT64;
    const int64_t s = signed_left ? left->data.int64_value : right->data.int64_value;
    const uint64_t u = signed_left ? right->data.uint64_value : left->data.uint64_value;
    const int order = s < 0 ? -1 : compare_u64((uint64_t)s, u);
    return signed_left ? order : -order;
  }
  if (left->kind == TURBODB_VALUE_DOUBLE) {
    const double a = left->data.double_value, b = right->data.double_value;
    return a < b ? -1 : (a > b ? 1 : 0);
  }
  if (left->kind == TURBODB_VALUE_BOOLEAN)
    return compare_u64(left->data.boolean_value, right->data.boolean_value);
  const turbodb_blob_t a = bytes_of(left), b = bytes_of(right);
  const size_t common = a.size < b.size ? a.size : b.size;
  const int order = common ? memcmp(a.data, b.data, common) : 0;
  return order ? (order < 0 ? -1 : 1) : compare_u64(a.size, b.size);
}

static turbodb_value_t evaluate(const orm_sql_predicate *predicate,
    const turbodb_value_t *left, const turbodb_value_t *right) {
  const bool a_null = left->kind == TURBODB_VALUE_NULL;
  const bool b_null = !right || right->kind == TURBODB_VALUE_NULL;
  if (predicate->op == ORM_SQL_IS_NULL) return turbodb_bool(a_null);
  if (predicate->op == ORM_SQL_IS_NOT_NULL) return turbodb_bool(!a_null);
  if (predicate->op == ORM_SQL_NOT)
    return a_null ? turbodb_null() : turbodb_bool(!left->data.boolean_value);
  if (predicate->op == ORM_SQL_AND || predicate->op == ORM_SQL_OR) {
    const bool decisive = predicate->op == ORM_SQL_OR;
    if ((!a_null && (left->data.boolean_value != 0) == decisive) ||
        (!b_null && (right->data.boolean_value != 0) == decisive))
      return turbodb_bool(decisive);
    return a_null || b_null ? turbodb_null() : turbodb_bool(!decisive);
  }
  if (a_null || b_null)
    return predicate->op == ORM_SQL_NULL_SAFE_EQUAL
        ? turbodb_bool(a_null && b_null) : turbodb_null();
  const int order = compare_values(left, right, predicate->real_comparison);
  switch (predicate->op) {
    case ORM_SQL_EQUAL: case ORM_SQL_NULL_SAFE_EQUAL: return turbodb_bool(order == 0);
    case ORM_SQL_NOT_EQUAL: return turbodb_bool(order != 0);
    case ORM_SQL_LESS: return turbodb_bool(order < 0);
    case ORM_SQL_LESS_EQUAL: return turbodb_bool(order <= 0);
    case ORM_SQL_GREATER: return turbodb_bool(order > 0);
    default: return turbodb_bool(order >= 0); /* Bound GREATER_EQUAL only. */
  }
}

turbodb_status_t orm_tidesdb_sql_predicate_eval(const orm_sql_predicate *predicate,
    const turbodb_value_t *left, const turbodb_value_t *right,
    orm_tidesdb_sql_budget *budget, turbodb_value_t *out, turbodb_error_t *error) {
  if (!predicate || !left || !budget || !out ||
      (unary(predicate->op) ? right != NULL : right == NULL))
    return value_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                        "TidesDB SQL eval: invalid predicate arguments");
  if (predicate->op < ORM_SQL_EQUAL || predicate->op > ORM_SQL_OR)
    return value_error(error, TURBODB_STATUS_INVALID_STATE,
                        "TidesDB SQL eval: invalid bound predicate operation");
  if (!valid_value(left, predicate->left) ||
      (right && !valid_value(right, predicate->right)))
    return value_error(error, TURBODB_STATUS_TYPE_ERROR,
                        "TidesDB SQL eval: value does not match its bound type");
  turbodb_status_t status = charge_steps(predicate, left, right, budget, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (!valid_text(left) || (right && !valid_text(right)))
    return value_error(error, TURBODB_STATUS_TYPE_ERROR,
                        "TidesDB SQL eval: TEXT must contain valid UTF-8");
  *out = evaluate(predicate, left, right);
  return TURBODB_STATUS_OK;
}

static bool like_type(turbodb_value_kind_t kind) {
  return kind == TURBODB_VALUE_TEXT || kind == TURBODB_VALUE_NULL;
}

turbodb_status_t orm_tidesdb_sql_like_bind(orm_sql_type left, orm_sql_type right,
    int escape, bool negated, orm_sql_like *out, turbodb_error_t *error) {
  if (!out || escape < ORM_SQL_LIKE_NO_ESCAPE || escape > ORM_SQL_ASCII_MAX)
    return value_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid LIKE output or escape byte");
  if (!valid_type(left) || !valid_type(right))
    return value_error(error, TURBODB_STATUS_TYPE_ERROR, "invalid LIKE operand type");
  if (!like_type(left.kind) || !like_type(right.kind))
    return value_error(error, TURBODB_STATUS_UNSUPPORTED, "LIKE requires TEXT or NULL operands");
  *out = (orm_sql_like){left, right, {TURBODB_VALUE_BOOLEAN, left.nullable || right.nullable}, escape, negated};
  return TURBODB_STATUS_OK;
}

static bool ascii_text(const turbodb_value_t *value) {
  if (value->kind == TURBODB_VALUE_NULL) return true;
  for (size_t i = 0; i < value->data.text_value.len; ++i)
    if ((unsigned char)value->data.text_value.data[i] > ORM_SQL_ASCII_MAX) return false;
  return true;
}

/* Remember only the last %. Each retry advances its consumed prefix, while
 * the fixed suffix is replayed. No recursion or payload-sized workspace;
 * potentially quadratic suffix retries are explicitly step bounded. */
static unsigned char like_fold(unsigned char ch, bool insensitive) {
  return insensitive && ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
}
static turbodb_status_t like_match(vstr text, vstr pattern, int escape, bool insensitive,
    orm_tidesdb_sql_budget *budget, bool *out, turbodb_error_t *error) {
  size_t input = 0, cursor = 0, star = SIZE_MAX, retry = 0;
  orm_sql_budget_amount step = {0}; step.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1;
  while (input < text.len || cursor < pattern.len) {
    const turbodb_status_t status = orm_tidesdb_sql_budget_reserve(budget, &step, error);
    if (status != TURBODB_STATUS_OK) return status;
    if (cursor < pattern.len) {
      unsigned char token = (unsigned char)pattern.data[cursor++];
      const bool escaped = token == escape;
      if (escaped && cursor < pattern.len) token = (unsigned char)pattern.data[cursor++];
      if (!escaped && token == '%') { star = cursor; retry = input; continue; }
      if (input < text.len && ((!escaped && token == '_') ||
          like_fold(token,insensitive) == like_fold((unsigned char)text.data[input],insensitive))) {
        ++input; continue;
      }
    }
    if (star != SIZE_MAX && retry < text.len) { input = ++retry; cursor = star; continue; }
    *out = false; return TURBODB_STATUS_OK;
  }
  *out = true; return TURBODB_STATUS_OK;
}

turbodb_status_t orm_tidesdb_sql_like_eval(const orm_sql_like *like,
    const turbodb_value_t *left, const turbodb_value_t *right,
    orm_tidesdb_sql_budget *budget, turbodb_value_t *out, turbodb_error_t *error) {
  if (!like || !left || !right || !budget || !out)
    return value_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid LIKE evaluation arguments");
  if (!valid_value(left, like->left) || !valid_value(right, like->right))
    return value_error(error, TURBODB_STATUS_TYPE_ERROR, "LIKE value does not match its bound type");
  const turbodb_blob_t a = bytes_of(left), b = bytes_of(right);
  uint64_t steps = 1;
  /* Both UTF-8 and ASCII validation scan bytes; reserve before either pass. */
  if (!add_steps(&steps, a.size) || !add_steps(&steps, a.size) ||
      !add_steps(&steps, b.size) || !add_steps(&steps, b.size))
    return value_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "LIKE validation step count overflows");
  orm_sql_budget_amount charge = {0}; charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = steps;
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve(budget, &charge, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (!valid_text(left) || !valid_text(right))
    return value_error(error, TURBODB_STATUS_TYPE_ERROR, "LIKE requires valid UTF-8 TEXT");
  if (!ascii_text(left) || !ascii_text(right))
    return value_error(error, TURBODB_STATUS_UNSUPPORTED, "LIKE currently supports ASCII TEXT only");
  if (left->kind == TURBODB_VALUE_NULL || right->kind == TURBODB_VALUE_NULL) { *out = turbodb_null(); return TURBODB_STATUS_OK; }
  bool matched = false;
  status = like_match(left->data.text_value, right->data.text_value, like->escape,
      like->ascii_insensitive, budget, &matched, error);
  if (status == TURBODB_STATUS_OK) *out = turbodb_bool(like->negated ? !matched : matched);
  return status;
}

static bool arithmetic_unary(orm_sql_arithmetic_op op) {
  return op >= ORM_SQL_POSITIVE && op <= ORM_SQL_CEIL;
}

static bool numeric_type(turbodb_value_kind_t kind) {
  return kind == TURBODB_VALUE_NULL || integral(kind) || kind == TURBODB_VALUE_DOUBLE;
}

static bool arithmetic_division(orm_sql_arithmetic_op op) {
  return op >= ORM_SQL_DIVIDE && op <= ORM_SQL_MODULO;
}
static bool arithmetic_rounding(orm_sql_arithmetic_op op) {
  return op == ORM_SQL_ROUND || op == ORM_SQL_TRUNCATE;
}

turbodb_status_t orm_tidesdb_sql_arithmetic_bind(orm_sql_arithmetic_op op,
    orm_sql_type left, const orm_sql_type *right, orm_sql_arithmetic *out,
    turbodb_error_t *error) {
  if (op < ORM_SQL_ADD || op > ORM_SQL_TRUNCATE)
    return value_error(error, TURBODB_STATUS_UNSUPPORTED, "unsupported arithmetic operation");
  if (!out || (arithmetic_unary(op) ? right != NULL : right == NULL))
    return value_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid arithmetic output or arity");
  if (!valid_type(left) || (right && !valid_type(*right)))
    return value_error(error, TURBODB_STATUS_TYPE_ERROR, "invalid arithmetic operand type");
  if (arithmetic_rounding(op)) {
    if (!numeric_type(left.kind) || (!integral(right->kind) && right->kind != TURBODB_VALUE_NULL))
      return value_error(error, TURBODB_STATUS_UNSUPPORTED, "rounding requires numeric value and integer precision");
    *out = (orm_sql_arithmetic){op,left,*right,{left.kind,left.nullable || right->nullable}};
    return TURBODB_STATUS_OK;
  }
  const bool integer_pair = right && (integral(left.kind) || left.kind == TURBODB_VALUE_NULL) &&
      (integral(right->kind) || right->kind == TURBODB_VALUE_NULL);
  const bool real_pair = right && op != ORM_SQL_INTEGER_DIVIDE &&
      (left.kind == TURBODB_VALUE_DOUBLE || right->kind == TURBODB_VALUE_DOUBLE);
  const bool same = !right || left.kind == TURBODB_VALUE_NULL || right->kind == TURBODB_VALUE_NULL || left.kind == right->kind;
  if (!numeric_type(left.kind) || (right && !numeric_type(right->kind)) ||
      (!same && !real_pair && !(integer_pair && (op == ORM_SQL_INTEGER_DIVIDE || op == ORM_SQL_MODULO))) ||
      (op == ORM_SQL_NEGATE && left.kind == TURBODB_VALUE_UINT64))
    return value_error(error, TURBODB_STATUS_UNSUPPORTED, "unsupported arithmetic type or implicit conversion");
  if ((op == ORM_SQL_INTEGER_DIVIDE && !integer_pair) ||
      (op == ORM_SQL_DIVIDE && !real_pair && (integral(left.kind) || integral(right->kind))))
    return value_error(error,TURBODB_STATUS_UNSUPPORTED,"exact division or noninteger DIV requires DECIMAL");
  turbodb_value_kind_t kind = real_pair ? TURBODB_VALUE_DOUBLE : op == ORM_SQL_SIGN && left.kind != TURBODB_VALUE_NULL ? TURBODB_VALUE_INT64 :
      left.kind == TURBODB_VALUE_NULL && right ? right->kind : left.kind;
  if (op == ORM_SQL_INTEGER_DIVIDE && (left.kind == TURBODB_VALUE_UINT64 || right->kind == TURBODB_VALUE_UINT64)) kind = TURBODB_VALUE_UINT64;
  *out = (orm_sql_arithmetic){op, left, right ? *right : (orm_sql_type){TURBODB_VALUE_NULL, true},
      {kind, arithmetic_division(op) || left.nullable || (right && right->nullable)}};
  return TURBODB_STATUS_OK;
}

static turbodb_status_t arithmetic_range_error(turbodb_error_t *error) {
  return value_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "arithmetic result is out of range");
}

static turbodb_status_t arithmetic_real(orm_sql_arithmetic_op op,
    const turbodb_value_t *left, const turbodb_value_t *right, turbodb_value_t *out, turbodb_error_t *error) {
  const double a=numeric_real_value(left),b=numeric_real_value(right);
  double value;
  switch(op) {
    case ORM_SQL_ADD: value=a+b; break;
    case ORM_SQL_SUBTRACT: value=a-b; break;
    case ORM_SQL_MULTIPLY: value=a*b; break;
    case ORM_SQL_DIVIDE: value=a/b; break;
    case ORM_SQL_MODULO: value=fmod(a,b); break;
    default: return value_error(error,TURBODB_STATUS_INVALID_STATE,"invalid real arithmetic operation");
  }
  if(!isfinite(value)) return arithmetic_range_error(error);
  *out=turbodb_f64(value); return TURBODB_STATUS_OK;
}

static uint64_t signed_magnitude(int64_t value) {
  return value < 0 ? (uint64_t)(-(value + 1)) + 1u : (uint64_t)value;
}

static turbodb_status_t arithmetic_round_integer(orm_sql_arithmetic_op op,
    const turbodb_value_t *input, uint64_t digits, turbodb_value_t *out, turbodb_error_t *error) {
  enum { DECIMAL_RADIX = 10, INTEGER_DECIMAL_POWERS = 20 };
  if (digits >= INTEGER_DECIMAL_POWERS) {
    *out = input->kind == TURBODB_VALUE_UINT64 ? turbodb_u64(0) : turbodb_i64(0);
    return TURBODB_STATUS_OK;
  }
  uint64_t scale = 1;
  for (uint64_t i = 0; i < digits; ++i) scale *= DECIMAL_RADIX;
  const bool negative = input->kind == TURBODB_VALUE_INT64 && input->data.int64_value < 0;
  const uint64_t magnitude = input->kind == TURBODB_VALUE_UINT64 ? input->data.uint64_value : signed_magnitude(input->data.int64_value);
  const uint64_t maximum = input->kind == TURBODB_VALUE_UINT64 ? UINT64_MAX : (uint64_t)INT64_MAX + (negative ? 1u : 0u);
  const uint64_t remainder = magnitude % scale;
  uint64_t rounded = magnitude - remainder;
  if (op == ORM_SQL_ROUND && remainder >= scale / 2u) {
    if (scale > maximum - rounded) return arithmetic_range_error(error);
    rounded += scale;
  }
  *out = input->kind == TURBODB_VALUE_UINT64 ? turbodb_u64(rounded) :
      turbodb_i64(negative ? rounded == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)rounded : (int64_t)rounded);
  return TURBODB_STATUS_OK;
}

static turbodb_status_t arithmetic_round(orm_sql_arithmetic_op op,
    const turbodb_value_t *input, const turbodb_value_t *precision, turbodb_value_t *out, turbodb_error_t *error) {
  const bool negative = precision->kind == TURBODB_VALUE_INT64 && precision->data.int64_value < 0;
  const uint64_t digits = precision->kind == TURBODB_VALUE_UINT64 ? precision->data.uint64_value : signed_magnitude(precision->data.int64_value);
  if (input->kind != TURBODB_VALUE_DOUBLE) {
    if (!negative) { *out = *input; return TURBODB_STATUS_OK; }
    return arithmetic_round_integer(op,input,digits,out,error);
  }
  if (digits > DBL_MAX_10_EXP) {
    *out = negative ? turbodb_f64(0.0) : *input;
    return TURBODB_STATUS_OK;
  }
  const double scale = pow(10.0,(double)digits);
  const double scaled = negative ? input->data.double_value / scale : input->data.double_value * scale;
  /* MySQL preserves finite input when extra fractional precision cannot be
   * represented. The bound above keeps scale itself finite. */
  if (!negative && !isfinite(scaled)) { *out = *input; return TURBODB_STATUS_OK; }
  const double integral_value = op == ORM_SQL_ROUND ? rint(scaled) : trunc(scaled);
  const double result = negative ? integral_value * scale : integral_value / scale;
  if (!isfinite(result)) return arithmetic_range_error(error);
  *out = turbodb_f64(result);
  return TURBODB_STATUS_OK;
}

static turbodb_status_t arithmetic_i64(orm_sql_arithmetic_op op, int64_t a, int64_t b,
    turbodb_value_t *out, turbodb_error_t *error) {
  if (op == ORM_SQL_ADD) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return arithmetic_range_error(error);
    *out = turbodb_i64(a + b);
  } else if (op == ORM_SQL_SUBTRACT) {
    if ((b > 0 && a < INT64_MIN + b) || (b < 0 && a > INT64_MAX + b)) return arithmetic_range_error(error);
    *out = turbodb_i64(a - b);
  } else {
    const bool negative = (a < 0) != (b < 0);
    const uint64_t x = signed_magnitude(a), y = signed_magnitude(b);
    const uint64_t maximum = (uint64_t)INT64_MAX + (negative ? 1u : 0u);
    if (x && y > maximum / x) return arithmetic_range_error(error);
    const uint64_t product = x * y;
    *out = turbodb_i64(negative ? product == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)product : (int64_t)product);
  }
  return TURBODB_STATUS_OK;
}

static turbodb_status_t arithmetic_u64(orm_sql_arithmetic_op op, uint64_t a, uint64_t b,
    turbodb_value_t *out, turbodb_error_t *error) {
  if (op == ORM_SQL_ADD) {
    if (b > UINT64_MAX - a) return arithmetic_range_error(error);
    *out = turbodb_u64(a + b);
  } else if (op == ORM_SQL_SUBTRACT) {
    if (a < b) return arithmetic_range_error(error);
    *out = turbodb_u64(a - b);
  } else {
    if (a && b > UINT64_MAX / a) return arithmetic_range_error(error);
    *out = turbodb_u64(a * b);
  }
  return TURBODB_STATUS_OK;
}

/* Magnitude division avoids signed MIN/-1 undefined behavior. MySQL's integer
 * DIV path rejects negative magnitude beyond INT64_MAX before signed conversion. */
static turbodb_status_t arithmetic_integer_division(const orm_sql_arithmetic *arithmetic,
    const turbodb_value_t *left, const turbodb_value_t *right, turbodb_value_t *out, turbodb_error_t *error) {
  const bool negative_left = left->kind == TURBODB_VALUE_INT64 && left->data.int64_value < 0;
  const bool negative_right = right->kind == TURBODB_VALUE_INT64 && right->data.int64_value < 0;
  const uint64_t a = left->kind == TURBODB_VALUE_UINT64 ? left->data.uint64_value : signed_magnitude(left->data.int64_value);
  const uint64_t b = right->kind == TURBODB_VALUE_UINT64 ? right->data.uint64_value : signed_magnitude(right->data.int64_value);
  const uint64_t value = arithmetic->op == ORM_SQL_MODULO ? a % b : a / b;
  const bool negative = arithmetic->op == ORM_SQL_MODULO ? negative_left : negative_left != negative_right;
  if (arithmetic->result.kind == TURBODB_VALUE_UINT64) {
    if (negative && value) return arithmetic_range_error(error);
    *out = turbodb_u64(value);
  } else {
    const uint64_t maximum = (uint64_t)INT64_MAX +
        (negative && arithmetic->op == ORM_SQL_MODULO ? 1u : 0u);
    if (value > maximum) return arithmetic_range_error(error);
    *out = turbodb_i64(negative ? value == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)value : (int64_t)value);
  }
  return TURBODB_STATUS_OK;
}

turbodb_status_t orm_tidesdb_sql_arithmetic_eval_condition(const orm_sql_arithmetic *arithmetic,
    const turbodb_value_t *left, const turbodb_value_t *right,
    orm_tidesdb_sql_budget *budget, turbodb_value_t *out, orm_sql_numeric_condition *condition, turbodb_error_t *error) {
  if (!arithmetic || !left || !budget || !out || !condition ||
      (arithmetic_unary(arithmetic->op) ? right != NULL : right == NULL))
    return value_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid arithmetic arguments");
  if (arithmetic->op < ORM_SQL_ADD || arithmetic->op > ORM_SQL_TRUNCATE)
    return value_error(error, TURBODB_STATUS_INVALID_STATE, "invalid bound arithmetic operation");
  if (!valid_value(left, arithmetic->left) || (right && !valid_value(right, arithmetic->right)))
    return value_error(error, TURBODB_STATUS_TYPE_ERROR, "arithmetic value does not match its bound type");
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1;
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve(budget, &amount, error);
  if (status != TURBODB_STATUS_OK) return status;
  turbodb_value_t result;
  orm_sql_numeric_condition evaluated = ORM_SQL_NUMERIC_EXACT;
  if (left->kind == TURBODB_VALUE_NULL || (right && right->kind == TURBODB_VALUE_NULL)) result = turbodb_null();
  else if (arithmetic_rounding(arithmetic->op))
    status = arithmetic_round(arithmetic->op,left,right,&result,error);
  else if (arithmetic_division(arithmetic->op)) {
    const bool zero = right->kind == TURBODB_VALUE_DOUBLE ? right->data.double_value == 0.0 :
        right->kind == TURBODB_VALUE_UINT64 ? right->data.uint64_value == 0 : right->data.int64_value == 0;
    if (zero) { result = turbodb_null(); evaluated = ORM_SQL_NUMERIC_DIVISION_BY_ZERO; }
    else if (arithmetic->result.kind == TURBODB_VALUE_DOUBLE)
      status = arithmetic_real(arithmetic->op,left,right,&result,error);
    else status = arithmetic_integer_division(arithmetic,left,right,&result,error);
  }
  else if (arithmetic_unary(arithmetic->op)) {
    result = *left;
    if (arithmetic->op == ORM_SQL_NEGATE) {
      if (left->kind == TURBODB_VALUE_INT64) {
        if (left->data.int64_value == INT64_MIN) return arithmetic_range_error(error);
        result = turbodb_i64(-left->data.int64_value);
      } else result = turbodb_f64(-left->data.double_value);
    } else if (arithmetic->op == ORM_SQL_ABSOLUTE) {
      if (left->kind == TURBODB_VALUE_INT64) {
        const int64_t value = left->data.int64_value;
        if (value == INT64_MIN) return arithmetic_range_error(error);
        result = turbodb_i64(value < 0 ? -value : value);
      } else if (left->kind == TURBODB_VALUE_DOUBLE) result = turbodb_f64(fabs(left->data.double_value));
    } else if (arithmetic->op == ORM_SQL_SIGN) {
      if (left->kind == TURBODB_VALUE_INT64) result = turbodb_i64((left->data.int64_value > 0) - (left->data.int64_value < 0));
      else if (left->kind == TURBODB_VALUE_UINT64) result = turbodb_i64(left->data.uint64_value != 0);
      else result = turbodb_i64((left->data.double_value > 0.0) - (left->data.double_value < 0.0));
    } else if (left->kind == TURBODB_VALUE_DOUBLE && (arithmetic->op == ORM_SQL_FLOOR || arithmetic->op == ORM_SQL_CEIL)) {
      result = turbodb_f64(arithmetic->op == ORM_SQL_FLOOR ? floor(left->data.double_value) : ceil(left->data.double_value));
    }
  } else if (arithmetic->result.kind == TURBODB_VALUE_DOUBLE)
    status = arithmetic_real(arithmetic->op,left,right,&result,error);
  else if (left->kind == TURBODB_VALUE_INT64)
    status = arithmetic_i64(arithmetic->op, left->data.int64_value, right->data.int64_value, &result, error);
  else if (left->kind == TURBODB_VALUE_UINT64)
    status = arithmetic_u64(arithmetic->op, left->data.uint64_value, right->data.uint64_value, &result, error);
  else return value_error(error,TURBODB_STATUS_INVALID_STATE,"invalid arithmetic result type");
  if (status == TURBODB_STATUS_OK) { *out = result; *condition = evaluated; }
  return status;
}

turbodb_status_t orm_tidesdb_sql_arithmetic_eval(const orm_sql_arithmetic *arithmetic,
    const turbodb_value_t *left, const turbodb_value_t *right,
    orm_tidesdb_sql_budget *budget, turbodb_value_t *out, turbodb_error_t *error) {
  if (!out) return value_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid arithmetic output");
  turbodb_value_t result; orm_sql_numeric_condition condition;
  turbodb_status_t status = orm_tidesdb_sql_arithmetic_eval_condition(arithmetic,left,right,budget,&result,&condition,error);
  if (status == TURBODB_STATUS_OK && condition == ORM_SQL_NUMERIC_DIVISION_BY_ZERO)
    return value_error(error,TURBODB_STATUS_SQL_ERROR,"division by zero");
  if (status == TURBODB_STATUS_OK) *out = result;
  return status;
}

int orm_sql_value_order(const turbodb_value_t *a, const turbodb_value_t *b) {
  if (a->kind == TURBODB_VALUE_NULL || b->kind == TURBODB_VALUE_NULL)
    return (b->kind == TURBODB_VALUE_NULL) - (a->kind == TURBODB_VALUE_NULL);
  if (a->kind == TURBODB_VALUE_BOOLEAN)
    return (a->data.boolean_value > b->data.boolean_value) - (a->data.boolean_value < b->data.boolean_value);
  if (a->kind == TURBODB_VALUE_INT64)
    return (a->data.int64_value > b->data.int64_value) - (a->data.int64_value < b->data.int64_value);
  if (a->kind == TURBODB_VALUE_UINT64)
    return (a->data.uint64_value > b->data.uint64_value) - (a->data.uint64_value < b->data.uint64_value);
  return (a->data.double_value > b->data.double_value) - (a->data.double_value < b->data.double_value);
}

#include "query.h"

#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ORM_REDIS_QUERY_BASE_ARGUMENTS = 12u };

static orm_status_t orm_redis_query_fail(orm_error_t *error,
                                         orm_status_t status,
                                         const char *message) {
  orm_error_set(error, status, message);
  return status;
}

static void orm_redis_string_vec_destroy(vec_t *values) {
  size_t index;
  for (index = 0u; index < vec_size(values); ++index)
    tstr_freep((tstr *)vec_at(values, index));
  vec_destroy(values);
}

void orm_redis_query_destroy(orm_redis_query *query) {
  if (query == NULL) return;
  orm_redis_string_vec_destroy(&query->arguments);
  orm_redis_string_vec_destroy(&query->output_columns);
  memset(query, 0, sizeof(*query));
}

static orm_status_t orm_redis_push_owned(vec_t *values, tstr owned,
                                         orm_error_t *error,
                                         const char *operation) {
  stl_status status;
  if (owned == NULL)
    return orm_redis_query_fail(error, ORM_STATUS_OUT_OF_MEMORY, operation);
  status = vec_push(values, &owned);
  if (status != STL_OK) {
    tstr_free(owned);
    return orm_redis_query_fail(
        error, status == STL_CAPACITY_EXCEEDED ? ORM_STATUS_LIMIT_EXCEEDED
                                   : ORM_STATUS_OUT_OF_MEMORY,
        operation);
  }
  return ORM_STATUS_OK;
}

static orm_status_t orm_redis_push_cstr(vec_t *values, const char *value,
                                        orm_error_t *error) {
  return orm_redis_push_owned(values, tstr_dup(value), error,
                              "append Redis command argument");
}

static orm_status_t orm_redis_push_view(vec_t *values, vstr value,
                                        orm_error_t *error) {
  return orm_redis_push_owned(values, tstr_from_v(value), error,
                              "append Redis command argument");
}

static orm_status_t orm_redis_append(tstr *text, const char *data, size_t size,
                                     size_t maximum, orm_error_t *error) {
  tstr next;
  if (size > maximum || tstr_len(*text) > maximum - size)
    return orm_redis_query_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                                "Redis query exceeds max_query_bytes");
  next = tstr_cat_len(*text, data, size);
  if (next == NULL)
    return orm_redis_query_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                                "append Redis query");
  *text = next;
  return ORM_STATUS_OK;
}

static orm_status_t orm_redis_format_double(double value, tstr *out,
                                            orm_error_t *error) {
  char scientific[64];
  char decimal[768];
  char digits[DBL_DECIMAL_DIG + 1];
  const char *cursor;
  char *end;
  size_t digit_count = 0u;
  size_t output = 0u;
  long exponent;
  long decimal_position;
  int negative;
  int length;
  if (!isfinite(value))
    return orm_redis_query_fail(error, ORM_STATUS_OUT_OF_RANGE,
                                "Redis double parameter must be finite");
  if (value == 0.0) {
    *out = tstr_dup("0");
    return *out != NULL ? ORM_STATUS_OK
                        : orm_redis_query_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                                               "format Redis double");
  }
  length = snprintf(scientific, sizeof(scientific), "%.17e", value);
  if (length <= 0 || (size_t)length >= sizeof(scientific))
    return orm_redis_query_fail(error, ORM_STATUS_TYPE_ERROR,
                                "format Redis double");
  cursor = scientific;
  negative = *cursor == '-';
  if (*cursor == '-' || *cursor == '+') ++cursor;
  while (*cursor != '\0' && *cursor != 'e' && *cursor != 'E') {
    if (*cursor != '.') digits[digit_count++] = *cursor;
    ++cursor;
  }
  if (*cursor == '\0' || digit_count == 0u)
    return orm_redis_query_fail(error, ORM_STATUS_TYPE_ERROR,
                                "format Redis double exponent");
  exponent = strtol(cursor + 1, &end, 10);
  if (*end != '\0')
    return orm_redis_query_fail(error, ORM_STATUS_TYPE_ERROR,
                                "format Redis double exponent");
  while (digit_count > 1u && digits[digit_count - 1u] == '0') --digit_count;
  decimal_position = exponent + 1;
  if (negative) decimal[output++] = '-';
  if (decimal_position <= 0) {
    long zeros;
    decimal[output++] = '0';
    decimal[output++] = '.';
    for (zeros = 0; zeros < -decimal_position; ++zeros) decimal[output++] = '0';
    memcpy(decimal + output, digits, digit_count);
    output += digit_count;
  } else if ((size_t)decimal_position >= digit_count) {
    size_t zeros;
    memcpy(decimal + output, digits, digit_count);
    output += digit_count;
    for (zeros = digit_count; zeros < (size_t)decimal_position; ++zeros)
      decimal[output++] = '0';
  } else {
    memcpy(decimal + output, digits, (size_t)decimal_position);
    output += (size_t)decimal_position;
    decimal[output++] = '.';
    memcpy(decimal + output, digits + decimal_position,
           digit_count - (size_t)decimal_position);
    output += digit_count - (size_t)decimal_position;
  }
  if (output >= sizeof(decimal))
    return orm_redis_query_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                                "Redis double text exceeds internal bound");
  *out = tstr_dup_len(decimal, output);
  return *out != NULL ? ORM_STATUS_OK
                      : orm_redis_query_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                                             "format Redis double");
}

orm_status_t orm_redis_value_text(const orm_owned_value *value, tstr *out,
                                  orm_error_t *error) {
  char buffer[64];
  int length;
  if (value == NULL || out == NULL)
    return orm_redis_query_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                                "invalid Redis value");
  *out = NULL;
  switch (value->kind) {
    case ORM_VALUE_NULL: return ORM_STATUS_OK;
    case ORM_VALUE_BOOLEAN:
      *out = tstr_dup(value->data.boolean_value != 0u ? "1" : "0");
      break;
    case ORM_VALUE_INT64:
      length = snprintf(buffer, sizeof(buffer), "%" PRId64,
                        value->data.int64_value);
      if (length > 0 && (size_t)length < sizeof(buffer))
        *out = tstr_dup_len(buffer, (size_t)length);
      break;
    case ORM_VALUE_UINT64:
      length = snprintf(buffer, sizeof(buffer), "%" PRIu64,
                        value->data.uint64_value);
      if (length > 0 && (size_t)length < sizeof(buffer))
        *out = tstr_dup_len(buffer, (size_t)length);
      break;
    case ORM_VALUE_DOUBLE: return orm_redis_format_double(value->data.double_value,
                                                          out, error);
    case ORM_VALUE_TEXT:
    case ORM_VALUE_BLOB:
      *out = tstr_clone(value->bytes);
      break;
    default:
      return orm_redis_query_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                                  "unknown Redis value kind");
  }
  return *out != NULL ? ORM_STATUS_OK
                      : orm_redis_query_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                                             "encode Redis value");
}

static int orm_redis_tag_special(unsigned char value) {
  static const char special[] = ",.<>{}[]\"':;!@#$%^&*()-+=~|\\/?";
  return value <= (unsigned char)' ' || strchr(special, (int)value) != NULL;
}

static orm_status_t orm_redis_append_tag(tstr *query,
                                         const orm_owned_value *value,
                                         size_t maximum,
                                         orm_error_t *error) {
  size_t index;
  for (index = 0u; index < tstr_len(value->bytes); ++index) {
    const unsigned char next = (unsigned char)value->bytes[index];
    if (orm_redis_tag_special(next)) {
      orm_status_t status = orm_redis_append(query, "\\", 1u, maximum, error);
      if (status != ORM_STATUS_OK) return status;
    }
    {
      const orm_status_t status = orm_redis_append(
          query, (const char *)&value->bytes[index], 1u, maximum, error);
      if (status != ORM_STATUS_OK) return status;
    }
  }
  return ORM_STATUS_OK;
}

static orm_status_t orm_redis_append_like(tstr *query,
                                          const orm_owned_value *value,
                                          size_t maximum,
                                          orm_error_t *error) {
  size_t index;
  for (index = 0u; index < tstr_len(value->bytes); ++index) {
    const char next = value->bytes[index];
    const char translated = next == '%' ? '*' : next == '_' ? '?' : next;
    if (next == '*' || next == '?' || next == '\'' || next == '\\') {
      orm_status_t status = orm_redis_append(query, "\\", 1u, maximum, error);
      if (status != ORM_STATUS_OK) return status;
    }
    {
      const orm_status_t status =
          orm_redis_append(query, &translated, 1u, maximum, error);
      if (status != ORM_STATUS_OK) return status;
    }
  }
  return ORM_STATUS_OK;
}

static orm_status_t orm_redis_render_predicate(
    tstr *query, const orm_predicate *predicate, size_t maximum,
    orm_error_t *error) {
  orm_status_t status;
  tstr encoded = NULL;
  status = orm_redis_append(query, "@", 1u, maximum, error);
  if (status == ORM_STATUS_OK)
    status = orm_redis_append(query, predicate->column,
                              tstr_len(predicate->column), maximum, error);
  if (status != ORM_STATUS_OK) return status;
  if (predicate->value.kind == ORM_VALUE_NULL)
    return orm_redis_query_fail(error, ORM_STATUS_UNSUPPORTED,
                                "Redis Query Engine does not index SQL NULL predicates");
  if (predicate->value.kind == ORM_VALUE_TEXT) {
    if (predicate->comparison == ORM_COMPARE_EQUAL ||
        predicate->comparison == ORM_COMPARE_NOT_EQUAL) {
      if (predicate->comparison == ORM_COMPARE_NOT_EQUAL) {
        tstr prefixed = tstr_dup("-");
        if (prefixed == NULL) return orm_redis_query_fail(
            error, ORM_STATUS_OUT_OF_MEMORY, "render Redis predicate");
        prefixed = tstr_cat_len(prefixed, *query, tstr_len(*query));
        if (prefixed == NULL) return orm_redis_query_fail(
            error, ORM_STATUS_OUT_OF_MEMORY, "render Redis predicate");
        tstr_free(*query);
        *query = prefixed;
      }
      status = orm_redis_append(query, ":{", 2u, maximum, error);
      if (status == ORM_STATUS_OK)
        status = orm_redis_append_tag(query, &predicate->value, maximum, error);
      if (status == ORM_STATUS_OK)
        status = orm_redis_append(query, "}", 1u, maximum, error);
      return status;
    }
    if (predicate->comparison == ORM_COMPARE_LIKE) {
      status = orm_redis_append(query, ":(\"w'", 5u, maximum, error);
      if (status == ORM_STATUS_OK)
        status = orm_redis_append_like(query, &predicate->value, maximum, error);
      if (status == ORM_STATUS_OK)
        status = orm_redis_append(query, "'\")", 3u, maximum, error);
      return status;
    }
    if (predicate->comparison == ORM_COMPARE_NOT_LIKE) {
      tstr field = tstr_clone(*query);
      tstr rebuilt = tstr_dup("(");
      if (field == NULL || rebuilt == NULL) {
        tstr_free(field);
        tstr_free(rebuilt);
        return orm_redis_query_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                                    "render Redis NOT LIKE predicate");
      }
      status = orm_redis_append(&rebuilt, field, tstr_len(field), maximum,
                                error);
      if (status == ORM_STATUS_OK)
        status = orm_redis_append(&rebuilt, ":(\"w'*'\") -", 11u,
                                  maximum, error);
      if (status == ORM_STATUS_OK)
        status = orm_redis_append(&rebuilt, field, tstr_len(field), maximum,
                                  error);
      if (status == ORM_STATUS_OK)
        status = orm_redis_append(&rebuilt, ":(\"w'", 5u, maximum, error);
      if (status == ORM_STATUS_OK)
        status = orm_redis_append_like(&rebuilt, &predicate->value, maximum,
                                       error);
      if (status == ORM_STATUS_OK)
        status = orm_redis_append(&rebuilt, "'\"))", 4u, maximum, error);
      tstr_free(field);
      if (status != ORM_STATUS_OK) {
        tstr_free(rebuilt);
        return status;
      }
      tstr_free(*query);
      *query = rebuilt;
      return ORM_STATUS_OK;
    }
    return orm_redis_query_fail(error, ORM_STATUS_UNSUPPORTED,
                                "Redis text predicate comparison is unsupported");
  }
  status = orm_redis_value_text(&predicate->value, &encoded, error);
  if (status != ORM_STATUS_OK) return status;
  if (predicate->comparison == ORM_COMPARE_NOT_EQUAL) {
    tstr field = tstr_clone(*query);
    tstr rebuilt = tstr_dup("(");
    if (field == NULL || rebuilt == NULL) {
      tstr_free(field);
      tstr_free(rebuilt);
      tstr_free(encoded);
      return orm_redis_query_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                                  "render Redis inequality predicate");
    }
    status = orm_redis_append(&rebuilt, field, tstr_len(field), maximum,
                              error);
    if (status == ORM_STATUS_OK)
      status = orm_redis_append(&rebuilt, ":[-inf +inf] -", 14u, maximum,
                                error);
    if (status == ORM_STATUS_OK)
      status = orm_redis_append(&rebuilt, field, tstr_len(field), maximum,
                                error);
    if (status == ORM_STATUS_OK)
      status = orm_redis_append(&rebuilt, ":[", 2u, maximum, error);
    if (status == ORM_STATUS_OK)
      status = orm_redis_append(&rebuilt, encoded, tstr_len(encoded), maximum,
                                error);
    if (status == ORM_STATUS_OK)
      status = orm_redis_append(&rebuilt, " ", 1u, maximum, error);
    if (status == ORM_STATUS_OK)
      status = orm_redis_append(&rebuilt, encoded, tstr_len(encoded), maximum,
                                error);
    if (status == ORM_STATUS_OK)
      status = orm_redis_append(&rebuilt, "])", 2u, maximum, error);
    tstr_free(field);
    tstr_free(encoded);
    if (status != ORM_STATUS_OK) {
      tstr_free(rebuilt);
      return status;
    }
    tstr_free(*query);
    *query = rebuilt;
    return ORM_STATUS_OK;
  }
  switch (predicate->comparison) {
    case ORM_COMPARE_EQUAL: status = orm_redis_append(query, ":[", 2u, maximum, error); break;
    case ORM_COMPARE_LESS: status = orm_redis_append(query, ":[-inf (", 8u, maximum, error); break;
    case ORM_COMPARE_LESS_EQUAL: status = orm_redis_append(query, ":[-inf ", 7u, maximum, error); break;
    case ORM_COMPARE_GREATER: status = orm_redis_append(query, ":[(", 3u, maximum, error); break;
    case ORM_COMPARE_GREATER_EQUAL: status = orm_redis_append(query, ":[", 2u, maximum, error); break;
    default:
      tstr_free(encoded);
      return orm_redis_query_fail(error, ORM_STATUS_UNSUPPORTED,
                                  "Redis numeric predicate comparison is unsupported");
  }
  if (status == ORM_STATUS_OK)
    status = orm_redis_append(query, encoded, tstr_len(encoded), maximum, error);
  if (status == ORM_STATUS_OK) {
    const char *suffix =
        predicate->comparison == ORM_COMPARE_EQUAL ? " " :
        predicate->comparison == ORM_COMPARE_GREATER ||
        predicate->comparison == ORM_COMPARE_GREATER_EQUAL ? " +inf" : "]";
    status = orm_redis_append(query, suffix, strlen(suffix), maximum, error);
  }
  if (status == ORM_STATUS_OK &&
      (predicate->comparison == ORM_COMPARE_EQUAL ||
       predicate->comparison == ORM_COMPARE_GREATER ||
       predicate->comparison == ORM_COMPARE_GREATER_EQUAL))
    status = orm_redis_append(query,
        predicate->comparison == ORM_COMPARE_EQUAL ? encoded : "]",
        predicate->comparison == ORM_COMPARE_EQUAL ? tstr_len(encoded) : 1u,
        maximum, error);
  if (status == ORM_STATUS_OK && predicate->comparison == ORM_COMPARE_EQUAL)
    status = orm_redis_append(query, "]", 1u, maximum, error);
  tstr_free(encoded);
  return status;
}

static orm_status_t orm_redis_render_filter(const orm_query_plan *plan,
                                            const orm_limits *limits,
                                            tstr *out,
                                            orm_error_t *error) {
  size_t index;
  orm_status_t status;
  if (vec_size(&plan->predicates) == 0u) {
    *out = tstr_dup("*");
    return *out != NULL ? ORM_STATUS_OK
                        : orm_redis_query_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                                               "allocate Redis query");
  }
  *out = tstr_dup("(");
  if (*out == NULL) return orm_redis_query_fail(
      error, ORM_STATUS_OUT_OF_MEMORY, "allocate Redis query");
  for (index = 0u; index < vec_size(&plan->predicates); ++index) {
    tstr predicate_text = tstr_dup("");
    const orm_predicate *predicate =
        (const orm_predicate *)vec_at_const(&plan->predicates, index);
    if (predicate_text == NULL) {
      status = ORM_STATUS_OUT_OF_MEMORY;
      orm_error_set(error, status, "allocate Redis predicate");
      goto fail;
    }
    status = index == 0u ? ORM_STATUS_OK
                         : orm_redis_append(out, " ", 1u,
                                            limits->max_query_bytes, error);
    if (status == ORM_STATUS_OK)
      status = orm_redis_render_predicate(&predicate_text, predicate,
                                          limits->max_query_bytes, error);
    if (status == ORM_STATUS_OK)
      status = orm_redis_append(out, predicate_text, tstr_len(predicate_text),
                                limits->max_query_bytes, error);
    tstr_free(predicate_text);
    if (status != ORM_STATUS_OK) goto fail;
  }
  status = orm_redis_append(out, ")", 1u, limits->max_query_bytes, error);
  if (status == ORM_STATUS_OK) return status;
fail:
  tstr_freep(out);
  return status;
}

orm_status_t orm_redis_query_build(const orm_query_plan *plan,
                                   const orm_limits *limits,
                                   vstr index_prefix,
                                   orm_redis_query *out,
                                   orm_error_t *error) {
  size_t maximum_arguments;
  size_t index;
  uint64_t count;
  tstr filter = NULL;
  tstr index_name = NULL;
  char number[32];
  orm_status_t status;
  if (plan == NULL || limits == NULL || out == NULL ||
      plan->kind != ORM_QUERY_SELECT || !orm_view_valid(index_prefix, false))
    return orm_redis_query_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                                "invalid Redis query request");
  memset(out, 0, sizeof(*out));
  if (plan->select_all || vec_size(&plan->columns) == 0u)
    return orm_redis_query_fail(error, ORM_STATUS_UNSUPPORTED,
                                "Redis SELECT requires an explicit projection");
  if (limits->max_predicates > (SIZE_MAX - ORM_REDIS_QUERY_BASE_ARGUMENTS -
                                limits->max_columns) / 2u)
    return orm_redis_query_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                                "Redis command argument bound overflows");
  maximum_arguments = ORM_REDIS_QUERY_BASE_ARGUMENTS + limits->max_columns +
                      limits->max_predicates * 2u;
  if (vec_init_bytes(&out->arguments, sizeof(tstr), _Alignof(tstr),
                     maximum_arguments) != STL_OK ||
      vec_init_bytes(&out->output_columns, sizeof(tstr), _Alignof(tstr),
                     limits->max_columns) != STL_OK) {
    orm_redis_query_destroy(out);
    return orm_redis_query_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                                "initialize Redis query");
  }
  index_name = tstr_from_v(index_prefix);
  if (index_name != NULL) index_name = tstr_cat(index_name, plan->table);
  status = index_name != NULL ? ORM_STATUS_OK : ORM_STATUS_OUT_OF_MEMORY;
  if (status != ORM_STATUS_OK)
    orm_error_set(error, status, "build Redis index name");
  if (status == ORM_STATUS_OK)
    status = orm_redis_render_filter(plan, limits, &filter, error);
  if (status == ORM_STATUS_OK) status = orm_redis_push_cstr(&out->arguments, "FT.SEARCH", error);
  if (status == ORM_STATUS_OK) {
    status = orm_redis_push_owned(&out->arguments, index_name, error,
                                  "append Redis index name");
    if (status == ORM_STATUS_OK) index_name = NULL;
  }
  if (status == ORM_STATUS_OK) {
    status = orm_redis_push_owned(&out->arguments, filter, error,
                                  "append Redis query filter");
    if (status == ORM_STATUS_OK) filter = NULL;
  }
  if (status == ORM_STATUS_OK) status = orm_redis_push_cstr(&out->arguments, "RETURN", error);
  if (status == ORM_STATUS_OK) {
    (void)snprintf(number, sizeof(number), "%zu", vec_size(&plan->columns));
    status = orm_redis_push_cstr(&out->arguments, number, error);
  }
  for (index = 0u; status == ORM_STATUS_OK && index < vec_size(&plan->columns); ++index) {
    const tstr *column = (const tstr *)vec_at_const(&plan->columns, index);
    status = orm_redis_push_view(&out->arguments, tstr_to_v(*column), error);
    if (status == ORM_STATUS_OK)
      status = orm_redis_push_view(&out->output_columns, tstr_to_v(*column), error);
  }
  if (status == ORM_STATUS_OK && plan->ordering.present) {
    status = orm_redis_push_cstr(&out->arguments, "SORTBY", error);
    if (status == ORM_STATUS_OK)
      status = orm_redis_push_view(&out->arguments, tstr_to_v(plan->ordering.column), error);
    if (status == ORM_STATUS_OK)
      status = orm_redis_push_cstr(&out->arguments,
          plan->ordering.order == ORM_ORDER_DESCENDING ? "DESC" : "ASC", error);
  }
  count = plan->has_limit ? plan->limit : limits->max_result_rows;
  if (status == ORM_STATUS_OK && count > limits->max_result_rows)
    status = orm_redis_query_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                                  "Redis query limit exceeds max_result_rows");
  if (status == ORM_STATUS_OK) status = orm_redis_push_cstr(&out->arguments, "LIMIT", error);
  if (status == ORM_STATUS_OK) {
    (void)snprintf(number, sizeof(number), "%" PRIu64,
                   plan->has_offset ? plan->offset : 0u);
    status = orm_redis_push_cstr(&out->arguments, number, error);
  }
  if (status == ORM_STATUS_OK) {
    (void)snprintf(number, sizeof(number), "%" PRIu64, count);
    status = orm_redis_push_cstr(&out->arguments, number, error);
  }
  if (status == ORM_STATUS_OK) status = orm_redis_push_cstr(&out->arguments, "DIALECT", error);
  if (status == ORM_STATUS_OK) status = orm_redis_push_cstr(&out->arguments, "2", error);
  tstr_free(index_name);
  tstr_free(filter);
  if (status != ORM_STATUS_OK) orm_redis_query_destroy(out);
  return status;
}

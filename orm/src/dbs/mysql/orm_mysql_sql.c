#include "orm_mysql_sql.h"

#include <stdlib.h>
#include <string.h>

typedef enum orm_mysql_scan_state {
  ORM_MYSQL_SCAN_NORMAL = 0,
  ORM_MYSQL_SCAN_SINGLE_QUOTE,
  ORM_MYSQL_SCAN_DOUBLE_QUOTE,
  ORM_MYSQL_SCAN_BACKTICK,
  ORM_MYSQL_SCAN_LINE_COMMENT,
  ORM_MYSQL_SCAN_BLOCK_COMMENT
} orm_mysql_scan_state;

typedef enum orm_mysql_placeholder_mode {
  ORM_MYSQL_PLACEHOLDER_NONE = 0,
  ORM_MYSQL_PLACEHOLDER_PORTABLE,
  ORM_MYSQL_PLACEHOLDER_NATIVE
} orm_mysql_placeholder_mode;

static orm_status_t orm_mysql_sql_append(
    orm_sql_query *query, const orm_limits *limits,
    const char *bytes, size_t size, orm_error_t *error) {
  tstr next;
  if (query == NULL || limits == NULL ||
      (size != 0u && bytes == NULL)) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid MySQL SQL fragment");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  if (size > limits->max_query_bytes ||
      tstr_len(query->text) > limits->max_query_bytes - size) {
    orm_error_set(error, ORM_STATUS_LIMIT_EXCEEDED,
                  "MySQL SQL exceeds max_query_bytes");
    return ORM_STATUS_LIMIT_EXCEEDED;
  }
  next = tstr_cat_len(query->text, bytes, size);
  if (next == NULL) {
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "append MySQL SQL fragment");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  query->text = next;
  return ORM_STATUS_OK;
}

static int orm_mysql_line_comment(
    const char *sql, size_t size, size_t offset) {
  unsigned char after;
  if (offset + 1u >= size || sql[offset] != '-' || sql[offset + 1u] != '-')
    return 0;
  if (offset + 2u >= size)
    return 1;
  after = (unsigned char)sql[offset + 2u];
  return after <= (unsigned char)' ';
}

static orm_status_t orm_mysql_push_parameter(
    orm_sql_query *query, const orm_limits *limits,
    const orm_owned_value *value, orm_error_t *error) {
  const stl_status pushed = vec_push(&query->parameters, &value);
  if (pushed == STL_OK)
    return ORM_STATUS_OK;
  if (pushed == STL_CAPACITY_EXCEEDED) {
    orm_error_set(error, ORM_STATUS_LIMIT_EXCEEDED,
                  "MySQL positional parameter count exceeds limits");
    return ORM_STATUS_LIMIT_EXCEEDED;
  }
  orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                "append MySQL positional parameter");
  return ORM_STATUS_OUT_OF_MEMORY;
}

static orm_status_t orm_mysql_lower_placeholders(
    const orm_sql_query *portable, const orm_limits *limits,
    orm_sql_query *out, orm_error_t *error) {
  orm_mysql_scan_state state = ORM_MYSQL_SCAN_NORMAL;
  orm_mysql_placeholder_mode mode = ORM_MYSQL_PLACEHOLDER_NONE;
  unsigned char *portable_seen = NULL;
  size_t native_index = 0u;
  size_t copy_start = 0u;
  size_t index = 0u;
  const size_t sql_size = tstr_len(portable->text);
  const size_t parameter_count = vec_size(&portable->parameters);
  orm_status_t status = ORM_STATUS_OK;

  memset(out, 0, sizeof(*out));
  if (vec_init_bytes(&out->parameters,
                     sizeof(const orm_owned_value *),
                     _Alignof(const orm_owned_value *),
                     limits->max_parameters) != STL_OK) {
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "initialize MySQL parameter order");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  out->text = tstr_new();
  if (out->text == NULL) {
    orm_sql_query_destroy(out);
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "initialize MySQL SQL text");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  if (parameter_count != 0u) {
    portable_seen =
        (unsigned char *)calloc(parameter_count, sizeof(*portable_seen));
    if (portable_seen == NULL) {
      orm_sql_query_destroy(out);
      orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                    "track MySQL portable parameters");
      return ORM_STATUS_OUT_OF_MEMORY;
    }
  }

  while (index < sql_size && status == ORM_STATUS_OK) {
    const char next = portable->text[index];
    const char after =
        index + 1u < sql_size ? portable->text[index + 1u] : '\0';
    switch (state) {
      case ORM_MYSQL_SCAN_NORMAL:
        if (next == '\'') {
          state = ORM_MYSQL_SCAN_SINGLE_QUOTE;
        } else if (next == '"') {
          state = ORM_MYSQL_SCAN_DOUBLE_QUOTE;
        } else if (next == '`') {
          state = ORM_MYSQL_SCAN_BACKTICK;
        } else if (next == '#') {
          state = ORM_MYSQL_SCAN_LINE_COMMENT;
        } else if (orm_mysql_line_comment(portable->text, sql_size, index)) {
          state = ORM_MYSQL_SCAN_LINE_COMMENT;
          ++index;
        } else if (next == '/' && after == '*') {
          state = ORM_MYSQL_SCAN_BLOCK_COMMENT;
          ++index;
        } else if (next == '?') {
          size_t end = index + 1u;
          size_t one_based = 0u;
          const orm_owned_value *value = NULL;

          status = orm_mysql_sql_append(
              out, limits, portable->text + copy_start,
              index - copy_start, error);
          if (status != ORM_STATUS_OK)
            break;

          if (end < sql_size &&
              portable->text[end] >= '1' && portable->text[end] <= '9') {
            if (mode == ORM_MYSQL_PLACEHOLDER_NATIVE) {
              orm_error_set(
                  error, ORM_STATUS_INVALID_ARGUMENT,
                  "MySQL raw SQL cannot mix ?N and native ? placeholders");
              status = ORM_STATUS_INVALID_ARGUMENT;
              break;
            }
            mode = ORM_MYSQL_PLACEHOLDER_PORTABLE;
            while (end < sql_size &&
                   portable->text[end] >= '0' &&
                   portable->text[end] <= '9') {
              const size_t digit =
                  (size_t)(portable->text[end] - '0');
              if (one_based > (SIZE_MAX - digit) / 10u) {
                orm_error_set(error, ORM_STATUS_OUT_OF_RANGE,
                              "MySQL placeholder index overflow");
                status = ORM_STATUS_OUT_OF_RANGE;
                break;
              }
              one_based = one_based * 10u + digit;
              ++end;
            }
            if (status != ORM_STATUS_OK)
              break;
            if (one_based == 0u || one_based > parameter_count) {
              orm_error_set(
                  error, ORM_STATUS_INVALID_ARGUMENT,
                  "MySQL ?N placeholder has no matching bind");
              status = ORM_STATUS_INVALID_ARGUMENT;
              break;
            }
            value = *(const orm_owned_value *const *)
                vec_at_const(&portable->parameters, one_based - 1u);
            portable_seen[one_based - 1u] = 1u;
          } else {
            if (mode == ORM_MYSQL_PLACEHOLDER_PORTABLE) {
              orm_error_set(
                  error, ORM_STATUS_INVALID_ARGUMENT,
                  "MySQL raw SQL cannot mix ?N and native ? placeholders");
              status = ORM_STATUS_INVALID_ARGUMENT;
              break;
            }
            mode = ORM_MYSQL_PLACEHOLDER_NATIVE;
            if (native_index >= parameter_count) {
              orm_error_set(
                  error, ORM_STATUS_INVALID_ARGUMENT,
                  "MySQL native placeholder has no matching bind");
              status = ORM_STATUS_INVALID_ARGUMENT;
              break;
            }
            value = *(const orm_owned_value *const *)
                vec_at_const(&portable->parameters, native_index++);
          }

          if (value == NULL) {
            orm_error_set(error, ORM_STATUS_INTERNAL_ERROR,
                          "MySQL parameter storage is invalid");
            status = ORM_STATUS_INTERNAL_ERROR;
            break;
          }
          status = orm_mysql_sql_append(out, limits, "?", 1u, error);
          if (status == ORM_STATUS_OK)
            status = orm_mysql_push_parameter(out, limits, value, error);
          if (status != ORM_STATUS_OK)
            break;
          if (mode == ORM_MYSQL_PLACEHOLDER_PORTABLE) {
            index = end - 1u;
            copy_start = end;
          } else {
            copy_start = index + 1u;
          }
        }
        break;
      case ORM_MYSQL_SCAN_SINGLE_QUOTE:
        if (next == '\\' && after != '\0')
          ++index;
        else if (next == '\'' && after == '\'')
          ++index;
        else if (next == '\'')
          state = ORM_MYSQL_SCAN_NORMAL;
        break;
      case ORM_MYSQL_SCAN_DOUBLE_QUOTE:
        if (next == '\\' && after != '\0')
          ++index;
        else if (next == '"' && after == '"')
          ++index;
        else if (next == '"')
          state = ORM_MYSQL_SCAN_NORMAL;
        break;
      case ORM_MYSQL_SCAN_BACKTICK:
        if (next == '`' && after == '`')
          ++index;
        else if (next == '`')
          state = ORM_MYSQL_SCAN_NORMAL;
        break;
      case ORM_MYSQL_SCAN_LINE_COMMENT:
        if (next == '\n' || next == '\r')
          state = ORM_MYSQL_SCAN_NORMAL;
        break;
      case ORM_MYSQL_SCAN_BLOCK_COMMENT:
        if (next == '*' && after == '/') {
          ++index;
          state = ORM_MYSQL_SCAN_NORMAL;
        }
        break;
      default:
        status = ORM_STATUS_INTERNAL_ERROR;
        break;
    }
    ++index;
  }

  if (status == ORM_STATUS_OK)
    status = orm_mysql_sql_append(
        out, limits, portable->text + copy_start,
        sql_size - copy_start, error);

  if (status == ORM_STATUS_OK && mode == ORM_MYSQL_PLACEHOLDER_NATIVE &&
      native_index != parameter_count) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "MySQL native placeholder count does not match binds");
    status = ORM_STATUS_INVALID_ARGUMENT;
  }
  if (status == ORM_STATUS_OK && mode == ORM_MYSQL_PLACEHOLDER_PORTABLE) {
    size_t parameter;
    for (parameter = 0u; parameter < parameter_count; ++parameter) {
      if (portable_seen[parameter] == 0u) {
        orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                      "MySQL portable bind is not referenced by SQL");
        status = ORM_STATUS_INVALID_ARGUMENT;
        break;
      }
    }
  }
  if (status == ORM_STATUS_OK &&
      mode == ORM_MYSQL_PLACEHOLDER_NONE &&
      parameter_count != 0u) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "MySQL SQL has binds but no placeholders");
    status = ORM_STATUS_INVALID_ARGUMENT;
  }

  free(portable_seen);
  if (status != ORM_STATUS_OK)
    orm_sql_query_destroy(out);
  return status;
}

orm_status_t orm_mysql_sql_render(
    const orm_query_plan *plan,
    const orm_limits *limits,
    orm_sql_query *out_query,
    orm_error_t *error) {
  orm_query_plan projected;
  orm_sql_query portable;
  orm_status_t status;

  if (plan == NULL || limits == NULL || out_query == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid MySQL SQL render request");
    return ORM_STATUS_INVALID_ARGUMENT;
  }

  projected = *plan;
  if (projected.kind == ORM_QUERY_SELECT &&
      projected.has_offset && !projected.has_limit) {
    projected.has_limit = true;
    projected.limit = UINT64_MAX;
  }

  status = orm_sql_render(
      &projected, limits, ORM_SQL_SQLITE, &portable, error);
  if (status != ORM_STATUS_OK)
    return status;

  status = orm_mysql_lower_placeholders(
      &portable, limits, out_query, error);
  orm_sql_query_destroy(&portable);
  if (status == ORM_STATUS_OK)
    orm_error_set(error, ORM_STATUS_OK, NULL);
  return status;
}

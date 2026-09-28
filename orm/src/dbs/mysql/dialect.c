#include "dialect.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static orm_status_t mysql_dialect_fail(
    orm_error_t *error, orm_status_t status, const char *message) {
  orm_error_set(error, status, message);
  return status;
}

static orm_status_t mysql_dialect_append(
    mysql_dialect_query_t *query, const orm_limits *limits,
    const char *bytes, size_t size, orm_error_t *error) {
  tstr next;
  if (query == NULL || limits == NULL ||
      (bytes == NULL && size != 0u))
    return mysql_dialect_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "invalid MySQL dialect fragment");
  if (size > limits->max_query_bytes ||
      tstr_len(query->sql) > limits->max_query_bytes - size)
    return mysql_dialect_fail(
        error, ORM_STATUS_LIMIT_EXCEEDED,
        "MySQL structured query exceeds max_query_bytes");
  next = tstr_cat_len(query->sql, bytes, size);
  if (next == NULL)
    return mysql_dialect_fail(
        error, ORM_STATUS_OUT_OF_MEMORY,
        "append MySQL dialect fragment");
  query->sql = next;
  return ORM_STATUS_OK;
}

static orm_status_t mysql_dialect_append_cstr(
    mysql_dialect_query_t *query, const orm_limits *limits,
    const char *text, orm_error_t *error) {
  return mysql_dialect_append(
      query, limits, text, strlen(text), error);
}

static orm_status_t mysql_dialect_append_u64(
    mysql_dialect_query_t *query, const orm_limits *limits,
    uint64_t value, orm_error_t *error) {
  char buffer[32];
  const int length =
      snprintf(buffer, sizeof(buffer), "%" PRIu64, value);
  if (length <= 0 || (size_t)length >= sizeof(buffer))
    return mysql_dialect_fail(
        error, ORM_STATUS_INTERNAL_ERROR,
        "format MySQL integer failed");
  return mysql_dialect_append(
      query, limits, buffer, (size_t)length, error);
}

static orm_status_t mysql_dialect_identifier(
    mysql_dialect_query_t *query, const orm_limits *limits,
    tstr identifier, orm_error_t *error) {
  const size_t size = tstr_len(identifier);
  size_t begin = 0u;
  size_t i;
  orm_status_t status;

  if (identifier == NULL || size == 0u)
    return mysql_dialect_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "invalid MySQL identifier");

  for (i = 0u; i <= size; ++i) {
    if (i == size || identifier[i] == '.') {
      if (i == begin)
        return mysql_dialect_fail(
            error, ORM_STATUS_INVALID_ARGUMENT,
            "invalid qualified MySQL identifier");
      status = mysql_dialect_append_cstr(
          query, limits, "`", error);
      if (status == ORM_STATUS_OK)
        status = mysql_dialect_append(
            query, limits, identifier + begin, i - begin, error);
      if (status == ORM_STATUS_OK)
        status = mysql_dialect_append_cstr(
            query, limits, "`", error);
      if (status != ORM_STATUS_OK)
        return status;
      if (i != size) {
        status = mysql_dialect_append_cstr(
            query, limits, ".", error);
        if (status != ORM_STATUS_OK)
          return status;
      }
      begin = i + 1u;
    }
  }
  return ORM_STATUS_OK;
}

static orm_status_t mysql_dialect_parameter(
    mysql_dialect_query_t *query, const orm_limits *limits,
    const orm_owned_value *value, orm_error_t *error) {
  stl_status pushed;
  if (value == NULL)
    return mysql_dialect_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "invalid MySQL structured parameter");
  pushed = vec_push(&query->parameters, &value);
  if (pushed != STL_OK)
    return mysql_dialect_fail(
        error,
        pushed == STL_CAPACITY_EXCEEDED
            ? ORM_STATUS_LIMIT_EXCEEDED
            : ORM_STATUS_OUT_OF_MEMORY,
        "append MySQL structured parameter");
  return mysql_dialect_append_cstr(
      query, limits, "?", error);
}

static const char *mysql_dialect_comparison(
    orm_compare_t comparison) {
  switch (comparison) {
    case ORM_COMPARE_EQUAL: return " = ";
    case ORM_COMPARE_NOT_EQUAL: return " != ";
    case ORM_COMPARE_LESS: return " < ";
    case ORM_COMPARE_LESS_EQUAL: return " <= ";
    case ORM_COMPARE_GREATER: return " > ";
    case ORM_COMPARE_GREATER_EQUAL: return " >= ";
    case ORM_COMPARE_LIKE: return " LIKE ";
    case ORM_COMPARE_NOT_LIKE: return " NOT LIKE ";
    default: return NULL;
  }
}

static orm_status_t mysql_dialect_predicates(
    const orm_query_plan *plan, const orm_limits *limits,
    mysql_dialect_query_t *query, orm_error_t *error) {
  size_t i;
  orm_status_t status;
  if (vec_size(&plan->predicates) == 0u)
    return ORM_STATUS_OK;

  status = mysql_dialect_append_cstr(
      query, limits, " WHERE ", error);
  for (i = 0u; status == ORM_STATUS_OK &&
                   i < vec_size(&plan->predicates); ++i) {
    const orm_predicate *predicate =
        (const orm_predicate *)vec_at_const(
            &plan->predicates, i);
    const char *comparison;
    if (predicate == NULL)
      return mysql_dialect_fail(
          error, ORM_STATUS_INTERNAL_ERROR,
          "invalid MySQL predicate storage");
    if (i != 0u)
      status = mysql_dialect_append_cstr(
          query, limits, " AND ", error);
    if (status == ORM_STATUS_OK)
      status = mysql_dialect_identifier(
          query, limits, predicate->column, error);
    if (status != ORM_STATUS_OK)
      return status;

    if (predicate->value.kind == ORM_VALUE_NULL) {
      comparison =
          predicate->comparison == ORM_COMPARE_EQUAL
              ? " IS NULL"
              : (predicate->comparison == ORM_COMPARE_NOT_EQUAL
                     ? " IS NOT NULL"
                     : NULL);
      if (comparison == NULL)
        return mysql_dialect_fail(
            error, ORM_STATUS_INVALID_ARGUMENT,
            "only equality comparisons accept a null MySQL predicate");
      status = mysql_dialect_append_cstr(
          query, limits, comparison, error);
    } else {
      comparison =
          mysql_dialect_comparison(predicate->comparison);
      if (comparison == NULL)
        return mysql_dialect_fail(
            error, ORM_STATUS_INVALID_ARGUMENT,
            "unknown MySQL comparison");
      status = mysql_dialect_append_cstr(
          query, limits, comparison, error);
      if (status == ORM_STATUS_OK)
        status = mysql_dialect_parameter(
            query, limits, &predicate->value, error);
    }
  }
  return status;
}

static orm_status_t mysql_dialect_select(
    const orm_query_plan *plan, const orm_limits *limits,
    mysql_dialect_query_t *query, orm_error_t *error) {
  size_t i;
  orm_status_t status;

  if (!plan->select_all && vec_size(&plan->columns) == 0u)
    return mysql_dialect_fail(
        error, ORM_STATUS_INVALID_STATE,
        "MySQL SELECT has no projected columns");

  status = mysql_dialect_append_cstr(
      query, limits, "SELECT ", error);
  if (status == ORM_STATUS_OK && plan->select_all) {
    status = mysql_dialect_append_cstr(
        query, limits, "*", error);
  } else {
    for (i = 0u; status == ORM_STATUS_OK &&
                     i < vec_size(&plan->columns); ++i) {
      const tstr *column =
          (const tstr *)vec_at_const(&plan->columns, i);
      if (column == NULL)
        return mysql_dialect_fail(
            error, ORM_STATUS_INTERNAL_ERROR,
            "invalid MySQL projection storage");
      if (i != 0u)
        status = mysql_dialect_append_cstr(
            query, limits, ", ", error);
      if (status == ORM_STATUS_OK)
        status = mysql_dialect_identifier(
            query, limits, *column, error);
    }
  }

  if (status == ORM_STATUS_OK)
    status = mysql_dialect_append_cstr(
        query, limits, " FROM ", error);
  if (status == ORM_STATUS_OK)
    status = mysql_dialect_identifier(
        query, limits, plan->table, error);
  if (status == ORM_STATUS_OK)
    status = mysql_dialect_predicates(
        plan, limits, query, error);

  if (status == ORM_STATUS_OK && plan->ordering.present) {
    if (plan->ordering.order != ORM_ORDER_ASCENDING &&
        plan->ordering.order != ORM_ORDER_DESCENDING)
      return mysql_dialect_fail(
          error, ORM_STATUS_INVALID_ARGUMENT,
          "invalid MySQL ordering");
    status = mysql_dialect_append_cstr(
        query, limits, " ORDER BY ", error);
    if (status == ORM_STATUS_OK)
      status = mysql_dialect_identifier(
          query, limits, plan->ordering.column, error);
    if (status == ORM_STATUS_OK)
      status = mysql_dialect_append_cstr(
          query, limits,
          plan->ordering.order == ORM_ORDER_DESCENDING
              ? " DESC" : " ASC",
          error);
  }

  if (status == ORM_STATUS_OK && plan->has_limit) {
    status = mysql_dialect_append_cstr(
        query, limits, " LIMIT ", error);
    if (status == ORM_STATUS_OK)
      status = mysql_dialect_append_u64(
          query, limits, plan->limit, error);
  } else if (status == ORM_STATUS_OK && plan->has_offset) {
    status = mysql_dialect_append_cstr(
        query, limits,
        " LIMIT 18446744073709551615", error);
  }

  if (status == ORM_STATUS_OK && plan->has_offset) {
    status = mysql_dialect_append_cstr(
        query, limits, " OFFSET ", error);
    if (status == ORM_STATUS_OK)
      status = mysql_dialect_append_u64(
          query, limits, plan->offset, error);
  }
  return status;
}

static orm_status_t mysql_dialect_insert(
    const orm_query_plan *plan, const orm_limits *limits,
    mysql_dialect_query_t *query, orm_error_t *error) {
  size_t i;
  orm_status_t status;

  if (vec_size(&plan->assignments) == 0u)
    return mysql_dialect_fail(
        error, ORM_STATUS_INVALID_STATE,
        "MySQL INSERT has no assignments");

  status = mysql_dialect_append_cstr(
      query, limits, "INSERT INTO ", error);
  if (status == ORM_STATUS_OK)
    status = mysql_dialect_identifier(
        query, limits, plan->table, error);
  if (status == ORM_STATUS_OK)
    status = mysql_dialect_append_cstr(
        query, limits, " (", error);

  for (i = 0u; status == ORM_STATUS_OK &&
                   i < vec_size(&plan->assignments); ++i) {
    const orm_assignment *assignment =
        (const orm_assignment *)vec_at_const(
            &plan->assignments, i);
    if (assignment == NULL)
      return mysql_dialect_fail(
          error, ORM_STATUS_INTERNAL_ERROR,
          "invalid MySQL assignment storage");
    if (i != 0u)
      status = mysql_dialect_append_cstr(
          query, limits, ", ", error);
    if (status == ORM_STATUS_OK)
      status = mysql_dialect_identifier(
          query, limits, assignment->column, error);
  }

  if (status == ORM_STATUS_OK)
    status = mysql_dialect_append_cstr(
        query, limits, ") VALUES (", error);
  for (i = 0u; status == ORM_STATUS_OK &&
                   i < vec_size(&plan->assignments); ++i) {
    const orm_assignment *assignment =
        (const orm_assignment *)vec_at_const(
            &plan->assignments, i);
    if (i != 0u)
      status = mysql_dialect_append_cstr(
          query, limits, ", ", error);
    if (status != ORM_STATUS_OK)
      break;
    status = assignment->value.kind == ORM_VALUE_NULL
                 ? mysql_dialect_append_cstr(
                       query, limits, "NULL", error)
                 : mysql_dialect_parameter(
                       query, limits, &assignment->value, error);
  }
  if (status == ORM_STATUS_OK)
    status = mysql_dialect_append_cstr(
        query, limits, ")", error);
  return status;
}

static orm_status_t mysql_dialect_update(
    const orm_query_plan *plan, const orm_limits *limits,
    mysql_dialect_query_t *query, orm_error_t *error) {
  size_t i;
  orm_status_t status;

  if (vec_size(&plan->assignments) == 0u)
    return mysql_dialect_fail(
        error, ORM_STATUS_INVALID_STATE,
        "MySQL UPDATE has no assignments");

  status = mysql_dialect_append_cstr(
      query, limits, "UPDATE ", error);
  if (status == ORM_STATUS_OK)
    status = mysql_dialect_identifier(
        query, limits, plan->table, error);
  if (status == ORM_STATUS_OK)
    status = mysql_dialect_append_cstr(
        query, limits, " SET ", error);

  for (i = 0u; status == ORM_STATUS_OK &&
                   i < vec_size(&plan->assignments); ++i) {
    const orm_assignment *assignment =
        (const orm_assignment *)vec_at_const(
            &plan->assignments, i);
    if (assignment == NULL)
      return mysql_dialect_fail(
          error, ORM_STATUS_INTERNAL_ERROR,
          "invalid MySQL assignment storage");
    if (i != 0u)
      status = mysql_dialect_append_cstr(
          query, limits, ", ", error);
    if (status == ORM_STATUS_OK)
      status = mysql_dialect_identifier(
          query, limits, assignment->column, error);
    if (status == ORM_STATUS_OK)
      status = mysql_dialect_append_cstr(
          query, limits, " = ", error);
    if (status == ORM_STATUS_OK)
      status = assignment->value.kind == ORM_VALUE_NULL
                   ? mysql_dialect_append_cstr(
                         query, limits, "NULL", error)
                   : mysql_dialect_parameter(
                         query, limits, &assignment->value, error);
  }
  if (status == ORM_STATUS_OK)
    status = mysql_dialect_predicates(
        plan, limits, query, error);
  return status;
}

static orm_status_t mysql_dialect_delete(
    const orm_query_plan *plan, const orm_limits *limits,
    mysql_dialect_query_t *query, orm_error_t *error) {
  orm_status_t status = mysql_dialect_append_cstr(
      query, limits, "DELETE FROM ", error);
  if (status == ORM_STATUS_OK)
    status = mysql_dialect_identifier(
        query, limits, plan->table, error);
  if (status == ORM_STATUS_OK)
    status = mysql_dialect_predicates(
        plan, limits, query, error);
  return status;
}

orm_status_t mysql_dialect_render(
    const orm_query_plan *plan, const orm_limits *limits,
    mysql_dialect_query_t *out, orm_error_t *error) {
  orm_status_t status;

  if (out != NULL)
    memset(out, 0, sizeof(*out));
  if (plan == NULL || limits == NULL || out == NULL ||
      plan->kind == ORM_QUERY_RAW)
    return mysql_dialect_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "invalid MySQL structured render request");

  if (vec_init_bytes(
          &out->parameters,
          sizeof(const orm_owned_value *),
          _Alignof(const orm_owned_value *),
          limits->max_parameters) != STL_OK)
    return mysql_dialect_fail(
        error, ORM_STATUS_OUT_OF_MEMORY,
        "initialize MySQL structured parameter view");

  out->sql = tstr_new();
  if (out->sql == NULL) {
    mysql_dialect_query_destroy(out);
    return mysql_dialect_fail(
        error, ORM_STATUS_OUT_OF_MEMORY,
        "initialize MySQL structured SQL");
  }

  switch (plan->kind) {
    case ORM_QUERY_SELECT:
      status = mysql_dialect_select(
          plan, limits, out, error);
      break;
    case ORM_QUERY_INSERT:
      status = mysql_dialect_insert(
          plan, limits, out, error);
      break;
    case ORM_QUERY_UPDATE:
      status = mysql_dialect_update(
          plan, limits, out, error);
      break;
    case ORM_QUERY_DELETE:
      status = mysql_dialect_delete(
          plan, limits, out, error);
      break;
    default:
      status = mysql_dialect_fail(
          error, ORM_STATUS_INVALID_ARGUMENT,
          "unknown MySQL structured query kind");
      break;
  }

  if (status != ORM_STATUS_OK) {
    mysql_dialect_query_destroy(out);
    return status;
  }
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

void mysql_dialect_query_destroy(
    mysql_dialect_query_t *query) {
  if (query == NULL)
    return;
  tstr_freep(&query->sql);
  vec_destroy(&query->parameters);
  memset(query, 0, sizeof(*query));
}

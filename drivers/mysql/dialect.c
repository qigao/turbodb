#include "dialect.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static orm_status_t mysql_render_fail(
    orm_error_t *error, orm_status_t status, const char *message) {
  orm_error_set(error, status, message);
  return status;
}

static orm_status_t mysql_append(
    orm_mysql_rendered_query *query, const orm_limits *limits,
    const char *bytes, size_t size, orm_error_t *error) {
  tstr next;
  if (query == NULL || limits == NULL ||
      (size != 0u && bytes == NULL))
    return mysql_render_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "invalid MySQL render fragment");
  if (size > limits->max_query_bytes ||
      tstr_len(query->text) > limits->max_query_bytes - size)
    return mysql_render_fail(
        error, ORM_STATUS_LIMIT_EXCEEDED,
        "MySQL query exceeds max_query_bytes");
  next = tstr_cat_len(query->text, bytes, size);
  if (next == NULL)
    return mysql_render_fail(
        error, ORM_STATUS_OUT_OF_MEMORY,
        "append MySQL query fragment");
  query->text = next;
  return ORM_STATUS_OK;
}

static orm_status_t mysql_append_cstr(
    orm_mysql_rendered_query *query, const orm_limits *limits,
    const char *text, orm_error_t *error) {
  return mysql_append(query, limits, text, strlen(text), error);
}

static orm_status_t mysql_append_tstr(
    orm_mysql_rendered_query *query, const orm_limits *limits,
    tstr text, orm_error_t *error) {
  return mysql_append(query, limits, text, tstr_len(text), error);
}

static orm_status_t mysql_append_u64(
    orm_mysql_rendered_query *query, const orm_limits *limits,
    uint64_t value, orm_error_t *error) {
  char buffer[32];
  const int length =
      snprintf(buffer, sizeof(buffer), "%" PRIu64, value);
  if (length <= 0 || (size_t)length >= sizeof(buffer))
    return mysql_render_fail(
        error, ORM_STATUS_INTERNAL_ERROR,
        "format MySQL integer failed");
  return mysql_append(
      query, limits, buffer, (size_t)length, error);
}

static orm_status_t mysql_push_parameter(
    orm_mysql_rendered_query *query, const orm_limits *limits,
    const orm_owned_value *value, orm_error_t *error) {
  if (query == NULL || limits == NULL || value == NULL)
    return mysql_render_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "invalid MySQL parameter");
  if (query->parameter_count >= limits->max_parameters)
    return mysql_render_fail(
        error, ORM_STATUS_LIMIT_EXCEEDED,
        "too many MySQL parameters");
  query->parameters[query->parameter_count++] = value;
  return mysql_append_cstr(query, limits, "?", error);
}

static const char *mysql_comparison(orm_compare_t comparison) {
  switch (comparison) {
    case ORM_COMPARE_EQUAL: return " = ";
    case ORM_COMPARE_NOT_EQUAL: return " != ";
    case ORM_COMPARE_LESS: return " < ";
    case ORM_COMPARE_LESS_EQUAL: return " <= ";
    case ORM_COMPARE_GREATER: return " > ";
    case ORM_COMPARE_GREATER_EQUAL: return " >= ";
    case ORM_COMPARE_LIKE: return " like ";
    case ORM_COMPARE_NOT_LIKE: return " not like ";
    default: return NULL;
  }
}

static orm_status_t mysql_render_predicates(
    const orm_query_plan *plan, const orm_limits *limits,
    orm_mysql_rendered_query *query, orm_error_t *error) {
  size_t index;
  orm_status_t status;
  if (vec_size(&plan->predicates) == 0u)
    return ORM_STATUS_OK;

  status = mysql_append_cstr(query, limits, " where ", error);
  if (status != ORM_STATUS_OK)
    return status;

  for (index = 0u; index < vec_size(&plan->predicates); ++index) {
    const orm_predicate *predicate =
        (const orm_predicate *)vec_at_const(
            &plan->predicates, index);
    const char *comparison;
    if (predicate == NULL)
      return mysql_render_fail(
          error, ORM_STATUS_INTERNAL_ERROR,
          "invalid MySQL predicate storage");

    if (index != 0u) {
      status = mysql_append_cstr(
          query, limits, " and ", error);
      if (status != ORM_STATUS_OK)
        return status;
    }
    status = mysql_append_tstr(
        query, limits, predicate->column, error);
    if (status != ORM_STATUS_OK)
      return status;

    if (predicate->value.kind == ORM_VALUE_NULL) {
      if (predicate->comparison == ORM_COMPARE_EQUAL)
        comparison = " is null";
      else if (predicate->comparison == ORM_COMPARE_NOT_EQUAL)
        comparison = " is not null";
      else
        return mysql_render_fail(
            error, ORM_STATUS_INVALID_ARGUMENT,
            "only equality comparisons accept a null value");
      status = mysql_append_cstr(
          query, limits, comparison, error);
    } else {
      comparison = mysql_comparison(predicate->comparison);
      if (comparison == NULL)
        return mysql_render_fail(
            error, ORM_STATUS_INVALID_ARGUMENT,
            "unknown MySQL comparison");
      status = mysql_append_cstr(
          query, limits, comparison, error);
      if (status == ORM_STATUS_OK)
        status = mysql_push_parameter(
            query, limits, &predicate->value, error);
    }
    if (status != ORM_STATUS_OK)
      return status;
  }
  return ORM_STATUS_OK;
}

static orm_status_t mysql_render_select(
    const orm_query_plan *plan, const orm_limits *limits,
    orm_mysql_rendered_query *query, orm_error_t *error) {
  size_t index;
  orm_status_t status;

  if (!plan->select_all && vec_size(&plan->columns) == 0u)
    return mysql_render_fail(
        error, ORM_STATUS_INVALID_STATE,
        "MySQL SELECT has no projected columns");

  status = mysql_append_cstr(query, limits, "select ", error);
  if (status != ORM_STATUS_OK)
    return status;

  if (plan->select_all) {
    status = mysql_append_cstr(query, limits, "*", error);
  } else {
    for (index = 0u;
         status == ORM_STATUS_OK &&
         index < vec_size(&plan->columns);
         ++index) {
      const tstr *column =
          (const tstr *)vec_at_const(&plan->columns, index);
      if (column == NULL)
        return mysql_render_fail(
            error, ORM_STATUS_INTERNAL_ERROR,
            "invalid MySQL projection storage");
      if (index != 0u)
        status = mysql_append_cstr(
            query, limits, ", ", error);
      if (status == ORM_STATUS_OK)
        status = mysql_append_tstr(
            query, limits, *column, error);
    }
  }

  if (status == ORM_STATUS_OK)
    status = mysql_append_cstr(query, limits, " from ", error);
  if (status == ORM_STATUS_OK)
    status = mysql_append_tstr(query, limits, plan->table, error);
  if (status == ORM_STATUS_OK)
    status = mysql_render_predicates(
        plan, limits, query, error);

  if (status == ORM_STATUS_OK && plan->ordering.present) {
    status = mysql_append_cstr(
        query, limits, " order by ", error);
    if (status == ORM_STATUS_OK)
      status = mysql_append_tstr(
          query, limits, plan->ordering.column, error);
    if (status == ORM_STATUS_OK)
      status = mysql_append_cstr(
          query, limits,
          plan->ordering.order == ORM_ORDER_DESCENDING
              ? " desc"
              : " asc",
          error);
  }

  if (status == ORM_STATUS_OK && plan->has_limit) {
    status = mysql_append_cstr(
        query, limits, " limit ", error);
    if (status == ORM_STATUS_OK)
      status = mysql_append_u64(
          query, limits, plan->limit, error);
  } else if (status == ORM_STATUS_OK && plan->has_offset) {
    /*
     * MySQL requires LIMIT when OFFSET is present. The maximum unsigned
     * BIGINT count is the documented no-practical-limit sentinel.
     */
    status = mysql_append_cstr(
        query, limits,
        " limit 18446744073709551615", error);
  }

  if (status == ORM_STATUS_OK && plan->has_offset) {
    status = mysql_append_cstr(
        query, limits, " offset ", error);
    if (status == ORM_STATUS_OK)
      status = mysql_append_u64(
          query, limits, plan->offset, error);
  }
  return status;
}

static orm_status_t mysql_render_insert(
    const orm_query_plan *plan, const orm_limits *limits,
    orm_mysql_rendered_query *query, orm_error_t *error) {
  size_t index;
  orm_status_t status;

  if (vec_size(&plan->assignments) == 0u)
    return mysql_render_fail(
        error, ORM_STATUS_INVALID_STATE,
        "MySQL INSERT has no assignments");

  status = mysql_append_cstr(
      query, limits, "insert into ", error);
  if (status == ORM_STATUS_OK)
    status = mysql_append_tstr(
        query, limits, plan->table, error);
  if (status == ORM_STATUS_OK)
    status = mysql_append_cstr(query, limits, " (", error);

  for (index = 0u;
       status == ORM_STATUS_OK &&
       index < vec_size(&plan->assignments);
       ++index) {
    const orm_assignment *assignment =
        (const orm_assignment *)vec_at_const(
            &plan->assignments, index);
    if (assignment == NULL)
      return mysql_render_fail(
          error, ORM_STATUS_INTERNAL_ERROR,
          "invalid MySQL assignment storage");
    if (index != 0u)
      status = mysql_append_cstr(
          query, limits, ", ", error);
    if (status == ORM_STATUS_OK)
      status = mysql_append_tstr(
          query, limits, assignment->column, error);
  }

  if (status == ORM_STATUS_OK)
    status = mysql_append_cstr(
        query, limits, ") values (", error);

  for (index = 0u;
       status == ORM_STATUS_OK &&
       index < vec_size(&plan->assignments);
       ++index) {
    const orm_assignment *assignment =
        (const orm_assignment *)vec_at_const(
            &plan->assignments, index);
    if (index != 0u)
      status = mysql_append_cstr(
          query, limits, ", ", error);
    if (status != ORM_STATUS_OK)
      break;
    status = assignment->value.kind == ORM_VALUE_NULL
                 ? mysql_append_cstr(
                       query, limits, "null", error)
                 : mysql_push_parameter(
                       query, limits,
                       &assignment->value, error);
  }
  if (status == ORM_STATUS_OK)
    status = mysql_append_cstr(query, limits, ")", error);
  return status;
}

static orm_status_t mysql_render_update(
    const orm_query_plan *plan, const orm_limits *limits,
    orm_mysql_rendered_query *query, orm_error_t *error) {
  size_t index;
  orm_status_t status;

  if (vec_size(&plan->assignments) == 0u)
    return mysql_render_fail(
        error, ORM_STATUS_INVALID_STATE,
        "MySQL UPDATE has no assignments");

  status = mysql_append_cstr(query, limits, "update ", error);
  if (status == ORM_STATUS_OK)
    status = mysql_append_tstr(
        query, limits, plan->table, error);
  if (status == ORM_STATUS_OK)
    status = mysql_append_cstr(query, limits, " set ", error);

  for (index = 0u;
       status == ORM_STATUS_OK &&
       index < vec_size(&plan->assignments);
       ++index) {
    const orm_assignment *assignment =
        (const orm_assignment *)vec_at_const(
            &plan->assignments, index);
    if (assignment == NULL)
      return mysql_render_fail(
          error, ORM_STATUS_INTERNAL_ERROR,
          "invalid MySQL assignment storage");
    if (index != 0u)
      status = mysql_append_cstr(
          query, limits, ", ", error);
    if (status == ORM_STATUS_OK)
      status = mysql_append_tstr(
          query, limits, assignment->column, error);
    if (status == ORM_STATUS_OK)
      status = mysql_append_cstr(
          query, limits, " = ", error);
    if (status == ORM_STATUS_OK)
      status = assignment->value.kind == ORM_VALUE_NULL
                   ? mysql_append_cstr(
                         query, limits, "null", error)
                   : mysql_push_parameter(
                         query, limits,
                         &assignment->value, error);
  }

  if (status == ORM_STATUS_OK)
    status = mysql_render_predicates(
        plan, limits, query, error);
  return status;
}

static orm_status_t mysql_render_delete(
    const orm_query_plan *plan, const orm_limits *limits,
    orm_mysql_rendered_query *query, orm_error_t *error) {
  orm_status_t status =
      mysql_append_cstr(query, limits, "delete from ", error);
  if (status == ORM_STATUS_OK)
    status = mysql_append_tstr(
        query, limits, plan->table, error);
  if (status == ORM_STATUS_OK)
    status = mysql_render_predicates(
        plan, limits, query, error);
  return status;
}

void orm_mysql_rendered_query_destroy(
    orm_mysql_rendered_query *query) {
  if (query == NULL)
    return;
  tstr_freep(&query->text);
  free(query->parameters);
  memset(query, 0, sizeof(*query));
}

orm_status_t orm_mysql_render_plan(
    const orm_query_plan *plan, const orm_limits *limits,
    orm_mysql_rendered_query *out, orm_error_t *error) {
  size_t capacity;
  orm_status_t status;

  if (out != NULL)
    memset(out, 0, sizeof(*out));
  if (plan == NULL || limits == NULL || out == NULL)
    return mysql_render_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "invalid MySQL structured render request");
  if (plan->kind == ORM_QUERY_RAW)
    return mysql_render_fail(
        error, ORM_STATUS_UNSUPPORTED,
        "RAW SQL does not use the MySQL structured renderer");

  if (vec_size(&plan->assignments) >
      SIZE_MAX - vec_size(&plan->predicates))
    return mysql_render_fail(
        error, ORM_STATUS_LIMIT_EXCEEDED,
        "MySQL parameter capacity exceeds platform range");
  capacity =
      vec_size(&plan->assignments) +
      vec_size(&plan->predicates);
  if (capacity != 0u) {
    if (capacity > SIZE_MAX / sizeof(*out->parameters))
      return mysql_render_fail(
          error, ORM_STATUS_LIMIT_EXCEEDED,
          "MySQL parameter array exceeds platform range");
    out->parameters = (const orm_owned_value **)calloc(
        capacity, sizeof(*out->parameters));
    if (out->parameters == NULL)
      return mysql_render_fail(
          error, ORM_STATUS_OUT_OF_MEMORY,
          "allocate MySQL structured parameters");
  }

  switch (plan->kind) {
    case ORM_QUERY_SELECT:
      status = mysql_render_select(
          plan, limits, out, error);
      break;
    case ORM_QUERY_INSERT:
      status = mysql_render_insert(
          plan, limits, out, error);
      break;
    case ORM_QUERY_UPDATE:
      status = mysql_render_update(
          plan, limits, out, error);
      break;
    case ORM_QUERY_DELETE:
      status = mysql_render_delete(
          plan, limits, out, error);
      break;
    default:
      status = mysql_render_fail(
          error, ORM_STATUS_INVALID_ARGUMENT,
          "unknown MySQL structured query kind");
      break;
  }

  if (status != ORM_STATUS_OK) {
    orm_mysql_rendered_query_destroy(out);
    return status;
  }
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

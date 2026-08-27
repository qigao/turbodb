#include "orm_sql_render.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static orm_status_t orm_sql_append(orm_sql_query *query,
                                   const orm_limits *limits,
                                   const char *bytes, size_t size,
                                   orm_error_t *error) {
  tstr next;
  if (query == NULL || limits == NULL ||
      (size != 0u && bytes == NULL)) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid SQL render fragment");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  if (size > limits->max_query_bytes ||
      tstr_len(query->text) > limits->max_query_bytes - size) {
    orm_error_set(error, ORM_STATUS_LIMIT_EXCEEDED,
                  "SQL query exceeds max_query_bytes");
    return ORM_STATUS_LIMIT_EXCEEDED;
  }
  next = tstr_cat_len(query->text, bytes, size);
  if (next == NULL) {
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY, "append SQL fragment");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  query->text = next;
  return ORM_STATUS_OK;
}

static orm_status_t orm_sql_append_cstr(orm_sql_query *query,
                                        const orm_limits *limits,
                                        const char *text,
                                        orm_error_t *error) {
  return orm_sql_append(query, limits, text, strlen(text), error);
}

static orm_status_t orm_sql_append_tstr(orm_sql_query *query,
                                        const orm_limits *limits, tstr text,
                                        orm_error_t *error) {
  return orm_sql_append(query, limits, text, tstr_len(text), error);
}

static orm_status_t orm_sql_append_u64(orm_sql_query *query,
                                       const orm_limits *limits,
                                       uint64_t value,
                                       orm_error_t *error) {
  char buffer[32];
  const int length = snprintf(buffer, sizeof(buffer), "%" PRIu64, value);
  if (length <= 0 || (size_t)length >= sizeof(buffer)) {
    orm_error_set(error, ORM_STATUS_INTERNAL_ERROR,
                  "format SQL integer failed");
    return ORM_STATUS_INTERNAL_ERROR;
  }
  return orm_sql_append(query, limits, buffer, (size_t)length, error);
}

static orm_status_t orm_sql_push_parameter(
    orm_sql_query *query, const orm_limits *limits, orm_sql_dialect dialect,
    const orm_owned_value *value, orm_error_t *error) {
  char placeholder[32];
  size_t one_based;
  int length;
  stl_status pushed;
  if (value == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "SQL parameter is null");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  pushed = vec_push(&query->parameters, &value);
  if (pushed != STL_OK) {
    const orm_status_t status =
        pushed == STL_CAPACITY_EXCEEDED ? ORM_STATUS_LIMIT_EXCEEDED
                                        : ORM_STATUS_OUT_OF_MEMORY;
    orm_error_set(error, status, "append SQL parameter");
    return status;
  }
  one_based = vec_size(&query->parameters);
  length = snprintf(placeholder, sizeof(placeholder),
                    dialect == ORM_SQL_POSTGRES ? "$%zu" : "?%zu",
                    one_based);
  if (length <= 0 || (size_t)length >= sizeof(placeholder)) {
    orm_error_set(error, ORM_STATUS_INTERNAL_ERROR,
                  "format SQL placeholder failed");
    return ORM_STATUS_INTERNAL_ERROR;
  }
  return orm_sql_append(query, limits, placeholder, (size_t)length, error);
}

static const char *orm_sql_comparison(orm_compare_t comparison) {
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

static orm_status_t orm_sql_render_predicates(
    const orm_query_plan *plan, const orm_limits *limits,
    orm_sql_dialect dialect, orm_sql_query *query, orm_error_t *error) {
  size_t index;
  orm_status_t status;
  if (vec_size(&plan->predicates) == 0u)
    return ORM_STATUS_OK;
  status = orm_sql_append_cstr(query, limits, " where ", error);
  if (status != ORM_STATUS_OK)
    return status;
  for (index = 0u; index < vec_size(&plan->predicates); ++index) {
    const orm_predicate *predicate =
        (const orm_predicate *)vec_at_const(&plan->predicates, index);
    const char *comparison;
    if (predicate == NULL) {
      orm_error_set(error, ORM_STATUS_INTERNAL_ERROR,
                    "ORM predicate storage is invalid");
      return ORM_STATUS_INTERNAL_ERROR;
    }
    if (index != 0u) {
      status = orm_sql_append_cstr(query, limits, " and ", error);
      if (status != ORM_STATUS_OK)
        return status;
    }
    status = orm_sql_append_tstr(query, limits, predicate->column, error);
    if (status != ORM_STATUS_OK)
      return status;
    if (predicate->value.kind == ORM_VALUE_NULL) {
      if (predicate->comparison == ORM_COMPARE_EQUAL)
        comparison = " is null";
      else if (predicate->comparison == ORM_COMPARE_NOT_EQUAL)
        comparison = " is not null";
      else {
        orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                      "only equality comparisons accept a null value");
        return ORM_STATUS_INVALID_ARGUMENT;
      }
      status = orm_sql_append_cstr(query, limits, comparison, error);
    } else {
      comparison = orm_sql_comparison(predicate->comparison);
      if (comparison == NULL) {
        orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                      "unknown SQL comparison");
        return ORM_STATUS_INVALID_ARGUMENT;
      }
      status = orm_sql_append_cstr(query, limits, comparison, error);
      if (status == ORM_STATUS_OK)
        status = orm_sql_push_parameter(query, limits, dialect,
                                        &predicate->value, error);
    }
    if (status != ORM_STATUS_OK)
      return status;
  }
  return ORM_STATUS_OK;
}

static orm_status_t orm_sql_render_select(const orm_query_plan *plan,
                                          const orm_limits *limits,
                                          orm_sql_dialect dialect,
                                          orm_sql_query *query,
                                          orm_error_t *error) {
  size_t index;
  orm_status_t status;
  if (!plan->select_all && vec_size(&plan->columns) == 0u) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "SELECT query has no projected columns");
    return ORM_STATUS_INVALID_STATE;
  }
  status = orm_sql_append_cstr(query, limits, "select ", error);
  if (status != ORM_STATUS_OK)
    return status;
  if (plan->select_all) {
    status = orm_sql_append_cstr(query, limits, "*", error);
  } else {
    for (index = 0u; index < vec_size(&plan->columns); ++index) {
      const tstr *column = (const tstr *)vec_at_const(&plan->columns, index);
      if (index != 0u) {
        status = orm_sql_append_cstr(query, limits, ", ", error);
        if (status != ORM_STATUS_OK)
          return status;
      }
      status = orm_sql_append_tstr(query, limits, *column, error);
      if (status != ORM_STATUS_OK)
        return status;
    }
  }
  if (status == ORM_STATUS_OK)
    status = orm_sql_append_cstr(query, limits, " from ", error);
  if (status == ORM_STATUS_OK)
    status = orm_sql_append_tstr(query, limits, plan->table, error);
  if (status == ORM_STATUS_OK)
    status = orm_sql_render_predicates(plan, limits, dialect, query, error);
  if (status == ORM_STATUS_OK && plan->ordering.present) {
    status = orm_sql_append_cstr(query, limits, " order by ", error);
    if (status == ORM_STATUS_OK)
      status = orm_sql_append_tstr(query, limits, plan->ordering.column,
                                   error);
    if (status == ORM_STATUS_OK)
      status = orm_sql_append_cstr(
          query, limits,
          plan->ordering.order == ORM_ORDER_DESCENDING ? " desc" : " asc",
          error);
  }
  if (status == ORM_STATUS_OK && plan->has_limit) {
    status = orm_sql_append_cstr(query, limits, " limit ", error);
    if (status == ORM_STATUS_OK)
      status = orm_sql_append_u64(query, limits, plan->limit, error);
  } else if (status == ORM_STATUS_OK && plan->has_offset &&
             dialect == ORM_SQL_SQLITE) {
    status = orm_sql_append_cstr(query, limits, " limit -1", error);
  }
  if (status == ORM_STATUS_OK && plan->has_offset) {
    status = orm_sql_append_cstr(query, limits, " offset ", error);
    if (status == ORM_STATUS_OK)
      status = orm_sql_append_u64(query, limits, plan->offset, error);
  }
  return status;
}

static orm_status_t orm_sql_render_insert(const orm_query_plan *plan,
                                          const orm_limits *limits,
                                          orm_sql_dialect dialect,
                                          orm_sql_query *query,
                                          orm_error_t *error) {
  size_t index;
  orm_status_t status;
  if (vec_size(&plan->assignments) == 0u) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "INSERT query has no assignments");
    return ORM_STATUS_INVALID_STATE;
  }
  status = orm_sql_append_cstr(query, limits, "insert into ", error);
  if (status == ORM_STATUS_OK)
    status = orm_sql_append_tstr(query, limits, plan->table, error);
  if (status == ORM_STATUS_OK)
    status = orm_sql_append_cstr(query, limits, " (", error);
  for (index = 0u; status == ORM_STATUS_OK &&
                   index < vec_size(&plan->assignments); ++index) {
    const orm_assignment *assignment =
        (const orm_assignment *)vec_at_const(&plan->assignments, index);
    if (index != 0u)
      status = orm_sql_append_cstr(query, limits, ", ", error);
    if (status == ORM_STATUS_OK)
      status = orm_sql_append_tstr(query, limits, assignment->column, error);
  }
  if (status == ORM_STATUS_OK)
    status = orm_sql_append_cstr(query, limits, ") values (", error);
  for (index = 0u; status == ORM_STATUS_OK &&
                   index < vec_size(&plan->assignments); ++index) {
    const orm_assignment *assignment =
        (const orm_assignment *)vec_at_const(&plan->assignments, index);
    if (index != 0u)
      status = orm_sql_append_cstr(query, limits, ", ", error);
    if (status != ORM_STATUS_OK)
      break;
    status = assignment->value.kind == ORM_VALUE_NULL
                 ? orm_sql_append_cstr(query, limits, "null", error)
                 : orm_sql_push_parameter(query, limits, dialect,
                                          &assignment->value, error);
  }
  if (status == ORM_STATUS_OK)
    status = orm_sql_append_cstr(query, limits, ")", error);
  return status;
}

static orm_status_t orm_sql_render_update(const orm_query_plan *plan,
                                          const orm_limits *limits,
                                          orm_sql_dialect dialect,
                                          orm_sql_query *query,
                                          orm_error_t *error) {
  size_t index;
  orm_status_t status;
  if (vec_size(&plan->assignments) == 0u) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "UPDATE query has no assignments");
    return ORM_STATUS_INVALID_STATE;
  }
  status = orm_sql_append_cstr(query, limits, "update ", error);
  if (status == ORM_STATUS_OK)
    status = orm_sql_append_tstr(query, limits, plan->table, error);
  if (status == ORM_STATUS_OK)
    status = orm_sql_append_cstr(query, limits, " set ", error);
  for (index = 0u; status == ORM_STATUS_OK &&
                   index < vec_size(&plan->assignments); ++index) {
    const orm_assignment *assignment =
        (const orm_assignment *)vec_at_const(&plan->assignments, index);
    if (index != 0u)
      status = orm_sql_append_cstr(query, limits, ", ", error);
    if (status == ORM_STATUS_OK)
      status = orm_sql_append_tstr(query, limits, assignment->column, error);
    if (status == ORM_STATUS_OK)
      status = orm_sql_append_cstr(query, limits, " = ", error);
    if (status == ORM_STATUS_OK)
      status = assignment->value.kind == ORM_VALUE_NULL
                   ? orm_sql_append_cstr(query, limits, "null", error)
                   : orm_sql_push_parameter(query, limits, dialect,
                                            &assignment->value, error);
  }
  if (status == ORM_STATUS_OK)
    status = orm_sql_render_predicates(plan, limits, dialect, query, error);
  return status;
}

static orm_status_t orm_sql_render_delete(const orm_query_plan *plan,
                                          const orm_limits *limits,
                                          orm_sql_dialect dialect,
                                          orm_sql_query *query,
                                          orm_error_t *error) {
  orm_status_t status = orm_sql_append_cstr(query, limits, "delete from ",
                                             error);
  if (status == ORM_STATUS_OK)
    status = orm_sql_append_tstr(query, limits, plan->table, error);
  if (status == ORM_STATUS_OK)
    status = orm_sql_render_predicates(plan, limits, dialect, query, error);
  return status;
}

static orm_status_t orm_sql_render_raw(const orm_query_plan *plan,
                                       const orm_limits *limits,
                                       orm_sql_query *query,
                                       orm_error_t *error) {
  size_t index;
  orm_status_t status = orm_sql_append_tstr(query, limits, plan->raw_sql,
                                             error);
  for (index = 0u; status == ORM_STATUS_OK &&
                   index < vec_size(&plan->raw_parameters); ++index) {
    const orm_owned_value *value = (const orm_owned_value *)
        vec_at_const(&plan->raw_parameters, index);
    const stl_status pushed = vec_push(&query->parameters, &value);
    if (pushed != STL_OK) {
      status = pushed == STL_CAPACITY_EXCEEDED ? ORM_STATUS_LIMIT_EXCEEDED
                                               : ORM_STATUS_OUT_OF_MEMORY;
      orm_error_set(error, status, "append raw SQL parameter");
    }
  }
  return status;
}

orm_status_t orm_sql_render(const orm_query_plan *plan,
                            const orm_limits *limits,
                            orm_sql_dialect dialect,
                            orm_sql_query *out_query,
                            orm_error_t *error) {
  orm_status_t status;
  if (plan == NULL || limits == NULL || out_query == NULL ||
      (dialect != ORM_SQL_SQLITE && dialect != ORM_SQL_POSTGRES)) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid SQL render request");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  memset(out_query, 0, sizeof(*out_query));
  if (vec_init_bytes(&out_query->parameters,
                     sizeof(const orm_owned_value *),
                     _Alignof(const orm_owned_value *),
                     limits->max_parameters) != STL_OK) {
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "initialize SQL parameter view");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  out_query->text = tstr_new();
  if (out_query->text == NULL) {
    orm_sql_query_destroy(out_query);
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "initialize rendered SQL text");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  switch (plan->kind) {
    case ORM_QUERY_SELECT:
      status = orm_sql_render_select(plan, limits, dialect, out_query, error);
      break;
    case ORM_QUERY_INSERT:
      status = orm_sql_render_insert(plan, limits, dialect, out_query, error);
      break;
    case ORM_QUERY_UPDATE:
      status = orm_sql_render_update(plan, limits, dialect, out_query, error);
      break;
    case ORM_QUERY_DELETE:
      status = orm_sql_render_delete(plan, limits, dialect, out_query, error);
      break;
    case ORM_QUERY_RAW:
      status = orm_sql_render_raw(plan, limits, out_query, error);
      break;
    default:
      orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                    "unknown ORM query kind");
      status = ORM_STATUS_INVALID_ARGUMENT;
      break;
  }
  if (status != ORM_STATUS_OK) {
    orm_sql_query_destroy(out_query);
    return status;
  }
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

void orm_sql_query_destroy(orm_sql_query *query) {
  if (query == NULL)
    return;
  tstr_freep(&query->text);
  vec_destroy(&query->parameters);
  memset(query, 0, sizeof(*query));
}

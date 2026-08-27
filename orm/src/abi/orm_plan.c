#include "orm_internal.h"

#include <string.h>

static orm_status_t orm_vec_status(stl_status status, orm_error_t *error,
                                   const char *role) {
  orm_status_t mapped;
  if (status == STL_OK)
    return ORM_STATUS_OK;
  mapped = status == STL_CAPACITY_EXCEEDED ? ORM_STATUS_LIMIT_EXCEEDED
                                           : ORM_STATUS_OUT_OF_MEMORY;
  orm_error_set(error, mapped, role);
  return mapped;
}

enum { ORM_IDENTIFIER_SEGMENT_MAX_BYTES = 63u };

static bool orm_identifier_start(unsigned char value) {
  return (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
         (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
         value == (unsigned char)'_';
}

static bool orm_identifier_valid(vstr input, bool qualified,
                                 orm_status_t *status) {
  size_t index;
  size_t segment_size = 0u;
  *status = ORM_STATUS_INVALID_ARGUMENT;
  for (index = 0u; index < input.len; ++index) {
    const unsigned char value = (unsigned char)input.data[index];
    if (qualified && value == (unsigned char)'.') {
      if (segment_size == 0u)
        return false;
      segment_size = 0u;
      continue;
    }
    if ((segment_size == 0u && !orm_identifier_start(value)) ||
        (segment_size != 0u && !orm_identifier_start(value) &&
         !(value >= (unsigned char)'0' && value <= (unsigned char)'9')))
      return false;
    ++segment_size;
    if (segment_size > ORM_IDENTIFIER_SEGMENT_MAX_BYTES) {
      *status = ORM_STATUS_LIMIT_EXCEEDED;
      return false;
    }
  }
  return segment_size != 0u;
}

static orm_status_t orm_copy_identifier(vstr input, size_t max_bytes,
                                        tstr *output, orm_error_t *error,
                                        const char *role, bool qualified) {
  orm_status_t validation = ORM_STATUS_INVALID_ARGUMENT;
  if (output == NULL || !orm_view_valid(input, false) ||
      input.len > max_bytes || memchr(input.data, '\0', input.len) != NULL ||
      !orm_identifier_valid(input, qualified, &validation)) {
    orm_error_set(error, input.len > max_bytes ? ORM_STATUS_LIMIT_EXCEEDED
                                               : validation,
                  role);
    return input.len > max_bytes ? ORM_STATUS_LIMIT_EXCEEDED
                                 : validation;
  }
  *output = tstr_dup_len(input.data, input.len);
  if (*output == NULL) {
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY, role);
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  return ORM_STATUS_OK;
}

static void orm_owned_value_destroy(orm_owned_value *value) {
  if (value == NULL)
    return;
  tstr_freep(&value->bytes);
  memset(value, 0, sizeof(*value));
}

static orm_status_t orm_owned_value_copy(orm_owned_value *output,
                                         orm_value_t input,
                                         orm_query_plan *plan,
                                         const orm_limits *limits,
                                         orm_error_t *error) {
  size_t bytes = 0u;
  const void *data = NULL;
  if (output == NULL || plan == NULL || limits == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid ORM value destination");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  memset(output, 0, sizeof(*output));
  if (input.reserved != 0u) {
    orm_error_set(error, ORM_STATUS_ABI_MISMATCH,
                  "orm_value_t.reserved must be zero");
    return ORM_STATUS_ABI_MISMATCH;
  }
  output->kind = input.kind;
  switch (input.kind) {
    case ORM_VALUE_NULL:
      break;
    case ORM_VALUE_INT64:
      output->data.int64_value = input.data.int64_value;
      break;
    case ORM_VALUE_UINT64:
      output->data.uint64_value = input.data.uint64_value;
      break;
    case ORM_VALUE_DOUBLE:
      output->data.double_value = input.data.double_value;
      break;
    case ORM_VALUE_BOOLEAN:
      if (input.data.boolean_value > 1u) {
        orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                      "boolean ORM parameter must be 0 or 1");
        return ORM_STATUS_INVALID_ARGUMENT;
      }
      output->data.boolean_value = input.data.boolean_value;
      break;
    case ORM_VALUE_TEXT:
      if (!orm_view_valid(input.data.text_value, true)) {
        orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                      "invalid ORM text parameter");
        return ORM_STATUS_INVALID_ARGUMENT;
      }
      data = input.data.text_value.data;
      bytes = input.data.text_value.len;
      break;
    case ORM_VALUE_BLOB:
      if (input.data.blob_value.size != 0u &&
          input.data.blob_value.data == NULL) {
        orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                      "invalid ORM blob parameter");
        return ORM_STATUS_INVALID_ARGUMENT;
      }
      data = input.data.blob_value.data;
      bytes = input.data.blob_value.size;
      break;
    default:
      orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                    "unknown ORM value kind");
      return ORM_STATUS_INVALID_ARGUMENT;
  }
  if (bytes > limits->max_parameter_bytes - plan->parameter_bytes) {
    orm_error_set(error, ORM_STATUS_LIMIT_EXCEEDED,
                  "ORM parameter bytes exceed configured limit");
    return ORM_STATUS_LIMIT_EXCEEDED;
  }
  if (input.kind == ORM_VALUE_TEXT || input.kind == ORM_VALUE_BLOB) {
    output->bytes = tstr_new_len(data, bytes);
    if (output->bytes == NULL) {
      orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                    "copy ORM parameter value");
      return ORM_STATUS_OUT_OF_MEMORY;
    }
  }
  plan->parameter_bytes += bytes;
  return ORM_STATUS_OK;
}

static void orm_string_vec_clear(vec_t *values) {
  size_t index;
  for (index = 0u; index < vec_size(values); ++index) {
    tstr *value = (tstr *)vec_at(values, index);
    if (value != NULL)
      tstr_freep(value);
  }
  (void)vec_clear(values);
}

orm_status_t orm_plan_init(orm_query_plan *plan, orm_query_kind kind,
                           vstr input, const orm_limits *limits,
                           orm_error_t *error) {
  orm_status_t status;
  if (plan == NULL || limits == NULL || !orm_view_valid(input, false)) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid ORM query input");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  memset(plan, 0, sizeof(*plan));
  plan->kind = kind;
  if (vec_init_bytes(&plan->columns, sizeof(tstr), _Alignof(tstr),
                     limits->max_columns) != STL_OK ||
      vec_init_bytes(&plan->assignments, sizeof(orm_assignment),
                     _Alignof(orm_assignment), limits->max_assignments) !=
          STL_OK ||
      vec_init_bytes(&plan->predicates, sizeof(orm_predicate),
                     _Alignof(orm_predicate), limits->max_predicates) !=
          STL_OK ||
      vec_init_bytes(&plan->raw_parameters, sizeof(orm_owned_value),
                     _Alignof(orm_owned_value), limits->max_parameters) !=
          STL_OK) {
    orm_plan_destroy(plan);
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "initialize ORM query containers");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  if (kind == ORM_QUERY_RAW) {
    if (input.len > limits->max_query_bytes ||
        memchr(input.data, '\0', input.len) != NULL) {
      orm_plan_destroy(plan);
      orm_error_set(error, input.len > limits->max_query_bytes
                               ? ORM_STATUS_LIMIT_EXCEEDED
                               : ORM_STATUS_INVALID_ARGUMENT,
                    "invalid raw query text");
      return input.len > limits->max_query_bytes ? ORM_STATUS_LIMIT_EXCEEDED
                                                  : ORM_STATUS_INVALID_ARGUMENT;
    }
    plan->raw_sql = tstr_dup_len(input.data, input.len);
    if (plan->raw_sql == NULL) {
      orm_plan_destroy(plan);
      orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY, "copy raw query text");
      return ORM_STATUS_OUT_OF_MEMORY;
    }
  } else {
    status = orm_copy_identifier(input, limits->max_query_bytes, &plan->table,
                                 error, "invalid ORM table name", true);
    if (status != ORM_STATUS_OK) {
      orm_plan_destroy(plan);
      return status;
    }
  }
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

void orm_plan_destroy(orm_query_plan *plan) {
  size_t index;
  if (plan == NULL)
    return;
  orm_string_vec_clear(&plan->columns);
  for (index = 0u; index < vec_size(&plan->assignments); ++index) {
    orm_assignment *value = (orm_assignment *)vec_at(&plan->assignments, index);
    if (value != NULL) {
      tstr_freep(&value->column);
      orm_owned_value_destroy(&value->value);
    }
  }
  for (index = 0u; index < vec_size(&plan->predicates); ++index) {
    orm_predicate *value = (orm_predicate *)vec_at(&plan->predicates, index);
    if (value != NULL) {
      tstr_freep(&value->column);
      orm_owned_value_destroy(&value->value);
    }
  }
  for (index = 0u; index < vec_size(&plan->raw_parameters); ++index)
    orm_owned_value_destroy(
        (orm_owned_value *)vec_at(&plan->raw_parameters, index));
  vec_destroy(&plan->columns);
  vec_destroy(&plan->assignments);
  vec_destroy(&plan->predicates);
  vec_destroy(&plan->raw_parameters);
  tstr_freep(&plan->ordering.column);
  tstr_freep(&plan->table);
  tstr_freep(&plan->raw_sql);
  memset(plan, 0, sizeof(*plan));
}

orm_status_t orm_plan_select_all(orm_query_plan *plan, orm_error_t *error) {
  if (plan == NULL || plan->kind != ORM_QUERY_SELECT) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "select_all requires a SELECT query");
    return ORM_STATUS_INVALID_STATE;
  }
  orm_string_vec_clear(&plan->columns);
  plan->select_all = true;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

orm_status_t orm_plan_add_column(orm_query_plan *plan, vstr column,
                                 const orm_limits *limits,
                                 orm_error_t *error) {
  tstr owned = NULL;
  orm_status_t status;
  stl_status pushed;
  if (plan == NULL || limits == NULL || plan->kind != ORM_QUERY_SELECT) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "add_column requires a SELECT query");
    return ORM_STATUS_INVALID_STATE;
  }
  status = orm_copy_identifier(column, limits->max_query_bytes, &owned, error,
                               "invalid ORM column name", true);
  if (status != ORM_STATUS_OK)
    return status;
  pushed = vec_push(&plan->columns, &owned);
  if (pushed != STL_OK) {
    tstr_free(owned);
    return orm_vec_status(pushed, error, "too many ORM result columns");
  }
  plan->select_all = false;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

orm_status_t orm_plan_add_assignment(orm_query_plan *plan, vstr column,
                                     orm_value_t value,
                                     const orm_limits *limits,
                                     orm_error_t *error) {
  orm_assignment assignment;
  orm_status_t status;
  stl_status pushed;
  if (plan == NULL || limits == NULL ||
      (plan->kind != ORM_QUERY_INSERT && plan->kind != ORM_QUERY_UPDATE)) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "set requires an INSERT or UPDATE query");
    return ORM_STATUS_INVALID_STATE;
  }
  memset(&assignment, 0, sizeof(assignment));
  status = orm_copy_identifier(column, limits->max_query_bytes,
                               &assignment.column, error,
                               "invalid ORM assignment column", false);
  if (status != ORM_STATUS_OK)
    return status;
  status = orm_owned_value_copy(&assignment.value, value, plan, limits, error);
  if (status != ORM_STATUS_OK) {
    tstr_free(assignment.column);
    return status;
  }
  pushed = vec_push(&plan->assignments, &assignment);
  if (pushed != STL_OK) {
    plan->parameter_bytes -= tstr_len(assignment.value.bytes);
    tstr_free(assignment.column);
    orm_owned_value_destroy(&assignment.value);
    return orm_vec_status(pushed, error, "too many ORM assignments");
  }
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

orm_status_t orm_plan_add_predicate(orm_query_plan *plan, vstr column,
                                    orm_compare_t comparison,
                                    orm_value_t value,
                                    const orm_limits *limits,
                                    orm_error_t *error) {
  orm_predicate predicate;
  orm_status_t status;
  stl_status pushed;
  if (plan == NULL || limits == NULL || plan->kind == ORM_QUERY_INSERT ||
      plan->kind == ORM_QUERY_RAW || comparison < ORM_COMPARE_EQUAL ||
      comparison > ORM_COMPARE_NOT_LIKE) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "where requires SELECT, UPDATE, or DELETE");
    return ORM_STATUS_INVALID_STATE;
  }
  memset(&predicate, 0, sizeof(predicate));
  status = orm_copy_identifier(column, limits->max_query_bytes,
                               &predicate.column, error,
                               "invalid ORM predicate column", true);
  if (status != ORM_STATUS_OK)
    return status;
  predicate.comparison = comparison;
  status = orm_owned_value_copy(&predicate.value, value, plan, limits, error);
  if (status != ORM_STATUS_OK) {
    tstr_free(predicate.column);
    return status;
  }
  pushed = vec_push(&plan->predicates, &predicate);
  if (pushed != STL_OK) {
    plan->parameter_bytes -= tstr_len(predicate.value.bytes);
    tstr_free(predicate.column);
    orm_owned_value_destroy(&predicate.value);
    return orm_vec_status(pushed, error, "too many ORM predicates");
  }
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

orm_status_t orm_plan_add_bind(orm_query_plan *plan, orm_value_t value,
                               const orm_limits *limits,
                               orm_error_t *error) {
  orm_owned_value owned;
  orm_status_t status;
  stl_status pushed;
  if (plan == NULL || limits == NULL || plan->kind != ORM_QUERY_RAW) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "bind requires a raw query");
    return ORM_STATUS_INVALID_STATE;
  }
  status = orm_owned_value_copy(&owned, value, plan, limits, error);
  if (status != ORM_STATUS_OK)
    return status;
  pushed = vec_push(&plan->raw_parameters, &owned);
  if (pushed != STL_OK) {
    plan->parameter_bytes -= tstr_len(owned.bytes);
    orm_owned_value_destroy(&owned);
    return orm_vec_status(pushed, error, "too many raw query parameters");
  }
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

orm_status_t orm_plan_set_order(orm_query_plan *plan, vstr column,
                                orm_order_t order, const orm_limits *limits,
                                orm_error_t *error) {
  tstr owned = NULL;
  orm_status_t status;
  if (plan == NULL || limits == NULL || plan->kind != ORM_QUERY_SELECT ||
      (order != ORM_ORDER_ASCENDING && order != ORM_ORDER_DESCENDING)) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "order_by requires a SELECT query and valid direction");
    return ORM_STATUS_INVALID_STATE;
  }
  status = orm_copy_identifier(column, limits->max_query_bytes, &owned, error,
                               "invalid ORM order column", true);
  if (status != ORM_STATUS_OK)
    return status;
  tstr_freep(&plan->ordering.column);
  plan->ordering.column = owned;
  plan->ordering.order = order;
  plan->ordering.present = true;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

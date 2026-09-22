#include "orm_driver_backend_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BRIDGE_HEADER(T) {(uint32_t)sizeof(T), ORM_DRIVER_ABI_VERSION}
#define BRIDGE_KNOWN_FLAGS \
  (ORM_DRIVER_PLAN_SELECT_ALL | ORM_DRIVER_PLAN_HAS_LIMIT | \
   ORM_DRIVER_PLAN_HAS_OFFSET)

static orm_status_t bridge_result(orm_error_t *error, orm_status_t status,
                                  const char *message) {
  if (error != NULL) {
    memset(error, 0, sizeof(*error));
    error->struct_size = (uint32_t)sizeof(*error);
    error->status = status;
    if (message != NULL)
      (void)snprintf(error->message, sizeof(error->message), "%s", message);
  }
  return status;
}

static int bridge_header_valid(const orm_driver_header_v1 *header,
                               size_t known_bytes) {
  return header != NULL &&
         header->abi_version == ORM_DRIVER_ABI_VERSION &&
         header->struct_size >= known_bytes &&
         header->struct_size <= ORM_DRIVER_DESCRIPTOR_MAX_BYTES;
}

static orm_status_t bridge_size(uint64_t value, size_t *out,
                                orm_error_t *error, const char *role) {
  if (out == NULL || value == 0u || value > (uint64_t)SIZE_MAX)
    return bridge_result(error, ORM_STATUS_LIMIT_EXCEEDED, role);
  *out = (size_t)value;
  return ORM_STATUS_OK;
}

static orm_status_t bridge_limits(const orm_driver_limits_v1 *input,
                                  orm_limits *out,
                                  orm_error_t *error) {
  size_t max_parameters;
  size_t max_columns;
  size_t max_predicates;
  size_t max_assignments;
  size_t max_query_bytes;
  size_t max_parameter_bytes;
  orm_status_t status;

  if (input == NULL || out == NULL ||
      !bridge_header_valid(&input->header, sizeof(*input)) ||
      input->max_result_rows == 0u || input->max_result_bytes == 0u)
    return bridge_result(error, ORM_STATUS_ABI_MISMATCH,
                         "invalid Driver plan limits");

  status = bridge_size(input->max_parameters, &max_parameters, error,
                       "Driver max_parameters exceeds host size");
  if (status == ORM_STATUS_OK)
    status = bridge_size(input->max_columns, &max_columns, error,
                         "Driver max_columns exceeds host size");
  if (status == ORM_STATUS_OK)
    status = bridge_size(input->max_predicates, &max_predicates, error,
                         "Driver max_predicates exceeds host size");
  if (status == ORM_STATUS_OK)
    status = bridge_size(input->max_assignments, &max_assignments, error,
                         "Driver max_assignments exceeds host size");
  if (status == ORM_STATUS_OK)
    status = bridge_size(input->max_query_bytes, &max_query_bytes, error,
                         "Driver max_query_bytes exceeds host size");
  if (status == ORM_STATUS_OK)
    status = bridge_size(input->max_parameter_bytes, &max_parameter_bytes,
                         error,
                         "Driver max_parameter_bytes exceeds host size");
  if (status != ORM_STATUS_OK)
    return status;

  memset(out, 0, sizeof(*out));
  out->max_parameters = max_parameters;
  out->max_columns = max_columns;
  out->max_predicates = max_predicates;
  out->max_assignments = max_assignments;
  out->max_query_bytes = max_query_bytes;
  out->max_parameter_bytes = max_parameter_bytes;
  out->max_result_rows = input->max_result_rows;
  out->max_result_bytes = input->max_result_bytes;
  return ORM_STATUS_OK;
}

static int bridge_identifier_start(unsigned char value) {
  return (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
         (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
         value == (unsigned char)'_';
}

static int bridge_identifier(orm_driver_bytes_v1 value, size_t max_bytes,
                             int qualified) {
  size_t segment = 0u;
  if (value.size == 0u || value.size > (uint64_t)max_bytes ||
      value.size > (uint64_t)SIZE_MAX || value.data == NULL)
    return 0;
  const unsigned char *bytes = (const unsigned char *)value.data;
  for (size_t i = 0u; i < (size_t)value.size; ++i) {
    const unsigned char next = bytes[i];
    if (qualified && next == (unsigned char)'.') {
      if (segment == 0u) return 0;
      segment = 0u;
      continue;
    }
    const int alpha = bridge_identifier_start(next);
    if ((segment == 0u && !alpha) ||
        (segment != 0u && !alpha &&
         !(next >= (unsigned char)'0' && next <= (unsigned char)'9')))
      return 0;
    ++segment;
    if (segment > 63u) return 0;
  }
  return segment != 0u;
}

static orm_status_t bridge_copy_bytes(orm_driver_bytes_v1 input,
                                      size_t max_bytes, int reject_nul,
                                      tstr *out, orm_error_t *error,
                                      const char *role) {
  const char *bytes = (const char *)input.data;
  if (out == NULL || input.size > (uint64_t)max_bytes ||
      input.size > (uint64_t)SIZE_MAX ||
      (input.size != 0u && bytes == NULL) ||
      (reject_nul && input.size != 0u &&
       memchr(bytes, '\0', (size_t)input.size) != NULL))
    return bridge_result(error,
        input.size > (uint64_t)max_bytes ? ORM_STATUS_LIMIT_EXCEEDED
                                         : ORM_STATUS_INVALID_ARGUMENT,
        role);
  const char *source = bytes != NULL ? bytes : "";
  *out = tstr_new_len(source, (size_t)input.size);
  if (*out == NULL)
    return bridge_result(error, ORM_STATUS_OUT_OF_MEMORY, role);
  return ORM_STATUS_OK;
}

static orm_status_t bridge_copy_identifier(orm_driver_bytes_v1 input,
                                           size_t max_bytes, int qualified,
                                           tstr *out, orm_error_t *error,
                                           const char *role) {
  if (!bridge_identifier(input, max_bytes, qualified))
    return bridge_result(error, ORM_STATUS_INVALID_ARGUMENT, role);
  return bridge_copy_bytes(input, max_bytes, 1, out, error, role);
}

static void bridge_owned_value_destroy(orm_owned_value *value) {
  if (value == NULL) return;
  tstr_freep(&value->bytes);
  memset(value, 0, sizeof(*value));
}

static orm_status_t bridge_value(const orm_driver_value_v1 *input,
                                 orm_query_plan *plan,
                                 const orm_limits *limits,
                                 orm_owned_value *out,
                                 orm_error_t *error) {
  size_t bytes = 0u;
  const void *data = NULL;
  if (input == NULL || plan == NULL || limits == NULL || out == NULL ||
      !bridge_header_valid(&input->header, sizeof(*input)) ||
      input->reserved != 0u)
    return bridge_result(error, ORM_STATUS_ABI_MISMATCH,
                         "invalid Driver plan value");
  memset(out, 0, sizeof(*out));
  out->kind = (orm_value_kind_t)input->kind;
  switch (input->kind) {
    case ORM_VALUE_NULL:
      break;
    case ORM_VALUE_INT64:
      out->data.int64_value = input->data.sint;
      break;
    case ORM_VALUE_UINT64:
      out->data.uint64_value = input->data.uint;
      break;
    case ORM_VALUE_DOUBLE:
      out->data.double_value = input->data.real;
      break;
    case ORM_VALUE_BOOLEAN:
      if (input->data.boolean > 1u)
        return bridge_result(error, ORM_STATUS_INVALID_ARGUMENT,
                             "invalid Driver boolean value");
      out->data.boolean_value = input->data.boolean;
      break;
    case ORM_VALUE_TEXT:
    case ORM_VALUE_BLOB:
      if (input->data.bytes.size > (uint64_t)SIZE_MAX ||
          (input->data.bytes.size != 0u && input->data.bytes.data == NULL))
        return bridge_result(error, ORM_STATUS_INVALID_ARGUMENT,
                             "invalid Driver byte value");
      bytes = (size_t)input->data.bytes.size;
      data = input->data.bytes.data;
      break;
    default:
      return bridge_result(error, ORM_STATUS_INVALID_ARGUMENT,
                           "unknown Driver value kind");
  }
  if (bytes > limits->max_parameter_bytes - plan->parameter_bytes)
    return bridge_result(error, ORM_STATUS_LIMIT_EXCEEDED,
                         "Driver parameter bytes exceed configured limit");
  if (input->kind == ORM_VALUE_TEXT || input->kind == ORM_VALUE_BLOB) {
    const char *source = data != NULL ? (const char *)data : "";
    out->bytes = tstr_new_len(source, bytes);
    if (out->bytes == NULL)
      return bridge_result(error, ORM_STATUS_OUT_OF_MEMORY,
                           "copy Driver parameter value");
  }
  plan->parameter_bytes += bytes;
  return ORM_STATUS_OK;
}

static orm_status_t bridge_vec_status(stl_status status, orm_error_t *error,
                                      const char *role) {
  if (status == STL_OK) return ORM_STATUS_OK;
  return bridge_result(error,
      status == STL_CAPACITY_EXCEEDED ? ORM_STATUS_LIMIT_EXCEEDED
                                      : ORM_STATUS_OUT_OF_MEMORY,
      role);
}

void orm_driver_backend_plan_destroy(orm_query_plan *plan) {
  if (plan == NULL) return;
  for (size_t i = 0u; i < vec_size(&plan->columns); ++i) {
    tstr *value = (tstr *)vec_at(&plan->columns, i);
    if (value != NULL) tstr_freep(value);
  }
  for (size_t i = 0u; i < vec_size(&plan->assignments); ++i) {
    orm_assignment *value = (orm_assignment *)vec_at(&plan->assignments, i);
    if (value != NULL) {
      tstr_freep(&value->column);
      bridge_owned_value_destroy(&value->value);
    }
  }
  for (size_t i = 0u; i < vec_size(&plan->predicates); ++i) {
    orm_predicate *value = (orm_predicate *)vec_at(&plan->predicates, i);
    if (value != NULL) {
      tstr_freep(&value->column);
      bridge_owned_value_destroy(&value->value);
    }
  }
  for (size_t i = 0u; i < vec_size(&plan->raw_parameters); ++i)
    bridge_owned_value_destroy(
        (orm_owned_value *)vec_at(&plan->raw_parameters, i));
  vec_destroy(&plan->columns);
  vec_destroy(&plan->assignments);
  vec_destroy(&plan->predicates);
  vec_destroy(&plan->raw_parameters);
  tstr_freep(&plan->ordering.column);
  tstr_freep(&plan->table);
  tstr_freep(&plan->raw_sql);
  memset(plan, 0, sizeof(*plan));
}

static orm_status_t bridge_init_plan(orm_query_plan *plan,
                                     const orm_limits *limits,
                                     orm_error_t *error) {
  memset(plan, 0, sizeof(*plan));
  if (vec_init_bytes(&plan->columns, sizeof(tstr), _Alignof(tstr),
                     limits->max_columns) != STL_OK ||
      vec_init_bytes(&plan->assignments, sizeof(orm_assignment),
                     _Alignof(orm_assignment),
                     limits->max_assignments) != STL_OK ||
      vec_init_bytes(&plan->predicates, sizeof(orm_predicate),
                     _Alignof(orm_predicate),
                     limits->max_predicates) != STL_OK ||
      vec_init_bytes(&plan->raw_parameters, sizeof(orm_owned_value),
                     _Alignof(orm_owned_value),
                     limits->max_parameters) != STL_OK) {
    orm_driver_backend_plan_destroy(plan);
    return bridge_result(error, ORM_STATUS_OUT_OF_MEMORY,
                         "initialize Driver backend plan storage");
  }
  return ORM_STATUS_OK;
}

static orm_status_t bridge_validate_table(
    orm_driver_table_v1 table, size_t known_bytes,
    void *out_ops, orm_error_t *error, const char *role) {
  if (table.reserved != 0u || table.data == NULL ||
      table.bytes < known_bytes || table.bytes > ORM_DRIVER_DESCRIPTOR_MAX_BYTES)
    return bridge_result(error, ORM_STATUS_ABI_MISMATCH, role);
  orm_driver_header_v1 header;
  memcpy(&header, table.data, sizeof(header));
  if (!bridge_header_valid(&header, known_bytes))
    return bridge_result(error, ORM_STATUS_ABI_MISMATCH, role);
  memcpy(out_ops, table.data, known_bytes);
  return ORM_STATUS_OK;
}

static orm_status_t bridge_validate_shape(const orm_driver_plan_meta_v1 *meta,
                                          const orm_limits *limits,
                                          orm_error_t *error) {
  if (!bridge_header_valid(&meta->header, sizeof(*meta)) ||
      (meta->flags & ~BRIDGE_KNOWN_FLAGS) != 0u ||
      meta->column_count > limits->max_columns ||
      meta->assignment_count > limits->max_assignments ||
      meta->predicate_count > limits->max_predicates ||
      meta->raw_parameter_count > limits->max_parameters)
    return bridge_result(error, ORM_STATUS_ABI_MISMATCH,
                         "invalid Driver plan metadata");

  switch (meta->kind) {
    case ORM_DRIVER_PLAN_SELECT:
      if (meta->assignment_count != 0u || meta->raw_parameter_count != 0u ||
          ((meta->flags & ORM_DRIVER_PLAN_SELECT_ALL) != 0u &&
           meta->column_count != 0u))
        return bridge_result(error, ORM_STATUS_INVALID_ARGUMENT,
                             "invalid SELECT Driver plan shape");
      break;
    case ORM_DRIVER_PLAN_INSERT:
      if (meta->column_count != 0u || meta->predicate_count != 0u ||
          meta->raw_parameter_count != 0u ||
          (meta->flags & BRIDGE_KNOWN_FLAGS) != 0u)
        return bridge_result(error, ORM_STATUS_INVALID_ARGUMENT,
                             "invalid INSERT Driver plan shape");
      break;
    case ORM_DRIVER_PLAN_UPDATE:
      if (meta->column_count != 0u || meta->raw_parameter_count != 0u ||
          (meta->flags & BRIDGE_KNOWN_FLAGS) != 0u)
        return bridge_result(error, ORM_STATUS_INVALID_ARGUMENT,
                             "invalid UPDATE Driver plan shape");
      break;
    case ORM_DRIVER_PLAN_DELETE:
      if (meta->column_count != 0u || meta->assignment_count != 0u ||
          meta->raw_parameter_count != 0u ||
          (meta->flags & BRIDGE_KNOWN_FLAGS) != 0u)
        return bridge_result(error, ORM_STATUS_INVALID_ARGUMENT,
                             "invalid DELETE Driver plan shape");
      break;
    case ORM_DRIVER_PLAN_RAW_SQL:
      if (meta->column_count != 0u || meta->assignment_count != 0u ||
          meta->predicate_count != 0u ||
          (meta->flags & BRIDGE_KNOWN_FLAGS) != 0u)
        return bridge_result(error, ORM_STATUS_INVALID_ARGUMENT,
                             "invalid raw Driver plan shape");
      break;
    default:
      return bridge_result(error, ORM_STATUS_INVALID_ARGUMENT,
                           "unknown Driver plan kind");
  }
  return ORM_STATUS_OK;
}

orm_status_t orm_driver_backend_plan_materialize(
    const orm_driver_plan_view_v1 *view,
    const orm_driver_limits_v1 *driver_limits,
    orm_query_plan *out_plan,
    orm_error_t *error) {
  orm_driver_plan_metadata_ops_v1 metadata;
  orm_driver_plan_value_ops_v1 values;
  orm_driver_plan_meta_v1 meta = {BRIDGE_HEADER(orm_driver_plan_meta_v1)};
  orm_driver_ordering_v1 ordering = {
      BRIDGE_HEADER(orm_driver_ordering_v1), {NULL, 0u}, 0u, 0u};
  orm_limits limits;
  orm_status_t status;

  if (out_plan != NULL) memset(out_plan, 0, sizeof(*out_plan));
  if (view == NULL || driver_limits == NULL || out_plan == NULL ||
      !bridge_header_valid(&view->header, sizeof(*view)))
    return bridge_result(error, ORM_STATUS_ABI_MISMATCH,
                         "invalid Driver plan view");

  status = bridge_validate_table(view->metadata, sizeof(metadata),
                                 &metadata, error,
                                 "invalid Driver plan metadata table");
  if (status == ORM_STATUS_OK)
    status = bridge_validate_table(view->values, sizeof(values),
                                   &values, error,
                                   "invalid Driver plan value table");
  if (status == ORM_STATUS_OK)
    status = bridge_limits(driver_limits, &limits, error);
  if (status != ORM_STATUS_OK)
    return status;

  status = metadata.describe(view->context, &meta, error);
  if (status != ORM_STATUS_OK)
    return status;
  status = bridge_validate_shape(&meta, &limits, error);
  if (status != ORM_STATUS_OK)
    return status;
  status = bridge_init_plan(out_plan, &limits, error);
  if (status != ORM_STATUS_OK)
    return status;

  switch (meta.kind) {
    case ORM_DRIVER_PLAN_SELECT: out_plan->kind = ORM_QUERY_SELECT; break;
    case ORM_DRIVER_PLAN_INSERT: out_plan->kind = ORM_QUERY_INSERT; break;
    case ORM_DRIVER_PLAN_UPDATE: out_plan->kind = ORM_QUERY_UPDATE; break;
    case ORM_DRIVER_PLAN_DELETE: out_plan->kind = ORM_QUERY_DELETE; break;
    case ORM_DRIVER_PLAN_RAW_SQL: out_plan->kind = ORM_QUERY_RAW; break;
    default: status = ORM_STATUS_INVALID_ARGUMENT; goto fail;
  }

  if (out_plan->kind == ORM_QUERY_RAW) {
    if (meta.raw_sql.size == 0u) {
      status = bridge_result(error, ORM_STATUS_INVALID_ARGUMENT,
                             "raw Driver plan text is empty");
      goto fail;
    }
    status = bridge_copy_bytes(meta.raw_sql, limits.max_query_bytes, 1,
                               &out_plan->raw_sql, error,
                               "copy Driver raw SQL");
  } else {
    status = bridge_copy_identifier(meta.table, limits.max_query_bytes, 1,
                                    &out_plan->table, error,
                                    "copy Driver table name");
  }
  if (status != ORM_STATUS_OK) goto fail;

  out_plan->select_all =
      (meta.flags & ORM_DRIVER_PLAN_SELECT_ALL) != 0u;
  out_plan->has_limit = (meta.flags & ORM_DRIVER_PLAN_HAS_LIMIT) != 0u;
  out_plan->has_offset = (meta.flags & ORM_DRIVER_PLAN_HAS_OFFSET) != 0u;
  out_plan->limit = meta.limit;
  out_plan->offset = meta.offset;

  for (uint64_t i = 0u; i < meta.column_count; ++i) {
    orm_driver_bytes_v1 column = {NULL, 0u};
    tstr owned = NULL;
    status = metadata.column_at(view->context, i, &column, error);
    if (status == ORM_STATUS_OK)
      status = bridge_copy_identifier(column, limits.max_query_bytes, 1,
                                      &owned, error,
                                      "copy Driver result column");
    if (status == ORM_STATUS_OK)
      status = bridge_vec_status(vec_push(&out_plan->columns, &owned),
                                 error, "append Driver result column");
    if (status != ORM_STATUS_OK) {
      tstr_free(owned);
      goto fail;
    }
  }

  for (uint64_t i = 0u; i < meta.assignment_count; ++i) {
    orm_driver_assignment_v1 input = {
        BRIDGE_HEADER(orm_driver_assignment_v1), {NULL, 0u},
        {BRIDGE_HEADER(orm_driver_value_v1), 0u, 0u, {0}}};
    orm_assignment owned;
    memset(&owned, 0, sizeof(owned));
    status = values.assignment_at(view->context, i, &input, error);
    if (status == ORM_STATUS_OK &&
        (!bridge_header_valid(&input.header, sizeof(input)) ||
         !bridge_header_valid(&input.value.header, sizeof(input.value))))
      status = bridge_result(error, ORM_STATUS_ABI_MISMATCH,
                             "invalid Driver assignment DTO");
    if (status == ORM_STATUS_OK)
      status = bridge_copy_identifier(input.column, limits.max_query_bytes, 0,
                                      &owned.column, error,
                                      "copy Driver assignment column");
    if (status == ORM_STATUS_OK)
      status = bridge_value(&input.value, out_plan, &limits,
                            &owned.value, error);
    if (status == ORM_STATUS_OK)
      status = bridge_vec_status(vec_push(&out_plan->assignments, &owned),
                                 error, "append Driver assignment");
    if (status != ORM_STATUS_OK) {
      tstr_free(owned.column);
      bridge_owned_value_destroy(&owned.value);
      goto fail;
    }
  }

  for (uint64_t i = 0u; i < meta.predicate_count; ++i) {
    orm_driver_predicate_v1 input = {
        BRIDGE_HEADER(orm_driver_predicate_v1), {NULL, 0u},
        0u, 0u, {BRIDGE_HEADER(orm_driver_value_v1), 0u, 0u, {0}}};
    orm_predicate owned;
    memset(&owned, 0, sizeof(owned));
    status = values.predicate_at(view->context, i, &input, error);
    if (status == ORM_STATUS_OK &&
        (!bridge_header_valid(&input.header, sizeof(input)) ||
         !bridge_header_valid(&input.value.header, sizeof(input.value)) ||
         input.reserved != 0u ||
         input.comparison > (uint32_t)ORM_COMPARE_NOT_LIKE))
      status = bridge_result(error, ORM_STATUS_ABI_MISMATCH,
                             "invalid Driver predicate DTO");
    if (status == ORM_STATUS_OK)
      status = bridge_copy_identifier(input.column, limits.max_query_bytes, 1,
                                      &owned.column, error,
                                      "copy Driver predicate column");
    if (status == ORM_STATUS_OK) {
      owned.comparison = (orm_compare_t)input.comparison;
      status = bridge_value(&input.value, out_plan, &limits,
                            &owned.value, error);
    }
    if (status == ORM_STATUS_OK)
      status = bridge_vec_status(vec_push(&out_plan->predicates, &owned),
                                 error, "append Driver predicate");
    if (status != ORM_STATUS_OK) {
      tstr_free(owned.column);
      bridge_owned_value_destroy(&owned.value);
      goto fail;
    }
  }

  for (uint64_t i = 0u; i < meta.raw_parameter_count; ++i) {
    orm_driver_value_v1 input = {
        BRIDGE_HEADER(orm_driver_value_v1), 0u, 0u, {0}};
    orm_owned_value owned;
    memset(&owned, 0, sizeof(owned));
    status = values.raw_parameter_at(view->context, i, &input, error);
    if (status == ORM_STATUS_OK)
      status = bridge_value(&input, out_plan, &limits, &owned, error);
    if (status == ORM_STATUS_OK)
      status = bridge_vec_status(vec_push(&out_plan->raw_parameters, &owned),
                                 error, "append Driver raw parameter");
    if (status != ORM_STATUS_OK) {
      bridge_owned_value_destroy(&owned);
      goto fail;
    }
  }

  status = metadata.ordering(view->context, &ordering, error);
  if (status != ORM_STATUS_OK) goto fail;
  if (!bridge_header_valid(&ordering.header, sizeof(ordering)) ||
      ordering.present > 1u) {
    status = bridge_result(error, ORM_STATUS_ABI_MISMATCH,
                           "invalid Driver ordering DTO");
    goto fail;
  }
  if (ordering.present != 0u) {
    if (out_plan->kind != ORM_QUERY_SELECT ||
        ordering.order > (uint32_t)ORM_ORDER_DESCENDING) {
      status = bridge_result(error, ORM_STATUS_INVALID_ARGUMENT,
                             "invalid Driver ordering");
      goto fail;
    }
    status = bridge_copy_identifier(ordering.column, limits.max_query_bytes, 1,
                                    &out_plan->ordering.column, error,
                                    "copy Driver ordering column");
    if (status != ORM_STATUS_OK) goto fail;
    out_plan->ordering.present = true;
    out_plan->ordering.order = (orm_order_t)ordering.order;
  }

  return bridge_result(error, ORM_STATUS_OK, NULL);

fail:
  orm_driver_backend_plan_destroy(out_plan);
  return status;
}

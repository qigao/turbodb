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
  size_t max_parameters = 0u;
  size_t max_columns = 0u;
  size_t max_predicates = 0u;
  size_t max_assignments = 0u;
  size_t max_query_bytes = 0u;
  size_t max_parameter_bytes = 0u;
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
  orm_driver_plan_meta_v1 meta;
  orm_driver_ordering_v1 ordering;
  orm_limits limits;
  orm_status_t status;

  memset(&metadata, 0, sizeof(metadata));
  memset(&values, 0, sizeof(values));
  memset(&meta, 0, sizeof(meta));
  meta.header = (orm_driver_header_v1)BRIDGE_HEADER(orm_driver_plan_meta_v1);
  memset(&ordering, 0, sizeof(ordering));
  ordering.header =
      (orm_driver_header_v1)BRIDGE_HEADER(orm_driver_ordering_v1);

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
    orm_driver_assignment_v1 input;
    orm_assignment owned;
    memset(&input, 0, sizeof(input));
    input.header =
        (orm_driver_header_v1)BRIDGE_HEADER(orm_driver_assignment_v1);
    input.value.header =
        (orm_driver_header_v1)BRIDGE_HEADER(orm_driver_value_v1);
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
    orm_driver_predicate_v1 input;
    orm_predicate owned;
    memset(&input, 0, sizeof(input));
    input.header =
        (orm_driver_header_v1)BRIDGE_HEADER(orm_driver_predicate_v1);
    input.value.header =
        (orm_driver_header_v1)BRIDGE_HEADER(orm_driver_value_v1);
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
    orm_driver_value_v1 input;
    orm_owned_value owned;
    memset(&input, 0, sizeof(input));
    input.header =
        (orm_driver_header_v1)BRIDGE_HEADER(orm_driver_value_v1);
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


typedef struct bridge_connection {
  orm_backend backend;
} bridge_connection;

typedef struct bridge_cursor {
  orm_row_cursor cursor;
} bridge_cursor;

typedef struct bridge_transaction {
  orm_transaction_backend transaction;
} bridge_transaction;

static int bridge_backend_valid(const orm_backend *backend) {
  return backend != NULL && backend->context != NULL &&
         backend->ops != NULL &&
         backend->ops->abi_version == ORM_BACKEND_OPS_ABI_VERSION &&
         backend->ops->struct_size >= sizeof(*backend->ops) &&
         backend->ops->destroy != NULL &&
         backend->ops->open_cursor != NULL &&
         backend->ops->execute_command != NULL &&
         backend->ops->begin_transaction != NULL;
}

static int bridge_row_cursor_valid(const orm_row_cursor *cursor) {
  return cursor != NULL && cursor->context != NULL &&
         cursor->ops != NULL &&
         cursor->ops->abi_version == ORM_ROW_CURSOR_OPS_ABI_VERSION &&
         cursor->ops->struct_size >= sizeof(*cursor->ops) &&
         cursor->ops->next != NULL &&
         cursor->ops->cancel != NULL &&
         cursor->ops->destroy != NULL;
}

static int bridge_transaction_valid(
    const orm_transaction_backend *transaction) {
  return transaction != NULL && transaction->context != NULL &&
         transaction->ops != NULL &&
         transaction->ops->abi_version ==
             ORM_TRANSACTION_BACKEND_OPS_ABI_VERSION &&
         transaction->ops->struct_size >= sizeof(*transaction->ops) &&
         transaction->ops->destroy != NULL &&
         transaction->ops->open_cursor != NULL &&
         transaction->ops->execute_command != NULL &&
         transaction->ops->commit != NULL &&
         transaction->ops->rollback != NULL;
}

static void bridge_backend_dispose(orm_backend *backend) {
  if (backend == NULL) return;
  if (backend->context != NULL && backend->ops != NULL &&
      backend->ops->destroy != NULL)
    backend->ops->destroy(backend->context);
  memset(backend, 0, sizeof(*backend));
}

static void bridge_row_cursor_dispose(orm_row_cursor *cursor) {
  if (cursor == NULL) return;
  if (cursor->context != NULL && cursor->ops != NULL &&
      cursor->ops->destroy != NULL)
    cursor->ops->destroy(cursor->context);
  memset(cursor, 0, sizeof(*cursor));
}

static void bridge_transaction_dispose(
    orm_transaction_backend *transaction) {
  if (transaction == NULL) return;
  if (transaction->context != NULL && transaction->ops != NULL &&
      transaction->ops->destroy != NULL)
    transaction->ops->destroy(transaction->context);
  memset(transaction, 0, sizeof(*transaction));
}

static orm_status_t bridge_driver_limits(
    const orm_driver_limits_v1 *input, orm_limits *out,
    orm_error_t *error) {
  return bridge_limits(input, out, error);
}

static orm_status_t bridge_step_begin(
    orm_driver_step_v1 *step, orm_error_t *error) {
  orm_driver_header_v1 header;
  if (step == NULL)
    return bridge_result(error, ORM_STATUS_INVALID_ARGUMENT,
                         "missing Driver cursor step output");
  memcpy(&header, &step->header, sizeof(header));
  if (!bridge_header_valid(&header, sizeof(*step)))
    return bridge_result(error, ORM_STATUS_ABI_MISMATCH,
                         "invalid Driver cursor step output");
  memset(step, 0, sizeof(*step));
  step->header = header;
  return ORM_STATUS_OK;
}

static orm_status_t ORM_DRIVER_CALL bridge_cursor_next(
    void *context, cserde_reader *reader,
    orm_driver_step_v1 *out_step, orm_error_t *error) {
  bridge_cursor *wrapper = (bridge_cursor *)context;
  orm_status_t status = bridge_step_begin(out_step, error);
  if (status != ORM_STATUS_OK)
    return status;
  if (wrapper == NULL || !bridge_row_cursor_valid(&wrapper->cursor) ||
      reader == NULL)
    return bridge_result(error, ORM_STATUS_INVALID_ARGUMENT,
                         "invalid backend cursor next");

  const orm_row_cursor_step step =
      wrapper->cursor.ops->next(wrapper->cursor.context, reader);
  switch (step.kind) {
    case ORM_ROW_CURSOR_ROW:
      out_step->kind = ORM_DRIVER_STEP_ROW;
      break;
    case ORM_ROW_CURSOR_ROW_AND_DONE:
      out_step->kind = ORM_DRIVER_STEP_ROW_AND_DONE;
      break;
    case ORM_ROW_CURSOR_WAIT:
      out_step->kind = ORM_DRIVER_STEP_WAIT;
      out_step->waitable = step.waitable;
      break;
    case ORM_ROW_CURSOR_DONE:
      out_step->kind = ORM_DRIVER_STEP_DONE;
      break;
    case ORM_ROW_CURSOR_ERROR:
      return bridge_result(
          error,
          step.status != ORM_STATUS_OK ? step.status
                                       : ORM_STATUS_DATASTORE_ERROR,
          step.message != NULL && step.message[0] != '\0'
              ? step.message
              : "backend cursor failed");
    default:
      return bridge_result(error, ORM_STATUS_ABI_MISMATCH,
                           "backend cursor returned an unknown step");
  }
  return bridge_result(error, ORM_STATUS_OK, NULL);
}

static void ORM_DRIVER_CALL bridge_cursor_cancel(void *context) {
  bridge_cursor *wrapper = (bridge_cursor *)context;
  if (wrapper != NULL && bridge_row_cursor_valid(&wrapper->cursor))
    wrapper->cursor.ops->cancel(wrapper->cursor.context);
}

static void ORM_DRIVER_CALL bridge_cursor_destroy(void *context) {
  bridge_cursor *wrapper = (bridge_cursor *)context;
  if (wrapper == NULL) return;
  bridge_row_cursor_dispose(&wrapper->cursor);
  free(wrapper);
}

static orm_status_t ORM_DRIVER_CALL bridge_cursor_configure_shape(
    void *context, const cmeta_data_desc *shape,
    orm_error_t *error) {
  bridge_cursor *wrapper = (bridge_cursor *)context;
  if (wrapper == NULL || !bridge_row_cursor_valid(&wrapper->cursor) ||
      shape == NULL)
    return bridge_result(error, ORM_STATUS_INVALID_ARGUMENT,
                         "invalid backend cursor shape");
  if (wrapper->cursor.ops->configure_shape == NULL)
    return bridge_result(error, ORM_STATUS_UNSUPPORTED,
                         "backend cursor does not support shape configuration");
  return wrapper->cursor.ops->configure_shape(
      wrapper->cursor.context, shape, error);
}

static orm_status_t ORM_DRIVER_CALL bridge_cursor_column_count(
    void *context, uint64_t *out_count, orm_error_t *error) {
  bridge_cursor *wrapper = (bridge_cursor *)context;
  if (out_count != NULL) *out_count = 0u;
  if (wrapper == NULL || !bridge_row_cursor_valid(&wrapper->cursor) ||
      out_count == NULL)
    return bridge_result(error, ORM_STATUS_INVALID_ARGUMENT,
                         "invalid backend cursor column count");
  if (wrapper->cursor.ops->column_count == NULL)
    return bridge_result(error, ORM_STATUS_UNSUPPORTED,
                         "backend cursor does not expose column count");
  const orm_status_t status = wrapper->cursor.ops->column_count(
      wrapper->cursor.context, out_count);
  return bridge_result(error, status, NULL);
}

static const orm_driver_cursor_ops_v1 bridge_cursor_ops = {
    BRIDGE_HEADER(orm_driver_cursor_ops_v1),
    bridge_cursor_next,
    bridge_cursor_cancel,
    bridge_cursor_destroy,
    bridge_cursor_configure_shape,
    bridge_cursor_column_count};

static orm_status_t bridge_wrap_cursor(
    orm_row_cursor *cursor, orm_driver_cursor_v1 *out,
    orm_error_t *error) {
  if (out != NULL) memset(out, 0, sizeof(*out));
  if (cursor == NULL || out == NULL || !bridge_row_cursor_valid(cursor))
    return bridge_result(error, ORM_STATUS_ABI_MISMATCH,
                         "backend returned an invalid cursor");

  bridge_cursor *wrapper =
      (bridge_cursor *)calloc(1u, sizeof(*wrapper));
  if (wrapper == NULL)
    return bridge_result(error, ORM_STATUS_OUT_OF_MEMORY,
                         "allocate Driver cursor wrapper");

  wrapper->cursor = *cursor;
  memset(cursor, 0, sizeof(*cursor));
  out->header =
      (orm_driver_header_v1)BRIDGE_HEADER(orm_driver_cursor_v1);
  out->context = wrapper;
  out->ops = (orm_driver_table_v1) {
      &bridge_cursor_ops, (uint32_t)sizeof(bridge_cursor_ops), 0u};
  return bridge_result(error, ORM_STATUS_OK, NULL);
}

static orm_status_t bridge_backend_open_cursor(
    orm_backend *backend,
    const orm_driver_plan_view_v1 *view,
    const orm_driver_limits_v1 *driver_limits,
    orm_driver_cursor_v1 *out,
    orm_error_t *error) {
  if (out != NULL) memset(out, 0, sizeof(*out));
  if (!bridge_backend_valid(backend) || view == NULL ||
      driver_limits == NULL || out == NULL)
    return bridge_result(error, ORM_STATUS_INVALID_ARGUMENT,
                         "invalid backend cursor request");

  orm_query_plan plan;
  orm_limits limits;
  orm_row_cursor cursor;
  memset(&plan, 0, sizeof(plan));
  memset(&limits, 0, sizeof(limits));
  memset(&cursor, 0, sizeof(cursor));

  orm_status_t status =
      orm_driver_backend_plan_materialize(
          view, driver_limits, &plan, error);
  if (status == ORM_STATUS_OK)
    status = bridge_driver_limits(driver_limits, &limits, error);
  if (status == ORM_STATUS_OK)
    status = backend->ops->open_cursor(
        backend->context, &plan, &limits, &cursor, error);
  orm_driver_backend_plan_destroy(&plan);
  if (status != ORM_STATUS_OK) {
    bridge_row_cursor_dispose(&cursor);
    return status;
  }

  status = bridge_wrap_cursor(&cursor, out, error);
  if (status != ORM_STATUS_OK)
    bridge_row_cursor_dispose(&cursor);
  return status;
}

static orm_status_t bridge_backend_execute_command(
    orm_backend *backend,
    const orm_driver_plan_view_v1 *view,
    const orm_driver_limits_v1 *driver_limits,
    uint64_t *affected_rows,
    orm_error_t *error) {
  if (affected_rows != NULL) *affected_rows = 0u;
  if (!bridge_backend_valid(backend) || view == NULL ||
      driver_limits == NULL || affected_rows == NULL)
    return bridge_result(error, ORM_STATUS_INVALID_ARGUMENT,
                         "invalid backend command request");

  orm_query_plan plan;
  orm_limits limits;
  memset(&plan, 0, sizeof(plan));
  memset(&limits, 0, sizeof(limits));

  orm_status_t status =
      orm_driver_backend_plan_materialize(
          view, driver_limits, &plan, error);
  if (status == ORM_STATUS_OK)
    status = bridge_driver_limits(driver_limits, &limits, error);
  if (status == ORM_STATUS_OK)
    status = backend->ops->execute_command(
        backend->context, &plan, &limits, affected_rows, error);
  orm_driver_backend_plan_destroy(&plan);
  if (status != ORM_STATUS_OK) *affected_rows = 0u;
  return status;
}

static orm_status_t bridge_transaction_open_cursor_impl(
    bridge_transaction *wrapper,
    const orm_driver_plan_view_v1 *view,
    const orm_driver_limits_v1 *driver_limits,
    orm_driver_cursor_v1 *out,
    orm_error_t *error) {
  if (out != NULL) memset(out, 0, sizeof(*out));
  if (wrapper == NULL ||
      !bridge_transaction_valid(&wrapper->transaction) ||
      view == NULL || driver_limits == NULL || out == NULL)
    return bridge_result(error, ORM_STATUS_INVALID_ARGUMENT,
                         "invalid backend transaction cursor request");

  orm_query_plan plan;
  orm_limits limits;
  orm_row_cursor cursor;
  memset(&plan, 0, sizeof(plan));
  memset(&limits, 0, sizeof(limits));
  memset(&cursor, 0, sizeof(cursor));

  orm_status_t status =
      orm_driver_backend_plan_materialize(
          view, driver_limits, &plan, error);
  if (status == ORM_STATUS_OK)
    status = bridge_driver_limits(driver_limits, &limits, error);
  if (status == ORM_STATUS_OK)
    status = wrapper->transaction.ops->open_cursor(
        wrapper->transaction.context, &plan, &limits, &cursor, error);
  orm_driver_backend_plan_destroy(&plan);
  if (status != ORM_STATUS_OK) {
    bridge_row_cursor_dispose(&cursor);
    return status;
  }

  status = bridge_wrap_cursor(&cursor, out, error);
  if (status != ORM_STATUS_OK)
    bridge_row_cursor_dispose(&cursor);
  return status;
}

static orm_status_t bridge_transaction_execute_impl(
    bridge_transaction *wrapper,
    const orm_driver_plan_view_v1 *view,
    const orm_driver_limits_v1 *driver_limits,
    uint64_t *affected_rows,
    orm_error_t *error) {
  if (affected_rows != NULL) *affected_rows = 0u;
  if (wrapper == NULL ||
      !bridge_transaction_valid(&wrapper->transaction) ||
      view == NULL || driver_limits == NULL ||
      affected_rows == NULL)
    return bridge_result(error, ORM_STATUS_INVALID_ARGUMENT,
                         "invalid backend transaction command request");

  orm_query_plan plan;
  orm_limits limits;
  memset(&plan, 0, sizeof(plan));
  memset(&limits, 0, sizeof(limits));

  orm_status_t status =
      orm_driver_backend_plan_materialize(
          view, driver_limits, &plan, error);
  if (status == ORM_STATUS_OK)
    status = bridge_driver_limits(driver_limits, &limits, error);
  if (status == ORM_STATUS_OK)
    status = wrapper->transaction.ops->execute_command(
        wrapper->transaction.context, &plan, &limits,
        affected_rows, error);
  orm_driver_backend_plan_destroy(&plan);
  if (status != ORM_STATUS_OK) *affected_rows = 0u;
  return status;
}

static orm_status_t ORM_DRIVER_CALL bridge_transaction_open_cursor(
    void *context, const orm_driver_plan_view_v1 *view,
    const orm_driver_limits_v1 *limits,
    orm_driver_cursor_v1 *out, orm_error_t *error) {
  return bridge_transaction_open_cursor_impl(
      (bridge_transaction *)context, view, limits, out, error);
}

static orm_status_t ORM_DRIVER_CALL bridge_transaction_execute_command(
    void *context, const orm_driver_plan_view_v1 *view,
    const orm_driver_limits_v1 *limits,
    uint64_t *affected_rows, orm_error_t *error) {
  return bridge_transaction_execute_impl(
      (bridge_transaction *)context, view, limits, affected_rows, error);
}

static orm_status_t ORM_DRIVER_CALL bridge_transaction_commit(
    void *context, orm_error_t *error) {
  bridge_transaction *wrapper = (bridge_transaction *)context;
  if (wrapper == NULL ||
      !bridge_transaction_valid(&wrapper->transaction))
    return bridge_result(error, ORM_STATUS_INVALID_ARGUMENT,
                         "invalid backend transaction commit");
  return wrapper->transaction.ops->commit(
      wrapper->transaction.context, error);
}

static orm_status_t ORM_DRIVER_CALL bridge_transaction_rollback(
    void *context, orm_error_t *error) {
  bridge_transaction *wrapper = (bridge_transaction *)context;
  if (wrapper == NULL ||
      !bridge_transaction_valid(&wrapper->transaction))
    return bridge_result(error, ORM_STATUS_INVALID_ARGUMENT,
                         "invalid backend transaction rollback");
  return wrapper->transaction.ops->rollback(
      wrapper->transaction.context, error);
}

static orm_status_t bridge_transaction_savepoint_call(
    bridge_transaction *wrapper,
    orm_driver_bytes_v1 name,
    orm_status_t (*callback)(void *, vstr, orm_error_t *),
    orm_error_t *error) {
  if (wrapper == NULL ||
      !bridge_transaction_valid(&wrapper->transaction) ||
      callback == NULL ||
      name.size == 0u || name.size > (uint64_t)SIZE_MAX ||
      name.data == NULL)
    return bridge_result(error,
                         callback == NULL ? ORM_STATUS_UNSUPPORTED
                                          : ORM_STATUS_INVALID_ARGUMENT,
                         "invalid backend savepoint request");
  const vstr view = {
      (const char *)name.data, (size_t)name.size};
  return callback(wrapper->transaction.context, view, error);
}

static orm_status_t ORM_DRIVER_CALL bridge_transaction_savepoint(
    void *context, orm_driver_bytes_v1 name, orm_error_t *error) {
  bridge_transaction *wrapper = (bridge_transaction *)context;
  return bridge_transaction_savepoint_call(
      wrapper, name,
      wrapper != NULL && wrapper->transaction.ops != NULL
          ? wrapper->transaction.ops->savepoint : NULL,
      error);
}

static orm_status_t ORM_DRIVER_CALL bridge_transaction_rollback_to_savepoint(
    void *context, orm_driver_bytes_v1 name, orm_error_t *error) {
  bridge_transaction *wrapper = (bridge_transaction *)context;
  return bridge_transaction_savepoint_call(
      wrapper, name,
      wrapper != NULL && wrapper->transaction.ops != NULL
          ? wrapper->transaction.ops->rollback_to_savepoint : NULL,
      error);
}

static orm_status_t ORM_DRIVER_CALL bridge_transaction_release_savepoint(
    void *context, orm_driver_bytes_v1 name, orm_error_t *error) {
  bridge_transaction *wrapper = (bridge_transaction *)context;
  return bridge_transaction_savepoint_call(
      wrapper, name,
      wrapper != NULL && wrapper->transaction.ops != NULL
          ? wrapper->transaction.ops->release_savepoint : NULL,
      error);
}

static void ORM_DRIVER_CALL bridge_transaction_destroy(void *context) {
  bridge_transaction *wrapper = (bridge_transaction *)context;
  if (wrapper == NULL) return;
  bridge_transaction_dispose(&wrapper->transaction);
  free(wrapper);
}

static const orm_driver_transaction_ops_v1 bridge_transaction_ops = {
    BRIDGE_HEADER(orm_driver_transaction_ops_v1),
    bridge_transaction_destroy,
    bridge_transaction_open_cursor,
    bridge_transaction_execute_command,
    bridge_transaction_commit,
    bridge_transaction_rollback,
    bridge_transaction_savepoint,
    bridge_transaction_rollback_to_savepoint,
    bridge_transaction_release_savepoint};

static orm_status_t bridge_wrap_transaction(
    orm_transaction_backend *transaction,
    orm_driver_transaction_v1 *out,
    orm_error_t *error) {
  if (out != NULL) memset(out, 0, sizeof(*out));
  if (transaction == NULL || out == NULL ||
      !bridge_transaction_valid(transaction))
    return bridge_result(error, ORM_STATUS_ABI_MISMATCH,
                         "backend returned an invalid transaction");

  bridge_transaction *wrapper =
      (bridge_transaction *)calloc(1u, sizeof(*wrapper));
  if (wrapper == NULL)
    return bridge_result(error, ORM_STATUS_OUT_OF_MEMORY,
                         "allocate Driver transaction wrapper");
  wrapper->transaction = *transaction;
  memset(transaction, 0, sizeof(*transaction));

  out->header =
      (orm_driver_header_v1)BRIDGE_HEADER(orm_driver_transaction_v1);
  out->context = wrapper;
  out->ops = (orm_driver_table_v1) {
      &bridge_transaction_ops,
      (uint32_t)sizeof(bridge_transaction_ops), 0u};
  return bridge_result(error, ORM_STATUS_OK, NULL);
}

static void ORM_DRIVER_CALL bridge_connection_destroy(void *context) {
  bridge_connection *wrapper = (bridge_connection *)context;
  if (wrapper == NULL) return;
  bridge_backend_dispose(&wrapper->backend);
  free(wrapper);
}

static orm_status_t ORM_DRIVER_CALL bridge_connection_open_cursor(
    void *context, const orm_driver_plan_view_v1 *view,
    const orm_driver_limits_v1 *limits,
    orm_driver_cursor_v1 *out, orm_error_t *error) {
  bridge_connection *wrapper = (bridge_connection *)context;
  if (wrapper == NULL)
    return bridge_result(error, ORM_STATUS_INVALID_ARGUMENT,
                         "invalid Driver backend connection");
  return bridge_backend_open_cursor(
      &wrapper->backend, view, limits, out, error);
}

static orm_status_t ORM_DRIVER_CALL bridge_connection_execute_command(
    void *context, const orm_driver_plan_view_v1 *view,
    const orm_driver_limits_v1 *limits,
    uint64_t *affected_rows, orm_error_t *error) {
  bridge_connection *wrapper = (bridge_connection *)context;
  if (wrapper == NULL)
    return bridge_result(error, ORM_STATUS_INVALID_ARGUMENT,
                         "invalid Driver backend connection");
  return bridge_backend_execute_command(
      &wrapper->backend, view, limits, affected_rows, error);
}

static orm_status_t ORM_DRIVER_CALL bridge_connection_begin_transaction(
    void *context, orm_isolation_t isolation,
    orm_driver_transaction_v1 *out, orm_error_t *error) {
  if (out != NULL) memset(out, 0, sizeof(*out));
  bridge_connection *wrapper = (bridge_connection *)context;
  if (wrapper == NULL || !bridge_backend_valid(&wrapper->backend) ||
      out == NULL)
    return bridge_result(error, ORM_STATUS_INVALID_ARGUMENT,
                         "invalid Driver backend transaction begin");

  orm_transaction_backend transaction;
  memset(&transaction, 0, sizeof(transaction));
  orm_status_t status = wrapper->backend.ops->begin_transaction(
      wrapper->backend.context, isolation, &transaction, error);
  if (status != ORM_STATUS_OK) {
    bridge_transaction_dispose(&transaction);
    return status;
  }

  status = bridge_wrap_transaction(&transaction, out, error);
  if (status != ORM_STATUS_OK)
    bridge_transaction_dispose(&transaction);
  return status;
}

static const orm_driver_connection_ops_v1 bridge_connection_ops = {
    BRIDGE_HEADER(orm_driver_connection_ops_v1),
    bridge_connection_destroy,
    bridge_connection_open_cursor,
    bridge_connection_execute_command,
    bridge_connection_begin_transaction};

orm_status_t orm_driver_backend_connection_create(
    orm_backend_factory_v1 factory,
    const orm_config_t *config,
    const orm_driver_limits_v1 *driver_limits,
    orm_driver_connection_v1 *out_connection,
    orm_error_t *error) {
  if (out_connection != NULL)
    memset(out_connection, 0, sizeof(*out_connection));
  if (factory == NULL || config == NULL ||
      driver_limits == NULL || out_connection == NULL)
    return bridge_result(error, ORM_STATUS_INVALID_ARGUMENT,
                         "invalid Driver backend factory request");

  orm_limits limits;
  memset(&limits, 0, sizeof(limits));
  orm_status_t status =
      bridge_driver_limits(driver_limits, &limits, error);
  if (status != ORM_STATUS_OK)
    return status;

  bridge_connection *wrapper =
      (bridge_connection *)calloc(1u, sizeof(*wrapper));
  if (wrapper == NULL)
    return bridge_result(error, ORM_STATUS_OUT_OF_MEMORY,
                         "allocate Driver backend connection");

  status = factory(config, &limits, &wrapper->backend, error);
  if (status != ORM_STATUS_OK) {
    bridge_backend_dispose(&wrapper->backend);
    free(wrapper);
    return status;
  }
  if (!bridge_backend_valid(&wrapper->backend)) {
    bridge_backend_dispose(&wrapper->backend);
    free(wrapper);
    return bridge_result(error, ORM_STATUS_ABI_MISMATCH,
                         "backend factory returned an invalid connection");
  }

  out_connection->header =
      (orm_driver_header_v1)BRIDGE_HEADER(orm_driver_connection_v1);
  out_connection->context = wrapper;
  out_connection->ops = (orm_driver_table_v1) {
      &bridge_connection_ops,
      (uint32_t)sizeof(bridge_connection_ops), 0u};
  return bridge_result(error, ORM_STATUS_OK, NULL);
}

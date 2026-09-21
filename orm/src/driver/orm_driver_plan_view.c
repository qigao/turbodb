#include "orm_driver_plan_view.h"

#include <string.h>

#define VIEW_HEADER(T) {(uint32_t)sizeof(T), ORM_DRIVER_ABI_VERSION}
#define VIEW_TABLE(p) {(p), (uint32_t)sizeof(*(p)), 0u}

_Static_assert(sizeof(size_t) <= sizeof(uint64_t),
               "driver indices must represent native container sizes");

/* Borrowing and each accessor use O(1) auxiliary storage, allocate nothing and
 * leave the frozen plan unchanged. Byte lengths come from tstr, never strlen. */
static orm_status_t view_result(orm_error_t *error, orm_status_t status) {
  orm_error_init(error);
  orm_error_set(error, status, NULL);
  return status;
}

static orm_status_t view_output_begin(void *out, size_t known_bytes) {
  orm_driver_header_v1 header;
  if (out == NULL) return ORM_STATUS_INVALID_ARGUMENT;
  /* The caller guarantees readable header storage even for an invalid header.
   * Do not trust struct_size as permission to read or clear an unknown tail. */
  memcpy(&header, out, sizeof(header));
  if (header.abi_version != ORM_DRIVER_ABI_VERSION ||
      header.struct_size < known_bytes)
    return ORM_STATUS_ABI_MISMATCH;
  if (header.struct_size > ORM_DRIVER_DESCRIPTOR_MAX_BYTES)
    return ORM_STATUS_LIMIT_EXCEEDED;
  memset((unsigned char *)out + sizeof(header), 0, known_bytes - sizeof(header));
  return ORM_STATUS_OK;
}

static orm_driver_bytes_v1 view_bytes(tstr text) {
  const orm_driver_bytes_v1 value = {text, text == NULL ? 0u : (uint64_t)tstr_len(text)};
  return value;
}

static orm_status_t view_kind(orm_query_kind kind, uint32_t *out) {
  switch (kind) {
    case ORM_QUERY_SELECT: *out = ORM_DRIVER_PLAN_SELECT; break;
    case ORM_QUERY_INSERT: *out = ORM_DRIVER_PLAN_INSERT; break;
    case ORM_QUERY_UPDATE: *out = ORM_DRIVER_PLAN_UPDATE; break;
    case ORM_QUERY_DELETE: *out = ORM_DRIVER_PLAN_DELETE; break;
    case ORM_QUERY_RAW: *out = ORM_DRIVER_PLAN_RAW_SQL; break;
    default: return ORM_STATUS_INVALID_ARGUMENT;
  }
  return ORM_STATUS_OK;
}

static orm_status_t view_comparison(orm_compare_t comparison, uint32_t *out) {
  switch (comparison) {
    case ORM_COMPARE_EQUAL: *out = ORM_COMPARE_EQUAL; break;
    case ORM_COMPARE_NOT_EQUAL: *out = ORM_COMPARE_NOT_EQUAL; break;
    case ORM_COMPARE_LESS: *out = ORM_COMPARE_LESS; break;
    case ORM_COMPARE_LESS_EQUAL: *out = ORM_COMPARE_LESS_EQUAL; break;
    case ORM_COMPARE_GREATER: *out = ORM_COMPARE_GREATER; break;
    case ORM_COMPARE_GREATER_EQUAL: *out = ORM_COMPARE_GREATER_EQUAL; break;
    case ORM_COMPARE_LIKE: *out = ORM_COMPARE_LIKE; break;
    case ORM_COMPARE_NOT_LIKE: *out = ORM_COMPARE_NOT_LIKE; break;
    default: return ORM_STATUS_INVALID_ARGUMENT;
  }
  return ORM_STATUS_OK;
}

/* out is an internal temporary, not the caller's partially constructed DTO. */
static orm_status_t view_value(const orm_owned_value *owned, orm_driver_value_v1 *out) {
  const orm_driver_header_v1 header = VIEW_HEADER(orm_driver_value_v1);
  memset(out, 0, sizeof(*out));
  out->header = header;
  switch (owned->kind) {
    case ORM_VALUE_NULL:
      out->kind = ORM_VALUE_NULL;
      break;
    case ORM_VALUE_INT64:
      out->kind = ORM_VALUE_INT64; out->data.sint = owned->data.int64_value;
      break;
    case ORM_VALUE_UINT64:
      out->kind = ORM_VALUE_UINT64; out->data.uint = owned->data.uint64_value;
      break;
    case ORM_VALUE_DOUBLE:
      out->kind = ORM_VALUE_DOUBLE; out->data.real = owned->data.double_value;
      break;
    case ORM_VALUE_BOOLEAN:
      if (owned->data.boolean_value > 1u) return ORM_STATUS_INVALID_ARGUMENT;
      out->kind = ORM_VALUE_BOOLEAN; out->data.boolean = owned->data.boolean_value;
      break;
    case ORM_VALUE_TEXT:
      out->kind = ORM_VALUE_TEXT; out->data.bytes = view_bytes(owned->bytes);
      break;
    case ORM_VALUE_BLOB:
      out->kind = ORM_VALUE_BLOB; out->data.bytes = view_bytes(owned->bytes);
      break;
    default:
      return ORM_STATUS_INVALID_ARGUMENT;
  }
  return ORM_STATUS_OK;
}

static orm_status_t ORM_DRIVER_CALL view_describe(const void *context,
    orm_driver_plan_meta_v1 *out, orm_error_t *error) {
  orm_status_t status = view_output_begin(out, sizeof(*out));
  const orm_query_plan *plan = context;
  uint32_t kind;
  if (status != ORM_STATUS_OK) return view_result(error, status);
  if (plan == NULL) return view_result(error, ORM_STATUS_INVALID_ARGUMENT);
  status = view_kind(plan->kind, &kind);
  if (status != ORM_STATUS_OK) return view_result(error, status);
  out->kind = kind;
  out->flags = (plan->select_all ? ORM_DRIVER_PLAN_SELECT_ALL : 0u) |
               (plan->has_limit ? ORM_DRIVER_PLAN_HAS_LIMIT : 0u) |
               (plan->has_offset ? ORM_DRIVER_PLAN_HAS_OFFSET : 0u);
  out->table = view_bytes(plan->table);
  out->raw_sql = view_bytes(plan->raw_sql);
  out->column_count = (uint64_t)vec_size(&plan->columns);
  out->assignment_count = (uint64_t)vec_size(&plan->assignments);
  out->predicate_count = (uint64_t)vec_size(&plan->predicates);
  out->raw_parameter_count = (uint64_t)vec_size(&plan->raw_parameters);
  out->limit = plan->limit; out->offset = plan->offset;
  return view_result(error, ORM_STATUS_OK);
}

static orm_status_t ORM_DRIVER_CALL view_column(const void *context, uint64_t index,
    orm_driver_bytes_v1 *out, orm_error_t *error) {
  const orm_query_plan *plan = context;
  if (out != NULL) memset(out, 0, sizeof(*out));
  if (plan == NULL || out == NULL) return view_result(error, ORM_STATUS_INVALID_ARGUMENT);
  if (index >= (uint64_t)vec_size(&plan->columns))
    return view_result(error, ORM_STATUS_OUT_OF_RANGE);
  const tstr *column = vec_at_const(&plan->columns, (size_t)index);
  *out = view_bytes(*column);
  return view_result(error, ORM_STATUS_OK);
}

static orm_status_t ORM_DRIVER_CALL view_ordering(const void *context,
    orm_driver_ordering_v1 *out, orm_error_t *error) {
  orm_status_t status = view_output_begin(out, sizeof(*out));
  const orm_query_plan *plan = context;
  if (status != ORM_STATUS_OK) return view_result(error, status);
  if (plan == NULL) return view_result(error, ORM_STATUS_INVALID_ARGUMENT);
  if (!plan->ordering.present) return view_result(error, ORM_STATUS_OK);
  switch (plan->ordering.order) {
    case ORM_ORDER_ASCENDING: out->order = ORM_ORDER_ASCENDING; break;
    case ORM_ORDER_DESCENDING: out->order = ORM_ORDER_DESCENDING; break;
    default: return view_result(error, ORM_STATUS_INVALID_ARGUMENT);
  }
  out->column = view_bytes(plan->ordering.column);
  out->present = 1u;
  return view_result(error, ORM_STATUS_OK);
}

static orm_status_t ORM_DRIVER_CALL view_assignment(const void *context, uint64_t index,
    orm_driver_assignment_v1 *out, orm_error_t *error) {
  orm_status_t status = view_output_begin(out, sizeof(*out));
  const orm_query_plan *plan = context;
  orm_driver_value_v1 value;
  if (status != ORM_STATUS_OK) return view_result(error, status);
  if (plan == NULL) return view_result(error, ORM_STATUS_INVALID_ARGUMENT);
  if (index >= (uint64_t)vec_size(&plan->assignments))
    return view_result(error, ORM_STATUS_OUT_OF_RANGE);
  const orm_assignment *assignment = vec_at_const(&plan->assignments, (size_t)index);
  status = view_value(&assignment->value, &value);
  if (status != ORM_STATUS_OK) return view_result(error, status);
  out->column = view_bytes(assignment->column);
  memcpy(&out->value, &value, sizeof(value));
  return view_result(error, ORM_STATUS_OK);
}

static orm_status_t ORM_DRIVER_CALL view_predicate(const void *context, uint64_t index,
    orm_driver_predicate_v1 *out, orm_error_t *error) {
  orm_status_t status = view_output_begin(out, sizeof(*out));
  const orm_query_plan *plan = context;
  orm_driver_value_v1 value;
  uint32_t comparison;
  if (status != ORM_STATUS_OK) return view_result(error, status);
  if (plan == NULL) return view_result(error, ORM_STATUS_INVALID_ARGUMENT);
  if (index >= (uint64_t)vec_size(&plan->predicates))
    return view_result(error, ORM_STATUS_OUT_OF_RANGE);
  const orm_predicate *predicate = vec_at_const(&plan->predicates, (size_t)index);
  status = view_comparison(predicate->comparison, &comparison);
  if (status != ORM_STATUS_OK) return view_result(error, status);
  status = view_value(&predicate->value, &value);
  if (status != ORM_STATUS_OK) return view_result(error, status);
  out->column = view_bytes(predicate->column);
  out->comparison = comparison;
  memcpy(&out->value, &value, sizeof(value));
  return view_result(error, ORM_STATUS_OK);
}

static orm_status_t ORM_DRIVER_CALL view_parameter(const void *context, uint64_t index,
    orm_driver_value_v1 *out, orm_error_t *error) {
  orm_status_t status = view_output_begin(out, sizeof(*out));
  const orm_query_plan *plan = context;
  orm_driver_value_v1 value;
  if (status != ORM_STATUS_OK) return view_result(error, status);
  if (plan == NULL) return view_result(error, ORM_STATUS_INVALID_ARGUMENT);
  if (index >= (uint64_t)vec_size(&plan->raw_parameters))
    return view_result(error, ORM_STATUS_OUT_OF_RANGE);
  const orm_owned_value *owned = vec_at_const(&plan->raw_parameters, (size_t)index);
  status = view_value(owned, &value);
  if (status != ORM_STATUS_OK) return view_result(error, status);
  value.header = out->header;
  memcpy(out, &value, sizeof(value));
  return view_result(error, ORM_STATUS_OK);
}

static const orm_driver_plan_metadata_ops_v1 metadata_ops = {
    VIEW_HEADER(orm_driver_plan_metadata_ops_v1), view_describe, view_column, view_ordering};
static const orm_driver_plan_value_ops_v1 value_ops = {
    VIEW_HEADER(orm_driver_plan_value_ops_v1), view_assignment, view_predicate, view_parameter};

const orm_driver_plan_metadata_ops_v1 *orm_driver_plan_metadata_services_v1(void) {
  return &metadata_ops;
}

const orm_driver_plan_value_ops_v1 *orm_driver_plan_value_services_v1(void) {
  return &value_ops;
}

orm_status_t orm_driver_plan_borrow(const orm_query_plan *plan,
    orm_driver_plan_view_v1 *out, orm_error_t *error) {
  uint32_t kind;
  if (out != NULL) memset(out, 0, sizeof(*out));
  if (plan == NULL || out == NULL) return view_result(error, ORM_STATUS_INVALID_ARGUMENT);
  orm_status_t status = view_kind(plan->kind, &kind);
  if (status != ORM_STATUS_OK) return view_result(error, status);
  const orm_driver_plan_view_v1 view = {
      VIEW_HEADER(orm_driver_plan_view_v1), plan,
      VIEW_TABLE(&metadata_ops), VIEW_TABLE(&value_ops)};
  memcpy(out, &view, sizeof(view));
  return view_result(error, ORM_STATUS_OK);
}

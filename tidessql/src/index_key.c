#include "index.h"
#include "wire.h"
#include "error.h"
#include <float.h>
#include <math.h>
#include <string.h>

enum { INDEX_KEY_MARKER = 1, INDEX_KEY_CELL = INDEX_KEY_MARKER + ORM_SQL_WIRE_U64,
       INDEX_KEY_NULL = 0, INDEX_KEY_VALUE = 1, INDEX_KEY_PASSES = 2,
       INDEX_KEY_SIGN_BIT = ORM_SQL_WIRE_U64 * ORM_SQL_WIRE_BITS - 1 };
static const uint64_t index_key_sign = UINT64_C(1) << INDEX_KEY_SIGN_BIT;
_Static_assert(sizeof(double) == sizeof(uint64_t) && FLT_RADIX == 2 &&
    DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024 && DBL_MIN_EXP == -1021,
    "SQL DOUBLE index keys require IEEE binary64");

static turbodb_status_t index_key_error(turbodb_error_t *error, turbodb_status_t status, const char *reason) {
  tdsql_error_set(error, status, reason); return status;
}
turbodb_status_t orm_tidesdb_sql_index_key_size(const orm_sql_index_definition *definition,
    size_t *out, turbodb_error_t *error) {
  if (!definition || !definition->budget || !definition->parts.initialized ||
      !vec_size(&definition->parts) || !out)
    return index_key_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "SQL index key requires a bound definition and size output");
  const size_t count = vec_size(&definition->parts);
  if (count > SIZE_MAX / INDEX_KEY_CELL)
    return index_key_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL index key size overflow");
  *out = count * INDEX_KEY_CELL; return TURBODB_STATUS_OK;
}
static turbodb_status_t index_key_charge(const orm_sql_index_definition *definition, size_t size, turbodb_error_t *error) {
  if (size > UINT64_MAX / INDEX_KEY_PASSES)
    return index_key_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL index key step capacity overflow");
  orm_sql_budget_amount amount = {0};
  amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = (uint64_t)size * INDEX_KEY_PASSES;
  return orm_tidesdb_sql_budget_reserve(definition->budget, &amount, error);
}
static turbodb_status_t index_key_type(const orm_sql_index_part *part, turbodb_error_t *error) {
  if (part->type.kind != TURBODB_VALUE_INT64 && part->type.kind != TURBODB_VALUE_UINT64 &&
      part->type.kind != TURBODB_VALUE_DOUBLE)
    return index_key_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "SQL index key requires a bound numeric type");
  return TURBODB_STATUS_OK;
}
static turbodb_status_t index_key_validate(const orm_sql_index_part *part, const turbodb_value_t *row,
    size_t count, orm_tidesdb_sql_budget *budget, turbodb_error_t *error) {
  turbodb_status_t status = index_key_type(part, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (part->column >= count)
    return index_key_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "SQL index key column is outside the input row");
  orm_sql_predicate predicate; turbodb_value_t ignored;
  status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL, part->type, NULL, &predicate, error);
  return status == TURBODB_STATUS_OK ?
      orm_tidesdb_sql_predicate_eval(&predicate, &row[part->column], NULL, budget, &ignored, error) : status;
}
static void index_key_write(const orm_sql_index_part *part, const turbodb_value_t *value, uint8_t *out) {
  const bool null = value->kind == TURBODB_VALUE_NULL;
  const uint64_t word = null ? 0 : value->kind == TURBODB_VALUE_INT64 ?
      orm_sql_wire_signed_order(value->data.int64_value) : value->kind == TURBODB_VALUE_DOUBLE ?
      orm_sql_wire_double_order(value->data.double_value) : value->data.uint64_value;
  out[0] = null ? INDEX_KEY_NULL : INDEX_KEY_VALUE;
  orm_sql_wire_order_write(out + INDEX_KEY_MARKER, word);
  if (part->descending) for (size_t i = 0; i < INDEX_KEY_CELL; ++i) out[i] = (uint8_t)~out[i];
}
turbodb_status_t orm_tidesdb_sql_index_key_encode(const orm_sql_index_definition *definition,
    const turbodb_value_t *row, size_t count, uint8_t *out, size_t capacity,
    bool *contains_null, turbodb_error_t *error) {
  if (!row || !out || !contains_null)
    return index_key_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "SQL index key encoding requires input and outputs");
  size_t size = 0;
  turbodb_status_t status = orm_tidesdb_sql_index_key_size(definition, &size, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (capacity < size)
    return index_key_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL index key output capacity exceeded");
  status = index_key_charge(definition, size, error);
  bool null = false;
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < vec_size(&definition->parts); ++i) {
    const orm_sql_index_part *part = vec_at_const(&definition->parts, i);
    status = index_key_validate(part, row, count, definition->budget, error);
    if (status == TURBODB_STATUS_OK) null |= row[part->column].kind == TURBODB_VALUE_NULL;
  }
  if (status != TURBODB_STATUS_OK) return status;
  /* All validation and admission precede publication; this pass cannot fail. */
  for (size_t i = 0; i < vec_size(&definition->parts); ++i) {
    const orm_sql_index_part *part = vec_at_const(&definition->parts, i);
    index_key_write(part, &row[part->column], out + i * INDEX_KEY_CELL);
  }
  *contains_null = null; return TURBODB_STATUS_OK;
}
static turbodb_value_t index_key_read(const orm_sql_index_part *part, const uint8_t *data) {
  if (data[0] == (part->descending ? (uint8_t)~INDEX_KEY_NULL : INDEX_KEY_NULL)) return turbodb_null();
  uint64_t word = orm_sql_wire_order_read(data + INDEX_KEY_MARKER);
  if (part->descending) word = ~word;
  if (part->type.kind == TURBODB_VALUE_DOUBLE) return turbodb_f64(orm_sql_wire_order_double(word));
  return part->type.kind == TURBODB_VALUE_INT64 ? turbodb_i64(orm_sql_wire_order_signed(word)) : turbodb_u64(word);
}
static turbodb_status_t index_key_check_cell(const orm_sql_index_part *part, const uint8_t *data,
    bool *contains_null, turbodb_error_t *error) {
  const turbodb_status_t status = index_key_type(part, error);
  if (status != TURBODB_STATUS_OK) return status;
  const uint8_t marker = part->descending ? (uint8_t)~data[0] : data[0];
  uint64_t word = orm_sql_wire_order_read(data + INDEX_KEY_MARKER);
  if (part->descending) word = ~word;
  if (marker != INDEX_KEY_NULL && marker != INDEX_KEY_VALUE)
    return index_key_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL index key has an invalid NULL marker");
  if (marker == INDEX_KEY_NULL) {
    if (!part->type.nullable || word)
      return index_key_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL index key has an invalid NULL payload or nullability");
    *contains_null = true;
  } else if (part->type.kind == TURBODB_VALUE_DOUBLE) {
    const double value = orm_sql_wire_order_double(word);
    if (!isfinite(value) || (value == 0.0 && word != index_key_sign))
      return index_key_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL index key has a nonfinite or noncanonical DOUBLE payload");
  }
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_tidesdb_sql_index_key_decode(const orm_sql_index_definition *definition,
    const uint8_t *data, size_t size, turbodb_value_t *out, size_t capacity,
    bool *contains_null, turbodb_error_t *error) {
  if (!data || !out || !contains_null)
    return index_key_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "SQL index key decoding requires input and outputs");
  size_t expected = 0;
  turbodb_status_t status = orm_tidesdb_sql_index_key_size(definition, &expected, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (size != expected)
    return index_key_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL index key length does not match its definition");
  if (capacity < vec_size(&definition->parts))
    return index_key_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL index key value output capacity exceeded");
  status = index_key_charge(definition, size, error);
  bool null = false;
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < vec_size(&definition->parts); ++i)
    status = index_key_check_cell(vec_at_const(&definition->parts, i), data + i * INDEX_KEY_CELL, &null, error);
  if (status != TURBODB_STATUS_OK) return status;
  for (size_t i = 0; i < vec_size(&definition->parts); ++i)
    out[i] = index_key_read(vec_at_const(&definition->parts, i), data + i * INDEX_KEY_CELL);
  *contains_null = null; return TURBODB_STATUS_OK;
}

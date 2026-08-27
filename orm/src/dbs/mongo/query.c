#include "query.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static const char *orm_mongo_field(tstr column,
                                   const orm_mongo_settings *settings) {
  return tstr_cmp(column, settings->id_column) == 0 ? "_id" : column;
}

static orm_status_t orm_mongo_bson_error(orm_error_t *error,
                                         const char *operation) {
  orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT, operation);
  return ORM_STATUS_INVALID_ARGUMENT;
}

static orm_status_t orm_mongo_append_value(bson_t *out, const char *key,
                                           const orm_owned_value *value,
                                           orm_error_t *error) {
  bool appended = false;
  const size_t size = value != NULL ? tstr_len(value->bytes) : 0u;
  if (out == NULL || key == NULL || value == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid MongoDB value append");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  switch (value->kind) {
    case ORM_VALUE_NULL:
      appended = bson_append_null(out, key, -1);
      break;
    case ORM_VALUE_INT64:
      appended = bson_append_int64(out, key, -1, value->data.int64_value);
      break;
    case ORM_VALUE_UINT64:
      if (value->data.uint64_value > (uint64_t)INT64_MAX) {
        orm_error_set(error, ORM_STATUS_OUT_OF_RANGE,
                      "unsigned parameter exceeds MongoDB int64 range");
        return ORM_STATUS_OUT_OF_RANGE;
      }
      appended = bson_append_int64(out, key, -1,
                                   (int64_t)value->data.uint64_value);
      break;
    case ORM_VALUE_DOUBLE:
      appended = bson_append_double(out, key, -1, value->data.double_value);
      break;
    case ORM_VALUE_BOOLEAN:
      appended = bson_append_bool(out, key, -1,
                                  value->data.boolean_value != 0u);
      break;
    case ORM_VALUE_TEXT:
      if (size > (size_t)INT_MAX) {
        orm_error_set(error, ORM_STATUS_LIMIT_EXCEEDED,
                      "MongoDB text parameter exceeds int range");
        return ORM_STATUS_LIMIT_EXCEEDED;
      }
      appended = bson_append_utf8(out, key, -1, value->bytes, (int)size);
      break;
    case ORM_VALUE_BLOB:
      if (size > (size_t)UINT32_MAX) {
        orm_error_set(error, ORM_STATUS_LIMIT_EXCEEDED,
                      "MongoDB blob parameter exceeds uint32 range");
        return ORM_STATUS_LIMIT_EXCEEDED;
      }
      appended = bson_append_binary(
          out, key, -1, BSON_SUBTYPE_BINARY,
          (const uint8_t *)value->bytes, (uint32_t)size);
      break;
    default:
      orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                    "unknown MongoDB parameter kind");
      return ORM_STATUS_INVALID_ARGUMENT;
  }
  return appended ? ORM_STATUS_OK
                  : orm_mongo_bson_error(error,
                                         "append MongoDB BSON value failed");
}

static orm_status_t orm_mongo_like_pattern(const orm_owned_value *value,
                                           const orm_limits *limits,
                                           tstr *out_pattern,
                                           orm_error_t *error) {
  static const char prefix[] = "\\A(?:";
  static const char suffix[] = ")\\z";
  size_t index;
  tstr pattern = tstr_dup(prefix);
  if (value == NULL || limits == NULL || out_pattern == NULL ||
      value->kind != ORM_VALUE_TEXT) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "MongoDB LIKE requires a text parameter");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  if (pattern == NULL) {
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "allocate MongoDB LIKE pattern");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  for (index = 0u; index < tstr_len(value->bytes); ++index) {
    const char next = value->bytes[index];
    const char *fragment = NULL;
    size_t fragment_size = 0u;
    char escaped[2];
    if (next == '%') {
      fragment = ".*";
      fragment_size = 2u;
    } else if (next == '_') {
      fragment = ".";
      fragment_size = 1u;
    } else if (strchr(".^$*+?()[]{}|\\", next) != NULL) {
      escaped[0] = '\\';
      escaped[1] = next;
      fragment = escaped;
      fragment_size = 2u;
    } else {
      escaped[0] = next;
      fragment = escaped;
      fragment_size = 1u;
    }
    if (fragment_size > limits->max_query_bytes ||
        tstr_len(pattern) > limits->max_query_bytes - fragment_size) {
      tstr_free(pattern);
      orm_error_set(error, ORM_STATUS_LIMIT_EXCEEDED,
                    "MongoDB LIKE pattern exceeds max_query_bytes");
      return ORM_STATUS_LIMIT_EXCEEDED;
    }
    {
      tstr appended = tstr_cat_len(pattern, fragment, fragment_size);
      if (appended == NULL) {
        tstr_free(pattern);
        orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                      "append MongoDB LIKE pattern");
        return ORM_STATUS_OUT_OF_MEMORY;
      }
      pattern = appended;
    }
  }
  if (sizeof(suffix) - 1u > limits->max_query_bytes ||
      tstr_len(pattern) > limits->max_query_bytes - (sizeof(suffix) - 1u)) {
    tstr_free(pattern);
    orm_error_set(error, ORM_STATUS_LIMIT_EXCEEDED,
                  "MongoDB LIKE pattern exceeds max_query_bytes");
    return ORM_STATUS_LIMIT_EXCEEDED;
  }
  pattern = tstr_cat(pattern, suffix);
  if (pattern == NULL) {
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "finish MongoDB LIKE pattern");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  *out_pattern = pattern;
  return ORM_STATUS_OK;
}

static orm_status_t orm_mongo_append_predicate(
    bson_t *out, const orm_predicate *predicate,
    const orm_mongo_settings *settings, const orm_limits *limits,
    orm_error_t *error) {
  const char *field = orm_mongo_field(predicate->column, settings);
  const char *operator_key = NULL;
  bson_t operand;
  orm_status_t status;
  switch (predicate->comparison) {
    case ORM_COMPARE_EQUAL:
      return orm_mongo_append_value(out, field, &predicate->value, error);
    case ORM_COMPARE_NOT_EQUAL: operator_key = "$ne"; break;
    case ORM_COMPARE_LESS: operator_key = "$lt"; break;
    case ORM_COMPARE_LESS_EQUAL: operator_key = "$lte"; break;
    case ORM_COMPARE_GREATER: operator_key = "$gt"; break;
    case ORM_COMPARE_GREATER_EQUAL: operator_key = "$gte"; break;
    case ORM_COMPARE_LIKE:
    case ORM_COMPARE_NOT_LIKE: {
      tstr pattern = NULL;
      bson_t negative;
      status = orm_mongo_like_pattern(&predicate->value, limits, &pattern,
                                      error);
      if (status != ORM_STATUS_OK)
        return status;
      if (!bson_append_document_begin(out, field, -1, &operand)) {
        tstr_free(pattern);
        return orm_mongo_bson_error(error,
                                    "begin MongoDB regex operand failed");
      }
      if (predicate->comparison == ORM_COMPARE_LIKE) {
        const bool ok = bson_append_utf8(&operand, "$regex", -1, pattern,
                                         (int)tstr_len(pattern));
        tstr_free(pattern);
        if (!ok || !bson_append_document_end(out, &operand))
          return orm_mongo_bson_error(error,
                                      "append MongoDB regex failed");
      } else {
        if (!bson_append_document_begin(&operand, "$not", -1, &negative) ||
            !bson_append_utf8(&negative, "$regex", -1, pattern,
                              (int)tstr_len(pattern)) ||
            !bson_append_document_end(&operand, &negative) ||
            !bson_append_document_end(out, &operand)) {
          tstr_free(pattern);
          return orm_mongo_bson_error(error,
                                      "append MongoDB negative regex failed");
        }
        tstr_free(pattern);
      }
      return ORM_STATUS_OK;
    }
    default:
      orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                    "unknown MongoDB comparison");
      return ORM_STATUS_INVALID_ARGUMENT;
  }
  if (!bson_append_document_begin(out, field, -1, &operand))
    return orm_mongo_bson_error(error,
                                "begin MongoDB comparison operand failed");
  status = orm_mongo_append_value(&operand, operator_key,
                                  &predicate->value, error);
  if (status != ORM_STATUS_OK)
    return status;
  if (!bson_append_document_end(out, &operand))
    return orm_mongo_bson_error(error,
                                "finish MongoDB comparison operand failed");
  return ORM_STATUS_OK;
}

orm_status_t orm_mongo_append_filter(bson_t *out,
                                     const orm_query_plan *plan,
                                     const orm_mongo_settings *settings,
                                     const orm_limits *limits,
                                     orm_error_t *error) {
  const size_t count = plan != NULL ? vec_size(&plan->predicates) : 0u;
  size_t index;
  if (out == NULL || plan == NULL || settings == NULL || limits == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid MongoDB filter request");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  if (count == 1u) {
    return orm_mongo_append_predicate(
        out, (const orm_predicate *)vec_at_const(&plan->predicates, 0u),
        settings, limits, error);
  }
  if (count > 1u) {
    bson_t array;
    if (!bson_append_array_begin(out, "$and", -1, &array))
      return orm_mongo_bson_error(error, "begin MongoDB AND filter failed");
    for (index = 0u; index < count; ++index) {
      char key[32];
      bson_t item;
      const int length = snprintf(key, sizeof(key), "%zu", index);
      orm_status_t status;
      if (length <= 0 || (size_t)length >= sizeof(key) ||
          !bson_append_document_begin(&array, key, length, &item))
        return orm_mongo_bson_error(error,
                                    "begin MongoDB predicate item failed");
      status = orm_mongo_append_predicate(
          &item,
          (const orm_predicate *)vec_at_const(&plan->predicates, index),
          settings, limits, error);
      if (status != ORM_STATUS_OK)
        return status;
      if (!bson_append_document_end(&array, &item))
        return orm_mongo_bson_error(error,
                                    "finish MongoDB predicate item failed");
    }
    if (!bson_append_array_end(out, &array))
      return orm_mongo_bson_error(error, "finish MongoDB AND filter failed");
  }
  return ORM_STATUS_OK;
}

orm_status_t orm_mongo_append_find_options(
    bson_t *out, const orm_query_plan *plan,
    const orm_mongo_settings *settings, orm_error_t *error) {
  bson_t projection;
  size_t index;
  bool projects_id = false;
  if (out == NULL || plan == NULL || settings == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid MongoDB find options request");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  if (plan->select_all || vec_size(&plan->columns) == 0u) {
    orm_error_set(error, ORM_STATUS_UNSUPPORTED,
                  "MongoDB SELECT requires explicit projected columns");
    return ORM_STATUS_UNSUPPORTED;
  }
  if (!bson_append_document_begin(out, "projection", -1, &projection))
    return orm_mongo_bson_error(error,
                                "begin MongoDB projection failed");
  for (index = 0u; index < vec_size(&plan->columns); ++index) {
    const tstr column = *(const tstr *)vec_at_const(&plan->columns, index);
    const char *field = orm_mongo_field(column, settings);
    if (strcmp(field, "_id") == 0)
      projects_id = true;
    if (!bson_append_int32(&projection, field, -1, 1))
      return orm_mongo_bson_error(error,
                                  "append MongoDB projection failed");
  }
  if (!projects_id &&
      !bson_append_int32(&projection, "_id", -1, 0))
    return orm_mongo_bson_error(error,
                                "suppress MongoDB native id failed");
  if (!bson_append_document_end(out, &projection))
    return orm_mongo_bson_error(error,
                                "finish MongoDB projection failed");
  if (plan->ordering.present) {
    bson_t sort;
    if (!bson_append_document_begin(out, "sort", -1, &sort) ||
        !bson_append_int32(
            &sort, orm_mongo_field(plan->ordering.column, settings), -1,
            plan->ordering.order == ORM_ORDER_DESCENDING ? -1 : 1) ||
        !bson_append_document_end(out, &sort))
      return orm_mongo_bson_error(error, "append MongoDB sort failed");
  }
  if (plan->has_offset) {
    if (plan->offset > (uint64_t)INT64_MAX) {
      orm_error_set(error, ORM_STATUS_LIMIT_EXCEEDED,
                    "MongoDB offset exceeds int64 range");
      return ORM_STATUS_LIMIT_EXCEEDED;
    }
    if (!bson_append_int64(out, "skip", -1, (int64_t)plan->offset))
      return orm_mongo_bson_error(error, "append MongoDB offset failed");
  }
  if (plan->has_limit) {
    if (plan->limit > (uint64_t)INT64_MAX) {
      orm_error_set(error, ORM_STATUS_LIMIT_EXCEEDED,
                    "MongoDB limit exceeds int64 range");
      return ORM_STATUS_LIMIT_EXCEEDED;
    }
    if (!bson_append_int64(out, "limit", -1, (int64_t)plan->limit))
      return orm_mongo_bson_error(error, "append MongoDB limit failed");
  }
  return ORM_STATUS_OK;
}

orm_status_t orm_mongo_append_insert_document(
    bson_t *out, const orm_query_plan *plan,
    const orm_mongo_settings *settings, orm_error_t *error) {
  size_t index;
  if (out == NULL || plan == NULL || settings == NULL ||
      vec_size(&plan->assignments) == 0u) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "MongoDB INSERT has no assignments");
    return ORM_STATUS_INVALID_STATE;
  }
  for (index = 0u; index < vec_size(&plan->assignments); ++index) {
    const orm_assignment *assignment =
        (const orm_assignment *)vec_at_const(&plan->assignments, index);
    const orm_status_t status = orm_mongo_append_value(
        out, orm_mongo_field(assignment->column, settings),
        &assignment->value, error);
    if (status != ORM_STATUS_OK)
      return status;
  }
  return ORM_STATUS_OK;
}

orm_status_t orm_mongo_append_update_document(
    bson_t *out, const orm_query_plan *plan,
    const orm_mongo_settings *settings, orm_error_t *error) {
  bson_t assignments;
  size_t index;
  if (out == NULL || plan == NULL || settings == NULL ||
      vec_size(&plan->assignments) == 0u) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "MongoDB UPDATE has no assignments");
    return ORM_STATUS_INVALID_STATE;
  }
  if (!bson_append_document_begin(out, "$set", -1, &assignments))
    return orm_mongo_bson_error(error,
                                "begin MongoDB update document failed");
  for (index = 0u; index < vec_size(&plan->assignments); ++index) {
    const orm_assignment *assignment =
        (const orm_assignment *)vec_at_const(&plan->assignments, index);
    const char *field = orm_mongo_field(assignment->column, settings);
    const orm_status_t status =
        strcmp(field, "_id") == 0
            ? ORM_STATUS_UNSUPPORTED
            : orm_mongo_append_value(&assignments, field,
                                     &assignment->value, error);
    if (status == ORM_STATUS_UNSUPPORTED) {
      orm_error_set(error, status,
                    "MongoDB UPDATE cannot modify the id column");
      return status;
    }
    if (status != ORM_STATUS_OK)
      return status;
  }
  if (!bson_append_document_end(out, &assignments))
    return orm_mongo_bson_error(error,
                                "finish MongoDB update document failed");
  return ORM_STATUS_OK;
}

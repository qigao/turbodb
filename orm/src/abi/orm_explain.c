#include "orm_internal.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct orm_execution_plan_owned_node {
  uint32_t flags;
  uint64_t parent_index;
  uint64_t ordinal;
  orm_string_view_t node_type;
  orm_string_view_t relation;
  orm_string_view_t index_name;
  orm_string_view_t detail;
  double estimated_rows;
  double actual_rows;
  double startup_cost;
  double total_cost;
  double actual_startup_ms;
  double actual_total_ms;
} orm_execution_plan_owned_node;

struct orm_execution_plan {
  orm_explain_mode_t mode;
  orm_string_view_t provider;
  orm_string_view_t raw_detail;
  orm_execution_plan_owned_node *nodes;
  uint64_t count;
  uint64_t capacity;
  uint64_t copied_bytes;
  uint64_t max_nodes;
  uint64_t max_bytes;
};

static orm_status_t explain_fail(orm_error_t *error, orm_status_t status,
                                 const char *message) {
  orm_error_set(error, status, message);
  return status;
}

static void explain_free_view(orm_string_view_t *view) {
  if (view == NULL) return;
  free((void *)view->data);
  view->data = NULL;
  view->len = 0u;
}

void ORM_C_CALL orm_execution_plan_destroy(orm_execution_plan_t *plan) {
  if (plan == NULL) return;
  for (uint64_t i = 0u; i < plan->count; ++i) {
    orm_execution_plan_owned_node *node = &plan->nodes[i];
    explain_free_view(&node->node_type);
    explain_free_view(&node->relation);
    explain_free_view(&node->index_name);
    explain_free_view(&node->detail);
  }
  explain_free_view(&plan->provider);
  explain_free_view(&plan->raw_detail);
  free(plan->nodes);
  free(plan);
}

static orm_status_t explain_copy_view(orm_execution_plan_t *plan,
                                      orm_string_view_t input,
                                      orm_string_view_t *out,
                                      orm_error_t *error) {
  out->data = NULL;
  out->len = 0u;
  if (input.len == 0u) return ORM_STATUS_OK;
  if (input.data == NULL)
    return explain_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                        "execution-plan string view is invalid");
  if (input.len == SIZE_MAX ||
      plan->copied_bytes > plan->max_bytes ||
      (uint64_t)input.len > plan->max_bytes - plan->copied_bytes)
    return explain_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                        "execution-plan byte budget exceeded");
  char *copy = (char *)malloc(input.len + 1u);
  if (copy == NULL)
    return explain_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                        "copy execution-plan string");
  memcpy(copy, input.data, input.len);
  copy[input.len] = '\0';
  out->data = copy;
  out->len = input.len;
  plan->copied_bytes += (uint64_t)input.len;
  return ORM_STATUS_OK;
}

static orm_status_t explain_copy_cstr(orm_execution_plan_t *plan,
                                      const char *input,
                                      orm_string_view_t *out,
                                      orm_error_t *error) {
  const orm_string_view_t view =
      input != NULL ? orm_view(input) : (orm_string_view_t){NULL, 0u};
  return explain_copy_view(plan, view, out, error);
}

static orm_status_t explain_reserve_node(orm_execution_plan_t *plan,
                                         orm_error_t *error) {
  if (plan->count >= plan->max_nodes)
    return explain_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                        "execution-plan node budget exceeded");
  if (plan->count < plan->capacity) return ORM_STATUS_OK;
  uint64_t next = plan->capacity == 0u ? 16u : plan->capacity * 2u;
  if (next > plan->max_nodes) next = plan->max_nodes;
  if (next <= plan->capacity ||
      next > (uint64_t)(SIZE_MAX / sizeof(orm_execution_plan_owned_node)))
    return explain_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                        "execution-plan node storage exceeds platform range");
  void *resized = realloc(
      plan->nodes, (size_t)next * sizeof(orm_execution_plan_owned_node));
  if (resized == NULL)
    return explain_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                        "grow execution-plan node storage");
  plan->nodes = (orm_execution_plan_owned_node *)resized;
  memset(plan->nodes + plan->capacity, 0,
         (size_t)(next - plan->capacity) *
             sizeof(orm_execution_plan_owned_node));
  plan->capacity = next;
  return ORM_STATUS_OK;
}

static void explain_destroy_node(orm_execution_plan_owned_node *node) {
  if (node == NULL) return;
  explain_free_view(&node->node_type);
  explain_free_view(&node->relation);
  explain_free_view(&node->index_name);
  explain_free_view(&node->detail);
  memset(node, 0, sizeof(*node));
}

static orm_status_t explain_add_node(
    orm_execution_plan_t *plan, const orm_execution_plan_owned_node *source,
    orm_error_t *error) {
  orm_status_t status = explain_reserve_node(plan, error);
  if (status != ORM_STATUS_OK) return status;

  const uint64_t bytes_before = plan->copied_bytes;
  orm_execution_plan_owned_node node = *source;
  node.node_type = (orm_string_view_t){NULL, 0u};
  node.relation = (orm_string_view_t){NULL, 0u};
  node.index_name = (orm_string_view_t){NULL, 0u};
  node.detail = (orm_string_view_t){NULL, 0u};

  status = explain_copy_view(plan, source->node_type, &node.node_type, error);
  if (status == ORM_STATUS_OK)
    status = explain_copy_view(plan, source->relation, &node.relation, error);
  if (status == ORM_STATUS_OK)
    status = explain_copy_view(
        plan, source->index_name, &node.index_name, error);
  if (status == ORM_STATUS_OK)
    status = explain_copy_view(plan, source->detail, &node.detail, error);
  if (status != ORM_STATUS_OK) {
    explain_destroy_node(&node);
    plan->copied_bytes = bytes_before;
    return status;
  }
  plan->nodes[plan->count++] = node;
  return ORM_STATUS_OK;
}

static orm_status_t explain_append_raw(orm_execution_plan_t *plan,
                                       orm_string_view_t line,
                                       orm_error_t *error) {
  const size_t old_size = plan->raw_detail.len;
  const size_t separator = old_size == 0u ? 0u : 1u;
  if (old_size > SIZE_MAX - separator - 1u ||
      line.len > SIZE_MAX - old_size - separator - 1u)
    return explain_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                        "execution-plan raw detail exceeds platform range");
  const size_t new_size = old_size + separator + line.len;
  const uint64_t added = (uint64_t)separator + (uint64_t)line.len;
  if (plan->copied_bytes > plan->max_bytes ||
      added > plan->max_bytes - plan->copied_bytes)
    return explain_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                        "execution-plan raw detail exceeds byte budget");

  char *copy = (char *)realloc((void *)plan->raw_detail.data, new_size + 1u);
  if (copy == NULL)
    return explain_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                        "grow execution-plan raw detail");
  if (separator != 0u) copy[old_size] = '\n';
  if (line.len != 0u) memcpy(copy + old_size + separator, line.data, line.len);
  copy[new_size] = '\0';
  plan->raw_detail.data = copy;
  plan->raw_detail.len = new_size;
  plan->copied_bytes += added;
  return ORM_STATUS_OK;
}

static orm_status_t explain_query(orm_connection_t *connection,
                                  const char *prefix,
                                  orm_string_view_t sql,
                                  orm_result_t **out_result,
                                  orm_error_t *error) {
  orm_query_t *query = NULL;
  *out_result = NULL;
  const size_t prefix_size = strlen(prefix);
  if (sql.len == 0u || sql.data == NULL)
    return explain_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                        "SQL to explain is empty");
  if (prefix_size > SIZE_MAX - sql.len ||
      prefix_size + sql.len == SIZE_MAX)
    return explain_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                        "explained SQL exceeds platform range");
  const size_t total = prefix_size + sql.len;
  if (total > connection->limits.max_query_bytes)
    return explain_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                        "explained SQL exceeds configured query limit");

  char *rendered = (char *)malloc(total + 1u);
  if (rendered == NULL)
    return explain_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                        "allocate explained SQL");
  memcpy(rendered, prefix, prefix_size);
  memcpy(rendered + prefix_size, sql.data, sql.len);
  rendered[total] = '\0';

  const orm_string_view_t view = {rendered, total};
  orm_status_t status = orm_raw(connection, view, &query, error);
  free(rendered);
  if (status == ORM_STATUS_OK)
    status = orm_query_execute(query, out_result, error);
  orm_query_destroy(query);
  return status;
}

static orm_status_t explain_result_counts(
    orm_result_t *result, uint64_t *rows, uint64_t *columns,
    orm_error_t *error) {
  orm_status_t status = orm_result_row_count(result, rows, error);
  if (status == ORM_STATUS_OK)
    status = orm_result_column_count(result, columns, error);
  return status;
}

static orm_status_t explain_text_cell(
    orm_result_t *result, uint64_t row, uint64_t column,
    orm_string_view_t *out, orm_error_t *error) {
  uint8_t is_null = 0u;
  orm_status_t status =
      orm_result_is_null(result, row, column, &is_null, error);
  if (status != ORM_STATUS_OK) return status;
  if (is_null) {
    *out = (orm_string_view_t){NULL, 0u};
    return ORM_STATUS_OK;
  }
  return orm_result_get_text(result, row, column, out, error);
}

static orm_status_t explain_integer_cell(
    orm_result_t *result, uint64_t row, uint64_t column,
    int64_t *out, orm_error_t *error) {
  orm_value_kind_t kind = ORM_VALUE_NULL;
  orm_status_t status =
      orm_result_value_kind(result, row, column, &kind, error);
  if (status != ORM_STATUS_OK) return status;
  if (kind == ORM_VALUE_INT64)
    return orm_result_get_int64(result, row, column, out, error);
  if (kind == ORM_VALUE_UINT64) {
    uint64_t value = 0u;
    status = orm_result_get_uint64(result, row, column, &value, error);
    if (status != ORM_STATUS_OK) return status;
    if (value > (uint64_t)INT64_MAX)
      return explain_fail(error, ORM_STATUS_OUT_OF_RANGE,
                          "execution-plan integer is out of range");
    *out = (int64_t)value;
    return ORM_STATUS_OK;
  }
  return explain_fail(error, ORM_STATUS_TYPE_ERROR,
                      "execution-plan integer field has wrong type");
}

static int explain_name_equal(orm_string_view_t name, const char *expected) {
  const size_t size = strlen(expected);
  if (name.len != size) return 0;
  for (size_t i = 0u; i < size; ++i) {
    if (tolower((unsigned char)((const char *)name.data)[i]) !=
        tolower((unsigned char)expected[i]))
      return 0;
  }
  return 1;
}

static int explain_find_column(orm_result_t *result,
                               uint64_t columns,
                               const char *name) {
  orm_error_t ignored;
  orm_error_init(&ignored);
  for (uint64_t i = 0u; i < columns; ++i) {
    orm_string_view_t column = {NULL, 0u};
    if (orm_result_column_name(result, i, &column, &ignored) == ORM_STATUS_OK &&
        explain_name_equal(column, name))
      return (int)i;
  }
  return -1;
}

static int explain_parse_double_after(const char *text,
                                      const char *needle,
                                      double *out) {
  const char *found = strstr(text, needle);
  if (found == NULL) return 0;
  found += strlen(needle);
  errno = 0;
  char *end = NULL;
  const double value = strtod(found, &end);
  if (end == found || errno == ERANGE || !isfinite(value)) return 0;
  *out = value;
  return 1;
}

static int explain_parse_range_after(const char *text,
                                     const char *needle,
                                     double *first,
                                     double *second) {
  const char *found = strstr(text, needle);
  if (found == NULL) return 0;
  found += strlen(needle);
  errno = 0;
  char *end = NULL;
  const double left = strtod(found, &end);
  if (end == found || errno == ERANGE || !isfinite(left) ||
      end[0] != '.' || end[1] != '.')
    return 0;
  const char *right_text = end + 2;
  errno = 0;
  char *right_end = NULL;
  const double right = strtod(right_text, &right_end);
  if (right_end == right_text || errno == ERANGE || !isfinite(right))
    return 0;
  *first = left;
  *second = right;
  return 1;
}

static orm_string_view_t explain_subview(const char *begin,
                                         const char *end) {
  if (begin == NULL || end == NULL || end <= begin)
    return (orm_string_view_t){NULL, 0u};
  return (orm_string_view_t){begin, (size_t)(end - begin)};
}

static void explain_parse_text_fields(const char *text,
                                      orm_execution_plan_owned_node *node) {
  const char *cursor = text;
  while (*cursor != '\0' && isspace((unsigned char)*cursor)) ++cursor;
  if (cursor[0] == '-' && cursor[1] == '>') {
    cursor += 2;
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) ++cursor;
  }

  const char *operator_end = strstr(cursor, " on ");
  const char *using_pos = strstr(cursor, " using ");
  if (using_pos != NULL &&
      (operator_end == NULL || using_pos < operator_end))
    operator_end = using_pos;
  const char *paren = strstr(cursor, "  (");
  if (operator_end == NULL || (paren != NULL && paren < operator_end))
    operator_end = paren;
  if (operator_end == NULL) operator_end = cursor + strlen(cursor);
  node->node_type = explain_subview(cursor, operator_end);

  using_pos = strstr(cursor, " using ");
  const char *on_pos = strstr(cursor, " on ");
  if (using_pos != NULL) {
    const char *begin = using_pos + strlen(" using ");
    const char *end = begin;
    if (on_pos != NULL && using_pos < on_pos) {
      end = on_pos;
    } else {
      while (*end != '\0' && !isspace((unsigned char)*end) && *end != '(')
        ++end;
    }
    node->index_name = explain_subview(begin, end);
  }
  if (on_pos != NULL) {
    const char *begin = on_pos + strlen(" on ");
    const char *end = begin;
    while (*end != '\0' && !isspace((unsigned char)*end) && *end != '(')
      ++end;
    node->relation = explain_subview(begin, end);
  }

  double first = 0.0, second = 0.0;
  if (explain_parse_range_after(text, "cost=", &first, &second)) {
    node->startup_cost = first;
    node->total_cost = second;
    node->flags |= ORM_PLAN_NODE_HAS_STARTUP_COST |
                   ORM_PLAN_NODE_HAS_TOTAL_COST;
  }
  if (explain_parse_double_after(text, "rows=", &first)) {
    node->estimated_rows = first;
    node->flags |= ORM_PLAN_NODE_HAS_ESTIMATED_ROWS;
  }
  const char *actual = strstr(text, "actual time=");
  if (actual != NULL) {
    if (explain_parse_range_after(actual, "actual time=", &first, &second)) {
      node->actual_startup_ms = first;
      node->actual_total_ms = second;
      node->flags |= ORM_PLAN_NODE_HAS_ACTUAL_STARTUP_MS |
                     ORM_PLAN_NODE_HAS_ACTUAL_TOTAL_MS;
    }
    if (explain_parse_double_after(actual, "rows=", &first)) {
      node->actual_rows = first;
      node->flags |= ORM_PLAN_NODE_HAS_ACTUAL_ROWS;
    }
  }
}

static int explain_text_indent(orm_string_view_t line) {
  int indent = 0;
  size_t offset = 0u;
  const char *text = (const char *)line.data;
  while (offset < line.len &&
         (text[offset] == ' ' || text[offset] == '\t')) {
    indent += text[offset] == '\t' ? 2 : 1;
    ++offset;
  }
  return indent;
}

static int explain_is_text_node(const char *text) {
  if (text == NULL || text[0] == '\0') return 0;
  if (strstr(text, "Planning Time:") != NULL ||
      strstr(text, "Execution Time:") != NULL ||
      strstr(text, "Planning:") != NULL ||
      strstr(text, "JIT:") != NULL)
    return 0;
  return strstr(text, "cost=") != NULL ||
         strstr(text, "actual time=") != NULL ||
         strstr(text, "->") != NULL;
}

static orm_status_t explain_process_text_line(
    orm_execution_plan_t *plan, orm_string_view_t line,
    int *indent_stack, uint64_t *index_stack, size_t *depth,
    size_t stack_capacity, orm_error_t *error) {
  orm_status_t status = explain_append_raw(plan, line, error);
  if (status != ORM_STATUS_OK) return status;

  if (line.len > SIZE_MAX - 1u)
    return explain_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                        "execution-plan line exceeds platform range");
  char *owned = (char *)malloc(line.len + 1u);
  if (owned == NULL)
    return explain_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                        "copy execution-plan parser line");
  if (line.len != 0u) memcpy(owned, line.data, line.len);
  owned[line.len] = '\0';

  if (!explain_is_text_node(owned)) {
    free(owned);
    return ORM_STATUS_OK;
  }

  const int indent = explain_text_indent(line);
  while (*depth != 0u && indent <= indent_stack[*depth - 1u]) --(*depth);

  orm_execution_plan_owned_node node;
  memset(&node, 0, sizeof(node));
  node.parent_index =
      *depth == 0u ? ORM_EXECUTION_PLAN_ROOT_INDEX
                   : index_stack[*depth - 1u];
  node.ordinal = plan->count;
  node.detail = (orm_string_view_t){owned, line.len};
  explain_parse_text_fields(owned, &node);
  status = explain_add_node(plan, &node, error);
  free(owned);
  if (status != ORM_STATUS_OK) return status;

  if (*depth >= stack_capacity)
    return explain_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                        "execution-plan nesting exceeds parser limit");
  indent_stack[*depth] = indent;
  index_stack[*depth] = plan->count - 1u;
  ++(*depth);
  return ORM_STATUS_OK;
}

static orm_status_t explain_load_text_rows(
    orm_result_t *result, orm_execution_plan_t *plan,
    orm_error_t *error) {
  uint64_t rows = 0u, columns = 0u;
  orm_status_t status =
      explain_result_counts(result, &rows, &columns, error);
  if (status != ORM_STATUS_OK) return status;
  if (columns == 0u)
    return explain_fail(error, ORM_STATUS_TYPE_ERROR,
                        "text execution plan returned no columns");

  int indent_stack[128];
  uint64_t index_stack[128];
  size_t depth = 0u;
  for (uint64_t row = 0u; row < rows; ++row) {
    orm_string_view_t value = {NULL, 0u};
    status = explain_text_cell(result, row, 0u, &value, error);
    if (status != ORM_STATUS_OK) return status;
    if (value.data == NULL && value.len != 0u)
      return explain_fail(error, ORM_STATUS_TYPE_ERROR,
                          "text execution plan returned an invalid string");

    const char *data = value.data != NULL ? (const char *)value.data : "";
    size_t start = 0u;
    while (start <= value.len) {
      size_t end = start;
      while (end < value.len && data[end] != '\n' && data[end] != '\r')
        ++end;
      const orm_string_view_t line = {data + start, end - start};
      status = explain_process_text_line(
          plan, line, indent_stack, index_stack, &depth,
          sizeof(indent_stack) / sizeof(indent_stack[0]), error);
      if (status != ORM_STATUS_OK) return status;
      if (end == value.len) break;
      while (end < value.len && (data[end] == '\n' || data[end] == '\r'))
        ++end;
      start = end;
    }
  }
  return ORM_STATUS_OK;
}

static orm_status_t explain_load_sqlite(
    orm_connection_t *connection, orm_string_view_t sql,
    orm_execution_plan_t *plan, orm_error_t *error) {
  if (plan->mode != ORM_EXPLAIN_PLAN)
    return explain_fail(
        error, ORM_STATUS_UNSUPPORTED,
        "SQLite does not expose TurboDB ANALYZE execution-plan semantics");

  orm_result_t *result = NULL;
  orm_status_t status = explain_query(
      connection, "EXPLAIN QUERY PLAN ", sql, &result, error);
  if (status != ORM_STATUS_OK) return status;

  uint64_t rows = 0u, columns = 0u;
  status = explain_result_counts(result, &rows, &columns, error);
  if (status != ORM_STATUS_OK) goto done;
  if (columns < 4u) {
    status = explain_fail(error, ORM_STATUS_TYPE_ERROR,
                          "SQLite execution plan has unexpected shape");
    goto done;
  }

  int64_t *ids = rows != 0u ? (int64_t *)calloc((size_t)rows, sizeof(int64_t))
                            : NULL;
  int64_t *parents =
      rows != 0u ? (int64_t *)calloc((size_t)rows, sizeof(int64_t)) : NULL;
  if (rows != 0u && (ids == NULL || parents == NULL)) {
    free(ids);
    free(parents);
    status = explain_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                          "allocate SQLite plan identity map");
    goto done;
  }

  for (uint64_t row = 0u; status == ORM_STATUS_OK && row < rows; ++row) {
    orm_string_view_t detail = {NULL, 0u};
    status = explain_integer_cell(result, row, 0u, &ids[row], error);
    if (status == ORM_STATUS_OK)
      status = explain_integer_cell(result, row, 1u, &parents[row], error);
    if (status == ORM_STATUS_OK)
      status = explain_text_cell(result, row, 3u, &detail, error);
    if (status != ORM_STATUS_OK) break;

    status = explain_append_raw(plan, detail, error);
    if (status != ORM_STATUS_OK) break;

    orm_execution_plan_owned_node node;
    memset(&node, 0, sizeof(node));
    node.parent_index = ORM_EXECUTION_PLAN_ROOT_INDEX;
    for (uint64_t previous = 0u; previous < row; ++previous) {
      if (ids[previous] == parents[row]) {
        node.parent_index = previous;
        break;
      }
    }
    node.ordinal = row;
    node.detail = detail;

    const char *text = (const char *)detail.data;
    const char *end = text + detail.len;
    const char *space = memchr(text, ' ', detail.len);
    node.node_type = explain_subview(text, space != NULL ? space : end);

    const char *relation = NULL;
    if (detail.len >= 5u &&
        (strncmp(text, "SCAN ", 5u) == 0 ||
         strncmp(text, "SEARCH ", 7u) == 0)) {
      const char *on = strstr(text, " ");
      if (on != NULL) {
        ++on;
        if (strncmp(on, "TABLE ", 6u) == 0) on += 6u;
        relation = on;
      }
    }
    if (relation != NULL) {
      const char *relation_end = relation;
      while (relation_end < end &&
             !isspace((unsigned char)*relation_end))
        ++relation_end;
      node.relation = explain_subview(relation, relation_end);
    }
    const char *using_index = strstr(text, "USING INDEX ");
    if (using_index == NULL)
      using_index = strstr(text, "USING COVERING INDEX ");
    if (using_index != NULL) {
      using_index = strstr(using_index, "INDEX ");
      if (using_index != NULL) {
        using_index += strlen("INDEX ");
        const char *index_end = using_index;
        while (index_end < end &&
               !isspace((unsigned char)*index_end) &&
               *index_end != '(')
          ++index_end;
        node.index_name = explain_subview(using_index, index_end);
      }
    }
    status = explain_add_node(plan, &node, error);
  }
  free(ids);
  free(parents);

done:
  orm_result_destroy(result);
  return status;
}

static orm_status_t explain_load_mysql_plan(
    orm_result_t *result, orm_execution_plan_t *plan,
    orm_error_t *error) {
  uint64_t rows = 0u, columns = 0u;
  orm_status_t status =
      explain_result_counts(result, &rows, &columns, error);
  if (status != ORM_STATUS_OK) return status;

  const int select_type = explain_find_column(result, columns, "select_type");
  const int access_type = explain_find_column(result, columns, "type");
  const int table = explain_find_column(result, columns, "table");
  const int key = explain_find_column(result, columns, "key");
  const int estimated = explain_find_column(result, columns, "rows");
  const int extra = explain_find_column(result, columns, "Extra");
  if (table < 0)
    return explain_fail(error, ORM_STATUS_TYPE_ERROR,
                        "MySQL EXPLAIN result has no table column");

  for (uint64_t row = 0u; row < rows; ++row) {
    orm_execution_plan_owned_node node;
    memset(&node, 0, sizeof(node));
    node.parent_index = ORM_EXECUTION_PLAN_ROOT_INDEX;
    node.ordinal = row;

    orm_string_view_t select_value = {NULL, 0u};
    orm_string_view_t access_value = {NULL, 0u};
    orm_string_view_t table_value = {NULL, 0u};
    orm_string_view_t key_value = {NULL, 0u};
    orm_string_view_t extra_value = {NULL, 0u};

    if (select_type >= 0)
      status = explain_text_cell(
          result, row, (uint64_t)select_type, &select_value, error);
    if (status == ORM_STATUS_OK && access_type >= 0)
      status = explain_text_cell(
          result, row, (uint64_t)access_type, &access_value, error);
    if (status == ORM_STATUS_OK)
      status = explain_text_cell(
          result, row, (uint64_t)table, &table_value, error);
    if (status == ORM_STATUS_OK && key >= 0)
      status = explain_text_cell(
          result, row, (uint64_t)key, &key_value, error);
    if (status == ORM_STATUS_OK && extra >= 0)
      status = explain_text_cell(
          result, row, (uint64_t)extra, &extra_value, error);
    if (status != ORM_STATUS_OK) return status;

    node.node_type =
        access_value.len != 0u ? access_value : select_value;
    node.relation = table_value;
    node.index_name = key_value;
    node.detail = extra_value;

    if (estimated >= 0) {
      orm_value_kind_t kind = ORM_VALUE_NULL;
      status = orm_result_value_kind(
          result, row, (uint64_t)estimated, &kind, error);
      if (status != ORM_STATUS_OK) return status;
      if (kind == ORM_VALUE_INT64) {
        int64_t value = 0;
        status = orm_result_get_int64(
            result, row, (uint64_t)estimated, &value, error);
        if (status != ORM_STATUS_OK) return status;
        if (value >= 0) {
          node.estimated_rows = (double)value;
          node.flags |= ORM_PLAN_NODE_HAS_ESTIMATED_ROWS;
        }
      } else if (kind == ORM_VALUE_UINT64) {
        uint64_t value = 0u;
        status = orm_result_get_uint64(
            result, row, (uint64_t)estimated, &value, error);
        if (status != ORM_STATUS_OK) return status;
        node.estimated_rows = (double)value;
        node.flags |= ORM_PLAN_NODE_HAS_ESTIMATED_ROWS;
      } else if (kind == ORM_VALUE_DOUBLE) {
        status = orm_result_get_double(
            result, row, (uint64_t)estimated, &node.estimated_rows, error);
        if (status != ORM_STATUS_OK) return status;
        node.flags |= ORM_PLAN_NODE_HAS_ESTIMATED_ROWS;
      }
    }

    const orm_string_view_t fields[] = {
        select_value, access_value, table_value, key_value, extra_value};
    size_t raw_size = 4u;
    for (size_t field = 0u; field < sizeof(fields) / sizeof(fields[0]); ++field) {
      if (fields[field].len > SIZE_MAX - raw_size)
        return explain_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                            "MySQL EXPLAIN row exceeds platform range");
      raw_size += fields[field].len;
    }
    char *raw = (char *)malloc(raw_size + 1u);
    if (raw == NULL)
      return explain_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                          "allocate MySQL EXPLAIN raw row");
    size_t offset = 0u;
    for (size_t field = 0u; field < sizeof(fields) / sizeof(fields[0]); ++field) {
      if (field != 0u) raw[offset++] = '\t';
      if (fields[field].len != 0u) {
        memcpy(raw + offset, fields[field].data, fields[field].len);
        offset += fields[field].len;
      }
    }
    raw[offset] = '\0';
    const orm_string_view_t raw_view = {raw, offset};
    status = explain_append_raw(plan, raw_view, error);
    free(raw);
    if (status != ORM_STATUS_OK) return status;
    status = explain_add_node(plan, &node, error);
    if (status != ORM_STATUS_OK) return status;
  }
  return ORM_STATUS_OK;
}

static orm_status_t explain_mysql_version(
    orm_connection_t *connection, int *major, int *minor, int *patch,
    orm_error_t *error) {
  *major = *minor = *patch = 0;
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  orm_status_t status =
      orm_raw(connection, orm_view("SELECT VERSION()"), &query, error);
  if (status == ORM_STATUS_OK)
    status = orm_query_execute(query, &result, error);
  orm_query_destroy(query);
  if (status != ORM_STATUS_OK) {
    orm_result_destroy(result);
    return status;
  }

  uint64_t rows = 0u, columns = 0u;
  status = explain_result_counts(result, &rows, &columns, error);
  if (status != ORM_STATUS_OK || rows == 0u || columns == 0u) {
    orm_result_destroy(result);
    return status != ORM_STATUS_OK
               ? status
               : explain_fail(error, ORM_STATUS_UNSUPPORTED,
                              "MySQL server version is unavailable");
  }
  orm_string_view_t version = {NULL, 0u};
  status = explain_text_cell(result, 0u, 0u, &version, error);
  if (status != ORM_STATUS_OK) {
    orm_result_destroy(result);
    return status;
  }
  if (version.data == NULL || version.len == 0u ||
      version.len == SIZE_MAX) {
    orm_result_destroy(result);
    return explain_fail(error, ORM_STATUS_UNSUPPORTED,
                        "MySQL server version is invalid");
  }

  char *owned = (char *)malloc(version.len + 1u);
  if (owned == NULL) {
    orm_result_destroy(result);
    return explain_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                        "copy MySQL server version");
  }
  memcpy(owned, version.data, version.len);
  owned[version.len] = '\0';
  orm_result_destroy(result);

  if (strstr(owned, "MariaDB") != NULL) {
    free(owned);
    return explain_fail(
        error, ORM_STATUS_UNSUPPORTED,
        "MySQL ANALYZE mode requires MySQL 8.0.18 or newer");
  }
  char trailing = '\0';
  const int parsed = sscanf(
      owned, "%d.%d.%d%c", major, minor, patch, &trailing);
  free(owned);
  if (parsed < 3 || *major < 0 || *minor < 0 || *patch < 0)
    return explain_fail(error, ORM_STATUS_UNSUPPORTED,
                        "MySQL server version cannot prove ANALYZE support");
  return ORM_STATUS_OK;
}

static orm_status_t explain_mysql_require_analyze(
    orm_connection_t *connection, orm_error_t *error) {
  int major = 0, minor = 0, patch = 0;
  orm_status_t status =
      explain_mysql_version(connection, &major, &minor, &patch, error);
  if (status != ORM_STATUS_OK) return status;
  if (major > 8 || (major == 8 && (minor > 0 || patch >= 18)))
    return ORM_STATUS_OK;
  return explain_fail(
      error, ORM_STATUS_UNSUPPORTED,
      "MySQL ANALYZE mode requires MySQL 8.0.18 or newer");
}

static orm_status_t explain_load_mysql(
    orm_connection_t *connection, orm_string_view_t sql,
    orm_execution_plan_t *plan, orm_error_t *error) {
  if (plan->mode == ORM_EXPLAIN_ANALYZE) {
    orm_status_t capability =
        explain_mysql_require_analyze(connection, error);
    if (capability != ORM_STATUS_OK) return capability;
  }
  const char *prefix =
      plan->mode == ORM_EXPLAIN_ANALYZE ? "EXPLAIN ANALYZE " : "EXPLAIN ";
  orm_result_t *result = NULL;
  orm_status_t status = explain_query(
      connection, prefix, sql, &result, error);
  if (status != ORM_STATUS_OK) return status;

  if (plan->mode == ORM_EXPLAIN_ANALYZE)
    status = explain_load_text_rows(result, plan, error);
  else
    status = explain_load_mysql_plan(result, plan, error);
  orm_result_destroy(result);
  return status;
}

static orm_status_t explain_load_postgresql(
    orm_connection_t *connection, orm_string_view_t sql,
    orm_execution_plan_t *plan, orm_error_t *error) {
  const char *prefix =
      plan->mode == ORM_EXPLAIN_ANALYZE
          ? "EXPLAIN (ANALYZE TRUE, FORMAT TEXT) "
          : "EXPLAIN (FORMAT TEXT) ";
  orm_result_t *result = NULL;
  orm_status_t status =
      explain_query(connection, prefix, sql, &result, error);
  if (status == ORM_STATUS_OK)
    status = explain_load_text_rows(result, plan, error);
  orm_result_destroy(result);
  return status;
}

orm_status_t ORM_C_CALL orm_query_explain(
    orm_connection_t *connection, orm_string_view_t sql,
    orm_explain_mode_t mode, orm_execution_plan_t **out_plan,
    orm_error_t *error) {
  if (out_plan != NULL) *out_plan = NULL;
  if (connection == NULL || out_plan == NULL ||
      (mode != ORM_EXPLAIN_PLAN && mode != ORM_EXPLAIN_ANALYZE) ||
      sql.data == NULL || sql.len == 0u)
    return explain_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                        "invalid execution-plan request");

  orm_execution_plan_t *plan =
      (orm_execution_plan_t *)calloc(1u, sizeof(*plan));
  if (plan == NULL)
    return explain_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                        "allocate execution-plan snapshot");
  plan->mode = mode;
  plan->max_nodes = connection->limits.max_result_rows;
  plan->max_bytes = connection->limits.max_result_bytes;
  if (plan->max_nodes == 0u || plan->max_bytes == 0u) {
    orm_execution_plan_destroy(plan);
    return explain_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                        "execution-plan limits are zero");
  }

  const orm_string_view_t provider = {
      connection->driver_id, connection->driver_id_size};
  orm_status_t status =
      explain_copy_view(plan, provider, &plan->provider, error);
  if (status == ORM_STATUS_OK) {
    if (strcmp(connection->driver_id, "sqlite") == 0)
      status = explain_load_sqlite(connection, sql, plan, error);
    else if (strcmp(connection->driver_id, "mysql") == 0)
      status = explain_load_mysql(connection, sql, plan, error);
    else if (strcmp(connection->driver_id, "postgresql") == 0)
      status = explain_load_postgresql(connection, sql, plan, error);
    else
      status = explain_fail(error, ORM_STATUS_UNSUPPORTED,
                            "driver does not expose execution plans");
  }

  if (status != ORM_STATUS_OK) {
    orm_execution_plan_destroy(plan);
    return status;
  }
  *out_plan = plan;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

orm_status_t ORM_C_CALL orm_execution_plan_mode(
    const orm_execution_plan_t *plan, orm_explain_mode_t *out_mode,
    orm_error_t *error) {
  if (out_mode != NULL) *out_mode = ORM_EXPLAIN_PLAN;
  if (plan == NULL || out_mode == NULL)
    return explain_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                        "invalid execution-plan mode request");
  *out_mode = plan->mode;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

orm_status_t ORM_C_CALL orm_execution_plan_provider(
    const orm_execution_plan_t *plan, orm_string_view_t *out_provider,
    orm_error_t *error) {
  if (out_provider != NULL) *out_provider = (orm_string_view_t){NULL, 0u};
  if (plan == NULL || out_provider == NULL)
    return explain_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                        "invalid execution-plan provider request");
  *out_provider = plan->provider;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

orm_status_t ORM_C_CALL orm_execution_plan_node_count(
    const orm_execution_plan_t *plan, uint64_t *out_count,
    orm_error_t *error) {
  if (out_count != NULL) *out_count = 0u;
  if (plan == NULL || out_count == NULL)
    return explain_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                        "invalid execution-plan node-count request");
  *out_count = plan->count;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

orm_status_t ORM_C_CALL orm_execution_plan_node(
    const orm_execution_plan_t *plan, uint64_t index,
    orm_execution_plan_node_t *out_node, orm_error_t *error) {
  if (plan == NULL || out_node == NULL ||
      out_node->struct_size != sizeof(*out_node))
    return explain_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                        "invalid execution-plan node request");
  if (index >= plan->count)
    return explain_fail(error, ORM_STATUS_OUT_OF_RANGE,
                        "execution-plan node index is out of range");
  const orm_execution_plan_owned_node *source = &plan->nodes[index];
  out_node->flags = source->flags;
  out_node->parent_index = source->parent_index;
  out_node->ordinal = source->ordinal;
  out_node->node_type = source->node_type;
  out_node->relation = source->relation;
  out_node->index_name = source->index_name;
  out_node->detail = source->detail;
  out_node->estimated_rows = source->estimated_rows;
  out_node->actual_rows = source->actual_rows;
  out_node->startup_cost = source->startup_cost;
  out_node->total_cost = source->total_cost;
  out_node->actual_startup_ms = source->actual_startup_ms;
  out_node->actual_total_ms = source->actual_total_ms;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

orm_status_t ORM_C_CALL orm_execution_plan_raw_detail(
    const orm_execution_plan_t *plan, orm_string_view_t *out_detail,
    orm_error_t *error) {
  if (out_detail != NULL) *out_detail = (orm_string_view_t){NULL, 0u};
  if (plan == NULL || out_detail == NULL)
    return explain_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                        "invalid execution-plan raw-detail request");
  *out_detail = plan->raw_detail;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

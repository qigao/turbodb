#include "index_lookup.h"
#include "index_directory.h"
#include "relation.h"
#include "select.h"
#include "name.h"
#include "work.h"
#include <float.h>
#include <math.h>
#include "error.h"
#include <string.h>

typedef struct lookup_probe {
  turbodb_value_t value;
  size_t parameter;
  bool present, is_parameter, null_equal, exclusive;
} lookup_probe;
typedef struct lookup_column { lookup_probe equal, lower, upper; } lookup_column;
typedef struct lookup_term {
  sqlparser_id root, list;
  size_t column_offset, list_column, first, count, equal_parts, bound_bytes;
  orm_sql_index_access access;
  bool list_used;
} lookup_term;
typedef struct lookup_range {
  const uint8_t *lower, *upper;
  size_t lower_size, upper_size;
} lookup_range;
enum { LOOKUP_KEY_PASSES = 3, LOOKUP_BOUNDARIES = 2 };
static int lookup_compare(const uint8_t *a, size_t an, const uint8_t *b, size_t bn) {
  const int order = memcmp(a, b, an < bn ? an : bn);
  return order ? order : an < bn ? -1 : an > bn;
}
static int lookup_range_compare(const void *a, const void *b) {
  const lookup_range *x = a, *y = b;
  return lookup_compare(x->lower, x->lower_size, y->lower, y->lower_size);
}
static bool lookup_range_copy(void *to, const void *from) { *(lookup_range *)to = *(const lookup_range *)from; return true; }
static void lookup_range_move(void *to, void *from) { *(lookup_range *)to = *(lookup_range *)from; }
/* Views borrow the lookup's fixed boundary workspace, including during sort. */
static void lookup_range_destroy(void *value) { (void)value; }
static const cmeta_type_traits lookup_range_traits = {
  CMETA_TRAIT_COMPARE | CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY |
      CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY,
  NULL, NULL, lookup_range_compare, lookup_range_copy, lookup_range_move, lookup_range_destroy};
static const cmeta_type_desc lookup_range_type = {"sql_lookup_range", sizeof(lookup_range),
  _Alignof(lookup_range), CMETA_T_OBJECT, NULL, &lookup_range_traits};
static turbodb_status_t lookup_error(turbodb_error_t *error, turbodb_status_t status, const char *message) {
  tdsql_error_set(error, status, message); return status;
}
static turbodb_status_t lookup_steps(orm_tidesdb_sql_budget *budget, size_t steps, turbodb_error_t *error) {
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = steps;
  return orm_tidesdb_sql_budget_reserve(budget, &amount, error);
}
static bool lookup_numeric(turbodb_value_kind_t kind) {
  return kind == TURBODB_VALUE_INT64 || kind == TURBODB_VALUE_UINT64 || kind == TURBODB_VALUE_DOUBLE;
}
static bool lookup_probe_matches(turbodb_value_kind_t column, const orm_sql_binding_scope *scope, const lookup_probe *probe) {
  const turbodb_value_kind_t kind = probe->is_parameter ? scope->parameter_types[probe->parameter].kind : probe->value.kind;
  /* Integer-vs-real SQL comparison rounds integer values into the real domain;
   * mapping the real probe back to one integer key would omit equal rows. */
  return column == TURBODB_VALUE_DOUBLE || kind != TURBODB_VALUE_DOUBLE;
}
static bool lookup_comparison(sqlparser_operator op) {
  return op == SQLPARSER_OP_EQ || op == SQLPARSER_OP_NULL_SAFE_EQ || op == SQLPARSER_OP_LT ||
      op == SQLPARSER_OP_LE || op == SQLPARSER_OP_GT || op == SQLPARSER_OP_GE;
}
static sqlparser_operator lookup_reverse(sqlparser_operator op) {
  switch (op) {
    case SQLPARSER_OP_LT: return SQLPARSER_OP_GT;
    case SQLPARSER_OP_LE: return SQLPARSER_OP_GE;
    case SQLPARSER_OP_GT: return SQLPARSER_OP_LT;
    case SQLPARSER_OP_GE: return SQLPARSER_OP_LE;
    default: return op;
  }
}
/* Eligibility is distinct from failure: unsupported shapes select the ordinary
 * scan before any index is chosen. Never evaluate arbitrary SQL during planning. */
static turbodb_status_t lookup_scalar(const orm_sql_binding_scope *scope, sqlparser_id id,
    lookup_probe *out, turbodb_error_t *error) {
  const sqlparser_node *node = sqlparser_get_node(scope->document, id);
  if (!node) return TURBODB_STATUS_OK;
  if (node->kind == SQLPARSER_NULL) { out->present = true; out->value = turbodb_null(); return TURBODB_STATUS_OK; }
  if (node->kind == SQLPARSER_PARAMETER) {
    size_t first = 0, last = scope->parameter_count;
    while (first < last) {
      const turbodb_status_t status = lookup_steps(scope->budget, 1, error); if (status != TURBODB_STATUS_OK) return status;
      const size_t mid = first + (last - first) / 2;
      if (scope->parameter_offsets[mid] < node->span.offset) first = mid + 1; else last = mid;
    }
    if (first == scope->parameter_count || scope->parameter_offsets[first] != node->span.offset)
      return lookup_error(error, TURBODB_STATUS_INTERNAL_ERROR, "index probe parameter is not bound");
    if (lookup_numeric(scope->parameter_types[first].kind) || scope->parameter_types[first].kind == TURBODB_VALUE_NULL) {
      out->present = out->is_parameter = true; out->parameter = first;
    }
    return TURBODB_STATUS_OK;
  }
  bool negative = false;
  if (node->kind == SQLPARSER_UNARY && (node->as.unary.op == SQLPARSER_OP_NEGATE || node->as.unary.op == SQLPARSER_OP_POSITIVE)) {
    negative = node->as.unary.op == SQLPARSER_OP_NEGATE;
    node = sqlparser_get_node(scope->document, node->as.unary.operand);
  }
  if (!node || node->kind != SQLPARSER_NUMBER) return TURBODB_STATUS_OK;
  const vstr text = {sqlparser_text(scope->document, node->span), node->span.length};
  turbodb_status_t status = lookup_steps(scope->budget, text.len, error); if (status != TURBODB_STATUS_OK) return status;
  status = orm_tidesdb_sql_number_literal(text, negative, &out->value, error);
  if (status == TURBODB_STATUS_OK) out->present = true;
  return status;
}
static turbodb_status_t lookup_atom(const orm_sql_binding_scope *scope, const sqlparser_node *node,
    orm_sql_index_lookup *lookup, lookup_term *term, bool *eligible, turbodb_error_t *error) {
  sqlparser_id column = SQLPARSER_NONE, scalar = SQLPARSER_NONE; lookup_probe probe = {0};
  sqlparser_operator op = SQLPARSER_OP_EQ;
  if (node->kind == SQLPARSER_IN && !node->as.in.negated && !node->as.in.query &&
      !node->as.in.table && node->as.in.items.count && !term->list) {
    column = node->as.in.value;
  } else if (node->kind == SQLPARSER_UNARY && node->as.unary.op == SQLPARSER_OP_IS_NULL) {
    column = node->as.unary.operand; probe.present = probe.null_equal = true; probe.value = turbodb_null();
  } else if (node->kind == SQLPARSER_BETWEEN && !node->as.between.negated) {
    column = node->as.between.value; scalar = node->as.between.lower; op = SQLPARSER_OP_GE;
  } else if (node->kind == SQLPARSER_BINARY && lookup_comparison(node->as.binary.op)) {
    op = node->as.binary.op;
    column = node->as.binary.left; scalar = node->as.binary.right;
    const sqlparser_node *left = sqlparser_get_node(scope->document, column);
    if (!left || left->kind != SQLPARSER_NAME) { column = node->as.binary.right; scalar = node->as.binary.left; op = lookup_reverse(op); }
    probe.null_equal = node->as.binary.op == SQLPARSER_OP_NULL_SAFE_EQ;
  } else { *eligible = false; return TURBODB_STATUS_OK; }
  const sqlparser_node *name = sqlparser_get_node(scope->document, column);
  if (!name || name->kind != SQLPARSER_NAME) { *eligible = false; return TURBODB_STATUS_OK; }
  size_t slot = 0; turbodb_status_t status = orm_sql_bind_column(scope, column, &slot, error);
  if (status != TURBODB_STATUS_OK) return status;
  const turbodb_value_kind_t column_kind = scope->schema->columns[slot].type.kind;
  if (!lookup_numeric(column_kind)) { *eligible = false; return TURBODB_STATUS_OK; }
  if (node->kind == SQLPARSER_IN) {
    term->list = node->as.in.items.first; term->count = node->as.in.items.count; term->list_column = slot;
    return TURBODB_STATUS_OK;
  }
  if (scalar) status = lookup_scalar(scope, scalar, &probe, error);
  if (status == TURBODB_STATUS_OK && (!probe.present || !lookup_probe_matches(column_kind, scope, &probe))) *eligible = false;
  lookup_column *bound = vec_at(&lookup->probes, term->column_offset + slot);
  lookup_probe *target = op == SQLPARSER_OP_GT || op == SQLPARSER_OP_GE ? &bound->lower :
      op == SQLPARSER_OP_LT || op == SQLPARSER_OP_LE ? &bound->upper : &bound->equal;
  probe.exclusive = op == SQLPARSER_OP_GT || op == SQLPARSER_OP_LT;
  if (status == TURBODB_STATUS_OK && probe.present && !target->present) *target = probe;
  if (status == TURBODB_STATUS_OK && node->kind == SQLPARSER_BETWEEN) {
    probe = (lookup_probe){0}; status = lookup_scalar(scope, node->as.between.upper, &probe, error);
    if (status == TURBODB_STATUS_OK && (!probe.present || !lookup_probe_matches(column_kind, scope, &probe))) *eligible = false;
    if (status == TURBODB_STATUS_OK && probe.present && !bound->upper.present) bound->upper = probe;
  }
  return status;
}
/* Traverse only top-level OR: no distributive expansion or IN cross product.
 * The same AST-sized stack is reused for counting, collecting and AND binding. */
static turbodb_status_t lookup_branches(const orm_sql_binding_scope *scope, sqlparser_id where,
    vec_t *stack, vec_t *terms, size_t *count, turbodb_error_t *error) {
  size_t pending = 0; *count = 0;
  *(sqlparser_id *)vec_at(stack, pending++) = where;
  while (pending) {
    const sqlparser_id id = *(const sqlparser_id *)vec_at_const(stack, --pending);
    const sqlparser_node *node = sqlparser_get_node(scope->document, id);
    const turbodb_status_t status = lookup_steps(scope->budget, 1, error); if (status != TURBODB_STATUS_OK) return status;
    if (node->kind == SQLPARSER_BINARY && node->as.binary.op == SQLPARSER_OP_OR) {
      *(sqlparser_id *)vec_at(stack, pending++) = node->as.binary.right;
      *(sqlparser_id *)vec_at(stack, pending++) = node->as.binary.left;
    } else {
      if (terms) ((lookup_term *)vec_at(terms, *count))->root = id;
      ++*count;
    }
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t lookup_predicates(const orm_sql_binding_scope *scope, sqlparser_id where,
    orm_sql_index_lookup *lookup, bool *eligible, turbodb_error_t *error) {
  vec_t stack = {0}; size_t bytes = 0, count = 0, choices = 0;
  turbodb_status_t status = orm_sql_work_zero(&stack, sqlparser_node_count(scope->document), sizeof(sqlparser_id),
      _Alignof(sqlparser_id), scope->budget, &bytes, error);
  if (status == TURBODB_STATUS_OK) status = lookup_branches(scope, where, &stack, NULL, &count, error);
  if (status == TURBODB_STATUS_OK && count > SIZE_MAX / scope->schema->count)
    status = lookup_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "index branch column capacity overflow");
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&lookup->terms, count, sizeof(lookup_term),
      _Alignof(lookup_term), scope->budget, &lookup->term_bytes, error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&lookup->probes, count * scope->schema->count, sizeof(lookup_column),
      _Alignof(lookup_column), scope->budget, &lookup->probe_bytes, error);
  if (status == TURBODB_STATUS_OK) status = lookup_branches(scope, where, &stack, &lookup->terms, &count, error);
  *eligible = true;
  for (size_t i = 0; status == TURBODB_STATUS_OK && *eligible && i < count; ++i) {
    lookup_term *term = vec_at(&lookup->terms, i); term->column_offset = i * scope->schema->count;
    size_t pending = 0; *(sqlparser_id *)vec_at(&stack, pending++) = term->root; term->root = SQLPARSER_NONE;
    while (status == TURBODB_STATUS_OK && *eligible && pending) {
      const sqlparser_node *node = sqlparser_get_node(scope->document, *(const sqlparser_id *)vec_at_const(&stack, --pending));
      status = lookup_steps(scope->budget, 1, error); if (status != TURBODB_STATUS_OK) break;
      if (node->kind == SQLPARSER_BINARY && node->as.binary.op == SQLPARSER_OP_AND) {
        *(sqlparser_id *)vec_at(&stack, pending++) = node->as.binary.right;
        *(sqlparser_id *)vec_at(&stack, pending++) = node->as.binary.left;
      } else status = lookup_atom(scope, node, lookup, term, eligible, error);
    }
    if (term->count > SIZE_MAX - choices) status = lookup_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "index IN capacity overflow");
    else { term->first = choices; choices += term->count; }
  }
  if (status == TURBODB_STATUS_OK && *eligible) status = orm_sql_work_zero(&lookup->choices, choices, sizeof(lookup_probe),
      _Alignof(lookup_probe), scope->budget, &lookup->choice_bytes, error);
  for (size_t i = 0; status == TURBODB_STATUS_OK && *eligible && i < count; ++i) {
    lookup_term *term = vec_at(&lookup->terms, i); sqlparser_id item = term->list; term->list = SQLPARSER_NONE;
    for (size_t j = 0; status == TURBODB_STATUS_OK && *eligible && j < term->count; ++j) {
      status = lookup_steps(scope->budget, 1, error);
      lookup_probe *probe = vec_at(&lookup->choices, term->first + j);
      if (status == TURBODB_STATUS_OK) status = lookup_scalar(scope, item, probe, error);
      if (status == TURBODB_STATUS_OK && (!probe->present ||
          !lookup_probe_matches(scope->schema->columns[term->list_column].type.kind, scope, probe))) *eligible = false;
      item = sqlparser_get_node(scope->document, item)->next;
    }
  }
  const turbodb_status_t closed = orm_sql_work_release(&stack, bytes, scope->budget, status == TURBODB_STATUS_OK ? error : NULL);
  return status == TURBODB_STATUS_OK ? closed : status;
}
static turbodb_status_t lookup_choose(orm_sql_relation_source *source, orm_sql_index_lookup *lookup, turbodb_error_t *error) {
  orm_sql_index_set indexes = {0};
  const uint64_t table_id = orm_sql_wire_read(source->prefix + 1, ORM_SQL_WIRE_U64);
  turbodb_status_t status = orm_sql_index_set_load(source->owner, &source->schema, table_id,
      (orm_sql_catalog_snapshot){source->next_index_id, source->format}, &indexes, error);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < vec_size(&indexes.records); ++i) {
    orm_sql_index_record *record = vec_at(&indexes.records, i); bool usable = true;
    const size_t parts = vec_size(&record->definition.parts);
    for (size_t t = 0; status == TURBODB_STATUS_OK && usable && t < vec_size(&lookup->terms); ++t) {
      lookup_term *term = vec_at(&lookup->terms, t); term->equal_parts = 0; term->list_used = false; bool range = false;
      for (size_t p = 0; status == TURBODB_STATUS_OK && p < parts; ++p) {
        status = lookup_steps(lookup->budget, 1, error);
        const orm_sql_index_part *part = vec_at_const(&record->definition.parts, p);
        const lookup_column *column = vec_at_const(&lookup->probes, term->column_offset + part->column);
        if (term->count && term->list_column == part->column) { ++term->equal_parts; term->list_used = true; }
        else if (column->equal.present) ++term->equal_parts;
        else { range = column->lower.present || column->upper.present; break; }
      }
      usable = term->equal_parts || range;
      term->access = range ? ORM_SQL_INDEX_RANGE : term->equal_parts == parts ? ORM_SQL_INDEX_EQUAL : ORM_SQL_INDEX_PREFIX;
    }
    if (status == TURBODB_STATUS_OK && usable) {
      const lookup_term *first = vec_at_const(&lookup->terms, 0);
      lookup->equal_parts = first->equal_parts;
      lookup->access = vec_size(&lookup->terms) > 1 || first->list_used ? ORM_SQL_INDEX_RANGE : first->access;
      lookup->record = *record; *record = (orm_sql_index_record){0}; break;
    }
  }
  const turbodb_status_t closed = orm_sql_index_set_close(&indexes, status == TURBODB_STATUS_OK ? error : NULL);
  if (closed != TURBODB_STATUS_OK) source->owner->failed = true;
  return status == TURBODB_STATUS_OK ? closed : status;
}
turbodb_status_t orm_sql_index_lookup_plan(const orm_sql_query_scope *scope, const orm_sql_select *plan,
    orm_sql_relation_source *source, turbodb_error_t *error) {
  if (!scope || !plan || !source || !source->owner || source->lookup.budget || source->source.active || source->iterator || source->done)
    return lookup_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "index planning requires an unopened bound relation");
  const sqlparser_node *statement = sqlparser_get_node(scope->document, scope->root);
  if (source->format < ORM_SQL_STORE_FORMAT_INDEXED || !statement->as.select.where) return TURBODB_STATUS_OK;
  const sqlparser_node *table = sqlparser_get_node(scope->document, statement->as.select.from);
  orm_sql_binding_scope binding = {.document=scope->document,.schema=&source->schema,.qualifier=source->schema.name,
    .parameter_types=scope->parameter_types,.parameter_offsets=vec_data_const(&plan->parameter_offsets),
    .parameter_count=scope->parameter_count,.budget=scope->budget};
  const char *reason = NULL; turbodb_status_t status = TURBODB_STATUS_OK;
  if (table->as.table.alias) status = orm_sql_name_node(scope->document, table->as.table.alias, &binding.qualifier, &reason);
  if (status != TURBODB_STATUS_OK) return lookup_error(error, status, reason);
  orm_sql_index_lookup lookup = {.budget=scope->budget}; bool eligible = false;
  status = lookup_predicates(&binding, statement->as.select.where, &lookup, &eligible, error);
  if (status == TURBODB_STATUS_OK && eligible) status = lookup_choose(source, &lookup, error);
  if (status == TURBODB_STATUS_OK && lookup.record.definition.budget) {
    status = orm_tidesdb_sql_index_key_size(&lookup.record.definition, &lookup.tuple_bytes, error);
    size_t ranges = 0;
    for (size_t i = 0; status == TURBODB_STATUS_OK && i < vec_size(&lookup.terms); ++i) {
      lookup_term *term = vec_at(&lookup.terms, i);
      term->bound_bytes = lookup.tuple_bytes / vec_size(&lookup.record.definition.parts) *
          (term->equal_parts + (term->access == ORM_SQL_INDEX_RANGE));
      if (term->bound_bytes > lookup.bound_bytes) lookup.bound_bytes = term->bound_bytes;
      const size_t added = term->list_used ? term->count : 1;
      if (added > SIZE_MAX - ranges) status = lookup_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "index range count overflow");
      else ranges += added;
    }
    if (status == TURBODB_STATUS_OK && (lookup.tuple_bytes > SIZE_MAX - INDEX_PREFIX_BYTES - ORM_SQL_WIRE_U64 ||
        INDEX_PREFIX_BYTES + lookup.tuple_bytes + ORM_SQL_WIRE_U64 > source->owner->max_record_bytes))
      status = lookup_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "index lookup key exceeds record capacity");
    if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&lookup.row, source->schema.count, sizeof(turbodb_value_t),
        _Alignof(turbodb_value_t), scope->budget, &lookup.row_bytes, error);
    if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&lookup.key, INDEX_PREFIX_BYTES + lookup.tuple_bytes, 1, 1,
        scope->budget, &lookup.key_bytes, error);
    if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&lookup.scratch, lookup.tuple_bytes, 1, 1,
        scope->budget, &lookup.scratch_bytes, error);
    if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&lookup.upper, INDEX_PREFIX_BYTES + lookup.tuple_bytes, 1, 1,
        scope->budget, &lookup.upper_bytes, error);
    if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&lookup.entry, INDEX_PREFIX_BYTES + lookup.tuple_bytes, 1, 1,
        scope->budget, &lookup.entry_bytes, error);
    const size_t key_size = vec_size(&lookup.key);
    if (status == TURBODB_STATUS_OK && key_size > SIZE_MAX / LOOKUP_BOUNDARIES)
      status = lookup_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "index range key capacity overflow");
    if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&lookup.range_keys, ranges, key_size * LOOKUP_BOUNDARIES, 1,
        scope->budget, &lookup.range_key_bytes, error);
    if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&lookup.ranges, ranges, sizeof(lookup_range), _Alignof(lookup_range),
        scope->budget, &lookup.range_bytes, error);
    if (status == TURBODB_STATUS_OK) { source->lookup = lookup; return status; }
  }
  const turbodb_status_t closed = orm_sql_index_lookup_close(&lookup, status == TURBODB_STATUS_OK ? error : NULL);
  if (closed != TURBODB_STATUS_OK) source->owner->failed = true;
  return status == TURBODB_STATUS_OK ? closed : status;
}
static turbodb_status_t lookup_value(const lookup_probe *probe, const turbodb_value_t *parameters,
    size_t count, turbodb_value_t *out, turbodb_error_t *error) {
  if (probe->is_parameter && (!parameters || probe->parameter >= count))
    return lookup_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "index probe parameter is missing");
  const turbodb_value_t value = probe->is_parameter ? parameters[probe->parameter] : probe->value;
  if ((value.kind != TURBODB_VALUE_NULL && !lookup_numeric(value.kind)) || value.reserved ||
      (value.kind == TURBODB_VALUE_DOUBLE && !isfinite(value.data.double_value)))
    return lookup_error(error, TURBODB_STATUS_TYPE_ERROR, "index probe requires a finite numeric value or NULL");
  *out = value; return TURBODB_STATUS_OK;
}
/* Map into the column's ordered domain. Integer probes stay exact in integer
 * domains; real columns use the same integer-to-real conversion as predicates.
 * Integer overflow or unsigned wrap must not manufacture an in-domain key. */
static int lookup_ordered(turbodb_value_kind_t kind, turbodb_value_t value, uint64_t *word) {
  if (kind == TURBODB_VALUE_DOUBLE) {
    const double real = value.kind == TURBODB_VALUE_DOUBLE ? value.data.double_value :
        value.kind == TURBODB_VALUE_INT64 ? (double)value.data.int64_value : (double)value.data.uint64_value;
    *word = orm_sql_wire_double_order(real); return 0;
  }
  if (kind == TURBODB_VALUE_UINT64) {
    if (value.kind == TURBODB_VALUE_INT64 && value.data.int64_value < 0) return -1;
    *word = value.kind == TURBODB_VALUE_INT64 ? (uint64_t)value.data.int64_value : value.data.uint64_value;
  } else {
    if (value.kind == TURBODB_VALUE_UINT64 && value.data.uint64_value > INT64_MAX) return 1;
    *word = orm_sql_wire_signed_order(value.kind == TURBODB_VALUE_INT64 ? value.data.int64_value : (int64_t)value.data.uint64_value);
  }
  return 0;
}
static turbodb_value_t lookup_unordered(turbodb_value_kind_t kind, uint64_t word) {
  if (kind == TURBODB_VALUE_DOUBLE) return turbodb_f64(orm_sql_wire_order_double(word));
  return kind == TURBODB_VALUE_INT64 ? turbodb_i64(orm_sql_wire_order_signed(word)) : turbodb_u64(word);
}
typedef struct lookup_interval { uint64_t lower, upper; bool empty; } lookup_interval;
static turbodb_status_t lookup_bound(const lookup_probe *probe, turbodb_value_kind_t kind, bool lower,
    const turbodb_value_t *parameters, size_t count, lookup_interval *interval, turbodb_error_t *error) {
  if (!probe->present) return TURBODB_STATUS_OK;
  turbodb_value_t value; turbodb_status_t status = lookup_value(probe, parameters, count, &value, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (value.kind == TURBODB_VALUE_NULL) { interval->empty = true; return TURBODB_STATUS_OK; }
  uint64_t word = 0; const int position = lookup_ordered(kind, value, &word);
  if (position) {
    if ((lower && position > 0) || (!lower && position < 0)) interval->empty = true;
    return TURBODB_STATUS_OK;
  }
  if (probe->exclusive) {
    const uint64_t minimum = kind == TURBODB_VALUE_DOUBLE ? orm_sql_wire_double_order(-DBL_MAX) : 0;
    const uint64_t maximum = kind == TURBODB_VALUE_DOUBLE ? orm_sql_wire_double_order(DBL_MAX) : UINT64_MAX;
    if ((lower && word == maximum) || (!lower && word == minimum)) { interval->empty = true; return TURBODB_STATUS_OK; }
    if (lower) ++word; else --word;
    if (kind == TURBODB_VALUE_DOUBLE && word == orm_sql_wire_double_order(0.0) - 1) {
      if (lower) ++word; else --word;
    }
  }
  if (lower) interval->lower = word; else interval->upper = word;
  return TURBODB_STATUS_OK;
}
/* Shortest lexicographic successor excludes every key sharing this prefix.
 * Identity's leading namespace byte guarantees a successor, even when a DESC
 * NULL suffix is entirely 0xff. Sizes never exceed the preallocated full key. */
static void lookup_after(uint8_t *key, size_t *size) {
  size_t i = *size;
  while (i && key[i-1] == UINT8_MAX) --i;
  if (i) ++key[i-1];
  *size = i;
}
static turbodb_status_t lookup_encode(orm_sql_index_lookup *lookup, vec_t *output, turbodb_error_t *error) {
  uint8_t *key = vec_data(output); orm_sql_index_prefix(key, INDEX_DATA_NS, &lookup->record.identity);
  return orm_tidesdb_sql_index_key_encode(&lookup->record.definition, vec_data_const(&lookup->row), vec_size(&lookup->row),
      key + INDEX_PREFIX_BYTES, lookup->tuple_bytes, &lookup->contains_null, error);
}
static turbodb_status_t lookup_bind_one(orm_sql_index_lookup *lookup, const lookup_term *term,
    size_t choice, const turbodb_value_t *parameters, size_t count, turbodb_error_t *error) {
  lookup->empty = lookup->contains_null = false;
  lookup->lower_size = lookup->upper_size = INDEX_PREFIX_BYTES + term->bound_bytes;
  const size_t parts = vec_size(&lookup->record.definition.parts);
  if (parts > SIZE_MAX - lookup->tuple_bytes)
    return lookup_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "index probe step capacity overflow");
  turbodb_status_t status = lookup_steps(lookup->budget, parts + lookup->tuple_bytes, error);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < parts; ++i) {
    const orm_sql_index_part *part = vec_at_const(&lookup->record.definition.parts, i);
    turbodb_value_t value = part->type.kind == TURBODB_VALUE_INT64 ? turbodb_i64(0) :
        part->type.kind == TURBODB_VALUE_DOUBLE ? turbodb_f64(0.0) : turbodb_u64(0);
    if (i < term->equal_parts) {
      const lookup_probe *probe = term->list_used && term->list_column == part->column ?
          vec_at_const(&lookup->choices, term->first + choice) :
          &((const lookup_column *)vec_at_const(&lookup->probes, term->column_offset + part->column))->equal;
      status = lookup_value(probe, parameters, count, &value, error);
      if (status != TURBODB_STATUS_OK) break;
      if (value.kind == TURBODB_VALUE_NULL) {
        if (!probe->null_equal || !part->type.nullable) lookup->empty = true;
      } else {
        uint64_t word = 0;
        if (lookup_ordered(part->type.kind, value, &word)) lookup->empty = true;
        else value = lookup_unordered(part->type.kind, word);
      }
    }
    *(turbodb_value_t *)vec_at(&lookup->row, part->column) = value;
  }
  if (status != TURBODB_STATUS_OK || lookup->empty) return status;
  if (term->access == ORM_SQL_INDEX_RANGE) {
    const orm_sql_index_part *part = vec_at_const(&lookup->record.definition.parts, term->equal_parts);
    const lookup_column *column = vec_at_const(&lookup->probes, term->column_offset + part->column);
    lookup_interval interval = {.upper=UINT64_MAX};
    if (part->type.kind == TURBODB_VALUE_DOUBLE) {
      interval.lower = orm_sql_wire_double_order(-DBL_MAX);
      interval.upper = orm_sql_wire_double_order(DBL_MAX);
    }
    status = lookup_bound(&column->lower, part->type.kind, true, parameters, count, &interval, error);
    if (status == TURBODB_STATUS_OK) status = lookup_bound(&column->upper, part->type.kind, false, parameters, count, &interval, error);
    if (status != TURBODB_STATUS_OK) return status;
    if (interval.empty || interval.lower > interval.upper) { lookup->empty = true; return TURBODB_STATUS_OK; }
    *(turbodb_value_t *)vec_at(&lookup->row, part->column) = lookup_unordered(part->type.kind, part->descending ? interval.upper : interval.lower);
    status = lookup_encode(lookup, &lookup->key, error);
    *(turbodb_value_t *)vec_at(&lookup->row, part->column) = lookup_unordered(part->type.kind, part->descending ? interval.lower : interval.upper);
    if (status == TURBODB_STATUS_OK) status = lookup_encode(lookup, &lookup->upper, error);
  } else {
    status = lookup_encode(lookup, &lookup->key, error);
    if (status == TURBODB_STATUS_OK) memcpy(vec_data(&lookup->upper), vec_data_const(&lookup->key), lookup->upper_size);
  }
  if (status == TURBODB_STATUS_OK) lookup_after(vec_data(&lookup->upper), &lookup->upper_size);
  return status;
}
turbodb_status_t orm_sql_index_lookup_bind(orm_sql_index_lookup *lookup, const turbodb_value_t *parameters,
    size_t count, turbodb_error_t *error) {
  if (!lookup->budget) return TURBODB_STATUS_OK;
  lookup->range_count = lookup->range_position = 0;
  const size_t key_size = vec_size(&lookup->key);
  turbodb_status_t status = TURBODB_STATUS_OK;
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < vec_size(&lookup->terms); ++i) {
    const lookup_term *term = vec_at_const(&lookup->terms, i);
    const size_t choices = term->list_used ? term->count : 1;
    for (size_t j = 0; status == TURBODB_STATUS_OK && j < choices; ++j) {
      status = lookup_bind_one(lookup, term, j, parameters, count, error);
      if (status != TURBODB_STATUS_OK || lookup->empty) continue;
      status = lookup_steps(lookup->budget, key_size * LOOKUP_BOUNDARIES, error);
      if (status != TURBODB_STATUS_OK) break;
      uint8_t *keys = vec_at(&lookup->range_keys, lookup->range_count);
      memcpy(keys, vec_data_const(&lookup->key), lookup->lower_size);
      memcpy(keys + key_size, vec_data_const(&lookup->upper), lookup->upper_size);
      *(lookup_range *)vec_at(&lookup->ranges, lookup->range_count++) =
          (lookup_range){keys, keys + key_size, lookup->lower_size, lookup->upper_size};
    }
  }
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_sort(vec_data(&lookup->ranges), lookup->range_count,
      &lookup_range_type, key_size, lookup->budget, error);
  size_t merged = 0;
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < lookup->range_count; ++i) {
    const lookup_range current = *(const lookup_range *)vec_at_const(&lookup->ranges, i);
    status = lookup_steps(lookup->budget, key_size * LOOKUP_BOUNDARIES, error);
    if (status != TURBODB_STATUS_OK) break;
    lookup_range *previous = merged ? vec_at(&lookup->ranges, merged - 1) : NULL;
    if (previous && lookup_compare(current.lower, current.lower_size, previous->upper, previous->upper_size) <= 0) {
      if (lookup_compare(current.upper, current.upper_size, previous->upper, previous->upper_size) > 0) {
        previous->upper = current.upper; previous->upper_size = current.upper_size;
      }
    } else *(lookup_range *)vec_at(&lookup->ranges, merged++) = current;
  }
  lookup->range_count = status == TURBODB_STATUS_OK ? merged : 0;
  lookup->empty = !lookup->range_count;
  return status;
}
static turbodb_status_t lookup_unique(orm_sql_relation_source *source, const uint8_t *primary, bool contains_null, turbodb_error_t *error) {
  orm_sql_index_lookup *lookup = &source->lookup;
  if (!lookup->record.definition.unique || contains_null) return TURBODB_STATUS_OK;
  uint8_t *key = vec_data(&lookup->entry); key[0] = INDEX_UNIQUE_NS; store_buffer value = {0};
  turbodb_status_t status = orm_sql_store_get(source->owner, key, vec_size(&lookup->entry), source->owner->max_record_bytes, &value, error);
  key[0] = INDEX_DATA_NS;
  if (status == TURBODB_STATUS_OK && (!value.found || value.size != ORM_SQL_WIRE_U64 || memcmp(value.data, primary, ORM_SQL_WIRE_U64)))
    status = lookup_error(error, TURBODB_STATUS_DATASTORE_ERROR, "index lookup unique owner mismatch");
  const turbodb_status_t closed = orm_sql_store_buffer_close(source->owner, &value, status == TURBODB_STATUS_OK ? error : NULL);
  if (closed != TURBODB_STATUS_OK) source->owner->failed = true;
  return status == TURBODB_STATUS_OK ? closed : status;
}
turbodb_status_t orm_sql_index_lookup_read(orm_sql_relation_source *source, const turbodb_value_t **out, turbodb_error_t *error) {
  orm_sql_index_lookup *lookup = &source->lookup;
  turbodb_status_t status = TURBODB_STATUS_OK; int code = ORM_TDB_SUCCESS;
  uint8_t *key = NULL, *value = NULL; size_t size = 0, value_size = 0;
  const size_t prefix = vec_size(&lookup->entry);
  if (prefix > SIZE_MAX / LOOKUP_KEY_PASSES)
    return lookup_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "index lookup step capacity overflow");
  while (lookup->range_position < lookup->range_count) {
    const lookup_range *range = vec_at_const(&lookup->ranges, lookup->range_position);
    status = lookup_steps(lookup->budget, 1, error); if (status != TURBODB_STATUS_OK) return status;
    if (!source->iterator) {
      code = orm_tidesdb_iter_new(source->owner->transaction, source->owner->family, &source->iterator);
      if (code != ORM_TDB_SUCCESS) return orm_sql_store_native(error, code, "open index lookup");
    }
    code = source->advance ? orm_tidesdb_iter_next(source->iterator) :
        orm_tidesdb_iter_seek(source->iterator, range->lower, range->lower_size);
    if (code != ORM_TDB_SUCCESS && code != ORM_TDB_ERR_NOT_FOUND) return orm_sql_store_native(error, code, "seek index lookup");
    if (code == ORM_TDB_ERR_NOT_FOUND || !orm_tidesdb_iter_valid(source->iterator)) break;
    code = orm_tidesdb_iter_key(source->iterator, &key, &size);
    if (code != ORM_TDB_SUCCESS) return orm_sql_store_native(error, code, "read index lookup key");
    if (!key) return lookup_error(error, TURBODB_STATUS_DATASTORE_ERROR, "index lookup returned no key");
    status = lookup_steps(lookup->budget, prefix * LOOKUP_KEY_PASSES, error); if (status != TURBODB_STATUS_OK) return status;
    if (lookup_compare(key, size, range->upper, range->upper_size) >= 0) {
      ++lookup->range_position; source->advance = false; continue;
    }
    if (lookup_compare(key, size, range->lower, range->lower_size) < 0)
      return lookup_error(error, TURBODB_STATUS_DATASTORE_ERROR, "index iterator moved below its lower bound");
    break;
  }
  if (lookup->range_position == lookup->range_count || code == ORM_TDB_ERR_NOT_FOUND ||
      (source->iterator && !orm_tidesdb_iter_valid(source->iterator))) {
    source->done = true; *out = NULL; return TURBODB_STATUS_OK;
  }
  code = orm_tidesdb_iter_value(source->iterator, &value, &value_size);
  if (code != ORM_TDB_SUCCESS) return orm_sql_store_native(error, code, "read index lookup value");
  if (size > UINT64_MAX - value_size) return lookup_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "index lookup read overflow");
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_READ_ROWS] = 1;
  amount.value[ORM_SQL_BUDGET_READ_BYTES] = size + value_size;
  status = orm_tidesdb_sql_budget_reserve(lookup->budget, &amount, error); if (status != TURBODB_STATUS_OK) return status;
  if (size != prefix + ORM_SQL_WIRE_U64 || !value || value_size != ORM_SQL_WIRE_U64 || memcmp(value, key + prefix, ORM_SQL_WIRE_U64))
    return lookup_error(error, TURBODB_STATUS_DATASTORE_ERROR, "index lookup primary key mismatch");
  uint8_t primary[ORM_SQL_WIRE_U64]; memcpy(primary, key + prefix, sizeof(primary));
  memcpy(vec_data(&lookup->entry), key, prefix);
  const turbodb_value_t *row = NULL; status = orm_sql_relation_index_row(source, primary, &row, error);
  bool contains_null = false;
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_index_key_encode(&lookup->record.definition, row, source->schema.count,
      vec_data(&lookup->scratch), lookup->tuple_bytes, &contains_null, error);
  if (status == TURBODB_STATUS_OK) status = lookup_steps(lookup->budget, lookup->tuple_bytes, error);
  if (status == TURBODB_STATUS_OK && memcmp(vec_data_const(&lookup->scratch), (const uint8_t *)vec_data_const(&lookup->entry) + INDEX_PREFIX_BYTES, lookup->tuple_bytes))
    status = lookup_error(error, TURBODB_STATUS_DATASTORE_ERROR, "index lookup tuple differs from Data");
  if (status == TURBODB_STATUS_OK) status = lookup_unique(source, primary, contains_null, error);
  if (status == TURBODB_STATUS_OK) { source->advance = true; *out = row; }
  return status;
}
turbodb_status_t orm_sql_index_lookup_close(orm_sql_index_lookup *lookup, turbodb_error_t *error) {
  if (!lookup || !lookup->budget) return TURBODB_STATUS_OK;
  turbodb_status_t status = orm_tidesdb_sql_index_record_destroy(&lookup->record, error);
  vec_t *vectors[] = {&lookup->probes, &lookup->row, &lookup->key, &lookup->upper, &lookup->entry, &lookup->scratch,
      &lookup->terms, &lookup->choices, &lookup->ranges, &lookup->range_keys};
  const size_t bytes[] = {lookup->probe_bytes, lookup->row_bytes, lookup->key_bytes, lookup->upper_bytes, lookup->entry_bytes, lookup->scratch_bytes,
      lookup->term_bytes, lookup->choice_bytes, lookup->range_bytes, lookup->range_key_bytes};
  for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
    const turbodb_status_t closed = orm_sql_work_release(vectors[i], bytes[i], lookup->budget, status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = closed;
  }
  *lookup = (orm_sql_index_lookup){0}; return status;
}

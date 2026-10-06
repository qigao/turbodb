#ifndef ORM_TIDESDB_SQL_INDEX_LOOKUP_H
#define ORM_TIDESDB_SQL_INDEX_LOOKUP_H
#include "index_record.h"
#include "binding.h"

struct orm_sql_relation_source;
struct orm_sql_select;
typedef enum orm_sql_index_access {
  ORM_SQL_INDEX_EQUAL, ORM_SQL_INDEX_PREFIX, ORM_SQL_INDEX_RANGE
} orm_sql_index_access;
/* Private owned access plan embedded in the stable relation source. Numeric
 * constants/parameter ordinals only; no AST or parameter borrowing. Fixed Vec
 * capacity is charged at open, released at close. No independent store lease.
 * One synchronous consumer. Bound ranges are immutable while active; only the
 * range cursor and row-validation workspace change during reads. */
typedef struct orm_sql_index_lookup {
  orm_tidesdb_sql_budget *budget;
  orm_sql_index_record record;
  vec_t probes, row, key, upper, entry, scratch;
  vec_t terms, choices, ranges, range_keys;
  size_t probe_bytes, row_bytes, key_bytes, upper_bytes, entry_bytes, scratch_bytes, tuple_bytes;
  size_t term_bytes, choice_bytes, range_bytes, range_key_bytes, range_count, range_position;
  size_t equal_parts, bound_bytes, lower_size, upper_size;
  orm_sql_index_access access;
  bool empty, contains_null;
} orm_sql_index_lookup;
/* Called after full SELECT binding, before source attachment. Considers only
 * OR branches of AND trees of integer equality/IS NULL, inequalities, BETWEEN
 * and at most one positive scalar IN list per branch. All branches must use
 * one index's contiguous leading equalities and optional following range part.
 * No eligible index is normal and leaves the Data scan selected. Metadata,
 * allocation or budget errors propagate, never select a different access path.
 * Reads no Data/index entries. Failure leaves lookup empty. Planning uses
 * O(document nodes + branches*columns + IN items + ranges*key bytes) workspace,
 * plus directory storage. Each candidate checks every branch's leading parts.
 * LIMIT_EXCEEDED/OOM/native errors propagate; corrupt directory poisons owner. */
turbodb_status_t orm_sql_index_lookup_plan(const orm_sql_query_scope *, const struct orm_sql_select *,
    struct orm_sql_relation_source *, turbodb_error_t *);
/* Requires validated parameters, inactive source, after rewind on reuse.
 * Encodes and merges owned ranges in O(R log R*K) steps for R ranges/K key
 * bytes. Scratch sorting space is charged to WORK. Failure aborts opening. */
turbodb_status_t orm_sql_index_lookup_bind(orm_sql_index_lookup *, const turbodb_value_t *, size_t, turbodb_error_t *);
/* Internal relation callback: lazy prefix iterator plus same-snapshot Data
 * point reads, validating touched derived entries. Caller locks errors and
 * poisons corrupt owners. Borrowed row follows the relation next contract. */
turbodb_status_t orm_sql_index_lookup_read(struct orm_sql_relation_source *, const turbodb_value_t **, turbodb_error_t *);
turbodb_status_t orm_sql_index_lookup_close(orm_sql_index_lookup *, turbodb_error_t *);
#endif

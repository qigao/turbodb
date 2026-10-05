#ifndef ORM_TIDESDB_SQL_JOIN_H
#define ORM_TIDESDB_SQL_JOIN_H
#include "scan.h"

typedef enum orm_sql_join_kind { ORM_SQL_JOIN_INNER, ORM_SQL_JOIN_LEFT, ORM_SQL_JOIN_CROSS } orm_sql_join_kind;
typedef enum orm_sql_join_match { ORM_SQL_JOIN_MATCH_ON, ORM_SQL_JOIN_MATCH_USING } orm_sql_join_match;
typedef struct orm_sql_join_key { size_t left, right; } orm_sql_join_key;

/* Private synchronous right owner. open borrows the left row until close, must
 * fail without retained execution/borrows, and publishes the supplied source at
 * its stable address with the original budget/shape/types. close runs after its
 * scan lease ends; failure retains the owner and borrow for explicit close retry.
 * No callbacks at join construction or for an empty left input. */
typedef struct orm_sql_join_right_binding {
  void *context;
  turbodb_status_t (*open)(void *context, const turbodb_value_t *left, size_t columns, turbodb_error_t *error);
  turbodb_status_t (*close)(void *context, turbodb_error_t *error);
} orm_sql_join_right_binding;
typedef struct orm_sql_join_spec {
  orm_sql_join_kind kind;
  orm_sql_expr *condition; /* BOOL/NULL ON; required except CROSS, which forbids it. */
  const size_t *slots; /* Original left columns, right columns, then parameters. */
  size_t count;
  const turbodb_value_t *parameters;
  const orm_sql_type *parameter_types;
  size_t parameter_count;
  orm_sql_expr_query_sources queries;
  const size_t *query_slots;
  size_t query_count;
  orm_sql_join_right_binding right_binding;
  orm_sql_join_match match;
  const orm_sql_join_key *keys; /* Physical child slots; copied by open. */
  size_t key_count; /* USING with zero keys matches every examined pair. */
} orm_sql_join_spec;

/* Private, complete nested-loop execution stage; SQL Binder admission is separate.
 * One synchronous owner, zero initialize, stable address through close. Owns
 * both input scans, right snapshots, output/types, ON registers/mapping/parameters.
 * Borrows independent source descriptors and ON program through close; opening
 * either source elsewhere is BUSY. Parent/downstream closes before this stage.
 * Input native handles/transactions always belong to source owners. */
typedef struct orm_sql_join {
  orm_tidesdb_sql_budget *budget;
  orm_sql_scan left, right;
  orm_sql_rows rows;
  orm_sql_row_source source;
  orm_sql_expr_run condition;
  vec_t types, output, slots, inputs;
  size_t type_bytes, output_bytes, slot_bytes, input_bytes, metadata_bytes;
  orm_sql_snapshot parameters;
  const turbodb_value_t *pending;
  size_t position;
  orm_sql_join_kind kind;
  bool ready, matched, evaluating;
  orm_sql_scan_state state;
  turbodb_error_t failure;
  orm_sql_join_right_binding right_binding;
  orm_sql_row_source *right_source;
  vec_t right_types;
  size_t right_type_bytes;
  bool right_bound;
  vec_t keys, comparisons;
  size_t key_bytes, comparison_bytes;
} orm_sql_join;

/* Same active budget for both sources and ON; copies specification/parameters,
 * validates types even for empty inputs, reads nothing. LEFT changes only output
 * right nullability, not ON input types. Failure leaves empty out and unwinds new
 * source/program leases. Self-join requires two independent sources/cursors. */
/* USING mode forbids ON/query mappings and CROSS, validates and copies every
 * key pair before acquiring sources, then uses ordinary EQUAL (NULL is UNKNOWN).
 * It accepts zero keys for NATURAL without common columns, including LEFT.
 * Comparison types follow the original children before outer null extension. */
turbodb_status_t orm_tidesdb_sql_join_open(orm_sql_row_source *left, orm_sql_row_source *right,
    const orm_sql_join_spec *spec, orm_sql_join *out, turbodb_error_t *error);
/* With right_binding, materialization is per left row and reclaimed before the
 * left input advances; INNER/CROSS must continue after an empty right round.
 * The right descriptor is metadata-only until open, and is borrowed through join
 * close. Each round validates its published schema and charges existing budgets.
 * Cancel/error retain the current round until close; callback close failure
 * preserves the left lease and run for retry. Ordinary joins are unchanged. */
/* Dependent time is O(sum(right_rows_per_left)*ON + callback cost), storage is
 * bounded by one right round and fixed left/output/type metadata. */
/* Left-first, right materialized once only if a left row exists; rows deep copied
 * before another right pull. Each examined pair charges JOIN_PAIRS, including
 * rejected/UNKNOWN pairs. Only TRUE matches. LEFT emits one null-extended row
 * if no match; WHERE belongs downstream. No order promised to SQL callers.
 * O(L*R*ON) time, O(R*right_width+payload+left_width+output_width) storage, bounded
 * by shared work/materialized/pair/step limits. No implicit spill or algorithm
 * change. Physical reads charged only by original sources, never pair output.
 * Output borrowed until next/cancel/close. Error leaves out unchanged and locks
 * failure; previously returned rows may exist. Repeated terminal/error is inert.
 * Direct next/cancel/close is BUSY while downstream has the source lease or
 * during evaluation. ON query sources use independent mappings; borrowed until close. */
turbodb_status_t orm_tidesdb_sql_join_next(orm_sql_join *run, orm_sql_scan_row *out, turbodb_error_t *error);
orm_sql_row_source *orm_tidesdb_sql_join_source(orm_sql_join *run);
turbodb_status_t orm_tidesdb_sql_join_cancel(orm_sql_join *run, turbodb_error_t *error);
turbodb_status_t orm_tidesdb_sql_join_close(orm_sql_join *run, turbodb_error_t *error);
#endif

#ifndef ORM_TIDESDB_SQL_EXPR_H
#define ORM_TIDESDB_SQL_EXPR_H

#include "value.h"
#include "diagnostics.h"
#include <cstl/vec.h>
#include <sqlparser/sqlparser.h>

/* One resolved NAME/PARAMETER or explicitly validated group/window expression
 * occurrence per entry, strictly sorted by node ID. Replacing any other scalar
 * subtree requires replace_expression=true; its children are not compiled.
 * The ordinary scalar whitelist never admits aggregate/window functions implicitly.
 * Array index is its runtime slot. Resolution and SQL parameter ordering belong
 * to the caller; all entries must occur within this expression. */
typedef struct orm_sql_expr_input {
  sqlparser_id node;
  orm_sql_type type;
  bool replace_expression; /* Trusted Binder validated this non-NAME/PARAMETER subtree. */
} orm_sql_expr_input;

typedef enum orm_sql_subquery_kind {
  ORM_SQL_SUBQUERY_SCALAR, ORM_SQL_SUBQUERY_EXISTS, ORM_SQL_SUBQUERY_NOT_EXISTS,
  ORM_SQL_SUBQUERY_IN, ORM_SQL_SUBQUERY_NOT_IN
} orm_sql_subquery_kind;
typedef struct orm_sql_expr_query_type {
  orm_sql_subquery_kind kind;
  orm_sql_type result, element; /* element is the IN/NOT IN source column. */
} orm_sql_expr_query_type;
typedef struct orm_sql_expr_query_binding {
  sqlparser_id node;
  orm_sql_expr_query_type type;
  /* Binding only: consumers map original input columns to group/window input slots.
   * SIZE_MAX means no group-key value. No row access or retained map borrow. */
  void *capture_context;
  turbodb_status_t (*bind_capture)(void *context,const size_t *slots,size_t count,
      size_t row_count,turbodb_error_t *error);
} orm_sql_expr_query_binding;
/* Private synchronous query boundary. Stable descriptor/context until all runs
 * close; immutable except active_runs, managed only by expression run open/close.
 * Multiple sequential consumers may share it; no concurrent calls. The owner
 * must refuse direct mutation/destruction while active_runs is nonzero. Callback
 * outputs borrow owner storage until owner cancellation/close, not just until
 * the next callback; callbacks never retain probe or comparison.
 * IN receives a compiled EQUAL predicate; other kinds receive NULL comparison.
 * outer_row is the current source row when evaluation belongs to a row pipeline;
 * otherwise it is NULL with outer_count zero. Callbacks never retain it. No row
 * reads during open. */
typedef struct orm_sql_expr_query_source {
  orm_tidesdb_sql_budget *budget;
  orm_sql_expr_query_type type;
  void *context;
  turbodb_status_t (*eval)(void *context, const turbodb_value_t *probe, const orm_sql_predicate *comparison,
      const turbodb_value_t *outer_row, size_t outer_count, turbodb_value_t *out, turbodb_error_t *error);
  size_t active_runs;
} orm_sql_expr_query_source;
/* Borrowed registry for one execution; slots use the binding registry order.
 * Pointer array is needed only during open, referenced sources through close. */
typedef struct orm_sql_expr_query_sources {
  orm_sql_expr_query_source *const *items;
  size_t count;
  orm_sql_evaluation evaluation;
} orm_sql_expr_query_sources;
typedef struct orm_sql_expr_bindings {
  const orm_sql_expr_input *inputs;
  size_t input_count;
  const orm_sql_expr_query_binding *queries;
  size_t query_count;
} orm_sql_expr_bindings;

typedef enum orm_sql_expr_function {
  ORM_SQL_COALESCE, ORM_SQL_IFNULL, ORM_SQL_NULLIF,
  ORM_SQL_FUNCTION_ABS, ORM_SQL_FUNCTION_SIGN, ORM_SQL_FUNCTION_FLOOR, ORM_SQL_FUNCTION_CEIL,
  ORM_SQL_FUNCTION_MOD, ORM_SQL_FUNCTION_ROUND, ORM_SQL_FUNCTION_TRUNCATE
} orm_sql_expr_function;

/* Shared private Binder/compiler whitelist. Borrows document/call only during
 * the call, allocates nothing, leaves out unchanged on error. Accepts bare ASCII
 * names case-insensitively; rejects modifiers/unknown functions as UNSUPPORTED,
 * wrong arity as SQL_ERROR, malformed arguments as INVALID_ARGUMENT.
 * Function name nodes are structural and must not appear in input bindings. */
turbodb_status_t orm_tidesdb_sql_expr_resolve_call(const sqlparser_document *document,
    const sqlparser_node *call, orm_sql_expr_function *out, turbodb_error_t *error);
/* Shared classification for the resolved numeric-call whitelist. */
bool orm_tidesdb_sql_expr_function_arithmetic(orm_sql_expr_function function,
    orm_sql_arithmetic_op *out);
bool orm_tidesdb_sql_expr_arithmetic_binary(orm_sql_arithmetic_op operation);
/* Resolve supported numeric CAST targets; TYPE names/precision are structural,
 * not runtime inputs. FLOAT precision text is charged before scanning.
 * Failure preserves target. */
turbodb_status_t orm_tidesdb_sql_expr_resolve_cast(const sqlparser_document *document,
    const sqlparser_node *cast, orm_tidesdb_sql_budget *budget,
    orm_sql_cast_target *target, turbodb_error_t *error);

/* Caller owns zero-initialized storage; fields are private/read-only after
 * compile. Code owns scalar/TEXT constants and never borrows the document or inputs.
 * Known system variables compile to session IDs. Their runs require a valid
 * evaluation.session, copied once at run open; TEXT results have static lifetime.
 * Single synchronous owner, no concurrent compile/eval/destroy. */
typedef struct orm_sql_expr {
  vec_t code, literals;
  orm_tidesdb_sql_budget *budget;
  size_t work_bytes, register_count, result_register, input_count;
  size_t literal_work_bytes;
  size_t active_runs; /* Lifecycle only; compiled code stays immutable. */
  orm_sql_type result;
  size_t query_count;
  bool uses_session;
} orm_sql_expr;

typedef struct orm_sql_expr_run {
  orm_sql_expr *program;
  vec_t registers;
  size_t work_bytes;
  vec_t queries;
  size_t query_bytes;
  bool evaluating;
  orm_sql_evaluation evaluation;
} orm_sql_expr_run;

/* Private structural equality of compiled programs from the same binding scope
 * and budget. slots map input occurrences to scope columns/parameter positions;
 * arrays have input_count entries. Ignores diagnostic source offsets, compares
 * instructions, types, constants and resolved slots; no algebraic rewriting.
 * O(code + literal bytes), no allocation, charged steps. Leaves out on error. */
turbodb_status_t orm_tidesdb_sql_expr_same(const orm_sql_expr *left, const size_t *left_slots,
    const orm_sql_expr *right, const size_t *right_slots, bool *out, turbodb_error_t *error);

/* Zero output required. Open allocates fixed registers; eval never allocates or
 * grows them, and clears borrowed register values on every evaluated exit.
 * Query callbacks may allocate within their shared budget; the evaluator itself
 * does not allocate after open.
 * Program must stay at a stable address until close; destroy returns BUSY while
 * any run is open. Runs share one active budget, use distinct storage, and are
 * single-owner (no concurrent operations). Failure leaves empty output. */
turbodb_status_t orm_tidesdb_sql_expr_run_open(orm_sql_expr *program,
    orm_sql_expr_run *out, turbodb_error_t *error);
/* Explicit statement policy, copied into the run. Receiver borrows until close.
 * Old open uses QUERY without a receiver; mapped open inherits the registry. */
turbodb_status_t orm_tidesdb_sql_expr_run_open_evaluation(orm_sql_expr *program,
    orm_sql_evaluation evaluation, orm_sql_expr_run *out, turbodb_error_t *error);
/* Query-aware open validates every source and borrows one reference per binding,
 * including skipped branches. Copies the pointer array, not contexts. Same
 * budget/kind/element types and a result no weaker than compiled nullability are
 * required. Failed open refunds work and references. Old run_open rejects query
 * programs. Close is BUSY while evaluating; evaluation is non-reentrant. */
turbodb_status_t orm_tidesdb_sql_expr_run_open_queries(orm_sql_expr *program,
    orm_sql_expr_query_source *const *queries, size_t count, orm_sql_expr_run *out, turbodb_error_t *error);
/* Maps local query slots into an execution registry, then opens the same run.
 * NULL registry means no sources. Temporary pointer storage is charged and
 * refunded on all exits; no callback invocation. O(count) work and space. */
turbodb_status_t orm_tidesdb_sql_expr_run_open_mapped(orm_sql_expr *program,
    const size_t *slots, size_t count, const orm_sql_expr_query_sources *sources,
    orm_sql_expr_run *out, turbodb_error_t *error);
turbodb_status_t orm_tidesdb_sql_expr_run_eval(orm_sql_expr_run *run,
    const turbodb_value_t *inputs, size_t input_count, turbodb_value_t *out, turbodb_error_t *error);
/* Evaluates with a borrowed current source row for query dependencies. The row
 * remains valid only for this synchronous call and is never used by ordinary
 * expression instructions. */
turbodb_status_t orm_tidesdb_sql_expr_run_eval_row(orm_sql_expr_run *run,
    const turbodb_value_t *inputs, size_t input_count, const turbodb_value_t *outer_row,
    size_t outer_count, turbodb_value_t *out, turbodb_error_t *error);
turbodb_status_t orm_tidesdb_sql_expr_run_close(orm_sql_expr_run *run, turbodb_error_t *error);
/* Same temporary run/cleanup as expr_eval, with an explicit statement policy. */
turbodb_status_t orm_tidesdb_sql_expr_eval_evaluation(orm_sql_expr *program,
    const turbodb_value_t *inputs, size_t input_count, orm_sql_evaluation evaluation,
    turbodb_value_t *out, turbodb_error_t *error);

typedef struct orm_sql_expr_input_layout {
  const orm_sql_type *columns, *parameters;
  size_t column_count, parameter_count;
} orm_sql_expr_input_layout;

/* slots indexes columns followed by parameters. Checks all inputs once,
 * including unreachable ones. Same kinds and no weaker NULL guarantee than
 * bound types required. O(instructions + count), no allocation. */
turbodb_status_t orm_tidesdb_sql_expr_check_inputs(const orm_sql_expr *program,
    const orm_sql_expr_input_layout *layout, const size_t *slots,
    size_t count, turbodb_error_t *error);

/* Compile one BOOL/NULL expression root in a MySQL document. Supported leaves:
 * NULL, BOOL, finite decimal numbers (including unary sign and exponent), UTF-8 TEXT literals and
 * resolved input slots. STRING decoding honors the document's MySQL backslash
 * option and doubled delimiters; checks UTF-8 even in dead branches. First TEXT
 * reserves root.span.length bytes through work.c as a fixed capacity upper bound
 * for owned literal bytes. Views never move; no TEXT means no literal buffer.
 * Supported operators are value.h's predicate whitelist plus scalar IN/NOT IN
 * lists and BETWEEN/NOT BETWEEN. The left value is evaluated once; list items
 * retain strict input types and three-valued logic. DOUBLE/numeric pairs compare
 * as DOUBLE; BETWEEN aggregates all three types before either bound comparison.
 * Searched/simple CASE selects the first TRUE condition/equality, otherwise
 * ELSE or NULL. Simple operands are evaluated once; searched conditions must be
 * BOOL/NULL. Numeric results containing DOUBLE aggregate to DOUBLE; selected
 * I64/U64 values are promoted, possibly rounded. A pure I64/U64 mixture needs
 * DECIMAL and rejects after all results bind. Other non-NULL kinds must agree;
 * CASE nullability is their union.
 * All branches bind even when unreachable. WHEN nodes count toward AST budget,
 * not expression depth. Selected bytes borrow the input or owned program literal.
 * COALESCE/IFNULL use the same result-kind aggregation and select the first
 * non-NULL value; nullable only when all argument types are nullable.
 * NULLIF returns NULL on ordinary equality, otherwise the first argument; both
 * arguments execute once. Numeric addition/subtraction/multiplication and unary
 * signs use checked range; binary DOUBLE/numeric pairs produce DOUBLE, possibly
 * rounding integer peers. Other mixed integer arithmetic and dynamic U64
 * negation are rejected. NULL propagates.
 * ABS/SIGN/FLOOR/CEIL (CEILING alias) evaluate a single numeric argument once.
 * ABS/FLOOR/CEIL preserve numeric kind; SIGN returns I64. NULL propagates;
 * I64_MIN ABS returns LIMIT_EXCEEDED and nonfinite inputs TYPE_ERROR.
 * ROUND(value[,precision]) and TRUNCATE(value,precision) preserve the value
 * kind and use I64/U64/NULL precision. Default precision is a canonical I64 0;
 * both arguments bind and execute once, with NULL propagation and checked range.
 * ASCII TEXT LIKE/NOT LIKE supports %/_, case-sensitive matching, NULL and
 * literal ESCAPE. Default escape follows the document's backslash mode; an
 * explicit empty escape is rejected in NO_BACKSLASH_ESCAPES mode. Escape must
 * decode to at most one ASCII byte. Non-ASCII runtime operands are UNSUPPORTED.
 * Integer DIV/modulo and DOUBLE division/modulo use the value layer's strict
 * numeric types; exact division requiring DECIMAL remains UNSUPPORTED.
 * Numeric CAST to SIGNED/UNSIGNED [INTEGER], DOUBLE [PRECISION], FLOAT[(p)] and default-mode REAL is
 * supported; other targets, BLOB literals, other calls and unbound subqueries are explicitly UNSUPPORTED even in
 * unreachable branches. max_depth must be positive. Iterative, bounded CSTL
 * storage; failure leaves empty output, releases work bytes, retains consumed
 * AST/plan counters. budget must remain active through destroy. No SQL/storage
 * execution or schema inference. Errors include source offset, not SQL text. */
turbodb_status_t orm_tidesdb_sql_expr_compile(const sqlparser_document *document,
    sqlparser_id root, const orm_sql_expr_input *inputs, size_t input_count,
    size_t max_depth, orm_tidesdb_sql_budget *budget, orm_sql_expr *out,
    turbodb_error_t *error);

/* Same compiler/lifecycle, accepts any supported scalar result kind. Result
 * metadata is available in out->result. No implicit conversion to BOOL. */
turbodb_status_t orm_tidesdb_sql_expr_compile_value(const sqlparser_document *document,
    sqlparser_id root, const orm_sql_expr_input *inputs, size_t input_count,
    size_t max_depth, orm_tidesdb_sql_budget *budget, orm_sql_expr *out,
    turbodb_error_t *error);

/* Trusted query bindings, strictly node-ID ordered and all used in this root.
 * Binder must validate each inner query and supply its result/element metadata.
 * Only scalar SUBQUERY, EXISTS/NOT EXISTS and IN/NOT IN with a query qualify.
 * Ordinary inputs keep their existing rules. Inner query AST is opaque, while
 * IN's left expression is compiled/evaluated once. No query execution here.
 * Program owns metadata/instructions, never borrows AST/bindings/sources.
 * predicate selects BOOL/NULL root validation. Same bounds/cleanup as compile.
 * Structural equality of query instructions assumes the same document/scope
 * and compares query node identity rather than independently bound query bodies. */
turbodb_status_t orm_tidesdb_sql_expr_compile_queries(const sqlparser_document *document,
    sqlparser_id root, const orm_sql_expr_bindings *bindings, bool predicate,
    size_t max_depth, orm_tidesdb_sql_budget *budget, orm_sql_expr *out, turbodb_error_t *error);

/* Exact slot count required. Register values borrow the caller during eval;
 * type/UTF-8 checks occur when the slot is reached, including NULL checks.
 * AND/OR evaluate left first and skip right only on decisive FALSE/TRUE.
 * Temporary registers use the same budget and are released on every exit.
 * Output matches the compiled result type, unchanged on failure; may alias an
 * input. TEXT/BLOB output borrows the selected input payload or program literal,
 * which must outlive its use. Closing registers does not invalidate that payload.
 * Failed evaluation retains consumed steps and can be retried within budget.
 * Default eval uses query NULL semantics without a diagnostic receiver;
 * eval_evaluation/run_open_evaluation copies the statement policy and borrows
 * its initialized receiver through the synchronous call/run close. Division
 * by zero returns NULL plus warning in query/IGNORE mode or SQL_ERROR in strict
 * write mode. Warning count overflow fails without publishing the output.
 * O(instructions + scanned bytes) time, bounded O(registers) temporary space. */
turbodb_status_t orm_tidesdb_sql_expr_eval(orm_sql_expr *program,
    const turbodb_value_t *inputs, size_t input_count, turbodb_value_t *out,
    turbodb_error_t *error);

/* Frees code and returns its reserved capacity; zero/NULL is a no-op. Destroy
 * before ending the owning budget statement. Active runs return BUSY unchanged.
 * Returns a budget cleanup error if that lifetime contract was violated.
 * Does not end a native transaction. */
turbodb_status_t orm_tidesdb_sql_expr_destroy(orm_sql_expr *program, turbodb_error_t *error);

#endif

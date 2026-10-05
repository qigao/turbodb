#ifndef ORM_TIDESDB_SQL_VALUE_H
#define ORM_TIDESDB_SQL_VALUE_H

#include "budget.h"

/* Shared decimal literal conversion for legacy lowering and relational code.
 * No allocation; rejects fractions/exponents, checks signed/unsigned range.
 * Failure preserves output. Negative INT64_MIN is represented exactly. */
turbodb_status_t orm_tidesdb_sql_integer_literal(vstr text, bool negative,
    turbodb_value_t *out, turbodb_error_t *error);

/* Decimal SQL number conversion. Integral tokens retain exact I64/U64 kinds;
 * fractions or exponents produce finite F64 values. No allocation or locale
 * dependency. Failure preserves output. */
turbodb_status_t orm_tidesdb_sql_number_literal(vstr text, bool negative,
    turbodb_value_t *out, turbodb_error_t *error);

typedef struct orm_sql_type {
  turbodb_value_kind_t kind;
  bool nullable;
} orm_sql_type;

typedef enum orm_sql_cast_target {
  ORM_SQL_CAST_SIGNED, ORM_SQL_CAST_UNSIGNED, ORM_SQL_CAST_DOUBLE, ORM_SQL_CAST_FLOAT
} orm_sql_cast_target;
enum { ORM_SQL_CAST_FLOAT_PRECISION = 24, ORM_SQL_CAST_DOUBLE_PRECISION = 53 };
typedef struct orm_sql_cast {
  orm_sql_type source, result;
  orm_sql_cast_target target;
} orm_sql_cast;
typedef enum orm_sql_cast_condition {
  ORM_SQL_CAST_EXACT = 0, ORM_SQL_CAST_TRUNCATED = 1,
  ORM_SQL_CAST_COMPLEMENT = 2
} orm_sql_cast_condition;
/* Private numeric CAST, separate from column assignment. Targets I64/U64/F64;
 * NULL propagates. Integer text uses integer prefixes and complement warnings;
 * numeric signed/unsigned conversion wraps without warnings. Finite DOUBLE
 * uses the profile's nearest-even, signed-range lane (no Item provenance).
 * Descriptor is immutable; bytes borrow only for this call. Validate shape
 * before charging bytes+1, UTF-8 after admission; O(bytes), O(1), no allocation.
 * FLOAT checks single-precision range before rounding and carries the rounded
 * value in F64; overflows return OUT_OF_RANGE in every statement mode.
 * Failure preserves both outputs; out may alias input. */
turbodb_status_t orm_tidesdb_sql_cast_bind(orm_sql_type source,
    orm_sql_cast_target target, orm_sql_cast *out, turbodb_error_t *error);
turbodb_status_t orm_tidesdb_sql_cast_eval(const orm_sql_cast *cast,
    const turbodb_value_t *input, orm_tidesdb_sql_budget *budget,
    turbodb_value_t *out, orm_sql_cast_condition *condition, turbodb_error_t *error);

/* Private implicit numeric result promotion, not a column assignment/cast.
 * Accepts I64/U64/finite DOUBLE/NULL only; preserves NULL and converts numeric
 * values to DOUBLE, possibly rounding integers. Validates before charging one
 * execution step; no allocation/I/O/retention, O(1), output may alias input.
 * Invalid values return TYPE_ERROR; every failure preserves out. */
turbodb_status_t orm_tidesdb_sql_real_promote(const turbodb_value_t *input,
    orm_tidesdb_sql_budget *budget, turbodb_value_t *out, turbodb_error_t *error);

typedef enum orm_sql_assignment_adjustment {
  ORM_SQL_ASSIGNMENT_EXACT = 0,
  ORM_SQL_ASSIGNMENT_ROUNDED,
  ORM_SQL_ASSIGNMENT_TRUNCATED,
  ORM_SQL_ASSIGNMENT_OUT_OF_RANGE,
  ORM_SQL_ASSIGNMENT_INVALID,
  ORM_SQL_ASSIGNMENT_NULL_TO_NOT_NULL
} orm_sql_assignment_adjustment;

/* MySQL column-assignment conversion for the relational numeric schema.
 * Strict mode accepts representable numeric/boolean conversions and numeric
 * strings, but rejects invalid or out-of-range input. Permissive mode replaces
 * invalid/NULL numeric input with zero and clips range failures. Fractional
 * numeric values use half-away-from-zero integer rounding. BIGINT text parses
 * decimal fractions/exponents exactly before rounding; DOUBLE text always
 * produces DOUBLE. Strict mode rejects a non-space suffix, permissive mode
 * reports TRUNCATED after converting the numeric prefix. No allocation or byte
 * retention; O(bytes) time/O(1) space. The adjustment reports the single
 * user-visible condition for diagnostics. Input byte scans and one conversion
 * step are charged before work. Failure preserves out and adjustment. */
bool orm_tidesdb_sql_assignment_compatible(orm_sql_type target,
    orm_sql_type source);
turbodb_status_t orm_tidesdb_sql_assignment_convert(orm_sql_type target,
    const turbodb_value_t *input, bool permissive,
    orm_tidesdb_sql_budget *budget, turbodb_value_t *out,
    orm_sql_assignment_adjustment *adjustment, turbodb_error_t *error);

/* This whitelist describes scalar primitives, not executable SQL statements. */
typedef enum orm_sql_predicate_op {
  ORM_SQL_EQUAL,
  ORM_SQL_NOT_EQUAL,
  ORM_SQL_LESS,
  ORM_SQL_LESS_EQUAL,
  ORM_SQL_GREATER,
  ORM_SQL_GREATER_EQUAL,
  ORM_SQL_NULL_SAFE_EQUAL,
  ORM_SQL_IS_NULL,
  ORM_SQL_IS_NOT_NULL,
  ORM_SQL_NOT,
  ORM_SQL_AND,
  ORM_SQL_OR
} orm_sql_predicate_op;

/* Private immutable binding; only bind initializes it. Not an AST/plan owner. */
typedef struct orm_sql_predicate {
  orm_sql_predicate_op op;
  orm_sql_type left, right, result;
  bool real_comparison;
} orm_sql_predicate;

/* right must be NULL for unary operations, non-NULL otherwise. NULL type denotes
 * a literal NULL and must be nullable. Logical operands must be BOOL/NULL.
 * Comparisons allow identical kinds, I64/U64 pairs, DOUBLE/numeric pairs or a
 * NULL literal. DOUBLE comparisons may round integer peers; descriptors retain
 * original kinds. Expr may select real_comparison for both BETWEEN bounds after
 * validating its three numeric/NULL types. No string/BOOL numeric conversion.
 * Unknown operations and unsupported
 * pairs return UNSUPPORTED, malformed types TYPE_ERROR, bad pointers/arity
 * INVALID_ARGUMENT. Failure preserves *out; error is optional. */
turbodb_status_t orm_tidesdb_sql_predicate_bind(orm_sql_predicate_op op,
    orm_sql_type left, const orm_sql_type *right, orm_sql_predicate *out,
    turbodb_error_t *error);

/* Requires a successfully bound immutable predicate and an active budget.
 * Single synchronous owner. Borrowed input bytes must stay valid/immutable for
 * this call only. No allocation, retention, native I/O or transaction mutation.
 * Output is BOOL or NULL (UNKNOWN); only BOOL true passes a SQL filter.
 * Runtime kind/nullability mismatch, nonfinite F64, noncanonical BOOL, malformed
 * views or invalid UTF-8 return TYPE_ERROR. TEXT comparison is UTF-8 byte order
 * with significant trailing spaces and embedded NUL; BLOB is arbitrary bytes.
 * Checks/charges 1 + TEXT validation bytes + compared common-prefix bytes before
 * scanning input. Overflow/limit failure does not charge; invalid UTF-8 retains
 * charged steps. No failure changes *out. Output may alias an input scalar.
 * O(1) space; O(1) scalar time, O(left bytes + right bytes) string time.
 * AND/OR combine already evaluated values; branch evaluation/short circuiting
 * belongs to the future expression program, not this primitive. */
turbodb_status_t orm_tidesdb_sql_predicate_eval(const orm_sql_predicate *predicate,
    const turbodb_value_t *left, const turbodb_value_t *right,
    orm_tidesdb_sql_budget *budget, turbodb_value_t *out, turbodb_error_t *error);

typedef enum orm_sql_arithmetic_op {
  ORM_SQL_ADD, ORM_SQL_SUBTRACT, ORM_SQL_MULTIPLY, ORM_SQL_POSITIVE, ORM_SQL_NEGATE,
  ORM_SQL_ABSOLUTE, ORM_SQL_SIGN, ORM_SQL_FLOOR, ORM_SQL_CEIL,
  ORM_SQL_DIVIDE, ORM_SQL_INTEGER_DIVIDE, ORM_SQL_MODULO,
  ORM_SQL_ROUND, ORM_SQL_TRUNCATE
} orm_sql_arithmetic_op;

typedef struct orm_sql_arithmetic {
  orm_sql_arithmetic_op op;
  orm_sql_type left, right, result;
} orm_sql_arithmetic;

/* Private numeric primitives: I64/U64/finite F64, or literal NULL.
 * Unary +/- and ABS/SIGN/FLOOR/CEIL require right == NULL; dynamic unsigned
 * negation is UNSUPPORTED. ABS/FLOOR/CEIL preserve input kind; SIGN returns I64
 * for numeric input and NULL kind for a static NULL. I64_MIN ABS overflows.
 * DIV accepts I64/U64 pairs; integer MOD follows the left kind. Binary +,-,*,/,
 * MOD with any DOUBLE operand convert numeric peers to DOUBLE, possibly rounding
 * integers beyond double precision. Operand descriptors retain original kinds.
 * Two-integer exact '/' and noninteger DIV require
 * DECIMAL and are UNSUPPORTED. Division/modulo results are nullable.
 * ROUND/TRUNCATE require an I64/U64/NULL precision operand, preserve left kind,
 * and propagate NULL from either operand. ROUND's integer ties go away from
 * zero; DOUBLE uses rint after decimal scaling. TRUNCATE goes toward zero.
 * Extreme precision and positive DOUBLE scaling overflow follow the documented
 * numeric path; final nonfinite/overflow fails. Integers never pass through F64.
 * Other integer mixed-kind arithmetic and BOOL/TEXT/BLOB arithmetic reject. NULL propagates;
 * all-NULL binding has NULL result kind. Binding errors preserve out. */
turbodb_status_t orm_tidesdb_sql_arithmetic_bind(orm_sql_arithmetic_op op,
    orm_sql_type left, const orm_sql_type *right, orm_sql_arithmetic *out,
    turbodb_error_t *error);

/* Successfully bound immutable descriptor, single synchronous owner, borrowed
 * scalars for this call only. No allocation/I/O/retention. Validate kinds and
 * finite inputs before charging one execution step; then check range before
 * integer operations. Nonfinite F64 results/overflow return LIMIT_EXCEEDED;
 * invalid runtime values return TYPE_ERROR. O(1) time/space, no error changes
 * out, which may alias either input. Consumed steps are not refunded. */
turbodb_status_t orm_tidesdb_sql_arithmetic_eval(const orm_sql_arithmetic *arithmetic,
    const turbodb_value_t *left, const turbodb_value_t *right,
    orm_tidesdb_sql_budget *budget, turbodb_value_t *out, turbodb_error_t *error);

typedef enum orm_sql_numeric_condition {
  ORM_SQL_NUMERIC_EXACT, ORM_SQL_NUMERIC_DIVISION_BY_ZERO
} orm_sql_numeric_condition;
/* Same primitive protocol, but zero division publishes NULL plus a condition
 * for the statement owner. NULL operands produce EXACT, even with a zero peer.
 * Failure preserves both outputs. No diagnostic receiver or execution policy.
 * The ordinary eval wrapper rejects zero division and preserves out. */
turbodb_status_t orm_tidesdb_sql_arithmetic_eval_condition(const orm_sql_arithmetic *arithmetic,
    const turbodb_value_t *left, const turbodb_value_t *right, orm_tidesdb_sql_budget *budget,
    turbodb_value_t *out, orm_sql_numeric_condition *condition, turbodb_error_t *error);

enum { ORM_SQL_LIKE_NO_ESCAPE = -1, ORM_SQL_ASCII_MAX = 127 };
typedef struct orm_sql_like {
  orm_sql_type left, right, result;
  int escape;
  bool negated;
  bool ascii_insensitive; /* Opt-in SHOW name matching; bind defaults to false. */
} orm_sql_like;

/* Private ASCII TEXT/NULL pattern matching, case-sensitive by default. The
 * descriptor may explicitly select ASCII folding for SHOW names. escape is an ASCII
 * byte (including NUL), or LIKE_NO_ESCAPE. No implicit conversion. Binding
 * preserves out on invalid arguments/types or unsupported operand kinds. */
turbodb_status_t orm_tidesdb_sql_like_bind(orm_sql_type left, orm_sql_type right,
    int escape, bool negated, orm_sql_like *out, turbodb_error_t *error);
/* Requires a descriptor from like_bind and an active budget. Single synchronous
 * owner. % matches zero or more characters, _ one; escape quotes the following byte,
 * or itself at pattern end. NULL propagates. Validate UTF-8 (TYPE_ERROR) then
 * ASCII (UNSUPPORTED), even if the other operand is NULL. Borrowed inputs for
 * this call only, no allocation/retention. Charge validation bytes up front and
 * each matcher iteration before work; LIMIT_EXCEEDED stops retries. O(1) space,
 * worst O((left length+1)*(right length+1)) time, bounded by execution steps.
 * Failure preserves out; output may alias either operand. */
turbodb_status_t orm_tidesdb_sql_like_eval(const orm_sql_like *like,
    const turbodb_value_t *left, const turbodb_value_t *right,
    orm_tidesdb_sql_budget *budget, turbodb_value_t *out, turbodb_error_t *error);

/* Internal total order for already validated same-kind numeric/BOOL values or
 * NULL. Finite doubles only; NULL sorts first, signed zero compares equal.
 * No coercion, allocation or budget charge: caller admits comparison work. */
int orm_sql_value_order(const turbodb_value_t *left, const turbodb_value_t *right);
#endif

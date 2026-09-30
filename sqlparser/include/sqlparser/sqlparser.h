#ifndef TURBODB_SQLPARSER_H
#define TURBODB_SQLPARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sqlparser_document sqlparser_document;
typedef uint32_t sqlparser_id;

typedef enum sqlparser_dialect {
  SQLPARSER_DIALECT_UNKNOWN = -1, SQLPARSER_MYSQL, SQLPARSER_SQLITE
} sqlparser_dialect;
enum { SQLPARSER_NONE = 0, SQLPARSER_ERROR_CAPACITY = 160 };

typedef enum sqlparser_status {
  SQLPARSER_OK, SQLPARSER_INVALID_ARGUMENT, SQLPARSER_SYNTAX_ERROR,
  SQLPARSER_LIMIT_EXCEEDED, SQLPARSER_OUT_OF_MEMORY
} sqlparser_status;

typedef struct sqlparser_span { size_t offset, length; } sqlparser_span;
typedef struct sqlparser_list { sqlparser_id first, last; size_t count; } sqlparser_list;
typedef struct sqlparser_limits {
  /* max_statements counts top-level statements plus trigger body steps. */
  size_t max_input_bytes, max_nodes, max_statements, max_stack_entries;
} sqlparser_limits;
typedef struct sqlparser_error {
  sqlparser_status status;
  size_t offset;
  char message[SQLPARSER_ERROR_CAPACITY];
} sqlparser_error;

typedef enum sqlparser_kind {
  SQLPARSER_NAME, SQLPARSER_NUMBER, SQLPARSER_STRING, SQLPARSER_NULL,
  SQLPARSER_BOOLEAN, SQLPARSER_PARAMETER, SQLPARSER_VARIABLE, SQLPARSER_STAR,
  SQLPARSER_UNARY, SQLPARSER_BINARY, SQLPARSER_CALL, SQLPARSER_BETWEEN,
  SQLPARSER_IN, SQLPARSER_CASE, SQLPARSER_WHEN, SQLPARSER_SUBQUERY,
  SQLPARSER_SELECT, SQLPARSER_UNION, SQLPARSER_PROJECTION, SQLPARSER_TABLE,
  SQLPARSER_JOIN, SQLPARSER_ORDER, SQLPARSER_LIMIT, SQLPARSER_INSERT,
  SQLPARSER_ROW, SQLPARSER_ASSIGNMENT, SQLPARSER_UPDATE, SQLPARSER_DELETE,
  SQLPARSER_TRANSACTION, SQLPARSER_SET, SQLPARSER_SHOW, SQLPARSER_CREATE_TABLE,
  SQLPARSER_COLUMN, SQLPARSER_TYPE, SQLPARSER_CONSTRAINT, SQLPARSER_DROP_TABLE,
  SQLPARSER_BLOB, SQLPARSER_CAST, SQLPARSER_COLLATE, SQLPARSER_PRAGMA,
  SQLPARSER_CREATE_INDEX, SQLPARSER_DROP_INDEX, SQLPARSER_CREATE_VIEW,
  SQLPARSER_DROP_VIEW, SQLPARSER_ATTACH, SQLPARSER_DETACH,
  SQLPARSER_WITH, SQLPARSER_CTE, SQLPARSER_VALUES, SQLPARSER_TABLE_OPTION,
  SQLPARSER_CREATE_VIRTUAL_TABLE, SQLPARSER_MODULE_ARGUMENT,
  SQLPARSER_CREATE_TRIGGER, SQLPARSER_DROP_TRIGGER, SQLPARSER_RAISE,
  SQLPARSER_EXPLAIN, SQLPARSER_ANALYZE, SQLPARSER_REINDEX, SQLPARSER_VACUUM,
  SQLPARSER_ALTER_TABLE, SQLPARSER_QUERY_GROUP,
  SQLPARSER_DEFAULT_VALUE, SQLPARSER_TRUNCATE_TABLE,
  SQLPARSER_PREPARE, SQLPARSER_EXECUTE, SQLPARSER_DEALLOCATE,
  SQLPARSER_LOCK_TABLES, SQLPARSER_UNLOCK_TABLES, SQLPARSER_LOCK_TARGET,
  SQLPARSER_WINDOW
} sqlparser_kind;

typedef enum sqlparser_operator {
  SQLPARSER_OP_NONE, SQLPARSER_OP_OR, SQLPARSER_OP_AND, SQLPARSER_OP_NOT,
  SQLPARSER_OP_EQ, SQLPARSER_OP_NE, SQLPARSER_OP_LT, SQLPARSER_OP_LE,
  SQLPARSER_OP_GT, SQLPARSER_OP_GE, SQLPARSER_OP_NULL_SAFE_EQ,
  SQLPARSER_OP_ADD, SQLPARSER_OP_SUBTRACT, SQLPARSER_OP_MULTIPLY,
  SQLPARSER_OP_DIVIDE, SQLPARSER_OP_MODULO, SQLPARSER_OP_NEGATE,
  SQLPARSER_OP_POSITIVE, SQLPARSER_OP_BIT_AND, SQLPARSER_OP_BIT_OR,
  SQLPARSER_OP_BIT_NOT, SQLPARSER_OP_SHIFT_LEFT, SQLPARSER_OP_SHIFT_RIGHT,
  SQLPARSER_OP_IS_NULL, SQLPARSER_OP_IS_NOT_NULL, SQLPARSER_OP_LIKE,
  SQLPARSER_OP_NOT_LIKE, SQLPARSER_OP_EXISTS, SQLPARSER_OP_CONCAT,
  SQLPARSER_OP_IS, SQLPARSER_OP_IS_NOT, SQLPARSER_OP_GLOB,
  SQLPARSER_OP_NOT_GLOB, SQLPARSER_OP_REGEXP, SQLPARSER_OP_NOT_REGEXP,
  SQLPARSER_OP_MATCH, SQLPARSER_OP_NOT_MATCH, SQLPARSER_OP_XOR
} sqlparser_operator;

typedef enum sqlparser_join_kind {
  SQLPARSER_JOIN_INNER, SQLPARSER_JOIN_LEFT, SQLPARSER_JOIN_RIGHT,
  SQLPARSER_JOIN_CROSS, SQLPARSER_JOIN_NATURAL, SQLPARSER_JOIN_FULL
} sqlparser_join_kind;
typedef enum sqlparser_transaction_kind {
  SQLPARSER_BEGIN, SQLPARSER_START_TRANSACTION, SQLPARSER_COMMIT, SQLPARSER_ROLLBACK,
  SQLPARSER_SAVEPOINT, SQLPARSER_RELEASE, SQLPARSER_ROLLBACK_TO,
  SQLPARSER_SET_TRANSACTION
} sqlparser_transaction_kind;
typedef enum sqlparser_transaction_mode {
  SQLPARSER_TRANSACTION_DEFAULT, SQLPARSER_DEFERRED,
  SQLPARSER_IMMEDIATE, SQLPARSER_EXCLUSIVE
} sqlparser_transaction_mode;

/* Omission inherits server/session defaults and must remain distinct from NO. */
typedef enum sqlparser_choice {
  SQLPARSER_CHOICE_UNSPECIFIED, SQLPARSER_CHOICE_NO, SQLPARSER_CHOICE_YES
} sqlparser_choice;
typedef enum sqlparser_transaction_access {
  SQLPARSER_ACCESS_DEFAULT, SQLPARSER_READ_ONLY, SQLPARSER_READ_WRITE
} sqlparser_transaction_access;
typedef enum sqlparser_isolation {
  SQLPARSER_ISOLATION_DEFAULT, SQLPARSER_READ_UNCOMMITTED, SQLPARSER_READ_COMMITTED,
  SQLPARSER_REPEATABLE_READ, SQLPARSER_SERIALIZABLE
} sqlparser_isolation;
typedef enum sqlparser_conflict {
  SQLPARSER_CONFLICT_DEFAULT, SQLPARSER_CONFLICT_ROLLBACK, SQLPARSER_CONFLICT_ABORT,
  SQLPARSER_CONFLICT_FAIL, SQLPARSER_CONFLICT_IGNORE, SQLPARSER_CONFLICT_REPLACE
} sqlparser_conflict;
typedef enum sqlparser_compound_kind {
  SQLPARSER_COMPOUND_UNION, SQLPARSER_COMPOUND_INTERSECT, SQLPARSER_COMPOUND_EXCEPT
} sqlparser_compound_kind;
typedef enum sqlparser_table_option_kind {
  SQLPARSER_WITHOUT_ROWID, SQLPARSER_STRICT
} sqlparser_table_option_kind;
typedef enum sqlparser_trigger_timing {
  SQLPARSER_TRIGGER_BEFORE, SQLPARSER_TRIGGER_AFTER, SQLPARSER_TRIGGER_INSTEAD_OF
} sqlparser_trigger_timing;
typedef enum sqlparser_trigger_event {
  SQLPARSER_TRIGGER_INSERT, SQLPARSER_TRIGGER_UPDATE, SQLPARSER_TRIGGER_DELETE
} sqlparser_trigger_event;
typedef enum sqlparser_alter_action {
  SQLPARSER_RENAME_TABLE, SQLPARSER_RENAME_COLUMN, SQLPARSER_ADD_COLUMN, SQLPARSER_DROP_COLUMN,
  SQLPARSER_SET_ENGINE, SQLPARSER_SET_COLUMN_DEFAULT, SQLPARSER_DROP_COLUMN_DEFAULT
} sqlparser_alter_action;
typedef enum sqlparser_reference_action {
  SQLPARSER_REFERENCE_DEFAULT, SQLPARSER_REFERENCE_NO_ACTION, SQLPARSER_REFERENCE_RESTRICT,
  SQLPARSER_REFERENCE_CASCADE, SQLPARSER_REFERENCE_SET_NULL, SQLPARSER_REFERENCE_SET_DEFAULT
} sqlparser_reference_action;
typedef struct sqlparser_reference_options {
  sqlparser_reference_action on_delete, on_update;
  sqlparser_id match;
  bool has_deferrable, deferrable, initially_deferred;
} sqlparser_reference_options;
typedef enum sqlparser_scope {
  SQLPARSER_SCOPE_DEFAULT, SQLPARSER_SCOPE_SESSION, SQLPARSER_SCOPE_GLOBAL,
  SQLPARSER_SCOPE_LOCAL
} sqlparser_scope;
typedef enum sqlparser_set_kind {
  SQLPARSER_SET_ASSIGNMENTS, SQLPARSER_SET_NAMES, SQLPARSER_SET_CHARACTER_SET
} sqlparser_set_kind;
typedef enum sqlparser_show_kind {
  SQLPARSER_SHOW_DATABASES, SQLPARSER_SHOW_TABLES, SQLPARSER_SHOW_TABLE_STATUS,
  SQLPARSER_SHOW_VARIABLES, SQLPARSER_SHOW_COLLATION,
  SQLPARSER_SHOW_TRIGGERS, SQLPARSER_SHOW_EVENTS, SQLPARSER_SHOW_OPEN_TABLES,
  SQLPARSER_SHOW_COLUMNS, SQLPARSER_SHOW_INDEX, SQLPARSER_SHOW_STATUS,
  SQLPARSER_SHOW_CHARACTER_SET, SQLPARSER_SHOW_PROCEDURE_STATUS,
  SQLPARSER_SHOW_FUNCTION_STATUS, SQLPARSER_SHOW_CREATE_TABLE
} sqlparser_show_kind;

typedef enum sqlparser_lock_mode {
  SQLPARSER_LOCK_READ, SQLPARSER_LOCK_READ_LOCAL, SQLPARSER_LOCK_WRITE
} sqlparser_lock_mode;
typedef enum sqlparser_view_check {
  SQLPARSER_VIEW_CHECK_NONE, SQLPARSER_VIEW_CHECK_DEFAULT,
  SQLPARSER_VIEW_CHECK_LOCAL, SQLPARSER_VIEW_CHECK_CASCADED
} sqlparser_view_check;
typedef enum sqlparser_explain_format {
  SQLPARSER_EXPLAIN_DEFAULT, SQLPARSER_EXPLAIN_TRADITIONAL,
  SQLPARSER_EXPLAIN_JSON, SQLPARSER_EXPLAIN_TREE
} sqlparser_explain_format;
typedef enum sqlparser_constraint_kind {
  SQLPARSER_PRIMARY_KEY, SQLPARSER_UNIQUE, SQLPARSER_INDEX, SQLPARSER_NOT_NULL,
  SQLPARSER_NULLABLE, SQLPARSER_DEFAULT, SQLPARSER_AUTO_INCREMENT,
  SQLPARSER_REFERENCES, SQLPARSER_CHECK, SQLPARSER_FOREIGN_KEY,
  SQLPARSER_COLUMN_COLLATION, SQLPARSER_DEFERRABILITY,
  SQLPARSER_CONSTRAINT_DECLARATION, SQLPARSER_ON_UPDATE
} sqlparser_constraint_kind;

/* IDs and spans belong to one document. Zero means absent. Lists preserve SQL
 * order through next; next is not an expression child. Only the union member
 * corresponding to kind is valid. All text is raw SQL, including quotes.
 * MODULE_ARGUMENT is an opaque raw span, interpreted by the virtual-table
 * module. Column deferrability is a separate CONSTRAINT/DEFERRABILITY node;
 * table foreign keys store it in constraint.reference. DEFAULT_VALUE is the
 * bare MySQL DEFAULT keyword with no union payload; DEFAULT(column) is CALL. */
typedef struct sqlparser_node {
  sqlparser_kind kind;
  sqlparser_span span;
  sqlparser_id next;
  union {
    struct { size_t parts; } name;
    struct { sqlparser_operator op; sqlparser_id operand; } unary;
    struct { sqlparser_operator op; sqlparser_id left, right, escape; } binary;
    struct { sqlparser_id name; sqlparser_list arguments; bool distinct; } call;
    struct { sqlparser_id value, lower, upper; bool negated; } between;
    struct { sqlparser_id value, query; sqlparser_list items; bool negated; sqlparser_id table; } in;
    struct { sqlparser_id operand, otherwise; sqlparser_list branches; } case_expr;
    struct { sqlparser_id condition, result; } when;
    struct { sqlparser_id query; } subquery;
    struct {
      sqlparser_list columns, group_by, order_by;
      sqlparser_id from, where, having, limit;
      bool distinct;
      bool calc_found_rows;
    } select;
    struct { sqlparser_id left, right; bool all; sqlparser_list order_by; sqlparser_id limit; sqlparser_compound_kind kind; } compound;
    struct { sqlparser_id expression, alias; } projection;
    struct { sqlparser_id name, query, alias; sqlparser_list arguments; sqlparser_id indexed_by, group; bool table_function, not_indexed; } table;
    struct {
      sqlparser_join_kind kind;
      sqlparser_id left, right, condition;
      sqlparser_list using_columns;
      bool natural;
    } join;
    struct { sqlparser_id expression; bool descending; } order;
    struct { sqlparser_id count, offset; } limit;
    struct {
      sqlparser_id table, query;
      sqlparser_list columns, rows, assignments;
      bool replace;
      sqlparser_conflict conflict;
      bool default_values;
      bool low_priority;
    } insert;
    struct { sqlparser_list values; } row;
    struct { sqlparser_id name, value; sqlparser_scope scope; } assignment;
    struct { sqlparser_id table, where, limit; sqlparser_list assignments, order_by; sqlparser_conflict conflict; sqlparser_id indexed_by; bool not_indexed, low_priority; } update;
    struct {
      sqlparser_id table, where, limit;
      sqlparser_list order_by;
      sqlparser_id indexed_by;
      bool not_indexed, low_priority;
      /* Multi-table DELETE has separate target names and FROM join tree. */
      sqlparser_list targets;
      sqlparser_id from;
    } delete_stmt;
    struct {
      sqlparser_transaction_kind kind;
      sqlparser_transaction_mode mode;
      sqlparser_id name;
      sqlparser_choice chain, release;
      sqlparser_transaction_access access;
      sqlparser_isolation isolation;
      sqlparser_scope scope;
      bool consistent_snapshot;
    } transaction;
    struct { sqlparser_set_kind kind; sqlparser_list assignments; sqlparser_id value; } set;
    struct {
      sqlparser_show_kind kind;
      sqlparser_scope scope;
      sqlparser_id database, pattern, where;
      bool full;
      sqlparser_id table;
      bool extended;
    } show;
    struct { sqlparser_id table; sqlparser_list elements; bool temporary, if_not_exists; sqlparser_list options; sqlparser_id query, like_table; } create_table;
    struct { sqlparser_id name, type; sqlparser_list constraints; } column;
    /* Flags retain explicit syntax; ZEROFILL's implied UNSIGNED is not applied. */
    struct { sqlparser_id name; sqlparser_list arguments; bool is_unsigned, zerofill; } type;
    struct {
      sqlparser_constraint_kind kind;
      sqlparser_id name, expression, table;
      sqlparser_list columns, referenced_columns;
      sqlparser_conflict conflict;
      bool descending, autoincrement;
      sqlparser_reference_options reference;
      /* SQLite table keys retain ORDER/COLLATE terms alongside NAME columns. */
      sqlparser_list key_terms;
      /* SQLite CONSTRAINT names immediately preceding this constraint. A
       * trailing group is a CONSTRAINT_DECLARATION, not an enforced constraint.
       * name is the active name (last declaration until a table-constraint
       * separator/new column; the first table constraint inherits the column).
       * COLUMN_COLLATION retains its collation name in name instead. */
      sqlparser_list declarations;
    } constraint;
    struct { sqlparser_list tables; bool temporary, if_exists; } drop_table;
    struct { sqlparser_id expression, type; } cast;
    struct { sqlparser_id expression, name; } collate;
    struct { sqlparser_id name, value; } pragma;
    struct { sqlparser_id name, table, where; sqlparser_list columns; bool unique, if_not_exists; } create_index;
    struct { sqlparser_id name; bool if_exists; } drop_object;
    struct { sqlparser_id name, query; sqlparser_list columns; bool temporary, if_not_exists; sqlparser_view_check check; } create_view;
    struct { sqlparser_id file, schema; } attach;
    struct { sqlparser_list bindings; sqlparser_id body; bool recursive; } with;
    struct { sqlparser_id name, query; sqlparser_list columns; } cte;
    struct { sqlparser_list rows; } values;
    struct { sqlparser_table_option_kind kind; } table_option;
    struct { sqlparser_id name, module; sqlparser_list arguments; bool if_not_exists; } virtual_table;
    struct {
      sqlparser_id name, table, when;
      sqlparser_list columns, steps;
      sqlparser_trigger_timing timing;
      sqlparser_trigger_event event;
      bool temporary, if_not_exists;
    } trigger;
    struct { sqlparser_conflict action; sqlparser_id message; } raise;
    struct { sqlparser_id statement; bool query_plan; sqlparser_explain_format format; } explain;
    struct { sqlparser_id target, into; } maintenance;
    struct { sqlparser_alter_action action; sqlparser_id table, column, new_name, value; } alter;
    /* MySQL parenthesized query: outer clauses never overwrite query's tail. */
    struct { sqlparser_id query; sqlparser_list order_by; sqlparser_id limit; } query_group;
    /* SQL-level prepared statements retain source text/variables without
     * recursively parsing or executing their contents. */
    struct { sqlparser_id name, source; sqlparser_list parameters; } prepared;
    struct { sqlparser_list targets; } lock_tables;
    struct { sqlparser_id table, alias; sqlparser_lock_mode mode; } lock_target;
    struct { sqlparser_id call, name; sqlparser_list partition_by, order_by; } window;
  } as;
} sqlparser_node;

/* Defaults: 1 MiB SQL, 65536 nodes, 1024 statements, 4096 parser stack entries. */
sqlparser_limits sqlparser_default_limits(void);

/* Reports this library's build option, independently of any SQLite engine.
 * No parameters, ownership or failure state. Triggers always forbid the extension. */
bool sqlparser_sqlite_update_delete_limit_enabled(void);

/* Parse exactly length bytes, without requiring a NUL terminator. Embedded NUL
 * is a syntax error. limits==NULL selects defaults; every explicit limit must
 * be nonzero; max_stack_entries must be in [2, 1048576]. out must point to a
 * NULL document. An occupied output is rejected and left unchanged. Success publishes an owned,
 * immutable document; failure leaves *out NULL. error is optional and contains
 * a byte offset (length for EOF), status and a bounded diagnostic.
 * Each call has private state and may run concurrently with other calls. */
sqlparser_status sqlparser_parse(const char *sql, size_t length,
    const sqlparser_limits *limits, sqlparser_document **out, sqlparser_error *error);

/* Same ownership/error/limit contract as sqlparser_parse. The legacy entry uses
 * MYSQL. Dialect is explicit, fixed for the entire batch, never auto-detected.
 * SQLITE selects the documented SQLite subset, not database semantic checks.
 * Unknown dialect values return INVALID_ARGUMENT. */
sqlparser_status sqlparser_parse_dialect(const char *sql, size_t length,
    sqlparser_dialect dialect, const sqlparser_limits *limits,
    sqlparser_document **out, sqlparser_error *error);
/* The immutable document retains its dialect; NULL yields DIALECT_UNKNOWN. */
sqlparser_dialect sqlparser_get_dialect(const sqlparser_document *document);

/* NULL is accepted. Requires all document readers to have finished. */
void sqlparser_document_destroy(sqlparser_document *document);
/* NULL document yields an empty list/count or NULL. Returned nodes/text borrow
 * the immutable document and remain valid until destroy; concurrent reads are
 * allowed after the caller safely publishes the document. Invalid IDs/spans
 * return NULL. Text is not NUL-terminated at span.length. */
sqlparser_list sqlparser_statements(const sqlparser_document *document);
size_t sqlparser_node_count(const sqlparser_document *document);
const sqlparser_node *sqlparser_get_node(const sqlparser_document *document, sqlparser_id id);
const char *sqlparser_text(const sqlparser_document *document, sqlparser_span span);

#ifdef __cplusplus
}
#endif
#endif

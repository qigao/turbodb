#ifndef ORM_TIDESDB_SQL_INSERT_H
#define ORM_TIDESDB_SQL_INSERT_H
#include "catalog_store.h"
#include "diagnostics.h"
#include "binding.h"

/* Private type-only INSERT/REPLACE VALUES/SET/SELECT validation, including IGNORE and
 * duplicate-key assignments/aliases. Same borrowed input, ready/read-only
 * owner, budget, cleanup and no-side-effect contract as change_bind.
 * Checks columns/default presence, row shape, expressions and assignment
 * compatibility; value-dependent conversions/constraints belong to execute.
 * SELECT/compound/query-group inputs, including nonrecursive WITH, derived,
 * LATERAL and expression-query dependencies, validate columns without row reads.
 * Recursion and query expressions in VALUES/SET/duplicate assignments reject,
 * matching the existing execution subset.
 * No public prepared handle or unknown-parameter type inference here.
 * Usage: active budget + Catalog owner; scope={document,statement,types,count,
 * depth,budget}; insert_bind(&scope,owner,error); destroy document; end budget.
 * Executable examples and failure matrix: ../tests/integration/runtime_test.c. */
turbodb_status_t orm_sql_insert_bind(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *store, turbodb_error_t *error);

/* Private synchronous INSERT VALUES/SET/SELECT execution in the owner's transaction.
 * Borrows a parsed MySQL document and immutable parameters through return.
 * One statement; target columns may be omitted, partial, empty, or reordered.
 * Omitted nullable columns receive NULL; explicit schema defaults are reused;
 * a missing NOT NULL default is SQL_ERROR before any write.
 * Parameters follow source order. Numeric columns use MySQL assignment
 * conversion; IGNORE additionally substitutes/clips invalid values and records
 * bounded diagnostics.
 * Supports expr.h scalar expressions without column references and bare
 * DEFAULT in VALUES/SET/duplicate-key assignments. REPLACE follows MySQL
 * delete-conflicts-then-insert semantics. Priority modifiers are not accepted.
 * INSERT SELECT supports the
 * runtime SELECT/UNION/query-group/nonrecursive-WITH subset and materializes a
 * bounded snapshot before closing all sources and starting writes. MySQL IGNORE
 * skips primary/unique conflicts; ON DUPLICATE KEY UPDATE supports current-row
 * expressions, deprecated VALUES(column), and VALUES/SET row/column aliases,
 * including IGNORE for update key conflicts. Alias references map to the
 * immutable candidate row; unqualified target-column names keep current-row
 * meaning.
 * max_depth > 0; all fixed workspace and materialized rows use owner's budget.
 * Plain VALUES/SET is one atomic batch. SELECT and duplicate-key candidates
 * execute in source order inside an outer statement savepoint. Failure preserves affected;
 * success reports MySQL default affected rows. client_found_rows changes a
 * no-op duplicate-key update from 0 to 1; it does not commit the transaction.
 * Single owner, no live sources; caller must finish/rollback the owner later.
 * Example with an existing catalog table t(id BIGINT PRIMARY KEY): parse
 * "INSERT INTO t(id) VALUES (1),(2)", execute(doc, owner, NULL, 0, 32, 0,
 * false, NULL, &n, error),
 * destroy doc, then catalog_finish(owner, true, error). */
turbodb_status_t orm_tidesdb_sql_insert_execute(const sqlparser_document *document,
    orm_sql_catalog_store *store, const turbodb_value_t *parameters, size_t parameter_count,
    size_t max_depth, uint64_t max_iterations, bool client_found_rows,
    orm_sql_diagnostics *diagnostics, size_t *affected,
    turbodb_error_t *error);
/* Context-aware form; copies session and receiver, selects WRITE/IGNORE_WRITE
 * from the statement. Other ownership/failure contracts are unchanged. */
turbodb_status_t orm_sql_insert_execute_evaluation(const sqlparser_document *document,
    orm_sql_catalog_store *store, const turbodb_value_t *parameters, size_t parameter_count,
    size_t max_depth, uint64_t max_iterations, bool client_found_rows,
    orm_sql_evaluation evaluation, size_t *affected, turbodb_error_t *error);
#endif

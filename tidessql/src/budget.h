#ifndef ORM_TIDESDB_SQL_BUDGET_H
#define ORM_TIDESDB_SQL_BUDGET_H

#include "error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Private relational-engine contract; not installed.
 * Fixed-size counters only: no allocator, native transaction or data ownership.
 * One synchronous owner; no concurrent calls. Fields are read-only to callers
 * after init. Keep this ledger alive through statement cleanup and the last
 * retained owner's release. */
typedef enum orm_sql_budget_resource {
  ORM_SQL_BUDGET_WORK_BYTES,
  ORM_SQL_BUDGET_MATERIALIZED_ROWS,
  ORM_SQL_BUDGET_GROUPS,
  ORM_SQL_BUDGET_AST_NODES,
  ORM_SQL_BUDGET_PLAN_NODES,
  ORM_SQL_BUDGET_JOIN_PAIRS,
  ORM_SQL_BUDGET_EXECUTION_STEPS,
  ORM_SQL_BUDGET_WRITE_ROWS,
  ORM_SQL_BUDGET_WRITE_BYTES,
  ORM_SQL_BUDGET_READ_ROWS,
  ORM_SQL_BUDGET_READ_BYTES,
  ORM_SQL_BUDGET_RESOURCE_COUNT
} orm_sql_budget_resource;

typedef struct orm_sql_budget_amount {
  uint64_t value[ORM_SQL_BUDGET_RESOURCE_COUNT];
} orm_sql_budget_amount;

typedef struct orm_sql_transaction_budget_amount {
  uint64_t read_rows;
  uint64_t read_bytes;
  uint64_t write_bytes;
} orm_sql_transaction_budget_amount;

typedef struct orm_sql_budget_limits {
  orm_sql_budget_amount statement;
  orm_sql_transaction_budget_amount transaction;
} orm_sql_budget_limits;

typedef struct orm_tidesdb_sql_budget {
  orm_sql_budget_limits limits;
  orm_sql_budget_amount used;
  orm_sql_budget_amount peak;
  orm_sql_transaction_budget_amount transaction_used;
  uint64_t retained_work_bytes; /* Subset of used WORK_BYTES; survives end/begin. */
  bool statement_active;
} orm_tidesdb_sql_budget;

/* Initialize unused/quiescent storage. Every limit must be positive, work bytes
 * must fit size_t, transaction limits must cover the corresponding statement
 * limits. INVALID_ARGUMENT leaves *out unchanged. No defaults are implied. */
turbodb_status_t orm_tidesdb_sql_budget_init(orm_tidesdb_sql_budget *out,
    const orm_sql_budget_limits *limits, turbodb_error_t *error);

/* Begin clears statement usage/peaks except retained WORK_BYTES, which becomes
 * the new work/peak baseline; a second begin returns BUSY.
 * End requires released ordinary work, materialized rows and groups (else BUSY).
 * Counters remain observable until the next begin. No active statement gives
 * INVALID_STATE. Errors/cancellation use the same release + end sequence. */
turbodb_status_t orm_tidesdb_sql_budget_begin(orm_tidesdb_sql_budget *budget,
    turbodb_error_t *error);
turbodb_status_t orm_tidesdb_sql_budget_end(orm_tidesdb_sql_budget *budget,
    turbodb_error_t *error);

/* Call before work/read/write. All dimensions are charged atomically, including
 * cumulative transaction reads/writes. LIMIT_EXCEEDED leaves all counters intact.
 * Reads include rejected scan candidates; writes include catalog/index encoding.
 * Cumulative charges are conservative and never refunded on statement failure or
 * savepoint rollback. Zero amounts are allowed. Complexity and storage are O(1). */
turbodb_status_t orm_tidesdb_sql_budget_reserve(orm_tidesdb_sql_budget *budget,
    const orm_sql_budget_amount *amount, turbodb_error_t *error);

/* Reserve capacity * element_bytes + overhead_bytes as WORK_BYTES, checked in
 * size_t. element_bytes must be nonzero. *reserved is changed only on success.
 * Growth: reserve the full new allocation while the old one remains charged;
 * free old storage then release its charge. Failed allocation releases only the
 * new charge. This conservative rule covers realloc's possible copy peak too. */
turbodb_status_t orm_tidesdb_sql_budget_reserve_capacity(orm_tidesdb_sql_budget *budget,
    size_t capacity, size_t element_bytes, size_t overhead_bytes,
    size_t *reserved, turbodb_error_t *error);

/* Owner-only fixed metadata that outlives one statement. Same checked capacity
 * and shared WORK_BYTES limit as reserve_capacity; active statement required.
 * Retained bytes are a subset of charged WORK_BYTES, never an extra allowance.
 * On success caller owns one receipt (*reserved), released exactly once after
 * the retained object's native resources end. Failure preserves all outputs.
 * Example: begin budget, reserve_retained_capacity for a native owner, execute,
 * end/begin budget for later statements, finish native owner, release_retained.
 * Do not use for query/schema/row buffers that must end with the statement. */
turbodb_status_t orm_tidesdb_sql_budget_reserve_retained_capacity(orm_tidesdb_sql_budget *budget,
    size_t capacity, size_t element_bytes, size_t overhead_bytes,
    size_t *reserved, turbodb_error_t *error);
/* Active or inactive statement allowed. Subtracts from retained and used work;
 * leaves peaks and cumulative transaction counters unchanged. Amount must be
 * owned retained bytes (INVALID_ARGUMENT otherwise, with no state changes). */
turbodb_status_t orm_tidesdb_sql_budget_release_retained(orm_tidesdb_sql_budget *budget,
    uint64_t amount, turbodb_error_t *error);

/* Only WORK_BYTES, MATERIALIZED_ROWS and GROUPS can be released after actual
 * cleanup (or an unused reservation). Invalid resource/underflow returns
 * INVALID_ARGUMENT without mutation. Ordinary WORK release cannot consume
 * retained bytes. Peaks count reservations, not actual RSS. */
turbodb_status_t orm_tidesdb_sql_budget_release(orm_tidesdb_sql_budget *budget,
    orm_sql_budget_resource resource, uint64_t amount, turbodb_error_t *error);

/* Requires no active statement or retained work (else BUSY), and the owner must
 * have already ended/freed the native transaction. Resets cumulative accounting only; does
 * not commit, rollback or reopen a native transaction or clear owner errors. */
turbodb_status_t orm_tidesdb_sql_budget_reset_transaction(
    orm_tidesdb_sql_budget *budget, turbodb_error_t *error);

/* All calls except init require a successfully initialized ledger. Null required
 * pointers return INVALID_ARGUMENT; reserve and ordinary release require an
 * active statement. release_retained is also allowed between statements.
 * error is optional and uses the existing ORM error contract. */
#endif

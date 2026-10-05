#ifndef ORM_TIDESDB_SQL_CTE_STORE_H
#define ORM_TIDESDB_SQL_CTE_STORE_H
#include "scan.h"

typedef enum orm_sql_cte_state {
  ORM_SQL_CTE_PENDING, ORM_SQL_CTE_READY, ORM_SQL_CTE_FAILED, ORM_SQL_CTE_CANCELLED
} orm_sql_cte_state;

/* Internal compiled recursive member factory. All calls are synchronous.
 * open borrows frontier through close and publishes a stable producer on success.
 * close must release every frontier consumer, including after partial open.
 * A failed close retains ownership and must be retryable; the store keeps the
 * first execution error. context and callbacks outlive the store. */
typedef struct orm_sql_cte_recursion {
  void *context;
  turbodb_status_t (*open)(void *context, orm_sql_row_source *frontier,
      orm_sql_row_source **producer, turbodb_error_t *error);
  turbodb_status_t (*close)(void *context, turbodb_error_t *error);
  uint64_t max_iterations;
  bool distinct;
  /* Explicit page; zero-initialized descriptors retain unlimited generation.
   * Offset rows remain recursion inputs, but are hidden from external readers. */
  bool paged;
  uint64_t offset,limit;
} orm_sql_cte_recursion;

/* Private single-threaded materialized CTE result. Stable address, zero before
 * open. Borrows its producer until close; owns declared types and deep row copies.
 * First reader pull materializes completely, then publishes an immutable cache.
 * Failure never exposes partial rows and is shared by all readers. Work, rows and
 * steps are bounded by the producer's budget; no spill or implicit retry.
 * No WITH admission here: the SQL owner supplies a fully bound query source. */
typedef struct orm_sql_cte_store {
  orm_tidesdb_sql_budget *budget;
  orm_sql_scan input;
  orm_sql_rows rows;
  vec_t types;
  size_t type_bytes, metadata_bytes, readers;
  orm_sql_cte_state state;
  turbodb_error_t failure;
  bool evaluating;
  orm_sql_cte_recursion recursion;
  orm_sql_row_source frontier;
  orm_sql_scan iteration;
  vec_t index;
  size_t index_bytes, frontier_first, frontier_position, frontier_end, frontier_readers;
  uint64_t iterations;
  bool round_open;
} orm_sql_cte_store;

/* One reader per FROM occurrence; positions are independent. Its source and
 * borrowed types stay stable through consumer close. Returned rows remain valid
 * until store close; callers should observe the narrower row-source contract.
 * Reader open/close holds a store lease; Scan/JOIN holds source.active. */
typedef struct orm_sql_cte_reader {
  orm_sql_cte_store *store;
  orm_sql_row_source source;
  size_t position, metadata_bytes;
} orm_sql_cte_reader;

/* Private per-member cursor over one immutable iteration range, no row copies.
 * Stable and zero before open. Source/types borrow the recursive store. */
typedef struct orm_sql_cte_frontier_reader {
  orm_sql_cte_store *store;
  orm_sql_row_source source;
  size_t position, end, metadata_bytes;
  uint64_t iteration;
} orm_sql_cte_frontier_reader;
/* Fork the factory's frontier; each reader starts at this round's first row,
 * even if the default frontier or another reader has advanced. Only open/read
 * during the current factory round. Fixed metadata/steps charged, no row reads
 * during open. Close consumers then readers before factory.close returns OK;
 * leaked readers prevent round/store release. Close after failed evaluation is
 * allowed, active consumer -> BUSY. Cached rows borrow until store close.
 * O(1) open/next/close, no allocation or second frontier cache. */
turbodb_status_t orm_sql_cte_frontier_open(orm_sql_row_source *frontier,
    orm_sql_cte_frontier_reader *out,turbodb_error_t *error);
/* Private O(1) reset of this reader to the current immutable frontier's first
 * row. Same iteration only; close its consumer first (BUSY otherwise). Charges
 * one step without allocating, changing its lease or advancing the store.
 * An inactive/stale reader returns INVALID_STATE; quota errors leave position
 * unchanged. Source address/types and cached rows keep their existing lifetime. */
turbodb_status_t orm_sql_cte_frontier_rewind(orm_sql_cte_frontier_reader *reader,turbodb_error_t *error);
turbodb_status_t orm_sql_cte_frontier_close(orm_sql_cte_frontier_reader *reader,turbodb_error_t *error);

/* Opens a typed identity Scan without reading any rows. Occupied output and
 * source leases are unchanged on rejection; partial failure refunds all work.
 * O(columns) construction, O(rows*columns + payload bytes) materialization. */
turbodb_status_t orm_sql_cte_store_open(orm_sql_row_source *source,
    orm_sql_cte_store *out, turbodb_error_t *error);
/* Private recursive operator, not SQL admission. Materializes seed then invokes
 * the factory on each nonempty previous delta until no new rows remain. Output
 * kinds come only from seed and are nullable; member kinds must match or be NULL.
 * max_iterations is positive and counts factory opens, including the final empty
 * iteration. No partial result on exhaustion. DISTINCT matches Scan's supported
 * numeric/BOOL/NULL tuple rules, deduplicates seed and all subsequent rounds.
 * A page stops accepting rows at offset+limit without overflowing that sum;
 * limit zero pulls no seed/member. No extra probe/round after reaching the cap.
 * Blocking upstream operators may prepare their input before yielding a row.
 * Same leases/cleanup as ordinary store; never evaluates during construction.
 * ALL uses O(total row/payload bytes) storage. DISTINCT adds a derived O(N) index
 * rebuilt each round: O(columns * sum(N_i log N_i)) charged comparison steps.
 * Example: open_recursive(seed,factory), reader_open, consume, reader_close,
 * store_close; factory.close closes its Scan/SELECT run before returning OK. */
turbodb_status_t orm_sql_cte_store_open_recursive(orm_sql_row_source *seed,
    const orm_sql_cte_recursion *recursion, orm_sql_cte_store *out, turbodb_error_t *error);
/* No data reads. A late reader starts at row zero without re-executing READY
 * input. CLOSED/CANCELLED -> INVALID_STATE, FAILED -> original error. */
turbodb_status_t orm_sql_cte_reader_open(orm_sql_cte_store *store,
    orm_sql_cte_reader *out, turbodb_error_t *error);
/* NULL when closed. Pull gives a row or NULL for EOF, preserves output on error.
 * Reentrant reads are BUSY; cache reads are O(1) and charge one execution step. */
orm_sql_row_source *orm_sql_cte_reader_source(orm_sql_cte_reader *reader);
/* Private plan reuse after consumer close. Rewinds only this reader, retaining
 * its lease, stable source/types and the shared cache; no producer execution or
 * allocation. Charges one execution step, preserves cumulative quotas. PENDING
 * remains lazy, READY is replayed. Active consumer/evaluating -> BUSY, closed or
 * cancelled -> INVALID_STATE, failed store -> original error. Failure preserves
 * position and store state. Cached row views remain valid through store close. */
turbodb_status_t orm_sql_cte_reader_rewind(orm_sql_cte_reader *reader, turbodb_error_t *error);
/* Cancels all readers without releasing rows. Existing failures retain their
 * first cause. No input pull; evaluating -> BUSY without state change. */
turbodb_status_t orm_sql_cte_store_cancel(orm_sql_cte_store *store, turbodb_error_t *error);
/* Close consumers, readers, store, then producer. Active consumers/readers or
 * reentrant close -> BUSY unchanged. NULL/empty close is inert. */
turbodb_status_t orm_sql_cte_reader_close(orm_sql_cte_reader *reader, turbodb_error_t *error);
turbodb_status_t orm_sql_cte_store_close(orm_sql_cte_store *store, turbodb_error_t *error);
#endif

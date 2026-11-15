#include "backend.h"
#include "orm_async_wait.h"

#include <cflow/executor.h>
#include <stdlib.h>

enum { ORM_SQLITE_ASYNC_QUEUE_CAPACITY = 1, ORM_SQLITE_PROGRESS_INTERVAL = 1000 };

typedef struct orm_sqlite_async_cursor {
  orm_sqlite_backend_state *owner;
  const orm_query_plan *plan; /* Retained by the query/plugin cursor wrapper. */
  orm_limits limits;
  cflow_executor executor;
  orm_async_wait wait;
  orm_row_cursor native;
  cserde_reader row;
  orm_row_cursor_step step;
  orm_error_t error;
  atomic_int ready;
  atomic_int cancelled;
  int pending;
  int terminal;
} orm_sqlite_async_cursor;

static int orm_sqlite_async_interrupted(void *context) {
  orm_sqlite_async_cursor *cursor = context;
  return atomic_load(&cursor->cancelled);
}

/* Only this serial worker advances the statement. Release/acquire hands the
 * row reader to the caller; no further task is admitted until its next next().
 * SQLite column views therefore remain stable through synchronous decoding. */
static void orm_sqlite_async_run(void *context) {
  orm_sqlite_async_cursor *cursor = context;
  cursor->step = (orm_row_cursor_step)ORM_ROW_CURSOR_STEP_INIT;
  if (!atomic_load(&cursor->cancelled)) {
    if (cursor->native.ops == NULL) {
      sqlite3_progress_handler(cursor->owner->database,
          ORM_SQLITE_PROGRESS_INTERVAL, orm_sqlite_async_interrupted, cursor);
      const orm_status_t status = orm_sqlite_backend_open_rows(
          cursor->owner, cursor->plan, &cursor->limits,
          &cursor->native, &cursor->error);
      if (status != ORM_STATUS_OK) {
        cursor->step.kind = ORM_ROW_CURSOR_ERROR;
        cursor->step.status = status;
        cursor->step.message = cursor->error.message;
      }
    }
    if (cursor->native.ops != NULL && !atomic_load(&cursor->cancelled)) {
      cursor->row = (cserde_reader){0};
      cursor->step = cursor->native.ops->next(cursor->native.context, &cursor->row);
    }
  }
  atomic_store_explicit(&cursor->ready, 1, memory_order_release);
}

static void orm_sqlite_async_cancel(void *context) {
  orm_sqlite_async_cursor *cursor = context;
  if (atomic_exchange(&cursor->cancelled, 1)) return;
  cursor->terminal = 1;
  /* SQLite explicitly allows interrupt from another thread. The progress
   * handler also observes cancellation that precedes prepare/step entry. */
  sqlite3_interrupt(cursor->owner->database);
  orm_async_wait_destroy(&cursor->wait);
}

static orm_row_cursor_step orm_sqlite_async_next(void *context,
                                                cserde_reader *out_row) {
  orm_sqlite_async_cursor *cursor = context;
  orm_row_cursor_step result = ORM_ROW_CURSOR_STEP_INIT;
  if (cursor->terminal) return result;
  if (orm_async_wait_expired(&cursor->wait)) {
    orm_sqlite_async_cancel(cursor);
    result.kind = ORM_ROW_CURSOR_ERROR;
    /* Query failure leaves the connection usable after cursor disposal. */
    result.status = ORM_STATUS_SQL_ERROR;
    result.message = "SQLite async query deadline expired";
    return result;
  }
  if (cursor->pending &&
      atomic_load_explicit(&cursor->ready, memory_order_acquire)) {
    cursor->pending = 0;
    result = cursor->step;
    if (result.kind == ORM_ROW_CURSOR_ROW) *out_row = cursor->row;
    else cursor->terminal = 1;
    return result;
  }
  if (!cursor->pending) {
    atomic_store(&cursor->ready, 0);
    const cflow_admission_status status = cflow_executor_try_post(
        &cursor->executor, orm_sqlite_async_run, cursor);
    if (status != CFLOW_ADMISSION_ACCEPTED) {
      cursor->terminal = 1;
      result.kind = ORM_ROW_CURSOR_ERROR;
      result.status = status == CFLOW_ADMISSION_FULL ? ORM_STATUS_BUSY
                                                   : ORM_STATUS_INTERNAL_ERROR;
      result.message = "admit SQLite async worker task";
      return result;
    }
    cursor->pending = 1;
  }
  result = orm_async_wait_step(&cursor->wait);
  if (result.kind == ORM_ROW_CURSOR_ERROR) {
    if (result.status == ORM_STATUS_CONNECTION_ERROR)
      result.status = ORM_STATUS_SQL_ERROR;
    orm_sqlite_async_cancel(cursor);
  }
  return result;
}

static void orm_sqlite_async_destroy(void *context) {
  orm_sqlite_async_cursor *cursor = context;
  if (cursor == NULL) return;
  orm_sqlite_async_cancel(cursor);
  /* Control-plane join: borrowed plans, rows and database must outlive the
   * worker, even when cancellation cannot immediately interrupt VFS I/O. */
  cflow_executor_destroy(&cursor->executor);
  sqlite3_progress_handler(cursor->owner->database, 0, NULL, NULL);
  if (cursor->native.ops != NULL)
    cursor->native.ops->destroy(cursor->native.context);
  cursor->owner->async_active = 0;
  free(cursor);
}

static const orm_row_cursor_ops orm_sqlite_async_ops = {
    sizeof(orm_row_cursor_ops), ORM_ROW_CURSOR_OPS_ABI_VERSION,
    "sqlite-async", orm_sqlite_async_next, orm_sqlite_async_cancel,
    orm_sqlite_async_destroy, NULL, NULL};

orm_status_t orm_sqlite_backend_open_async(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    const orm_async_config_t *config, orm_row_cursor *out, orm_error_t *error) {
  orm_sqlite_backend_state *owner = context;
  if (owner == NULL || plan == NULL || limits == NULL || config == NULL ||
      config->struct_size != sizeof(*config) || out == NULL ||
      out->ops != NULL || out->context != NULL ||
      !cflow_scheduler_valid(config->scheduler) ||
      !cflow_scheduler_has(config->scheduler, CMETA_SCHED_CAP_DELAYED) ||
      cflow_scheduler_has(config->scheduler, CMETA_SCHED_CAP_CONCURRENT) ||
      config->poll_interval_ticks == 0u || config->timeout_ticks == 0u) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT, "invalid SQLite async request");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  if (owner->async_active || owner->transaction_active ||
      atomic_load(&owner->cursor_count) != 0u) {
    orm_error_set(error, ORM_STATUS_BUSY, "SQLite connection has an active cursor or transaction");
    return ORM_STATUS_BUSY;
  }
  if (sqlite3_threadsafe() == 0 || sqlite3_db_mutex(owner->database) == NULL) {
    orm_error_set(error, ORM_STATUS_UNSUPPORTED, "SQLite async requires serialized threading support");
    return ORM_STATUS_UNSUPPORTED;
  }
  orm_sqlite_async_cursor *cursor = calloc(1u, sizeof(*cursor));
  if (cursor == NULL) {
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY, "allocate SQLite async cursor");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  cursor->owner = owner;
  cursor->plan = plan;
  cursor->limits = *limits;
  atomic_init(&cursor->ready, 0);
  atomic_init(&cursor->cancelled, 0);
  orm_error_init(&cursor->error);
  if (!orm_async_wait_init(&cursor->wait, config) ||
      !cflow_executor_serial_init_with_capacity(
          &cursor->executor, ORM_SQLITE_ASYNC_QUEUE_CAPACITY)) {
    orm_async_wait_destroy(&cursor->wait);
    free(cursor);
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY, "initialize SQLite async executor");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  owner->async_active = 1;
  out->ops = &orm_sqlite_async_ops;
  out->context = cursor;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

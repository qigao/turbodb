#include <libpq-fe.h>
#include <tinymock.h>

TINYMOCk_MOCK(int, test_consume, PGconn *)
TINYMOCk_MOCK(int, test_flush, PGconn *)
TINYMOCk_MOCK(int, test_busy, PGconn *)
TINYMOCk_MOCK(int, test_nonblocking, PGconn *, int)
TINYMOCk_MOCK_VOID(test_finish, PGconn *)

#define PQconsumeInput test_consume
#define PQflush test_flush
#define PQisBusy test_busy
#define PQsetnonblocking test_nonblocking
#define PQfinish test_finish
#include "../../../../drivers/postgresql/backend.c"
#undef PQconsumeInput
#undef PQflush
#undef PQisBusy
#undef PQsetnonblocking
#undef PQfinish

spec("PostgreSQL native nonblocking progress") {
  (void)ttest_config__;
  static cflow_scheduler scheduler;
  static orm_postgres_backend_state owner;
  static orm_postgres_async_state *query;
  before_each() {
    scheduler = (cflow_scheduler){0};
    check_true(cflow_scheduler_test_init(&scheduler));
    owner = (orm_postgres_backend_state){0};
    owner.connection = (PGconn *)&owner;
    owner.async_active = 1;
    query = calloc(1u, sizeof(*query));
    check_not_null(query);
    query->owner = &owner;
    const orm_async_config_t config = {sizeof(config), &scheduler, 2u, 10u};
    check_true(orm_async_wait_init(&query->wait, &config));
    mock_test_consume_set_default_return(TINYMOCk_RETURN(1));
    mock_test_flush_set_default_return(TINYMOCk_RETURN(0));
    mock_test_busy_set_default_return(TINYMOCk_RETURN(0));
    mock_test_nonblocking_set_default_return(TINYMOCk_RETURN(0));
    mock_test_finish_reset();
  }
  after_each() {
    orm_postgres_async_release(query);
    check_equal(owner.async_active, 0);
    cflow_scheduler_destroy(&scheduler);
    mock_test_consume_verify();
    mock_test_flush_verify();
    mock_test_busy_verify();
    mock_test_nonblocking_verify();
    mock_test_finish_verify();
  }
  it("waits while output is pending even when no input is busy") {
    mock_test_finish_expect(TINYMOCk_ARG(owner.connection));
    mock_test_flush_set_default_return(TINYMOCk_RETURN(1));
    orm_row_cursor_step step = orm_postgres_async_poll(query);
    check_equal(step.kind, ORM_ROW_CURSOR_WAIT);
    check_true(cflow_waitable_valid(&step.waitable));
    tinymock_mock_verify_times(&tinymock_test_consume, 1u);
    tinymock_mock_verify_times(&tinymock_test_flush, 1u);
    orm_postgres_async_abort(query);
    check_null(owner.connection);
    tinymock_mock_verify_times(&tinymock_test_finish, 1u);
  }
  it("waits on incomplete input and exposes results only after libpq is ready") {
    mock_test_busy_expect(TINYMOCk_ANY, TINYMOCk_RETURN(1));
    mock_test_busy_expect(TINYMOCk_ANY, TINYMOCk_RETURN(0));
    check_equal(orm_postgres_async_poll(query).kind, ORM_ROW_CURSOR_WAIT);
    (void)cflow_scheduler_advance(&scheduler, 2u);
    check_equal(orm_postgres_async_poll(query).kind, ORM_ROW_CURSOR_DONE);
    tinymock_mock_verify_never(&tinymock_test_finish);
  }
  it("reports receive failure before attempting flush") {
    mock_test_finish_expect(TINYMOCk_ARG(owner.connection));
    mock_test_consume_set_default_return(TINYMOCk_RETURN(0));
    orm_row_cursor_step step = orm_postgres_async_poll(query);
    check_equal(step.kind, ORM_ROW_CURSOR_ERROR);
    check_equal(step.status, ORM_STATUS_CONNECTION_ERROR);
    tinymock_mock_verify_never(&tinymock_test_flush);
    orm_postgres_async_abort(query);
  }
  it("reports flush failure and closes an unfinished connection exactly once") {
    mock_test_finish_expect(TINYMOCk_ARG(owner.connection));
    mock_test_flush_set_default_return(TINYMOCk_RETURN(-1));
    check_equal(orm_postgres_async_poll(query).kind, ORM_ROW_CURSOR_ERROR);
    orm_postgres_async_abort(query);
    orm_postgres_async_abort(query);
    tinymock_mock_verify_times(&tinymock_test_finish, 1u);
  }
  it("expires without calling libpq again") {
    mock_test_finish_expect(TINYMOCk_ARG(owner.connection));
    (void)cflow_scheduler_advance(&scheduler, 10u);
    check_equal(orm_postgres_async_poll(query).kind, ORM_ROW_CURSOR_ERROR);
    tinymock_mock_verify_never(&tinymock_test_consume);
    orm_postgres_async_abort(query);
  }
  it("rejects overlapping cursors before changing native connection mode") {
    owner.async_active = 0;
    owner.cursor_active = 1;
    orm_error_t error;
    orm_error_init(&error);
    orm_row_cursor cursor = {0};
    check_equal(orm_postgres_backend_open_async(&owner, NULL, NULL, &query->wait.config, &cursor, &error), ORM_STATUS_BUSY);
    tinymock_mock_verify_never(&tinymock_test_nonblocking);
    check_null(cursor.context);
  }
}

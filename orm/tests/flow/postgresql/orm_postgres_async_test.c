#include <libpq-fe.h>
#define TINYMOCK_GENERATE_FUNCTION_OVERRIDES 1
#include <tinymock.h>

FunctionDecl(value, int, test_consume,
    (void *, connection, CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
     &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER));
FunctionDecl(value, int, test_flush,
    (void *, connection, CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
     &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER));
FunctionDecl(value, int, test_busy,
    (void *, connection, CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
     &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER));
FunctionDecl(value, int, test_nonblocking,
    (void *, connection, CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
     &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER),
    (int, enabled, CMETA_PARAM_IN));
FunctionDecl(value, void, test_finish,
    (void *, connection, CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
     &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER));
TINYMOCk_FUNCTION_DECLARE(test_consume);
TINYMOCk_FUNCTION_DECLARE(test_flush);
TINYMOCk_FUNCTION_DECLARE(test_busy);
TINYMOCk_FUNCTION_DECLARE(test_nonblocking);
TINYMOCk_FUNCTION_DECLARE(test_finish);

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
    int success = 1, ready = 0;
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
    TINYMOCk_FUNCTION_RESET(test_consume);
    TINYMOCk_FUNCTION_RESET(test_flush);
    TINYMOCk_FUNCTION_RESET(test_busy);
    TINYMOCk_FUNCTION_RESET(test_nonblocking);
    TINYMOCk_FUNCTION_RESET(test_finish);
    check_true(TINYMOCk_FUNCTION_SET_RETURN(test_consume, success));
    check_true(TINYMOCk_FUNCTION_SET_RETURN(test_flush, ready));
    check_true(TINYMOCk_FUNCTION_SET_RETURN(test_busy, ready));
    check_true(TINYMOCk_FUNCTION_SET_RETURN(test_nonblocking, ready));
  }
  after_each() {
    orm_postgres_async_release(query);
    check_equal(owner.async_active, 0);
    cflow_scheduler_destroy(&scheduler);
    TINYMOCk_FUNCTION_DESTROY(test_consume);
    TINYMOCk_FUNCTION_DESTROY(test_flush);
    TINYMOCk_FUNCTION_DESTROY(test_busy);
    TINYMOCk_FUNCTION_DESTROY(test_nonblocking);
    TINYMOCk_FUNCTION_DESTROY(test_finish);
  }
  it("waits while output is pending even when no input is busy") {
    int pending = 1;
    check_true(TINYMOCk_FUNCTION_SET_RETURN(test_flush, pending));
    orm_row_cursor_step step = orm_postgres_async_poll(query);
    check_equal(step.kind, ORM_ROW_CURSOR_WAIT);
    check_true(cflow_waitable_valid(&step.waitable));
    TINYMOCk_FUNCTION_VERIFY_TIMES(test_consume, 1u);
    TINYMOCk_FUNCTION_VERIFY_TIMES(test_flush, 1u);
    orm_postgres_async_abort(query);
    check_null(owner.connection);
    TINYMOCk_FUNCTION_VERIFY_TIMES(test_finish, 1u);
    check_true(TINYMOCk_FUNCTION_ARG_POINTER_EQUAL(
        test_finish, 0u, "connection", &owner));
  }
  it("waits on incomplete input and exposes results only after libpq is ready") {
    int busy = 1;
    check_true(TINYMOCk_FUNCTION_SET_RETURN(test_busy, busy));
    check_equal(orm_postgres_async_poll(query).kind, ORM_ROW_CURSOR_WAIT);
    busy = 0;
    check_true(TINYMOCk_FUNCTION_SET_RETURN(test_busy, busy));
    (void)cflow_scheduler_advance(&scheduler, 2u);
    check_equal(orm_postgres_async_poll(query).kind, ORM_ROW_CURSOR_DONE);
    TINYMOCk_FUNCTION_VERIFY_NEVER(test_finish);
  }
  it("reports receive failure before attempting flush") {
    int failure = 0;
    check_true(TINYMOCk_FUNCTION_SET_RETURN(test_consume, failure));
    orm_row_cursor_step step = orm_postgres_async_poll(query);
    check_equal(step.kind, ORM_ROW_CURSOR_ERROR);
    check_equal(step.status, ORM_STATUS_CONNECTION_ERROR);
    TINYMOCk_FUNCTION_VERIFY_NEVER(test_flush);
    orm_postgres_async_abort(query);
    TINYMOCk_FUNCTION_VERIFY_TIMES(test_finish, 1u);
    check_true(TINYMOCk_FUNCTION_ARG_POINTER_EQUAL(
        test_finish, 0u, "connection", &owner));
  }
  it("reports flush failure and closes an unfinished connection exactly once") {
    int failure = -1;
    check_true(TINYMOCk_FUNCTION_SET_RETURN(test_flush, failure));
    check_equal(orm_postgres_async_poll(query).kind, ORM_ROW_CURSOR_ERROR);
    orm_postgres_async_abort(query);
    orm_postgres_async_abort(query);
    TINYMOCk_FUNCTION_VERIFY_TIMES(test_finish, 1u);
    check_true(TINYMOCk_FUNCTION_ARG_POINTER_EQUAL(
        test_finish, 0u, "connection", &owner));
  }
  it("expires without calling libpq again") {
    (void)cflow_scheduler_advance(&scheduler, 10u);
    check_equal(orm_postgres_async_poll(query).kind, ORM_ROW_CURSOR_ERROR);
    TINYMOCk_FUNCTION_VERIFY_NEVER(test_consume);
    orm_postgres_async_abort(query);
    TINYMOCk_FUNCTION_VERIFY_TIMES(test_finish, 1u);
    check_true(TINYMOCk_FUNCTION_ARG_POINTER_EQUAL(
        test_finish, 0u, "connection", &owner));
  }
  it("rejects overlapping cursors before changing native connection mode") {
    owner.async_active = 0;
    owner.cursor_active = 1;
    orm_error_t error;
    orm_error_init(&error);
    orm_row_cursor cursor = {0};
    check_equal(orm_postgres_backend_open_async(&owner, NULL, NULL, &query->wait.config, &cursor, &error), ORM_STATUS_BUSY);
    TINYMOCk_FUNCTION_VERIFY_NEVER(test_nonblocking);
    check_null(cursor.context);
  }
}

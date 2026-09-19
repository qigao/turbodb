#include "orm_internal.h"
#include <tinytest.h>
#include <string.h>

/* #28 desired retained-owner semantics; no substitute owner implementation.
 * The observer delegates to the actual SQLite destroy callback unchanged.
 * Each failing retention assertion occurs before any access to freed state. */
static orm_connection_t *owner_connection;
static orm_query_t *owner_query;
static orm_query_t *owner_second_query;
static orm_result_t *owner_result;
static cflow_publisher owner_publisher;
static cflow_publisher owner_second_publisher;
static orm_error_t owner_error;
static orm_backend_ops observed_ops;
static void (*native_destroy)(void *);
static unsigned backend_destroy_calls;
static const char owner_insert_sql[] = "insert into owner_data values (?1)";

static void observed_destroy(void *context) {
  ++backend_destroy_calls;
  native_destroy(context);
}

static void release_connection(void) {
  orm_connection_t *released = owner_connection;
  owner_connection = NULL;
  orm_disconnect(released);
}

static void release_query(orm_query_t **query) {
  orm_query_t *released = *query;
  *query = NULL;
  orm_query_destroy(released);
}

static void release_publisher(cflow_publisher *publisher) {
  if (!cflow_publisher_valid(publisher))
    return;
  /* Interface dispatch neither accepts an empty vtable nor clears its caller.
   * Consume the test's handle before dispatch so after_each cannot repeat it. */
  cflow_publisher released = *publisher;
  memset(publisher, 0, sizeof(*publisher));
  cflow_publisher_destroy(&released);
}

static void open_insert(void) {
  check_equal(orm_raw(owner_connection, orm_view(owner_insert_sql),
                      &owner_query, &owner_error), ORM_STATUS_OK);
  check_equal(orm_query_bind(owner_query, orm_i64(1), &owner_error), ORM_STATUS_OK);
}

static void open_lazy(cflow_publisher *out) {
  check_equal(orm_query_open_command_flow(owner_query, out, &owner_error),
              ORM_STATUS_OK);
  check_true(cflow_publisher_valid(out));
}

spec("native ORM retained ownership and execution freeze") {
  (void)ttest_config__;
  before_each() {
    orm_config_t config;
    orm_option_t filename;
    owner_connection = NULL;
    owner_query = NULL;
    owner_second_query = NULL;
    owner_result = NULL;
    memset(&owner_publisher, 0, sizeof(owner_publisher));
    memset(&owner_second_publisher, 0, sizeof(owner_second_publisher));
    backend_destroy_calls = 0u;
    native_destroy = NULL;
    orm_error_init(&owner_error);
    orm_config(&config);
    filename.keyword = orm_view("filename");
    filename.value = orm_view(":memory:");
    config.driver = orm_view("sqlite");
    config.options = &filename;
    config.option_count = 1u;
    check_equal(orm_connect(&config, &owner_connection, &owner_error), ORM_STATUS_OK);
    check_not_null(owner_connection);
    observed_ops = *owner_connection->backend.ops;
    native_destroy = observed_ops.destroy;
    observed_ops.destroy = observed_destroy;
    owner_connection->backend.ops = &observed_ops;
    /* Exercise actual SQL before testing lifetime, not a fake backend. */
    check_equal(orm_raw(owner_connection,
                        orm_view("create table owner_data(id integer)"),
                        &owner_query, &owner_error), ORM_STATUS_OK);
    check_equal(orm_query_execute(owner_query, &owner_result, &owner_error), ORM_STATUS_OK);
    orm_result_destroy(owner_result);
    owner_result = NULL;
    release_query(&owner_query);
    check_equal(backend_destroy_calls, 0u);
  }
  after_each() {
    /* No resume/native call after a failed retention assertion. On the old
     * core an unconsumed command destroy only frees its command state; plan
     * destruction does not dereference its connection. This keeps RED safe. */
    release_publisher(&owner_second_publisher);
    release_publisher(&owner_publisher);
    orm_result_destroy(owner_result);
    owner_result = NULL;
    release_query(&owner_second_query);
    release_query(&owner_query);
    release_connection();
  }

  it("normally destroys the actual backend exactly once") {
    open_insert();
    release_query(&owner_query);
    check_equal(backend_destroy_calls, 0u);
    release_connection();
    check_equal(backend_destroy_calls, 1u);
  }

  it("keeps a released connection alive while a query owns it") {
    open_insert();
    release_connection();
    check_equal(backend_destroy_calls, 0u);
    release_query(&owner_query);
    check_equal(backend_destroy_calls, 1u);
  }

  it("keeps the parent until both query owners have released") {
    open_insert();
    check_equal(orm_raw(owner_connection, orm_view(owner_insert_sql),
                        &owner_second_query, &owner_error), ORM_STATUS_OK);
    release_connection();
    check_equal(backend_destroy_calls, 0u);
    release_query(&owner_query);
    check_equal(backend_destroy_calls, 0u);
    release_query(&owner_second_query);
    check_equal(backend_destroy_calls, 1u);
  }

  it("keeps the query connection chain until an unconsumed Publisher is destroyed") {
    open_insert();
    open_lazy(&owner_publisher);
    release_connection();
    check_equal(backend_destroy_calls, 0u);
    release_query(&owner_query);
    check_equal(backend_destroy_calls, 0u);
    cflow_publisher_cancel(&owner_publisher);
    check_equal(backend_destroy_calls, 0u);
    release_publisher(&owner_publisher);
    check_equal(backend_destroy_calls, 1u);
  }

  it("rejects bind mutation before changing a published lazy plan") {
    open_insert();
    open_lazy(&owner_publisher);
    const size_t parameters = vec_size(&owner_query->plan.raw_parameters);
    const size_t bytes = owner_query->plan.parameter_bytes;
    check_equal(orm_query_bind(owner_query, orm_i64(2), &owner_error), ORM_STATUS_BUSY);
    check_equal(vec_size(&owner_query->plan.raw_parameters), parameters);
    check_equal(owner_query->plan.parameter_bytes, bytes);
    release_publisher(&owner_publisher);
    check_equal(orm_query_bind(owner_query, orm_i64(2), &owner_error), ORM_STATUS_OK);
  }

  it("does not confuse a cancellation request with releasing the plan hold") {
    open_insert();
    open_lazy(&owner_publisher);
    cflow_publisher_cancel(&owner_publisher);
    check_equal(orm_query_bind(owner_query, orm_i64(2), &owner_error), ORM_STATUS_BUSY);
    release_publisher(&owner_publisher);
    check_equal(orm_query_bind(owner_query, orm_i64(2), &owner_error), ORM_STATUS_OK);
  }

  it("keeps the plan frozen until the final Publisher is destroyed") {
    open_insert();
    open_lazy(&owner_publisher);
    open_lazy(&owner_second_publisher);
    release_publisher(&owner_publisher);
    check_equal(orm_query_bind(owner_query, orm_i64(2), &owner_error), ORM_STATUS_BUSY);
    release_publisher(&owner_second_publisher);
    check_equal(orm_query_bind(owner_query, orm_i64(2), &owner_error), ORM_STATUS_OK);
  }

  it("does not acquire a plan hold when Publisher creation is rejected") {
    open_insert();
    check_equal(orm_query_open_command_flow(owner_query, NULL, &owner_error),
                ORM_STATUS_INVALID_ARGUMENT);
    check_equal(orm_query_bind(owner_query, orm_i64(2), &owner_error), ORM_STATUS_OK);
    check_equal(vec_size(&owner_query->plan.raw_parameters), (size_t)2u);
    release_query(&owner_query);
    release_connection();
    check_equal(backend_destroy_calls, 1u);
  }
}

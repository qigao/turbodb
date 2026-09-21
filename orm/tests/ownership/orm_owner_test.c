#include "orm_internal.h"
#include "orm_owner.h"
#include <tinytest.h>
#include <string.h>

static orm_connection_t *connection;
static orm_query_t *query;
static orm_query_t *second;
static orm_error_t error;
static cflow_publisher publisher;
static void drop_publisher(void) {
  if (!cflow_publisher_valid(&publisher)) return;
  cflow_publisher released = publisher;
  memset(&publisher, 0, sizeof(publisher));
  cflow_publisher_destroy(&released);
}
static void make_query(void) {
  check_equal(orm_raw(connection, orm_view("insert into absent_table values (?1)"),
                     &query, &error), ORM_STATUS_OK);
}
spec("native checked owner integration") {
  (void)ttest_config__;
  before_each() {
    orm_config_t config;
    orm_option_t filename;
    connection = NULL; query = NULL; second = NULL;
    memset(&publisher, 0, sizeof(publisher));
    orm_error_init(&error); orm_config(&config);
    filename.keyword = orm_view("filename"); filename.value = orm_view(":memory:");
    config.driver = orm_view("sqlite"); config.options = &filename;
    config.option_count = 1u;
    check_equal(orm_connect(&config, &connection, &error), ORM_STATUS_OK);
  }
  after_each() {
    drop_publisher(); orm_query_release(second); orm_query_release(query);
    orm_connection_release(connection);
  }
  it("rejects null close arguments") {
    check_equal(orm_connection_close(NULL, &error), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(orm_query_close(NULL, NULL), ORM_STATUS_INVALID_ARGUMENT);
  }
  it("keeps a closed query's parent until the query handle is released") {
    make_query(); void *backend = connection->backend.context;
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_BUSY);
    check_true(connection->backend.context == backend);
    check_equal(connection->owner.phase, ORM_OWNER_OPEN);
    check_equal(orm_query_close(query, &error), ORM_STATUS_OK);
    check_equal(orm_query_close(query, &error), ORM_STATUS_OK);
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_BUSY);
    orm_query_release(query); query = NULL;
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_OK);
    check_null(connection->backend.context);
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_OK);
  }
  it("rejects new query admission to a closed connection") {
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_OK);
    check_equal(orm_raw(connection, orm_view("select 1"), &query, &error),
                ORM_STATUS_INVALID_STATE);
    check_null(query); check_equal(connection->owner.dependents, 0u);
  }
  it("rejects mutation and command admission to a closed query") {
    make_query(); check_equal(orm_query_close(query, &error), ORM_STATUS_OK);
    check_equal(orm_query_bind(query, orm_i64(1), &error), ORM_STATUS_INVALID_STATE);
    check_equal(orm_query_open_command_flow(query, &publisher, &error),
                ORM_STATUS_INVALID_STATE);
    check_false(cflow_publisher_valid(&publisher));
  }
  it("retains a query until the last user reference leaves") {
    make_query(); orm_query_retain(query); orm_query_release(query);
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_BUSY);
    check_equal(orm_query_bind(query, orm_i64(1), &error), ORM_STATUS_OK);
    check_equal(query->owner.references, 1u);
  }
  it("does not leak a connection dependency on failed query construction") {
    check_equal(orm_raw(connection, orm_view(""), &query, &error),
                ORM_STATUS_INVALID_ARGUMENT);
    check_null(query); check_equal(connection->owner.dependents, 0u);
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_OK);
  }
  it("enforces the native child budget before query allocation") {
    connection->owner.max_dependents = 1u; /* Private, unpublished test setup. */
    make_query();
    check_equal(orm_raw(connection, orm_view("select 1"), &second, &error),
                ORM_STATUS_LIMIT_EXCEEDED);
    check_null(second); check_equal(connection->owner.dependents, 1u);
    orm_query_release(query); query = NULL;
    check_equal(orm_raw(connection, orm_view("select 1"), &second, &error), ORM_STATUS_OK);
  }
  it("keeps checked close busy through cancellation until Publisher destruction") {
    make_query();
    check_equal(orm_query_open_command_flow(query, &publisher, &error), ORM_STATUS_OK);
    check_equal(orm_query_close(query, &error), ORM_STATUS_BUSY);
    cflow_publisher_cancel(&publisher);
    check_equal(orm_query_close(query, &error), ORM_STATUS_BUSY);
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_BUSY);
    drop_publisher();
    check_equal(orm_query_close(query, &error), ORM_STATUS_OK);
  }
  it("rejects every builder before changing a published plan") {
    check_equal(orm_query_create(connection, orm_view("records"), &query, &error), ORM_STATUS_OK);
    /* A native dependent is the same freeze primitive used by Publishers. */
    check_equal(orm_owner_admit(&query->owner), ORM_STATUS_OK);
    const orm_query_plan before = query->plan;
    const orm_key_part_t key = {orm_view("id"), orm_i64(1)};
    const orm_status_t statuses[] = {
      orm_query_select_all(query, &error),
      orm_query_add_column(query, orm_view("id"), &error),
      orm_query_set(query, orm_view("id"), orm_i64(1), &error),
      orm_query_where(query, orm_view("id"), ORM_COMPARE_EQUAL, orm_i64(1), &error),
      orm_query_where_key(query, &key, 1u, &error),
      orm_query_bind(query, orm_i64(1), &error),
      orm_query_order_by(query, orm_view("id"), ORM_ORDER_DESCENDING, &error),
      orm_query_set_limit(query, 1u, &error), orm_query_set_offset(query, 1u, &error)
    };
    const int unchanged = memcmp(&before, &query->plan, sizeof(before));
    const orm_owner_action action = orm_owner_release_dependent(&query->owner);
    check_equal(action, ORM_OWNER_KEEP);
    for (size_t i = 0u; i < sizeof(statuses) / sizeof(statuses[0]); ++i)
      check_equal(statuses[i], ORM_STATUS_BUSY);
    check_equal(unchanged, 0);
  }
}

#ifndef ORM_TEST_ASYNC_QUERY_H
#define ORM_TEST_ASYNC_QUERY_H
#include <orm.h>
#include <cmeta/struct.h>
#include <salts/thread.h>
#include <stdio.h>

Struct(async_test_row, (int, id));
static const cmeta_type_traits async_test_traits = {
    .flags = CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY};
static const cmeta_type_desc async_test_type = {
    "async_test_row", sizeof(async_test_row), _Alignof(async_test_row),
    CMETA_T_OBJECT, NULL, &async_test_traits, NULL};
static const cmeta_data_field_desc async_test_fields[] = {
    {"orm.test.async.id", "id", offsetof(async_test_row, id), &cmeta_data_int}};
static const cmeta_data_struct_shape async_test_shape = {
    StructMeta(async_test_row), async_test_fields, 1u};
static const cmeta_data_desc async_test_data = {
    .struct_size = offsetof(cmeta_data_desc, shape) +
                   sizeof(((cmeta_data_desc *)0)->shape),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orm.test.async",
    .display_name = "AsyncRow",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &async_test_type,
    .shape = &async_test_shape};
typedef struct async_test_observer { int count; int sum; int done; int failed; } async_test_observer;
static bool async_test_next(void *user, const cmeta_type_desc *type, const void *row) {
  (void)type;
  async_test_observer *observer = user;
  ++observer->count;
  observer->sum += ((const async_test_row *)row)->id;
  return true;
}
static void async_test_error(void *user, const char *message) {
  ((async_test_observer *)user)->failed = 1;
  fprintf(stderr, "async E2E: %s\n", message);
}
static void async_test_done(void *user) { ((async_test_observer *)user)->done = 1; }

/* A virtual scheduler is advanced at 1 ms intervals only in this E2E harness.
 * The hard iteration bound also covers a broken wake or missing terminal signal. */
static orm_status_t orm_test_async_query(orm_connection_t *connection, orm_error_t *error) {
  enum { DEADLINE_TICKS = 30000, POLL_TICKS = 1 };
  cflow_scheduler scheduler = {0};
  cflow_publisher publisher = {0};
  cflow_graph graph = {0};
  cflow_subscription subscription = {0};
  orm_query_t *query = NULL;
  orm_flow_config_t flow;
  async_test_observer observer = {0};
  cflow_subscriber_callbacks callbacks = {async_test_next, async_test_error, async_test_done, &observer};
  cflow_subscriber subscriber = cflow_subscriber_from_callbacks(&callbacks);
  orm_status_t status = ORM_STATUS_INTERNAL_ERROR;
  int second_requested = 0;
  if (!cflow_scheduler_test_init(&scheduler)) return status;
  cflow_graph_init(&graph, &async_test_type);
  const orm_async_config_t async = {sizeof(async), &scheduler, POLL_TICKS, DEADLINE_TICKS};
  orm_flow_config(&flow, &async_test_data);
  status = orm_raw(connection, orm_view("select 7 as id union all select 11 as id"), &query, error);
  if (status != ORM_STATUS_OK) goto cleanup;
  status = orm_query_open_async_flow(query, &flow, &async, &publisher, error);
  if (status != ORM_STATUS_OK) goto cleanup;
  status = ORM_STATUS_INTERNAL_ERROR;
  if (!cflow_subscribe(&subscription, &graph, &publisher, &scheduler, &subscriber)) goto cleanup;
  (void)cflow_scheduler_run_ready(&scheduler);
  if (observer.count != 0) goto cleanup;
  if (!cflow_subscription_request(&subscription, 1u)) goto cleanup;
  for (unsigned tick = 0u; tick <= DEADLINE_TICKS && !observer.done && !observer.failed; ++tick) {
    (void)cflow_scheduler_advance(&scheduler, POLL_TICKS);
    if (observer.count == 1 && !second_requested) {
      /* A second row cannot be delivered until this explicit second demand. */
      (void)cflow_scheduler_run_ready(&scheduler);
      if (observer.count != 1) goto cleanup;
      if (!cflow_subscription_request(&subscription, 2u)) goto cleanup;
      second_requested = 1;
    }
    cmeta_sleep_ms(POLL_TICKS);
  }
  if (observer.done && !observer.failed && observer.count == 2 && observer.sum == 18)
    status = ORM_STATUS_OK;
cleanup:
  cflow_subscription_close(&subscription);
  if (cflow_publisher_valid(&publisher)) cflow_publisher_destroy(&publisher);
  cflow_graph_destroy(&graph);
  orm_query_destroy(query);
  cflow_scheduler_destroy(&scheduler);
  return status;
}
#endif

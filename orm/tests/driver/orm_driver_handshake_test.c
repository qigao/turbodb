#include "orm_driver_fixture.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

#define HEADER(T) {sizeof(T), ORM_DRIVER_ABI_VERSION}
#define TABLE(p) (orm_driver_table_v1){(p), sizeof(*(p)), 0u}
#define FIXTURE_TEXT(s) fixture_bytes((s), sizeof(s) - 1u)
#define EXPECT_OK(call) check_equal((call), ORM_STATUS_OK)

/* Keep compound literals out of TinyTest's unevaluated _Generic operands:
 * MSVC otherwise diagnoses their synthesized temporaries with C4189. */
static orm_driver_bytes_v1 fixture_bytes(const void *data, uint64_t size) {
  orm_driver_bytes_v1 value = {data, size};
  return value;
}

typedef struct handshake_session {
  const orm_driver_api_v1 *api;
  const orm_driver_module_ops_v1 *ops;
  void *module;
  orm_driver_host_v1 host;
  orm_driver_limits_v1 limits;
} handshake_session;

static orm_driver_limits_v1 limits(void) {
  orm_driver_limits_v1 value = {HEADER(orm_driver_limits_v1),
      1u, 1u, 1u, 1u, ORM_C_DEFAULT_MAX_QUERY_BYTES,
      ORM_DRIVER_FIXTURE_VALUE_BYTES, 1u,
      ORM_DRIVER_FIXTURE_VALUE_BYTES + sizeof(ORM_DRIVER_FIXTURE_OPTION) - 1u};
  return value;
}

static void start(handshake_session *s) {
  uint32_t bytes = 0u;
  memset(s, 0, sizeof(*s));
  s->host = orm_driver_fixture_host();
  s->limits = limits();
  EXPECT_OK(orm_driver_get_api_v1(&s->host, sizeof(s->host), &s->api, &bytes));
  EXPECT_OK(orm_driver_validate_api_v1(s->api, bytes,
      orm_driver_fixture_bundle(), FIXTURE_TEXT(ORM_DRIVER_FIXTURE_ID), 0u, NULL));
  s->ops = s->api->module_ops.data;
  EXPECT_OK(s->ops->initialize(&s->host, &s->module, NULL));
  check_not_null(s->module);
}

static void stop(handshake_session *s) {
  EXPECT_OK(s->ops->finalize(s->module, NULL));
  s->module = NULL;
}

static orm_config_t config(orm_option_t *option) {
  orm_config_t value = {0};
  value.struct_size = sizeof(value);
  value.abi_version = ORM_C_ABI_VERSION;
  value.driver = vstr_from_cstr(ORM_DRIVER_FIXTURE_ID);
  value.options = option;
  value.option_count = 1u;
  return value;
}

static orm_status_t create(handshake_session *s, orm_driver_bytes_v1 value,
                           orm_driver_connection_v1 *out) {
  orm_option_t option = {vstr_from_cstr(ORM_DRIVER_FIXTURE_OPTION),
                        {value.data, (size_t)value.size}};
  orm_config_t cfg = config(&option);
  return s->api->create_connection(s->module, &cfg, &s->limits, out, NULL);
}

static const orm_driver_connection_ops_v1 *connection_ops(orm_driver_connection_v1 *c) {
  EXPECT_OK(orm_driver_validate_connection_v1(c, sizeof(*c),
      ORM_DRIVER_FIXTURE_CAPS, NULL));
  return c->ops.data;
}

static orm_status_t open_kind(handshake_session *s, orm_driver_connection_v1 *c,
                              uint32_t kind, orm_driver_cursor_v1 *out) {
  orm_driver_plan_meta_v1 meta = {0};
  meta.header = (orm_driver_header_v1)HEADER(orm_driver_plan_meta_v1);
  meta.kind = kind;
  meta.column_count = 1u;
  orm_driver_plan_view_v1 plan = {HEADER(orm_driver_plan_view_v1), &meta,
                                s->host.plan_metadata, s->host.plan_values};
  return connection_ops(c)->open_cursor(c->context, &plan, &s->limits, out, NULL);
}

static const orm_driver_cursor_ops_v1 *cursor_ops(orm_driver_cursor_v1 *c) {
  EXPECT_OK(orm_driver_validate_cursor_v1(c, sizeof(*c), ORM_DRIVER_FIXTURE_CAPS, NULL));
  return c->ops.data;
}

static void read_value(cserde_reader *reader, orm_driver_bytes_v1 expected) {
  cserde_token token = {0};
  check_equal(cserde_reader_next(reader, &token), CSERDE_OK);
  check_equal(token.kind, CSERDE_MAP_BEGIN);
  check_equal(cserde_reader_next(reader, &token), CSERDE_OK);
  check_equal(token.kind, CSERDE_STRING);
  check_equal(token.value.slice.size, sizeof(ORM_DRIVER_FIXTURE_OPTION) - 1u);
  check_equal(memcmp(token.value.slice.data, ORM_DRIVER_FIXTURE_OPTION,
                     token.value.slice.size), 0);
  check_equal(cserde_reader_next(reader, &token), CSERDE_OK);
  check_equal(token.kind, CSERDE_STRING);
  check_equal(token.value.slice.size, (size_t)expected.size);
  if (expected.size != 0u)
    check_equal(memcmp(token.value.slice.data, expected.data, (size_t)expected.size), 0);
  check_equal(cserde_reader_next(reader, &token), CSERDE_OK);
  check_equal(token.kind, CSERDE_MAP_END);
  check_equal(cserde_reader_next(reader, &token), CSERDE_DONE);
}

static void row(handshake_session *s, orm_driver_connection_v1 *c,
                orm_driver_bytes_v1 expected) {
  orm_driver_cursor_v1 cursor = {0};
  cserde_reader reader = {0};
  orm_driver_step_v1 step = {0};
  EXPECT_OK(open_kind(s, c, ORM_DRIVER_PLAN_SELECT, &cursor));
  const orm_driver_cursor_ops_v1 *ops = cursor_ops(&cursor);
  EXPECT_OK(ops->next(cursor.context, &reader, &step, NULL));
  check_equal(step.kind, ORM_DRIVER_STEP_ROW);
  read_value(&reader, expected);
  EXPECT_OK(ops->next(cursor.context, &reader, &step, NULL));
  check_equal(step.kind, ORM_DRIVER_STEP_DONE);
  check_null(reader.context);
  check_null(reader.ops);
  ops->destroy(cursor.context);
}

static void clean(void) {
  orm_driver_fixture_stats stats = orm_driver_fixture_stats_get();
  check_equal(stats.live_modules, 0u);
  check_equal(stats.live_connections, 0u);
  check_equal(stats.live_cursors, 0u);
  check_equal(stats.live_tickets, 0u);
  check_equal(stats.allocations, stats.deallocations);
  check_equal(stats.lease_acquires, stats.lease_releases);
}

spec("driver bootstrap handshake") {
  (void)ttest_config__;
  it("returns a static validated descriptor without initializing or allocating") {
    orm_driver_fixture_reset();
    orm_driver_host_v1 host = orm_driver_fixture_host();
    const orm_driver_api_v1 *api = NULL, *second = NULL;
    uint32_t bytes = 0u, second_bytes = 0u;
    EXPECT_OK(orm_driver_get_api_v1(&host, sizeof(host), &api, &bytes));
    EXPECT_OK(orm_driver_get_api_v1(&host, sizeof(host), &second, &second_bytes));
    check_true(api == second);
    check_equal(bytes, second_bytes);
    EXPECT_OK(orm_driver_validate_api_v1(api, bytes, orm_driver_fixture_bundle(),
                                        FIXTURE_TEXT(ORM_DRIVER_FIXTURE_ID), 0u, NULL));
    check_equal(api->capabilities, ORM_DRIVER_FIXTURE_CAPS);
    check_equal(api->execution_models, ORM_DRIVER_EXEC_CALLER_BLOCKING);
    check_equal(orm_driver_fixture_stats_get().init_calls, 0u);
    check_equal(orm_driver_fixture_stats_get().allocations, 0u);
    clean();
  }
  it("clears the other output when either bootstrap output is null") {
    orm_driver_fixture_reset();
    orm_driver_host_v1 host = orm_driver_fixture_host();
    const orm_driver_api_v1 *api = (const orm_driver_api_v1 *)&host;
    uint32_t bytes = UINT32_MAX;
    check_equal(orm_driver_get_api_v1(NULL, 0u, NULL, &bytes), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(bytes, 0u);
    check_equal(orm_driver_get_api_v1(NULL, 0u, &api, NULL), ORM_STATUS_INVALID_ARGUMENT);
    check_null(api);
    check_equal(orm_driver_get_api_v1(NULL, 0u, NULL, NULL), ORM_STATUS_INVALID_ARGUMENT);
    clean();
  }
  it("rejects every changed bundle byte with both outputs zero") {
    orm_driver_fixture_reset();
    for (uint32_t i = 0u; i < ORM_DRIVER_BUNDLE_ID_BYTES; ++i) {
      orm_driver_host_v1 host = orm_driver_fixture_host();
      host.bundle_id[i] ^= 1u;
      const orm_driver_api_v1 *api = (const orm_driver_api_v1 *)&host;
      uint32_t bytes = UINT32_MAX;
      check_equal(orm_driver_get_api_v1(&host, sizeof(host), &api, &bytes), ORM_STATUS_ABI_MISMATCH);
      check_null(api); check_equal(bytes, 0u);
    }
    check_equal(orm_driver_fixture_stats_get().init_calls, 0u);
    clean();
  }
  it("rejects physically short host buffers without a typed read") {
    orm_driver_fixture_reset();
    orm_driver_host_v1 host = orm_driver_fixture_host();
    for (uint32_t n = 0u; n < sizeof(host); ++n) {
      void *buffer = malloc(n == 0u ? 1u : n);
      check_not_null(buffer);
      memcpy(buffer, &host, n);
      const orm_driver_api_v1 *api = NULL;
      uint32_t bytes = UINT32_MAX;
      const orm_status_t status = orm_driver_get_api_v1(buffer, n, &api, &bytes);
      free(buffer);
      check_equal(status, ORM_STATUS_ABI_MISMATCH);
      check_null(api); check_equal(bytes, 0u);
    }
    clean();
  }
  it("rejects null host bad version and malformed optional execution services") {
    orm_driver_fixture_reset();
    orm_driver_host_v1 host = orm_driver_fixture_host();
    const orm_driver_api_v1 *api = NULL;
    uint32_t bytes = UINT32_MAX;
    check_equal(orm_driver_get_api_v1(NULL, sizeof(host), &api, &bytes), ORM_STATUS_INVALID_ARGUMENT);
    check_null(api); check_equal(bytes, 0u);
    host.header.abi_version++;
    check_equal(orm_driver_get_api_v1(&host, sizeof(host), &api, &bytes), ORM_STATUS_ABI_MISMATCH);
    host = orm_driver_fixture_host();
    host.execution.bytes = sizeof(orm_driver_execution_ops_v1);
    check_equal(orm_driver_get_api_v1(&host, sizeof(host), &api, &bytes), ORM_STATUS_INVALID_ARGUMENT);
    check_null(api); check_equal(bytes, 0u);
    clean();
  }
  it("does not call a missing required lifetime callback") {
    orm_driver_fixture_reset();
    orm_driver_host_v1 host = orm_driver_fixture_host();
    orm_driver_lifetime_ops_v1 bad = *(const orm_driver_lifetime_ops_v1 *)host.lifetime.data;
    bad.acquire = NULL; host.lifetime = TABLE(&bad);
    const orm_driver_api_v1 *api = NULL; uint32_t bytes = 0u;
    check_equal(orm_driver_get_api_v1(&host, sizeof(host), &api, &bytes), ORM_STATUS_ABI_MISMATCH);
    check_null(api); check_equal(bytes, 0u);
    check_equal(orm_driver_fixture_stats_get().allocations, 0u);
    clean();
  }
}

spec("driver fixture ownership protocol") {
  (void)ttest_config__;
  it("copies options and isolates two connection values including mutation") {
    orm_driver_fixture_reset(); handshake_session s; start(&s);
    char value[] = "first";
    orm_driver_connection_v1 first = {0}, second = {0};
    EXPECT_OK(create(&s, fixture_bytes(value, sizeof(value)-1u), &first));
    memcpy(value, "other", sizeof(value));
    EXPECT_OK(create(&s, FIXTURE_TEXT("second"), &second));
    check_true(first.context != second.context);
    row(&s, &first, FIXTURE_TEXT("first")); row(&s, &second, FIXTURE_TEXT("second"));
    EXPECT_OK(orm_driver_fixture_set_value(&first, FIXTURE_TEXT("changed")));
    row(&s, &first, FIXTURE_TEXT("changed")); row(&s, &second, FIXTURE_TEXT("second"));
    connection_ops(&first)->destroy(first.context);
    connection_ops(&second)->destroy(second.context);
    stop(&s); clean();
    check_equal(orm_driver_fixture_stats_get().destroy_calls, 2u);
    check_equal(orm_driver_fixture_stats_get().cursor_destroy_calls, 4u);
  }
  it("refuses finalization until every connection of that module is destroyed") {
    orm_driver_fixture_reset(); handshake_session s; start(&s);
    orm_driver_connection_v1 a = {0}, b = {0};
    EXPECT_OK(create(&s, FIXTURE_TEXT("a"), &a)); EXPECT_OK(create(&s, FIXTURE_TEXT("b"), &b));
    check_equal(s.ops->finalize(s.module, NULL), ORM_STATUS_BUSY);
    connection_ops(&a)->destroy(a.context);
    check_equal(s.ops->finalize(s.module, NULL), ORM_STATUS_BUSY);
    row(&s, &b, FIXTURE_TEXT("b")); connection_ops(&b)->destroy(b.context);
    stop(&s); clean();
  }
  it("does not confuse another module connections with its own") {
    orm_driver_fixture_reset(); handshake_session a, b; start(&a); start(&b);
    check_true(a.module != b.module);
    orm_driver_connection_v1 c = {0}; EXPECT_OK(create(&b, FIXTURE_TEXT("b"), &c));
    stop(&a); row(&b, &c, FIXTURE_TEXT("b"));
    connection_ops(&c)->destroy(c.context); stop(&b); clean();
  }
  it("uses copied host tables after initialize instead of retained host pointers") {
    orm_driver_fixture_reset(); handshake_session s; start(&s);
    orm_driver_lifetime_ops_v1 local = *(const orm_driver_lifetime_ops_v1 *)s.host.lifetime.data;
    orm_driver_host_v1 host = s.host;
    host.lifetime = TABLE(&local);
    void *module = NULL; EXPECT_OK(s.ops->initialize(&host, &module, NULL));
    stop(&s); s.module = module;
    memset(&host, 0, sizeof(host)); memset(&local, 0, sizeof(local));
    orm_driver_connection_v1 c = {0}; EXPECT_OK(create(&s, FIXTURE_TEXT("copy"), &c));
    row(&s, &c, FIXTURE_TEXT("copy")); connection_ops(&c)->destroy(c.context);
    stop(&s); clean();
  }
  it("revalidates the host before module allocation") {
    orm_driver_fixture_reset(); handshake_session s; start(&s);
    orm_driver_host_v1 bad = s.host; bad.bundle_id[0] ^= 1u;
    void *module = &bad;
    const uint32_t before = orm_driver_fixture_stats_get().allocations;
    check_equal(s.ops->initialize(&bad, &module, NULL), ORM_STATUS_ABI_MISMATCH);
    check_null(module); check_equal(orm_driver_fixture_stats_get().allocations, before);
    stop(&s); clean();
  }
  it("cleans both module failure injection points and consumes each once") {
    orm_driver_fixture_reset(); handshake_session s; start(&s); stop(&s);
    for (uint32_t point = ORM_DRIVER_FIXTURE_MODULE_BEFORE_ALLOC;
         point <= ORM_DRIVER_FIXTURE_MODULE_AFTER_ALLOC; ++point) {
      void *module = &s; orm_error_t error;
      memset(&error, 0xa5, sizeof(error));
      orm_driver_fixture_fail_next(point);
      check_equal(s.ops->initialize(&s.host, &module, &error), ORM_STATUS_OUT_OF_MEMORY);
      check_null(module); check_equal(error.status, ORM_STATUS_OUT_OF_MEMORY);
      clean();
      EXPECT_OK(s.ops->initialize(&s.host, &module, NULL));
      EXPECT_OK(s.ops->finalize(module, NULL)); clean();
    }
  }
  it("cleans both connection failure injection points with zero outputs") {
    orm_driver_fixture_reset(); handshake_session s; start(&s);
    for (uint32_t point = ORM_DRIVER_FIXTURE_CONNECTION_BEFORE_ALLOC;
         point <= ORM_DRIVER_FIXTURE_CONNECTION_AFTER_ALLOC; ++point) {
      orm_driver_connection_v1 c, zero = {0}; memset(&c, 0xa5, sizeof(c));
      const orm_driver_fixture_stats before = orm_driver_fixture_stats_get();
      orm_driver_fixture_fail_next(point);
      check_equal(create(&s, FIXTURE_TEXT("failure"), &c), ORM_STATUS_OUT_OF_MEMORY);
      check_equal(memcmp(&c, &zero, sizeof(c)), 0);
      orm_driver_fixture_stats after = orm_driver_fixture_stats_get();
      check_equal(after.live_connections, 0u);
      check_equal(after.allocations - before.allocations,
                  after.deallocations - before.deallocations);
      EXPECT_OK(create(&s, FIXTURE_TEXT("retry"), &c)); row(&s, &c, FIXTURE_TEXT("retry"));
      connection_ops(&c)->destroy(c.context);
    }
    stop(&s); clean();
  }
  it("cancels before a row without producing data or leaking its lease") {
    orm_driver_fixture_reset(); handshake_session s; start(&s);
    orm_driver_connection_v1 c = {0}; orm_driver_cursor_v1 cursor = {0};
    EXPECT_OK(create(&s, FIXTURE_TEXT("cancel"), &c));
    EXPECT_OK(open_kind(&s, &c, ORM_DRIVER_PLAN_SELECT, &cursor));
    const orm_driver_cursor_ops_v1 *ops = cursor_ops(&cursor);
    ops->cancel(cursor.context); ops->cancel(cursor.context);
    cserde_reader reader = {0}; orm_driver_step_v1 step = {0};
    EXPECT_OK(ops->next(cursor.context, &reader, &step, NULL));
    check_equal(step.kind, ORM_DRIVER_STEP_DONE); check_null(reader.context);
    ops->destroy(cursor.context); connection_ops(&c)->destroy(c.context);
    stop(&s); clean(); check_equal(orm_driver_fixture_stats_get().lease_releases, 1u);
  }
  it("keeps delivered row bytes alive through cancellation until cursor destroy") {
    orm_driver_fixture_reset(); handshake_session s; start(&s);
    orm_driver_connection_v1 c = {0}; orm_driver_cursor_v1 cursor = {0};
    EXPECT_OK(create(&s, FIXTURE_TEXT("held"), &c));
    EXPECT_OK(open_kind(&s, &c, ORM_DRIVER_PLAN_SELECT, &cursor));
    const orm_driver_cursor_ops_v1 *ops = cursor_ops(&cursor);
    cserde_reader reader = {0}; orm_driver_step_v1 step = {0};
    EXPECT_OK(ops->next(cursor.context, &reader, &step, NULL));
    check_equal(orm_driver_fixture_set_value(&c, FIXTURE_TEXT("mutation")), ORM_STATUS_BUSY);
    ops->cancel(cursor.context); read_value(&reader, FIXTURE_TEXT("held"));
    check_equal(orm_driver_fixture_stats_get().live_tickets, 1u);
    ops->destroy(cursor.context); connection_ops(&c)->destroy(c.context);
    stop(&s); clean();
  }
  it("enforces value length and distinguishes invalid from empty views") {
    orm_driver_fixture_reset(); handshake_session s; start(&s);
    unsigned char payload[ORM_DRIVER_FIXTURE_VALUE_BYTES + 1u]; memset(payload, 'x', sizeof(payload));
    orm_driver_connection_v1 c = {0};
    EXPECT_OK(create(&s, fixture_bytes(payload, ORM_DRIVER_FIXTURE_VALUE_BYTES), &c));
    row(&s, &c, fixture_bytes(payload, ORM_DRIVER_FIXTURE_VALUE_BYTES));
    connection_ops(&c)->destroy(c.context);
    check_equal(create(&s, fixture_bytes(payload, sizeof(payload)), &c), ORM_STATUS_LIMIT_EXCEEDED);
    check_null(c.context);
    check_equal(create(&s, fixture_bytes(NULL, 1u), &c), ORM_STATUS_INVALID_ARGUMENT);
    EXPECT_OK(create(&s, fixture_bytes(NULL, 0u), &c));
    row(&s, &c, fixture_bytes(NULL, 0u)); connection_ops(&c)->destroy(c.context);
    stop(&s); clean();
  }
  it("rejects unsupported plans and output limits before allocating a cursor") {
    orm_driver_fixture_reset(); handshake_session s; start(&s);
    orm_driver_connection_v1 c = {0}; orm_driver_cursor_v1 cursor, zero = {0};
    EXPECT_OK(create(&s, FIXTURE_TEXT("abc"), &c));
    const uint32_t before = orm_driver_fixture_stats_get().allocations;
    memset(&cursor, 0xa5, sizeof(cursor));
    check_equal(open_kind(&s, &c, ORM_DRIVER_PLAN_RAW_SQL, &cursor), ORM_STATUS_UNSUPPORTED);
    check_equal(memcmp(&cursor, &zero, sizeof(cursor)), 0);
    s.limits.max_result_rows = 0u;
    check_equal(open_kind(&s, &c, ORM_DRIVER_PLAN_SELECT, &cursor), ORM_STATUS_LIMIT_EXCEEDED);
    s.limits = limits(); s.limits.max_result_bytes = 1u;
    check_equal(open_kind(&s, &c, ORM_DRIVER_PLAN_SELECT, &cursor), ORM_STATUS_LIMIT_EXCEEDED);
    s.limits = limits(); s.limits.max_columns = 0u;
    check_equal(open_kind(&s, &c, ORM_DRIVER_PLAN_SELECT, &cursor), ORM_STATUS_LIMIT_EXCEEDED);
    check_equal(orm_driver_fixture_stats_get().allocations, before);
    connection_ops(&c)->destroy(c.context); stop(&s); clean();
  }
  it("reclaims partial cursor allocation when bounded host tickets are exhausted") {
    orm_driver_fixture_reset(); handshake_session s; start(&s);
    const orm_driver_lifetime_ops_v1 *life = s.host.lifetime.data;
    void *tickets[ORM_DRIVER_FIXTURE_TICKETS] = {0};
    for (uint32_t i = 0u; i < ORM_DRIVER_FIXTURE_TICKETS; ++i)
      EXPECT_OK(life->acquire(&s, &tickets[i], NULL));
    orm_driver_connection_v1 c = {0}; orm_driver_cursor_v1 cursor = {0};
    EXPECT_OK(create(&s, FIXTURE_TEXT("bounded"), &c));
    orm_driver_fixture_stats before = orm_driver_fixture_stats_get();
    check_equal(open_kind(&s, &c, ORM_DRIVER_PLAN_SELECT, &cursor), ORM_STATUS_LIMIT_EXCEEDED);
    check_null(cursor.context);
    orm_driver_fixture_stats after = orm_driver_fixture_stats_get();
    check_equal(after.allocations-before.allocations, after.deallocations-before.deallocations);
    for (uint32_t i = 0u; i < ORM_DRIVER_FIXTURE_TICKETS; ++i) life->release(tickets[i]);
    row(&s, &c, FIXTURE_TEXT("bounded")); connection_ops(&c)->destroy(c.context); stop(&s); clean();
  }
  it("rejects bad configuration or limits and does not allocate") {
    orm_driver_fixture_reset(); handshake_session s; start(&s);
    orm_option_t option = {vstr_from_cstr(ORM_DRIVER_FIXTURE_OPTION), vstr_from_cstr("v")};
    orm_config_t cfg = config(&option); orm_driver_connection_v1 c = {0};
    const uint32_t before = orm_driver_fixture_stats_get().allocations;
    cfg.abi_version++;
    check_equal(s.api->create_connection(s.module, &cfg, &s.limits, &c, NULL), ORM_STATUS_ABI_MISMATCH);
    cfg = config(&option); cfg.option_count = 2u;
    check_equal(s.api->create_connection(s.module, &cfg, &s.limits, &c, NULL), ORM_STATUS_LIMIT_EXCEEDED);
    cfg = config(&option); cfg.options = NULL;
    check_equal(s.api->create_connection(s.module, &cfg, &s.limits, &c, NULL), ORM_STATUS_INVALID_ARGUMENT);
    cfg = config(&option); s.limits.header.abi_version++;
    check_equal(s.api->create_connection(s.module, &cfg, &s.limits, &c, NULL), ORM_STATUS_ABI_MISMATCH);
    check_equal(orm_driver_fixture_stats_get().allocations, before); check_null(c.context);
    stop(&s); clean();
  }
}

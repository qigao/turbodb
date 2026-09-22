#include <orm_driver_abi.h>
#include <tinytest.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define HEADER(T) {sizeof(T), ORM_DRIVER_ABI_VERSION}
#define TABLE(p) (orm_driver_table_v1){(p), sizeof(*(p)), 0u}
#define END(T, field) ((uint32_t)(offsetof(T, field) + sizeof(((T *)0)->field)))
static uint32_t callback_calls;
static const uint8_t bundle[ORM_DRIVER_BUNDLE_ID_BYTES] = ORM_DRIVER_BUNDLE_ID_INIT;
static const orm_driver_bytes_v1 driver_id = {"contract_fixture", 16u};

static orm_status_t ORM_DRIVER_CALL trap_initialize(const orm_driver_host_v1 *h, void **o, orm_error_t *e) {
  (void)h; (void)o; (void)e;
  ++callback_calls;
  return ORM_STATUS_INTERNAL_ERROR;
}

static orm_status_t ORM_DRIVER_CALL trap_finish(void *c, orm_error_t *e) {
  (void)c; (void)e;
  ++callback_calls;
  return ORM_STATUS_INTERNAL_ERROR;
}

static orm_status_t ORM_DRIVER_CALL trap_open(void *c, const orm_driver_plan_view_v1 *p, const orm_driver_limits_v1 *l, orm_driver_cursor_v1 *o, orm_error_t *e) {
  (void)c; (void)p; (void)l; (void)o; (void)e;
  ++callback_calls;
  return ORM_STATUS_INTERNAL_ERROR;
}

static orm_status_t ORM_DRIVER_CALL trap_command(void *c, const orm_driver_plan_view_v1 *p, const orm_driver_limits_v1 *l, uint64_t *o, orm_error_t *e) {
  (void)c; (void)p; (void)l; (void)o; (void)e;
  ++callback_calls;
  return ORM_STATUS_INTERNAL_ERROR;
}

static orm_status_t ORM_DRIVER_CALL trap_begin(void *c, orm_isolation_t i, orm_driver_transaction_v1 *o, orm_error_t *e) {
  (void)c; (void)i; (void)o; (void)e;
  ++callback_calls;
  return ORM_STATUS_INTERNAL_ERROR;
}

static orm_status_t ORM_DRIVER_CALL trap_savepoint(void *c, orm_driver_bytes_v1 n, orm_error_t *e) {
  (void)c; (void)n; (void)e;
  ++callback_calls;
  return ORM_STATUS_INTERNAL_ERROR;
}

static orm_status_t ORM_DRIVER_CALL trap_next(void *c, cserde_reader *r, orm_driver_step_v1 *s, orm_error_t *e) {
  (void)c; (void)r; (void)s; (void)e;
  ++callback_calls;
  return ORM_STATUS_INTERNAL_ERROR;
}

static orm_status_t ORM_DRIVER_CALL trap_shape(void *c, const cmeta_data_desc *s, orm_error_t *e) {
  (void)c; (void)s; (void)e;
  ++callback_calls;
  return ORM_STATUS_INTERNAL_ERROR;
}

static orm_status_t ORM_DRIVER_CALL trap_count(void *c, uint64_t *n, orm_error_t *e) {
  (void)c; (void)n; (void)e;
  ++callback_calls;
  return ORM_STATUS_INTERNAL_ERROR;
}

static orm_status_t ORM_DRIVER_CALL trap_acquire(void *c, void **o, orm_error_t *e) {
  (void)c; (void)o; (void)e;
  ++callback_calls;
  return ORM_STATUS_INTERNAL_ERROR;
}

static orm_status_t ORM_DRIVER_CALL trap_submit(void *x, void *o, orm_driver_work_fn w, orm_driver_complete_fn f, void *c, void **t, orm_error_t *e) {
  (void)x; (void)o; (void)w; (void)f; (void)c; (void)t; (void)e;
  ++callback_calls;
  return ORM_STATUS_INTERNAL_ERROR;
}

static orm_status_t ORM_DRIVER_CALL trap_create(void *c, const orm_config_t *g, const orm_driver_limits_v1 *l, orm_driver_connection_v1 *o, orm_error_t *e) {
  (void)c; (void)g; (void)l; (void)o; (void)e;
  ++callback_calls;
  return ORM_STATUS_INTERNAL_ERROR;
}

static orm_status_t ORM_DRIVER_CALL trap_describe(const void *c, orm_driver_plan_meta_v1 *o, orm_error_t *e) {
  (void)c; (void)o; (void)e;
  ++callback_calls;
  return ORM_STATUS_INTERNAL_ERROR;
}

static orm_status_t ORM_DRIVER_CALL trap_column(const void *c, uint64_t i, orm_driver_bytes_v1 *o, orm_error_t *e) {
  (void)c; (void)i; (void)o; (void)e;
  ++callback_calls;
  return ORM_STATUS_INTERNAL_ERROR;
}

static orm_status_t ORM_DRIVER_CALL trap_ordering(const void *c, orm_driver_ordering_v1 *o, orm_error_t *e) {
  (void)c; (void)o; (void)e;
  ++callback_calls;
  return ORM_STATUS_INTERNAL_ERROR;
}

static orm_status_t ORM_DRIVER_CALL trap_assignment(const void *c, uint64_t i, orm_driver_assignment_v1 *o, orm_error_t *e) {
  (void)c; (void)i; (void)o; (void)e;
  ++callback_calls;
  return ORM_STATUS_INTERNAL_ERROR;
}

static orm_status_t ORM_DRIVER_CALL trap_predicate(const void *c, uint64_t i, orm_driver_predicate_v1 *o, orm_error_t *e) {
  (void)c; (void)i; (void)o; (void)e;
  ++callback_calls;
  return ORM_STATUS_INTERNAL_ERROR;
}

static orm_status_t ORM_DRIVER_CALL trap_parameter(const void *c, uint64_t i, orm_driver_value_v1 *o, orm_error_t *e) {
  (void)c; (void)i; (void)o; (void)e;
  ++callback_calls;
  return ORM_STATUS_INTERNAL_ERROR;
}

static void ORM_DRIVER_CALL trap_void(void *c) {
  (void)c;
  ++callback_calls;
}

typedef struct descriptor_fixture {
  orm_driver_api_v1 api;
  orm_driver_host_v1 host;
  orm_driver_module_ops_v1 module;
  orm_driver_connection_ops_v1 connection;
  orm_driver_transaction_ops_v1 transaction;
  orm_driver_cursor_ops_v1 cursor;
  orm_driver_plan_metadata_ops_v1 metadata;
  orm_driver_plan_value_ops_v1 values;
  orm_driver_lifetime_ops_v1 lifetime;
  orm_driver_execution_ops_v1 execution;
  orm_driver_bytes_v1 aliases[2];
} descriptor_fixture;

/* Traps are typed callback addresses only: validators must never enter them. */
static void setup(descriptor_fixture *f) {
  memset(f, 0, sizeof(*f));
  callback_calls = 0u;
  f->module = (orm_driver_module_ops_v1){HEADER(orm_driver_module_ops_v1),
      trap_initialize, trap_finish};
  f->connection = (orm_driver_connection_ops_v1){HEADER(orm_driver_connection_ops_v1),
      trap_void, trap_open, trap_command, trap_begin};
  f->transaction = (orm_driver_transaction_ops_v1){HEADER(orm_driver_transaction_ops_v1),
      trap_void, trap_open, trap_command, trap_finish, trap_finish,
      trap_savepoint, trap_savepoint, trap_savepoint};
  f->cursor = (orm_driver_cursor_ops_v1){HEADER(orm_driver_cursor_ops_v1),
      trap_next, trap_void, trap_void, trap_shape, trap_count};
  f->metadata = (orm_driver_plan_metadata_ops_v1){HEADER(orm_driver_plan_metadata_ops_v1),
      trap_describe, trap_column, trap_ordering};
  f->values = (orm_driver_plan_value_ops_v1){HEADER(orm_driver_plan_value_ops_v1),
      trap_assignment, trap_predicate, trap_parameter};
  f->lifetime = (orm_driver_lifetime_ops_v1){HEADER(orm_driver_lifetime_ops_v1),
      trap_acquire, trap_void};
  f->execution = (orm_driver_execution_ops_v1){HEADER(orm_driver_execution_ops_v1),
      trap_submit, trap_void, trap_void};
  f->api.header = (orm_driver_header_v1)HEADER(orm_driver_api_v1);
  memcpy(f->api.bundle_id, bundle, sizeof(bundle));
  f->api.canonical_id = driver_id;
  f->aliases[0] = (orm_driver_bytes_v1){"fixture", 7u};
  f->aliases[1] = (orm_driver_bytes_v1){"fixture_2", 9u};
  f->api.aliases = f->aliases;
  f->api.alias_count = 2u;
  f->api.capabilities = ORM_DRIVER_CAP_KNOWN_MASK;
  f->api.execution_models = ORM_DRIVER_EXEC_CALLER_BLOCKING;
  f->api.module_ops = TABLE(&f->module);
  f->api.create_connection = trap_create;
  f->api.connection_ops = TABLE(&f->connection);
  f->host.header = (orm_driver_header_v1)HEADER(orm_driver_host_v1);
  memcpy(f->host.bundle_id, bundle, sizeof(bundle));
  f->host.plan_metadata = TABLE(&f->metadata);
  f->host.plan_values = TABLE(&f->values);
  f->host.lifetime = TABLE(&f->lifetime);
  f->host.execution = TABLE(&f->execution);
}
#define EXPECT(expr, wanted) do { \
  orm_status_t got__ = (expr); \
  check_equal(got__, (orm_status_t)(wanted)); \
  check_equal(callback_calls, UINT32_C(0)); \
} while (0)
#define API(wanted) EXPECT(orm_driver_validate_api_v1(&f.api, sizeof(f.api), \
    bundle, driver_id, 2u, NULL), wanted)
#define HOST(wanted) EXPECT(orm_driver_validate_host_v1(&f.host, sizeof(f.host), \
    bundle, NULL), wanted)
#define TX(wanted) EXPECT(orm_driver_validate_transaction_v1(&o, sizeof(o), \
    caps, NULL), wanted)
#define CURSOR(wanted) EXPECT(orm_driver_validate_cursor_v1(&o, sizeof(o), \
    ORM_DRIVER_CAP_SELECT, NULL), wanted)

spec("driver descriptor validation") {
  (void)ttest_config__;
  it("accepts a complete descriptor without running callbacks") {
    descriptor_fixture f; setup(&f);
    API(ORM_STATUS_OK);
    HOST(ORM_STATUS_OK);
  }
  it("allows absent host execution services for a blocking consumer") {
    descriptor_fixture f; setup(&f);
    f.host.execution = (orm_driver_table_v1){NULL, 0u, 0u};
    HOST(ORM_STATUS_OK);
  }
  it("rejects every physically short main descriptor allocation") {
    descriptor_fixture f; setup(&f);
    for (uint32_t n = 0u; n < sizeof(f.api); ++n) {
      void *p = malloc(n == 0u ? 1u : n);
      orm_status_t status;
      check_not_null(p);
      memcpy(p, &f.api, n);
      status = orm_driver_validate_api_v1(p, n, bundle, driver_id, 2u, NULL);
      free(p);
      EXPECT(status, ORM_STATUS_ABI_MISMATCH);
    }
  }
  it("rejects every physically short host descriptor allocation") {
    descriptor_fixture f; setup(&f);
    for (uint32_t n = 0u; n < sizeof(f.host); ++n) {
      void *p = malloc(n == 0u ? 1u : n);
      orm_status_t status;
      check_not_null(p);
      memcpy(p, &f.host, n);
      status = orm_driver_validate_host_v1(p, n, bundle, NULL);
      free(p);
      EXPECT(status, ORM_STATUS_ABI_MISMATCH);
    }
  }
  it("rejects null inputs and null expected bundle safely") {
    descriptor_fixture f; setup(&f);
    EXPECT(orm_driver_validate_api_v1(NULL, 0u, bundle, driver_id, 2u, NULL),
           ORM_STATUS_INVALID_ARGUMENT);
    EXPECT(orm_driver_validate_host_v1(NULL, 0u, bundle, NULL), ORM_STATUS_INVALID_ARGUMENT);
    EXPECT(orm_driver_validate_api_v1(&f.api, sizeof(f.api), NULL, driver_id, 2u, NULL),
           ORM_STATUS_INVALID_ARGUMENT);
    EXPECT(orm_driver_validate_host_v1(&f.host, sizeof(f.host), NULL, NULL),
           ORM_STATUS_INVALID_ARGUMENT);
  }
  it("rejects incompatible main versions") {
    descriptor_fixture f; setup(&f);
    f.api.header.abi_version = 2u;
    f.host.header.abi_version = 2u;
    API(ORM_STATUS_ABI_MISMATCH);
    HOST(ORM_STATUS_ABI_MISMATCH);
  }
  it("compares all thirty two bundle bytes") {
    descriptor_fixture f;
    for (size_t i = 0u; i < sizeof(bundle); ++i) {
      setup(&f);
      f.api.bundle_id[i] ^= 1u;
      f.host.bundle_id[i] ^= 1u;
      API(ORM_STATUS_ABI_MISMATCH);
      HOST(ORM_STATUS_ABI_MISMATCH);
    }
  }
  it("reads unaligned main descriptors without a typed dereference") {
    descriptor_fixture f; setup(&f);
    unsigned char *p = malloc(sizeof(f.api) + 1u);
    orm_status_t status;
    check_not_null(p);
    memcpy(p + 1u, &f.api, sizeof(f.api));
    status = orm_driver_validate_api_v1(p + 1u, sizeof(f.api), bundle, driver_id, 2u, NULL);
    free(p);
    EXPECT(status, ORM_STATUS_OK);
  }
  it("ignores declared future tail bytes") {
    descriptor_fixture f; setup(&f);
    const uint32_t size = sizeof(f.api) + 7u;
    unsigned char *p = malloc(size);
    orm_status_t status;
    check_not_null(p);
    memset(p, 0xff, size);
    f.api.header.struct_size = size;
    memcpy(p, &f.api, sizeof(f.api));
    status = orm_driver_validate_api_v1(p, size, bundle, driver_id, 2u, NULL);
    free(p);
    EXPECT(status, ORM_STATUS_OK);
  }
  it("rejects malformed canonical IDs byte by byte") {
    const orm_driver_bytes_v1 bad[] = {
      {NULL, 1u}, {"", 0u}, {"UPPER", 5u}, {"0name", 5u}, {"has.dot", 7u},
      {"a\0b", 3u}, {"a\xff", 2u}, {"with space", 10u}};
    descriptor_fixture f;
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
      setup(&f); f.api.canonical_id = bad[i];
      API(ORM_STATUS_INVALID_ARGUMENT);
    }
  }
  it("accepts sixty three ID bytes and rejects sixty four") {
    descriptor_fixture f; setup(&f);
    char id[64]; memset(id, 'a', sizeof(id));
    f.api.canonical_id = (orm_driver_bytes_v1){id, 63u};
    EXPECT(orm_driver_validate_api_v1(&f.api, sizeof(f.api), bundle,
        f.api.canonical_id, 2u, NULL), ORM_STATUS_OK);
    f.api.canonical_id.size = 64u;
    EXPECT(orm_driver_validate_api_v1(&f.api, sizeof(f.api), bundle,
        f.api.canonical_id, 2u, NULL), ORM_STATUS_LIMIT_EXCEEDED);
  }
  it("rejects a different expected canonical ID") {
    descriptor_fixture f; setup(&f);
    const orm_driver_bytes_v1 expected = {"other", 5u};
    EXPECT(orm_driver_validate_api_v1(&f.api, sizeof(f.api), bundle,
        expected, 2u, NULL), ORM_STATUS_INVALID_ARGUMENT);
  }
  it("checks alias budget before accessing the array") {
    descriptor_fixture f; setup(&f);
    f.api.alias_count = 3u;
    API(ORM_STATUS_LIMIT_EXCEEDED);
    f.api.alias_count = UINT32_MAX;
    EXPECT(orm_driver_validate_api_v1(&f.api, sizeof(f.api), bundle,
        driver_id, UINT32_MAX, NULL), ORM_STATUS_LIMIT_EXCEEDED);
  }
  it("rejects a null nonempty alias array") {
    descriptor_fixture f; setup(&f); f.api.aliases = NULL;
    API(ORM_STATUS_INVALID_ARGUMENT);
  }
  it("accepts no aliases at a zero budget") {
    descriptor_fixture f; setup(&f);
    f.api.alias_count = 0u; f.api.aliases = NULL;
    EXPECT(orm_driver_validate_api_v1(&f.api, sizeof(f.api), bundle,
        driver_id, 0u, NULL), ORM_STATUS_OK);
  }
  it("rejects duplicate aliases and aliases equal to canonical ID") {
    descriptor_fixture f; setup(&f);
    f.aliases[1] = f.aliases[0]; API(ORM_STATUS_INVALID_ARGUMENT);
    setup(&f); f.aliases[0] = driver_id; API(ORM_STATUS_INVALID_ARGUMENT);
    setup(&f); f.aliases[0] = (orm_driver_bytes_v1){"BAD", 3u};
    API(ORM_STATUS_INVALID_ARGUMENT);
  }
  it("rejects nonzero main and table reserved fields") {
    descriptor_fixture f; setup(&f);
    f.api.reserved = 1u; API(ORM_STATUS_INVALID_ARGUMENT);
    setup(&f); f.api.module_ops.reserved = 1u; API(ORM_STATUS_INVALID_ARGUMENT);
    setup(&f); f.api.connection_ops.reserved = 1u; API(ORM_STATUS_INVALID_ARGUMENT);
    setup(&f); f.host.execution.reserved = 1u; HOST(ORM_STATUS_INVALID_ARGUMENT);
  }
  it("requires both module lifecycle callbacks") {
    descriptor_fixture f; setup(&f);
    f.module.initialize = NULL; API(ORM_STATUS_ABI_MISMATCH);
    setup(&f); f.module.finalize = NULL; API(ORM_STATUS_ABI_MISMATCH);
  }
  it("requires a factory and connection destroy callback") {
    descriptor_fixture f; setup(&f);
    f.api.create_connection = NULL; API(ORM_STATUS_ABI_MISMATCH);
    setup(&f); f.connection.destroy = NULL; API(ORM_STATUS_ABI_MISMATCH);
  }
  it("requires row and command callbacks for raw SQL") {
    descriptor_fixture f; setup(&f); f.api.capabilities = ORM_DRIVER_CAP_RAW_SQL;
    f.connection.open_cursor = NULL; API(ORM_STATUS_ABI_MISMATCH);
    f.connection.open_cursor = trap_open; f.connection.execute_command = NULL;
    API(ORM_STATUS_ABI_MISMATCH);
  }
  it("requires open_cursor for SELECT") {
    descriptor_fixture f; setup(&f); f.api.capabilities = ORM_DRIVER_CAP_SELECT;
    f.connection.open_cursor = NULL; API(ORM_STATUS_ABI_MISMATCH);
  }
  it("requires execute_command for every declared write kind") {
    const uint64_t kinds[] = {ORM_DRIVER_CAP_INSERT, ORM_DRIVER_CAP_UPDATE,
                             ORM_DRIVER_CAP_DELETE};
    descriptor_fixture f;
    for (size_t i = 0u; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
      setup(&f); f.api.capabilities = kinds[i];
      f.connection.execute_command = NULL; API(ORM_STATUS_ABI_MISMATCH);
    }
  }
  it("requires begin_transaction for transactions") {
    descriptor_fixture f; setup(&f); f.api.capabilities = ORM_DRIVER_CAP_TRANSACTION;
    f.connection.begin_transaction = NULL; API(ORM_STATUS_ABI_MISMATCH);
  }
  it("requires transaction capability for every isolation level") {
    descriptor_fixture f;
    for (uint32_t bit = 8u; bit <= 12u; ++bit) {
      setup(&f); f.api.capabilities = UINT64_C(1) << bit;
      API(ORM_STATUS_ABI_MISMATCH);
    }
  }
  it("rejects savepoint capability without transaction capability") {
    descriptor_fixture f; setup(&f); f.api.capabilities = ORM_DRIVER_CAP_SAVEPOINT;
    API(ORM_STATUS_ABI_MISMATCH);
  }
  it("rejects incremental rows without a row producing operation") {
    descriptor_fixture f; setup(&f); f.api.capabilities = ORM_DRIVER_CAP_INCREMENTAL_ROWS;
    API(ORM_STATUS_ABI_MISMATCH);
  }
  it("rejects unknown capability and execution bits") {
    descriptor_fixture f; setup(&f);
    f.api.capabilities |= UINT64_C(1) << 63; API(ORM_STATUS_UNSUPPORTED);
    setup(&f); f.api.execution_models |= UINT64_C(1) << 63; API(ORM_STATUS_UNSUPPORTED);
  }
  it("requires a declared execution model") {
    descriptor_fixture f; setup(&f); f.api.execution_models = 0u;
    API(ORM_STATUS_ABI_MISMATCH);
  }
  it("checks physical subtable bounds before callbacks") {
    descriptor_fixture f; setup(&f);
    for (uint32_t n = 0u; n < sizeof(f.module); ++n) {
      void *p = malloc(n == 0u ? 1u : n); orm_status_t status;
      check_not_null(p); memcpy(p, &f.module, n);
      f.api.module_ops = (orm_driver_table_v1){p, n, 0u};
      status = orm_driver_validate_api_v1(&f.api, sizeof(f.api), bundle, driver_id, 2u, NULL);
      free(p); EXPECT(status, ORM_STATUS_ABI_MISMATCH);
    }
  }
  it("accepts a physically short connection table when omitted capabilities permit it") {
    descriptor_fixture f; setup(&f);
    const uint32_t n = END(orm_driver_connection_ops_v1, open_cursor);
    void *p = malloc(n); orm_status_t status;
    check_not_null(p);
    f.connection.header.struct_size = n;
    memcpy(p, &f.connection, n);
    f.api.capabilities = ORM_DRIVER_CAP_SELECT;
    f.api.connection_ops = (orm_driver_table_v1){p, n, 0u};
    status = orm_driver_validate_api_v1(&f.api, sizeof(f.api), bundle, driver_id, 2u, NULL);
    free(p); EXPECT(status, ORM_STATUS_OK);
  }
  it("rejects incompatible nested table versions") {
    descriptor_fixture f; setup(&f);
    f.module.header.abi_version = 2u; API(ORM_STATUS_ABI_MISMATCH);
    setup(&f); f.connection.header.abi_version = 2u; API(ORM_STATUS_ABI_MISMATCH);
    setup(&f); f.metadata.header.abi_version = 2u; HOST(ORM_STATUS_ABI_MISMATCH);
    setup(&f); f.values.header.abi_version = 2u; HOST(ORM_STATUS_ABI_MISMATCH);
    setup(&f); f.lifetime.header.abi_version = 2u; HOST(ORM_STATUS_ABI_MISMATCH);
    setup(&f); f.execution.header.abi_version = 2u; HOST(ORM_STATUS_ABI_MISMATCH);
  }
  it("requires each host plan accessor") {
    descriptor_fixture f; setup(&f);
    f.metadata.describe = NULL; HOST(ORM_STATUS_ABI_MISMATCH);
    setup(&f); f.metadata.column_at = NULL; HOST(ORM_STATUS_ABI_MISMATCH);
    setup(&f); f.metadata.ordering = NULL; HOST(ORM_STATUS_ABI_MISMATCH);
    setup(&f); f.values.assignment_at = NULL; HOST(ORM_STATUS_ABI_MISMATCH);
    setup(&f); f.values.predicate_at = NULL; HOST(ORM_STATUS_ABI_MISMATCH);
    setup(&f); f.values.raw_parameter_at = NULL; HOST(ORM_STATUS_ABI_MISMATCH);
  }
  it("requires both host lease callbacks") {
    descriptor_fixture f; setup(&f);
    f.lifetime.acquire = NULL; HOST(ORM_STATUS_ABI_MISMATCH);
    setup(&f); f.lifetime.release = NULL; HOST(ORM_STATUS_ABI_MISMATCH);
  }
  it("requires all callbacks when host execution services are present") {
    descriptor_fixture f; setup(&f);
    f.execution.submit = NULL; HOST(ORM_STATUS_ABI_MISMATCH);
    setup(&f); f.execution.request_cancel = NULL; HOST(ORM_STATUS_ABI_MISMATCH);
    setup(&f); f.execution.release_task = NULL; HOST(ORM_STATUS_ABI_MISMATCH);
  }
  it("does not treat a malformed optional executor as absent") {
    descriptor_fixture f; setup(&f);
    f.host.execution.bytes = 0u; HOST(ORM_STATUS_ABI_MISMATCH);
    setup(&f); f.host.execution.data = NULL; HOST(ORM_STATUS_INVALID_ARGUMENT);
  }
  it("keeps descriptors byte for byte unchanged") {
    descriptor_fixture f, before; setup(&f); before = f;
    API(ORM_STATUS_OK); HOST(ORM_STATUS_OK);
    check_equal(memcmp(&f, &before, sizeof(f)), 0);
  }
  it("initializes optional diagnostics on both success and failure") {
    descriptor_fixture f; setup(&f); orm_error_t error;
    memset(&error, 0xff, sizeof(error));
    EXPECT(orm_driver_validate_host_v1(&f.host, sizeof(f.host), bundle, &error), ORM_STATUS_OK);
    check_equal(error.struct_size, (uint32_t)sizeof(error));
    check_equal(error.status, (orm_status_t)ORM_STATUS_OK);
    check_equal(error.message[0], '\0');
    f.api.header.abi_version = 2u;
    EXPECT(orm_driver_validate_api_v1(&f.api, sizeof(f.api), bundle,
        driver_id, 2u, &error), ORM_STATUS_ABI_MISMATCH);
    check_equal(error.status, (orm_status_t)ORM_STATUS_ABI_MISMATCH);
    check_equal(error.message[ORM_C_ERROR_MESSAGE_CAPACITY - 1u], '\0');
  }
}

spec("driver returned object validation") {
  (void)ttest_config__;
  it("validates a returned connection against declared capabilities") {
    descriptor_fixture f; setup(&f);
    orm_driver_connection_v1 o = {HEADER(orm_driver_connection_v1), &f, TABLE(&f.connection)};
    EXPECT(orm_driver_validate_connection_v1(&o, sizeof(o), f.api.capabilities, NULL), ORM_STATUS_OK);
    f.connection.begin_transaction = NULL;
    EXPECT(orm_driver_validate_connection_v1(&o, sizeof(o), f.api.capabilities, NULL), ORM_STATUS_ABI_MISMATCH);
  }
  it("requires transaction destroy commit and rollback") {
    descriptor_fixture f; setup(&f);
    uint64_t caps = ORM_DRIVER_CAP_TRANSACTION;
    orm_driver_transaction_v1 o = {HEADER(orm_driver_transaction_v1), &f, TABLE(&f.transaction)};
    TX(ORM_STATUS_OK);
    f.transaction.destroy = NULL; TX(ORM_STATUS_ABI_MISMATCH);
    f.transaction.destroy = trap_void; f.transaction.commit = NULL; TX(ORM_STATUS_ABI_MISMATCH);
    f.transaction.commit = trap_finish; f.transaction.rollback = NULL; TX(ORM_STATUS_ABI_MISMATCH);
  }
  it("requires all three savepoint callbacks as a group") {
    descriptor_fixture f; setup(&f);
    uint64_t caps = ORM_DRIVER_CAP_TRANSACTION | ORM_DRIVER_CAP_SAVEPOINT;
    orm_driver_transaction_v1 o = {HEADER(orm_driver_transaction_v1), &f, TABLE(&f.transaction)};
    TX(ORM_STATUS_OK);
    f.transaction.savepoint = NULL; TX(ORM_STATUS_ABI_MISMATCH);
    f.transaction.savepoint = trap_savepoint; f.transaction.rollback_to_savepoint = NULL;
    TX(ORM_STATUS_ABI_MISMATCH);
    f.transaction.rollback_to_savepoint = trap_savepoint; f.transaction.release_savepoint = NULL;
    TX(ORM_STATUS_ABI_MISMATCH);
    caps = ORM_DRIVER_CAP_TRANSACTION; TX(ORM_STATUS_ABI_MISMATCH);
    f.transaction.savepoint = NULL; f.transaction.rollback_to_savepoint = NULL;
    TX(ORM_STATUS_OK);
  }
  it("allows a transaction table ending after rollback without savepoints") {
    descriptor_fixture f; setup(&f);
    uint64_t caps = ORM_DRIVER_CAP_TRANSACTION;
    const uint32_t n = END(orm_driver_transaction_ops_v1, rollback);
    void *p = malloc(n); orm_status_t status;
    check_not_null(p); f.transaction.header.struct_size = n;
    memcpy(p, &f.transaction, n);
    orm_driver_transaction_v1 o = {HEADER(orm_driver_transaction_v1), &f, {p, n, 0u}};
    status = orm_driver_validate_transaction_v1(&o, sizeof(o), caps, NULL);
    free(p); EXPECT(status, ORM_STATUS_OK);
  }
  it("requires transaction row and write callbacks for declared operations") {
    descriptor_fixture f; setup(&f);
    uint64_t caps = ORM_DRIVER_CAP_TRANSACTION | ORM_DRIVER_CAP_SELECT;
    orm_driver_transaction_v1 o = {HEADER(orm_driver_transaction_v1), &f, TABLE(&f.transaction)};
    f.transaction.open_cursor = NULL; TX(ORM_STATUS_ABI_MISMATCH);
    caps = ORM_DRIVER_CAP_TRANSACTION | ORM_DRIVER_CAP_UPDATE;
    f.transaction.open_cursor = trap_open; f.transaction.execute_command = NULL;
    TX(ORM_STATUS_ABI_MISMATCH);
  }
  it("requires cursor next cancel and destroy") {
    descriptor_fixture f; setup(&f);
    orm_driver_cursor_v1 o = {HEADER(orm_driver_cursor_v1), &f, TABLE(&f.cursor)};
    CURSOR(ORM_STATUS_OK);
    f.cursor.next = NULL; CURSOR(ORM_STATUS_ABI_MISMATCH);
    f.cursor.next = trap_next; f.cursor.cancel = NULL; CURSOR(ORM_STATUS_ABI_MISMATCH);
    f.cursor.cancel = trap_void; f.cursor.destroy = NULL; CURSOR(ORM_STATUS_ABI_MISMATCH);
  }
  it("allows cursor tables ending before optional shape callbacks") {
    descriptor_fixture f; setup(&f);
    const uint32_t min = END(orm_driver_cursor_ops_v1, destroy);
    for (uint32_t n = min; n < sizeof(f.cursor); ++n) {
      void *p = malloc(n); orm_status_t status;
      check_not_null(p); f.cursor.header.struct_size = n;
      memcpy(p, &f.cursor, n);
      orm_driver_cursor_v1 o = {HEADER(orm_driver_cursor_v1), &f, {p, n, 0u}};
      status = orm_driver_validate_cursor_v1(&o, sizeof(o), ORM_DRIVER_CAP_SELECT, NULL);
      free(p); EXPECT(status, ORM_STATUS_OK);
    }
  }
  it("rejects every physically short returned object") {
    descriptor_fixture f; setup(&f);
    orm_driver_connection_v1 c = {HEADER(orm_driver_connection_v1), &f, TABLE(&f.connection)};
    orm_driver_transaction_v1 t = {HEADER(orm_driver_transaction_v1), &f, TABLE(&f.transaction)};
    orm_driver_cursor_v1 r = {HEADER(orm_driver_cursor_v1), &f, TABLE(&f.cursor)};
    for (uint32_t n = 0u; n < sizeof(c); ++n) {
      void *p = malloc(n == 0u ? 1u : n); orm_status_t a, b, d;
      check_not_null(p); memcpy(p, &c, n);
      a = orm_driver_validate_connection_v1(p, n, f.api.capabilities, NULL);
      memcpy(p, &t, n); b = orm_driver_validate_transaction_v1(p, n, f.api.capabilities, NULL);
      memcpy(p, &r, n); d = orm_driver_validate_cursor_v1(p, n, ORM_DRIVER_CAP_SELECT, NULL);
      free(p);
      EXPECT(a, ORM_STATUS_ABI_MISMATCH); EXPECT(b, ORM_STATUS_ABI_MISMATCH);
      EXPECT(d, ORM_STATUS_ABI_MISMATCH);
    }
  }
  it("rejects unknown returned object capability bits") {
    descriptor_fixture f; setup(&f);
    const uint64_t unknown = UINT64_C(1) << 63;
    orm_driver_connection_v1 c = {HEADER(orm_driver_connection_v1), &f, TABLE(&f.connection)};
    orm_driver_transaction_v1 t = {HEADER(orm_driver_transaction_v1), &f, TABLE(&f.transaction)};
    orm_driver_cursor_v1 r = {HEADER(orm_driver_cursor_v1), &f, TABLE(&f.cursor)};
    EXPECT(orm_driver_validate_connection_v1(&c, sizeof(c), unknown, NULL), ORM_STATUS_UNSUPPORTED);
    EXPECT(orm_driver_validate_transaction_v1(&t, sizeof(t), unknown, NULL), ORM_STATUS_UNSUPPORTED);
    EXPECT(orm_driver_validate_cursor_v1(&r, sizeof(r), unknown, NULL), ORM_STATUS_UNSUPPORTED);
  }
}

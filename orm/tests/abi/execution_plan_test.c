#include "orm_internal.h"

#include <cserde/cserde.h>
#include <tinytest.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct explain_reader_state {
  const cserde_token *tokens;
  size_t count;
  size_t index;
} explain_reader_state;

typedef struct explain_fixture_row {
  const cserde_token *tokens;
  size_t count;
} explain_fixture_row;

typedef struct explain_cursor {
  explain_reader_state reader;
  const explain_fixture_row *rows;
  size_t row_count;
  size_t row_index;
  uint64_t column_count;
} explain_cursor;

typedef struct explain_backend {
  char provider[16];
} explain_backend;

static cserde_status explain_reader_next(void *context, cserde_token *out) {
  explain_reader_state *state = (explain_reader_state *)context;
  if (state == NULL || out == NULL) return CSERDE_INVALID_ARGUMENT;
  if (state->index == state->count) return CSERDE_DONE;
  *out = state->tokens[state->index++];
  return CSERDE_OK;
}

static const cserde_reader_ops explain_reader_ops = {
    sizeof(cserde_reader_ops), CSERDE_READER_OPS_ABI_VERSION,
    explain_reader_next};

#define TEXT_TOKEN(text_) \
  {.kind = CSERDE_STRING, \
   .value.slice = {(const unsigned char *)(text_), sizeof(text_) - 1u, \
                   CSERDE_VIEW_STABLE}}

static const cserde_token mysql_plan_row[] = {
    {.kind = CSERDE_MAP_BEGIN},
    TEXT_TOKEN("select_type"), TEXT_TOKEN("SIMPLE"),
    TEXT_TOKEN("type"), TEXT_TOKEN("ref"),
    TEXT_TOKEN("table"), TEXT_TOKEN("users"),
    TEXT_TOKEN("key"), TEXT_TOKEN("idx_users_name"),
    TEXT_TOKEN("rows"), {.kind = CSERDE_UINT, .value.uint = 3u},
    TEXT_TOKEN("Extra"), TEXT_TOKEN("Using index"),
    {.kind = CSERDE_MAP_END}};

static const cserde_token mysql_analyze_row[] = {
    {.kind = CSERDE_MAP_BEGIN},
    TEXT_TOKEN("EXPLAIN"),
    TEXT_TOKEN(
        "-> Nested loop inner join  (cost=1.00..9.00 rows=2)\n"
        "    -> Index lookup on users using idx_users_name "
        "(cost=0.50..4.00 rows=1) "
        "(actual time=0.10..0.20 rows=1 loops=1)"),
    {.kind = CSERDE_MAP_END}};

static const cserde_token pg_plan_row0[] = {
    {.kind = CSERDE_MAP_BEGIN},
    TEXT_TOKEN("QUERY PLAN"),
    TEXT_TOKEN("Nested Loop  (cost=0.29..10.65 rows=1 width=36)"),
    {.kind = CSERDE_MAP_END}};
static const cserde_token pg_plan_row1[] = {
    {.kind = CSERDE_MAP_BEGIN},
    TEXT_TOKEN("QUERY PLAN"),
    TEXT_TOKEN(
        "  -> Index Scan using idx_users_name on users  "
        "(cost=0.15..4.17 rows=1 width=32)"),
    {.kind = CSERDE_MAP_END}};
static const cserde_token pg_plan_row2[] = {
    {.kind = CSERDE_MAP_BEGIN},
    TEXT_TOKEN("QUERY PLAN"),
    TEXT_TOKEN("  -> Seq Scan on teams  (cost=0.00..6.00 rows=1 width=4)"),
    {.kind = CSERDE_MAP_END}};

static const cserde_token pg_analyze_row0[] = {
    {.kind = CSERDE_MAP_BEGIN},
    TEXT_TOKEN("QUERY PLAN"),
    TEXT_TOKEN(
        "Seq Scan on users  (cost=0.00..35.50 rows=2550 width=36) "
        "(actual time=0.02..0.08 rows=2 loops=1)"),
    {.kind = CSERDE_MAP_END}};
static const cserde_token pg_analyze_row1[] = {
    {.kind = CSERDE_MAP_BEGIN},
    TEXT_TOKEN("QUERY PLAN"),
    TEXT_TOKEN("Execution Time: 0.12 ms"),
    {.kind = CSERDE_MAP_END}};

static const explain_fixture_row mysql_plan_rows[] = {
    {mysql_plan_row, sizeof(mysql_plan_row) / sizeof(mysql_plan_row[0])}};
static const explain_fixture_row mysql_analyze_rows[] = {
    {mysql_analyze_row,
     sizeof(mysql_analyze_row) / sizeof(mysql_analyze_row[0])}};
static const explain_fixture_row pg_plan_rows[] = {
    {pg_plan_row0, sizeof(pg_plan_row0) / sizeof(pg_plan_row0[0])},
    {pg_plan_row1, sizeof(pg_plan_row1) / sizeof(pg_plan_row1[0])},
    {pg_plan_row2, sizeof(pg_plan_row2) / sizeof(pg_plan_row2[0])}};
static const explain_fixture_row pg_analyze_rows[] = {
    {pg_analyze_row0, sizeof(pg_analyze_row0) / sizeof(pg_analyze_row0[0])},
    {pg_analyze_row1, sizeof(pg_analyze_row1) / sizeof(pg_analyze_row1[0])}};

static orm_row_cursor_step explain_cursor_next(
    void *context, cserde_reader *reader) {
  explain_cursor *cursor = (explain_cursor *)context;
  orm_row_cursor_step step = ORM_ROW_CURSOR_STEP_INIT;
  if (cursor == NULL || reader == NULL) {
    step.kind = ORM_ROW_CURSOR_ERROR;
    step.status = ORM_STATUS_INVALID_ARGUMENT;
    step.message = "invalid explain fixture cursor";
    return step;
  }
  if (cursor->row_index == cursor->row_count) {
    step.kind = ORM_ROW_CURSOR_DONE;
    return step;
  }
  const explain_fixture_row *row = &cursor->rows[cursor->row_index++];
  cursor->reader.tokens = row->tokens;
  cursor->reader.count = row->count;
  cursor->reader.index = 0u;
  if (cserde_reader_init(reader, &explain_reader_ops, &cursor->reader) !=
      CSERDE_OK) {
    step.kind = ORM_ROW_CURSOR_ERROR;
    step.status = ORM_STATUS_INTERNAL_ERROR;
    step.message = "initialize explain fixture reader";
    return step;
  }
  step.kind = cursor->row_index == cursor->row_count
                  ? ORM_ROW_CURSOR_ROW_AND_DONE
                  : ORM_ROW_CURSOR_ROW;
  return step;
}

static void explain_cursor_cancel(void *context) { (void)context; }
static void explain_cursor_destroy(void *context) { free(context); }

static orm_status_t explain_cursor_columns(void *context, uint64_t *out) {
  if (context == NULL || out == NULL) return ORM_STATUS_INVALID_ARGUMENT;
  *out = ((const explain_cursor *)context)->column_count;
  return ORM_STATUS_OK;
}

static const orm_row_cursor_ops explain_cursor_ops = {
    sizeof(orm_row_cursor_ops), ORM_ROW_CURSOR_OPS_ABI_VERSION,
    "execution-plan-fixture", explain_cursor_next, explain_cursor_cancel,
    explain_cursor_destroy, NULL, explain_cursor_columns};

static int starts_with(const char *text, const char *prefix) {
  return text != NULL &&
         strncmp(text, prefix, strlen(prefix)) == 0;
}

static orm_status_t explain_fake_open(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    orm_row_cursor *out, orm_error_t *error) {
  (void)limits;
  explain_backend *backend = (explain_backend *)context;
  if (out != NULL) memset(out, 0, sizeof(*out));
  if (backend == NULL || plan == NULL || out == NULL ||
      plan->kind != ORM_QUERY_RAW || plan->raw_sql == NULL)
    return ORM_STATUS_INVALID_ARGUMENT;

  const explain_fixture_row *rows = NULL;
  size_t row_count = 0u;
  uint64_t columns = 0u;
  const char *sql = plan->raw_sql;

  if (strcmp(backend->provider, "mysql") == 0) {
    if (starts_with(sql, "EXPLAIN ANALYZE ")) {
      rows = mysql_analyze_rows;
      row_count = sizeof(mysql_analyze_rows) / sizeof(mysql_analyze_rows[0]);
      columns = 1u;
    } else if (starts_with(sql, "EXPLAIN ")) {
      rows = mysql_plan_rows;
      row_count = sizeof(mysql_plan_rows) / sizeof(mysql_plan_rows[0]);
      columns = 6u;
    }
  } else if (strcmp(backend->provider, "postgresql") == 0) {
    if (starts_with(sql, "EXPLAIN (ANALYZE TRUE, FORMAT TEXT) ")) {
      rows = pg_analyze_rows;
      row_count = sizeof(pg_analyze_rows) / sizeof(pg_analyze_rows[0]);
      columns = 1u;
    } else if (starts_with(sql, "EXPLAIN (FORMAT TEXT) ")) {
      rows = pg_plan_rows;
      row_count = sizeof(pg_plan_rows) / sizeof(pg_plan_rows[0]);
      columns = 1u;
    }
  }

  if (rows == NULL) {
    orm_error_set(error, ORM_STATUS_SQL_ERROR,
                  "fixture received unexpected EXPLAIN SQL");
    return ORM_STATUS_SQL_ERROR;
  }

  explain_cursor *cursor =
      (explain_cursor *)calloc(1u, sizeof(*cursor));
  if (cursor == NULL) return ORM_STATUS_OUT_OF_MEMORY;
  cursor->rows = rows;
  cursor->row_count = row_count;
  cursor->column_count = columns;
  out->ops = &explain_cursor_ops;
  out->context = cursor;
  orm_error_init(error);
  return ORM_STATUS_OK;
}

static orm_status_t explain_fake_command(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    uint64_t *affected, orm_error_t *error) {
  (void)context;
  (void)plan;
  (void)limits;
  if (affected != NULL) *affected = 0u;
  orm_error_set(error, ORM_STATUS_UNSUPPORTED,
                "fixture command execution is unsupported");
  return ORM_STATUS_UNSUPPORTED;
}

static orm_status_t explain_fake_begin(
    void *context, orm_isolation_t isolation,
    orm_transaction_backend *out, orm_error_t *error) {
  (void)context;
  (void)isolation;
  if (out != NULL) memset(out, 0, sizeof(*out));
  orm_error_set(error, ORM_STATUS_UNSUPPORTED,
                "fixture transactions are unsupported");
  return ORM_STATUS_UNSUPPORTED;
}

static void explain_fake_destroy(void *context) { free(context); }

static const orm_backend_ops explain_fake_ops = {
    sizeof(orm_backend_ops), ORM_BACKEND_OPS_ABI_VERSION,
    explain_fake_destroy, explain_fake_open, explain_fake_command,
    explain_fake_begin, NULL};

static orm_status_t explain_fake_factory(
    const orm_config_t *config, const orm_limits *limits,
    orm_backend *out, orm_error_t *error) {
  (void)limits;
  if (config == NULL || out == NULL) return ORM_STATUS_INVALID_ARGUMENT;
  explain_backend *backend =
      (explain_backend *)calloc(1u, sizeof(*backend));
  if (backend == NULL) return ORM_STATUS_OUT_OF_MEMORY;
  if (config->driver.len >= sizeof(backend->provider)) {
    free(backend);
    return ORM_STATUS_LIMIT_EXCEEDED;
  }
  memcpy(backend->provider, config->driver.data, config->driver.len);
  backend->provider[config->driver.len] = '\0';
  out->ops = &explain_fake_ops;
  out->context = backend;
  orm_error_init(error);
  return ORM_STATUS_OK;
}

static orm_connection_t *open_fake(const char *provider,
                                   orm_error_t *error) {
  orm_config_t config;
  orm_connection_t *connection = NULL;
  orm_config(&config);
  config.driver = orm_view(provider);
  check_equal(
      orm_connect_with_factory_v1(
          &config, explain_fake_factory, &connection, error),
      ORM_STATUS_OK);
  return connection;
}

static orm_status_t execute_sql(orm_connection_t *connection,
                                const char *sql,
                                orm_error_t *error) {
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  orm_status_t status =
      orm_raw(connection, orm_view(sql), &query, error);
  if (status == ORM_STATUS_OK)
    status = orm_query_execute(query, &result, error);
  orm_result_destroy(result);
  orm_query_destroy(query);
  return status;
}

static int view_equals(orm_string_view_t view, const char *text) {
  const size_t size = strlen(text);
  return view.len == size &&
         (size == 0u || memcmp(view.data, text, size) == 0);
}

static orm_execution_plan_node_t plan_node(
    orm_execution_plan_t *plan, uint64_t index, orm_error_t *error) {
  orm_execution_plan_node_t node = ORM_EXECUTION_PLAN_NODE_INIT;
  check_equal(orm_execution_plan_node(plan, index, &node, error),
              ORM_STATUS_OK);
  return node;
}

spec("ORM provider-neutral execution plans") {
  it("normalizes SQLite EXPLAIN QUERY PLAN and rejects ANALYZE semantics") {
    orm_config_t config;
    orm_option_t filename;
    orm_connection_t *connection = NULL;
    orm_execution_plan_t *plan = NULL;
    orm_error_t error;
    uint64_t count = 0u;
    int saw_users = 0;
    int saw_index = 0;

    orm_error_init(&error);
    orm_config(&config);
    filename.keyword = orm_view("filename");
    filename.value = orm_view(":memory:");
    config.driver = orm_view("sqlite");
    config.options = &filename;
    config.option_count = 1u;

    check_equal(orm_connect(&config, &connection, &error), ORM_STATUS_OK);
    check_not_null(connection);
    check_equal(
        execute_sql(connection,
                    "create table users(id integer primary key, name text)",
                    &error),
        ORM_STATUS_OK);
    check_equal(
        execute_sql(connection,
                    "create index idx_users_name on users(name)", &error),
        ORM_STATUS_OK);

    check_equal(
        orm_query_explain(
            connection, orm_view("select * from users where name='Ada'"),
            ORM_EXPLAIN_PLAN, &plan, &error),
        ORM_STATUS_OK);
    check_not_null(plan);
    check_equal(
        orm_execution_plan_node_count(plan, &count, &error), ORM_STATUS_OK);
    check_true(count > 0u);
    for (uint64_t i = 0u; i < count; ++i) {
      const orm_execution_plan_node_t node = plan_node(plan, i, &error);
      if (view_equals(node.relation, "users")) saw_users = 1;
      if (view_equals(node.index_name, "idx_users_name")) saw_index = 1;
    }
    check_true(saw_users);
    check_true(saw_index);
    orm_execution_plan_destroy(plan);
    plan = NULL;

    check_equal(
        orm_query_explain(
            connection, orm_view("select * from users"),
            ORM_EXPLAIN_ANALYZE, &plan, &error),
        ORM_STATUS_UNSUPPORTED);
    check_null(plan);
    orm_disconnect(connection);
  }

  it("normalizes classic MySQL PLAN rows") {
    orm_error_t error;
    orm_execution_plan_t *plan = NULL;
    uint64_t count = 0u;
    orm_error_init(&error);
    orm_connection_t *connection = open_fake("mysql", &error);
    check_not_null(connection);

    check_equal(
        orm_query_explain(
            connection, orm_view("select * from users"),
            ORM_EXPLAIN_PLAN, &plan, &error),
        ORM_STATUS_OK);
    check_equal(
        orm_execution_plan_node_count(plan, &count, &error), ORM_STATUS_OK);
    check_equal(count, UINT64_C(1));
    const orm_execution_plan_node_t node = plan_node(plan, 0u, &error);
    check_true(view_equals(node.node_type, "ref"));
    check_true(view_equals(node.relation, "users"));
    check_true(view_equals(node.index_name, "idx_users_name"));
    check_true((node.flags & ORM_PLAN_NODE_HAS_ESTIMATED_ROWS) != 0u);
    check_true(node.estimated_rows == 3.0);

    orm_execution_plan_destroy(plan);
    orm_disconnect(connection);
  }

  it("normalizes MySQL ANALYZE text without hiding execution semantics") {
    orm_error_t error;
    orm_execution_plan_t *plan = NULL;
    uint64_t count = 0u;
    orm_error_init(&error);
    orm_connection_t *connection = open_fake("mysql", &error);

    check_equal(
        orm_query_explain(
            connection, orm_view("select * from users"),
            ORM_EXPLAIN_ANALYZE, &plan, &error),
        ORM_STATUS_OK);
    check_equal(
        orm_execution_plan_node_count(plan, &count, &error), ORM_STATUS_OK);
    check_equal(count, UINT64_C(2));
    const orm_execution_plan_node_t child = plan_node(plan, 1u, &error);
    check_equal(child.parent_index, UINT64_C(0));
    check_true(view_equals(child.relation, "users"));
    check_true(view_equals(child.index_name, "idx_users_name"));
    check_true((child.flags & ORM_PLAN_NODE_HAS_ACTUAL_ROWS) != 0u);
    check_true((child.flags & ORM_PLAN_NODE_HAS_ACTUAL_TOTAL_MS) != 0u);
    check_true(child.actual_rows == 1.0);
    check_true(child.actual_total_ms == 0.20);

    orm_execution_plan_destroy(plan);
    orm_disconnect(connection);
  }

  it("normalizes PostgreSQL text hierarchy cost rows and index") {
    orm_error_t error;
    orm_execution_plan_t *plan = NULL;
    uint64_t count = 0u;
    orm_error_init(&error);
    orm_connection_t *connection = open_fake("postgresql", &error);

    check_equal(
        orm_query_explain(
            connection, orm_view("select * from users join teams on true"),
            ORM_EXPLAIN_PLAN, &plan, &error),
        ORM_STATUS_OK);
    check_equal(
        orm_execution_plan_node_count(plan, &count, &error), ORM_STATUS_OK);
    check_equal(count, UINT64_C(3));

    const orm_execution_plan_node_t root = plan_node(plan, 0u, &error);
    const orm_execution_plan_node_t first = plan_node(plan, 1u, &error);
    const orm_execution_plan_node_t second = plan_node(plan, 2u, &error);
    check_equal(root.parent_index, ORM_EXECUTION_PLAN_ROOT_INDEX);
    check_equal(first.parent_index, UINT64_C(0));
    check_equal(second.parent_index, UINT64_C(0));
    check_true(view_equals(first.relation, "users"));
    check_true(view_equals(first.index_name, "idx_users_name"));
    check_true((first.flags & ORM_PLAN_NODE_HAS_STARTUP_COST) != 0u);
    check_true((first.flags & ORM_PLAN_NODE_HAS_TOTAL_COST) != 0u);
    check_true(first.startup_cost == 0.15);
    check_true(first.total_cost == 4.17);

    orm_execution_plan_destroy(plan);
    orm_disconnect(connection);
  }

  it("normalizes PostgreSQL ANALYZE actual timing and rows") {
    orm_error_t error;
    orm_execution_plan_t *plan = NULL;
    orm_error_init(&error);
    orm_connection_t *connection = open_fake("postgresql", &error);

    check_equal(
        orm_query_explain(
            connection, orm_view("select * from users"),
            ORM_EXPLAIN_ANALYZE, &plan, &error),
        ORM_STATUS_OK);
    const orm_execution_plan_node_t node = plan_node(plan, 0u, &error);
    check_true((node.flags & ORM_PLAN_NODE_HAS_ACTUAL_ROWS) != 0u);
    check_true((node.flags & ORM_PLAN_NODE_HAS_ACTUAL_STARTUP_MS) != 0u);
    check_true((node.flags & ORM_PLAN_NODE_HAS_ACTUAL_TOTAL_MS) != 0u);
    check_true(node.actual_rows == 2.0);
    check_true(node.actual_startup_ms == 0.02);
    check_true(node.actual_total_ms == 0.08);

    orm_execution_plan_destroy(plan);
    orm_disconnect(connection);
  }

  it("rejects unsupported providers without fallback") {
    orm_error_t error;
    orm_execution_plan_t *plan = NULL;
    orm_error_init(&error);
    orm_connection_t *connection = open_fake("mongodb", &error);

    check_equal(
        orm_query_explain(
            connection, orm_view("select * from users"),
            ORM_EXPLAIN_PLAN, &plan, &error),
        ORM_STATUS_UNSUPPORTED);
    check_null(plan);
    orm_disconnect(connection);
  }
}

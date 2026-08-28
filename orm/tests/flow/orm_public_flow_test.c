#include <orm.h>

#include <cmeta/struct.h>
#include "tinytest.h"

#include <stddef.h>
#include <string.h>

#define ORM_PUBLIC_FLOW_DATA_PREFIX_SIZE                                      \
  (offsetof(cmeta_data_desc, shape) +                                         \
   sizeof(((cmeta_data_desc *)0)->shape))

Struct(orm_public_flow_row,
    (int, id),
    (long, score)
);

static const cmeta_type_identity orm_public_flow_row_identity =
    CMETA_TYPE_ID_ATOM_INIT("orm.test.PublicFlowRow");
static const cmeta_type_traits orm_public_flow_row_traits = {
    .flags = CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY};
static const cmeta_type_desc orm_public_flow_row_type = {
    .name = "orm_public_flow_row",
    .size = sizeof(orm_public_flow_row),
    .align = _Alignof(orm_public_flow_row),
    .kind = CMETA_T_OBJECT,
    .pointee = NULL,
    .traits = &orm_public_flow_row_traits,
    .identity = &orm_public_flow_row_identity};
static const cmeta_data_field_desc orm_public_flow_row_fields[] = {
    {"orm.test.PublicFlowRow.id", "id", offsetof(orm_public_flow_row, id),
     &cmeta_data_int},
    {"orm.test.PublicFlowRow.score", "score",
     offsetof(orm_public_flow_row, score), &cmeta_data_long}};
static const cmeta_data_struct_shape orm_public_flow_row_shape = {
    .layout = StructMeta(orm_public_flow_row),
    .fields = orm_public_flow_row_fields,
    .field_count = sizeof(orm_public_flow_row_fields) /
                   sizeof(orm_public_flow_row_fields[0])};
static const cmeta_data_desc orm_public_flow_row_data = {
    .struct_size = ORM_PUBLIC_FLOW_DATA_PREFIX_SIZE,
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orm.test.PublicFlowRow.data",
    .display_name = "PublicFlowRow",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &orm_public_flow_row_type,
    .shape = &orm_public_flow_row_shape};

spec("ORM public reactive flow") {
  it("rejects a query LIMIT above max_result_rows") {
    orm_error_t error;
    orm_config_t connection_config;
    orm_option_t filename;
    orm_connection_t *connection = NULL;
    orm_query_t *query = NULL;

    orm_error_init(&error);
    orm_config(&connection_config);
    filename.keyword = orm_view("filename");
    filename.value = orm_view(":memory:");
    connection_config.driver = orm_view("sqlite");
    connection_config.options = &filename;
    connection_config.option_count = 1u;
    connection_config.max_result_rows = 1u;
    check_equal(orm_connect(&connection_config, &connection, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_create(connection, orm_view("rows"), &query, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_set_limit(query, 1u, &error), ORM_STATUS_OK);
    check_equal(orm_query_set_limit(query, 2u, &error),
                ORM_STATUS_LIMIT_EXCEEDED);

    orm_query_destroy(query);
    orm_disconnect(connection);
  }

  it("rejects a SQLite row beyond max_result_rows") {
    orm_error_t error;
    orm_config_t connection_config;
    orm_option_t filename;
    orm_connection_t *connection = NULL;
    orm_query_t *query = NULL;
    orm_flow_config_t flow_config;
    cflow_source source = {0};
    orm_public_flow_row row = {0};
    cflow_step step;

    orm_error_init(&error);
    orm_config(&connection_config);
    filename.keyword = orm_view("filename");
    filename.value = orm_view(":memory:");
    connection_config.driver = orm_view("sqlite");
    connection_config.options = &filename;
    connection_config.option_count = 1u;
    connection_config.max_result_rows = 1u;
    check_equal(orm_connect(&connection_config, &connection, &error),
                ORM_STATUS_OK);
    check_equal(orm_raw(connection,
                        orm_view("select 7 as id, 19 as score "
                                 "union all select 11, 29"),
                        &query, &error),
                ORM_STATUS_OK);
    orm_flow_config(&flow_config, &orm_public_flow_row_data);
    check_equal(orm_query_open_flow(query, &flow_config, &source, &error),
                ORM_STATUS_OK);

    step = cflow_source_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    step = cflow_source_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_ERROR);
    check_not_null(step.error);

    cflow_source_destroy(&source);
    orm_query_destroy(query);
    orm_disconnect(connection);
  }

  it("rejects a SQLite row beyond max_result_bytes") {
    orm_error_t error;
    orm_config_t connection_config;
    orm_option_t filename;
    orm_connection_t *connection = NULL;
    orm_query_t *query = NULL;
    orm_flow_config_t flow_config;
    cflow_source source = {0};
    orm_public_flow_row row = {0};
    cflow_step step;

    orm_error_init(&error);
    orm_config(&connection_config);
    filename.keyword = orm_view("filename");
    filename.value = orm_view(":memory:");
    connection_config.driver = orm_view("sqlite");
    connection_config.options = &filename;
    connection_config.option_count = 1u;
    connection_config.max_result_bytes = sizeof(int64_t) * 2u - 1u;
    check_equal(orm_connect(&connection_config, &connection, &error),
                ORM_STATUS_OK);
    check_equal(orm_raw(connection, orm_view("select 7 as id, 19 as score"),
                        &query, &error),
                ORM_STATUS_OK);
    orm_flow_config(&flow_config, &orm_public_flow_row_data);
    check_equal(orm_query_open_flow(query, &flow_config, &source, &error),
                ORM_STATUS_OK);

    step = cflow_source_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_ERROR);
    check_not_null(step.error);

    cflow_source_destroy(&source);
    orm_query_destroy(query);
    orm_disconnect(connection);
  }

  it("accepts a SQLite row at the exact max_result_bytes boundary") {
    orm_error_t error;
    orm_config_t connection_config;
    orm_option_t filename;
    orm_connection_t *connection = NULL;
    orm_query_t *query = NULL;
    orm_flow_config_t flow_config;
    cflow_source source = {0};
    orm_public_flow_row row = {0};
    cflow_step step;

    orm_error_init(&error);
    orm_config(&connection_config);
    filename.keyword = orm_view("filename");
    filename.value = orm_view(":memory:");
    connection_config.driver = orm_view("sqlite");
    connection_config.options = &filename;
    connection_config.option_count = 1u;
    connection_config.max_result_bytes = sizeof(int64_t) * 2u;
    check_equal(orm_connect(&connection_config, &connection, &error),
                ORM_STATUS_OK);
    check_equal(orm_raw(connection, orm_view("select 7 as id, 19 as score"),
                        &query, &error),
                ORM_STATUS_OK);
    orm_flow_config(&flow_config, &orm_public_flow_row_data);
    check_equal(orm_query_open_flow(query, &flow_config, &source, &error),
                ORM_STATUS_OK);

    step = cflow_source_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(row.id, 7);
    check_equal(row.score, 19L);
    step = cflow_source_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_DONE);

    cflow_source_destroy(&source);
    orm_query_destroy(query);
    orm_disconnect(connection);
  }

  it("opens a typed SQLite Source that advances one row per resume") {
    orm_error_t error;
    orm_config_t connection_config;
    orm_option_t filename;
    orm_connection_t *connection = NULL;
    orm_query_t *query = NULL;
    orm_flow_config_t flow_config;
    cflow_source source = {0};
    orm_public_flow_row first = {0};
    orm_public_flow_row second = {0};
    orm_public_flow_row terminal = {0};
    cflow_step step;

    orm_error_init(&error);
    orm_config(&connection_config);
    filename.keyword = orm_view("filename");
    filename.value = orm_view(":memory:");
    connection_config.driver = orm_view("sqlite");
    connection_config.options = &filename;
    connection_config.option_count = 1u;
    check_equal(orm_connect(&connection_config, &connection, &error),
                ORM_STATUS_OK);
    check_not_null(connection);
    check_equal(orm_raw(connection,
                        orm_view("select 7 as id, 19 as score "
                                 "union all select 11, 29 order by id"),
                        &query, &error),
                ORM_STATUS_OK);

    orm_flow_config(&flow_config, &orm_public_flow_row_data);
    check_equal(orm_query_open_flow(query, &flow_config, &source, &error),
                ORM_STATUS_OK);
    check_true(cflow_source_valid(&source));
    check_equal(orm_query_open_flow(query, &flow_config, &source, &error),
                ORM_STATUS_INVALID_STATE);
    check_true(cflow_source_valid(&source));

    step = cflow_source_resume(&source, NULL, &first);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(first.id, 7);
    check_equal(first.score, 19L);

    step = cflow_source_resume(&source, NULL, &second);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(second.id, 11);
    check_equal(second.score, 29L);

    step = cflow_source_resume(&source, NULL, &terminal);
    check_equal(step.kind, CFLOW_STEP_DONE);

    cflow_source_destroy(&source);
    orm_query_destroy(query);
    orm_disconnect(connection);
  }

  it("executes commands on first demand and emits affected rows once") {
    orm_error_t error;
    orm_config_t connection_config;
    orm_option_t filename;
    orm_connection_t *connection = NULL;
    orm_query_t *command = NULL;
    orm_query_t *probe = NULL;
    orm_flow_config_t row_config;
    cflow_source command_source = {0};
    cflow_source row_source = {0};
    orm_command_result_t command_result = ORM_COMMAND_RESULT_INIT;
    orm_public_flow_row row = {0};
    cflow_step step;

    orm_error_init(&error);
    orm_config(&connection_config);
    filename.keyword = orm_view("filename");
    filename.value = orm_view(":memory:");
    connection_config.driver = orm_view("sqlite");
    connection_config.options = &filename;
    connection_config.option_count = 1u;
    check_equal(orm_connect(&connection_config, &connection, &error),
                ORM_STATUS_OK);

    check_equal(orm_raw(connection,
                        orm_view("create table flow_command(id integer, "
                                 "score integer)"),
                        &command, &error), ORM_STATUS_OK);
    check_equal(orm_query_open_command_flow(command, &command_source, &error),
                ORM_STATUS_OK);
    check_equal(orm_raw(connection,
                        orm_view("select id, score from flow_command"),
                        &probe, &error), ORM_STATUS_OK);
    orm_flow_config(&row_config, &orm_public_flow_row_data);
    check_equal(orm_query_open_flow(probe, &row_config, &row_source, &error),
                ORM_STATUS_SQL_ERROR);
    check_false(cflow_source_valid(&row_source));
    orm_query_destroy(probe);
    probe = NULL;

    step = cflow_source_resume(&command_source, NULL, &command_result);
    check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(command_result.affected_rows, (uint64_t)0u);
    cflow_source_destroy(&command_source);
    command_source = (cflow_source){0};
    orm_query_destroy(command);
    command = NULL;

    check_equal(orm_raw(connection,
                        orm_view("insert into flow_command values(7, 19)"),
                        &command, &error), ORM_STATUS_OK);
    check_equal(orm_query_open_command_flow(command, &command_source, &error),
                ORM_STATUS_OK);
    step = cflow_source_resume(&command_source, NULL, &command_result);
    check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(command_result.affected_rows, (uint64_t)1u);
    cflow_source_destroy(&command_source);
    orm_query_destroy(command);

    check_equal(orm_raw(connection,
                        orm_view("select id, score from flow_command"),
                        &probe, &error), ORM_STATUS_OK);
    check_equal(orm_query_open_flow(probe, &row_config, &row_source, &error),
                ORM_STATUS_OK);
    step = cflow_source_resume(&row_source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(row.id, 7);
    check_equal(row.score, 19L);
    cflow_source_destroy(&row_source);
    orm_query_destroy(probe);
    orm_disconnect(connection);
  }

  it("materializes an owned bounded SQLite result through the public ABI") {
    const unsigned char expected_blob[] = {0x00u, 0x01u, 0xffu};
    orm_error_t error;
    orm_config_t connection_config;
    orm_option_t filename;
    orm_connection_t *connection = NULL;
    orm_query_t *query = NULL;
    orm_result_t *result = NULL;
    orm_string_view_t text = {0};
    orm_blob_t blob = {0};
    uint64_t rows = 0u;
    uint64_t columns = 0u;
    uint64_t affected = 0u;
    uint8_t is_null = 0u;
    orm_value_kind_t kind = ORM_VALUE_NULL;
    int64_t integer = 0;
    double floating = 0.0;

    orm_error_init(&error);
    orm_config(&connection_config);
    filename.keyword = orm_view("filename");
    filename.value = orm_view(":memory:");
    connection_config.driver = orm_view("sqlite");
    connection_config.options = &filename;
    connection_config.option_count = 1u;
    check_equal(orm_connect(&connection_config, &connection, &error),
                ORM_STATUS_OK);

    check_equal(
        orm_raw(connection,
                orm_view("select 7 as id, 2.5 as ratio, 'Alice' as name, "
                         "x'0001ff' as payload, null as note"),
                &query, &error),
        ORM_STATUS_OK);
    check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
    check_not_null(result);
    check_equal(orm_result_row_count(result, &rows, &error), ORM_STATUS_OK);
    check_equal(rows, (uint64_t)1u);
    check_equal(orm_result_column_count(result, &columns, &error),
                ORM_STATUS_OK);
    check_equal(columns, (uint64_t)5u);
    check_equal(orm_result_get_int64(result, 0u, 0u, &integer, &error),
                ORM_STATUS_OK);
    check_equal(integer, (int64_t)7);
    check_equal(orm_result_value_kind(result, 0u, 0u, &kind, &error),
                ORM_STATUS_OK);
    check_equal(kind, ORM_VALUE_INT64);
    check_equal(orm_result_get_double(result, 0u, 1u, &floating, &error),
                ORM_STATUS_OK);
    check_true(floating == 2.5);
    check_equal(orm_result_get_text(result, 0u, 2u, &text, &error),
                ORM_STATUS_OK);
    check_equal(text.len, (size_t)5u);
    check_true(memcmp(text.data, "Alice", text.len) == 0);
    check_equal(orm_result_get_blob(result, 0u, 3u, &blob, &error),
                ORM_STATUS_OK);
    check_equal(blob.size, sizeof(expected_blob));
    check_true(memcmp(blob.data, expected_blob, blob.size) == 0);
    check_equal(orm_result_is_null(result, 0u, 4u, &is_null, &error),
                ORM_STATUS_OK);
    check_equal(is_null, (uint8_t)1u);
    check_equal(orm_result_get_text(result, 0u, 4u, &text, &error),
                ORM_STATUS_NULL_VALUE);
    check_equal(orm_result_get_int64(result, 0u, 1u, &integer, &error),
                ORM_STATUS_TYPE_ERROR);
    check_equal(orm_result_get_text(result, 1u, 0u, &text, &error),
                ORM_STATUS_OUT_OF_RANGE);
    orm_result_destroy(result);
    orm_query_destroy(query);
    result = NULL;
    query = NULL;

    check_equal(
        orm_raw(connection, orm_view("create table materialized(id integer)"),
                &query, &error),
        ORM_STATUS_OK);
    check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
    check_equal(orm_result_affected_rows(result, &affected, &error),
                ORM_STATUS_OK);
    check_equal(affected, (uint64_t)0u);

    orm_result_destroy(result);
    orm_query_destroy(query);
    result = NULL;
    query = NULL;
    check_equal(orm_raw(connection, orm_view("PRAGMA foreign_keys"), &query,
                        &error),
                ORM_STATUS_OK);
    check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
    check_equal(orm_result_row_count(result, &affected, &error),
                ORM_STATUS_OK);
    check_equal(affected, (uint64_t)1u);
    orm_result_destroy(result);
    orm_query_destroy(query);
    result = NULL;
    query = NULL;
    check_equal(orm_raw(connection, orm_view("PRAGMA journal_mode=WAL"),
                        &query, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
    check_equal(orm_result_row_count(result, &affected, &error),
                ORM_STATUS_OK);
    check_equal(affected, (uint64_t)1u);
    orm_result_destroy(result);
    orm_query_destroy(query);
    orm_disconnect(connection);
  }

  it("fails materialization atomically at the configured row bound") {
    orm_error_t error;
    orm_config_t connection_config;
    orm_option_t filename;
    orm_connection_t *connection = NULL;
    orm_query_t *query = NULL;
    orm_result_t *result = NULL;

    orm_error_init(&error);
    orm_config(&connection_config);
    filename.keyword = orm_view("filename");
    filename.value = orm_view(":memory:");
    connection_config.driver = orm_view("sqlite");
    connection_config.options = &filename;
    connection_config.option_count = 1u;
    connection_config.max_result_rows = 1u;
    check_equal(orm_connect(&connection_config, &connection, &error),
                ORM_STATUS_OK);
    check_equal(orm_raw(connection,
                        orm_view("select 1 as id union all select 2"), &query,
                        &error),
                ORM_STATUS_OK);

    check_equal(orm_query_execute(query, &result, &error),
                ORM_STATUS_LIMIT_EXCEEDED);
    check_null(result);

    orm_query_destroy(query);
    orm_disconnect(connection);
  }

  it("preserves result columns when a query returns no rows") {
    orm_error_t error;
    orm_config_t connection_config;
    orm_option_t filename;
    orm_connection_t *connection = NULL;
    orm_query_t *query = NULL;
    orm_result_t *result = NULL;
    uint64_t rows = 1u;
    uint64_t columns = 0u;

    orm_error_init(&error);
    orm_config(&connection_config);
    filename.keyword = orm_view("filename");
    filename.value = orm_view(":memory:");
    connection_config.driver = orm_view("sqlite");
    connection_config.options = &filename;
    connection_config.option_count = 1u;
    check_equal(orm_connect(&connection_config, &connection, &error),
                ORM_STATUS_OK);
    check_equal(orm_raw(connection, orm_view("select 1 as id where 0"),
                        &query, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
    check_equal(orm_result_row_count(result, &rows, &error), ORM_STATUS_OK);
    check_equal(rows, (uint64_t)0u);
    check_equal(orm_result_column_count(result, &columns, &error),
                ORM_STATUS_OK);
    check_equal(columns, (uint64_t)1u);

    orm_result_destroy(result);
    orm_query_destroy(query);
    orm_disconnect(connection);
  }

  it("materializes rows from a raw mutation with RETURNING") {
    orm_error_t error;
    orm_config_t connection_config;
    orm_option_t filename;
    orm_connection_t *connection = NULL;
    orm_query_t *query = NULL;
    orm_result_t *result = NULL;
    int64_t value = 0;

    orm_error_init(&error);
    orm_config(&connection_config);
    filename.keyword = orm_view("filename");
    filename.value = orm_view(":memory:");
    connection_config.driver = orm_view("sqlite");
    connection_config.options = &filename;
    connection_config.option_count = 1u;
    check_equal(orm_connect(&connection_config, &connection, &error),
                ORM_STATUS_OK);
    check_equal(orm_raw(connection, orm_view("create table returned(id integer)"),
                        &query, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
    orm_result_destroy(result);
    orm_query_destroy(query);
    result = NULL;
    query = NULL;
    check_equal(orm_raw(connection,
                        orm_view("insert into returned values(7) returning id"),
                        &query, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
    check_equal(orm_result_get_int64(result, 0u, 0u, &value, &error),
                ORM_STATUS_OK);
    check_equal(value, (int64_t)7);

    orm_result_destroy(result);
    orm_query_destroy(query);
    orm_disconnect(connection);
  }

  it("executes a SQLite PRAGMA assignment as a command") {
    orm_error_t error;
    orm_config_t connection_config;
    orm_option_t filename;
    orm_connection_t *connection = NULL;
    orm_query_t *query = NULL;
    orm_result_t *result = NULL;
    uint64_t affected = 1u;

    orm_error_init(&error);
    orm_config(&connection_config);
    filename.keyword = orm_view("filename");
    filename.value = orm_view(":memory:");
    connection_config.driver = orm_view("sqlite");
    connection_config.options = &filename;
    connection_config.option_count = 1u;
    check_equal(orm_connect(&connection_config, &connection, &error),
                ORM_STATUS_OK);
    check_equal(orm_raw(connection, orm_view("PRAGMA foreign_keys=ON"),
                        &query, &error),
                ORM_STATUS_OK);

    check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
    check_equal(orm_result_affected_rows(result, &affected, &error),
                ORM_STATUS_OK);
    check_equal(affected, (uint64_t)0u);

    orm_result_destroy(result);
    orm_query_destroy(query);
    orm_disconnect(connection);
  }

  it("keeps a transaction result snapshot after commit") {
    orm_error_t error;
    orm_config_t connection_config;
    orm_option_t filename;
    orm_connection_t *connection = NULL;
    orm_transaction_t *transaction = NULL;
    orm_query_t *query = NULL;
    orm_result_t *result = NULL;
    int64_t value = 0;

    orm_error_init(&error);
    orm_config(&connection_config);
    filename.keyword = orm_view("filename");
    filename.value = orm_view(":memory:");
    connection_config.driver = orm_view("sqlite");
    connection_config.options = &filename;
    connection_config.option_count = 1u;
    check_equal(orm_connect(&connection_config, &connection, &error),
                ORM_STATUS_OK);
    check_equal(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                      &transaction, &error),
                ORM_STATUS_OK);
    check_equal(orm_raw(connection, orm_view("select 41 + 1 as answer"),
                        &query, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_execute_in_transaction(query, transaction, &result,
                                                 &error),
                ORM_STATUS_OK);
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    check_equal(orm_result_get_int64(result, 0u, 0u, &value, &error),
                ORM_STATUS_OK);
    check_equal(value, (int64_t)42);

    orm_result_destroy(result);
    orm_query_destroy(query);
    orm_transaction_destroy(transaction);
    orm_disconnect(connection);
  }
}

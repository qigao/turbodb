#include <orm.h>

#include <cmeta/struct.h>
#include "tinytest.h"

#include <stddef.h>

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
}

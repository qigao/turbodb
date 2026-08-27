#include "orm.h"

#include <cmeta/struct.h>
#include <tinytest.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define ORM_TIDES_PUBLIC_DATA_PREFIX_SIZE                                  \
  (offsetof(cmeta_data_desc, shape) +                                      \
   sizeof(((cmeta_data_desc *)0)->shape))

Struct(orm_tides_public_row, (long, id), (long, score));

static const cmeta_type_identity orm_tides_public_row_identity =
    CMETA_TYPE_ID_ATOM_INIT("orm.test.TidesPublicRow");
static const cmeta_type_traits orm_tides_public_row_traits = {
    CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY};
static const cmeta_type_desc orm_tides_public_row_type = {
    "orm_tides_public_row", sizeof(orm_tides_public_row),
    _Alignof(orm_tides_public_row), CMETA_T_OBJECT, NULL,
    &orm_tides_public_row_traits, &orm_tides_public_row_identity};
static const cmeta_data_field_desc orm_tides_public_row_fields[] = {
    {"orm.test.TidesPublicRow.id", "id", offsetof(orm_tides_public_row, id),
     &cmeta_data_long},
    {"orm.test.TidesPublicRow.score", "score",
     offsetof(orm_tides_public_row, score), &cmeta_data_long}};
static const cmeta_data_struct_shape orm_tides_public_row_shape = {
    StructMeta(orm_tides_public_row), orm_tides_public_row_fields, 2u};
static const cmeta_data_desc orm_tides_public_row_data = {
    ORM_TIDES_PUBLIC_DATA_PREFIX_SIZE, CMETA_DATA_DESC_ABI_VERSION,
    "orm.test.TidesPublicRow.data", "TidesPublicRow", CMETA_DATA_STRUCT,
    &orm_tides_public_row_type, &orm_tides_public_row_shape};

spec("TidesDB public reactive C facade") {
  it("executes a demanded command and streams the stored typed row") {
    char *path = tt_make_temp_dir("orm-tides-public-flow");
    orm_option_t options[4];
    orm_config_t config;
    orm_connection_t *connection = NULL;
    orm_query_t *query = NULL;
    orm_error_t error;
    cflow_source source = {0};
    orm_command_result_t command = ORM_COMMAND_RESULT_INIT;
    orm_flow_config_t flow_config;
    orm_tides_public_row row = {0};
    cflow_step step;

    check_not_null(path);
    if (path == NULL) return;

    orm_config(&config);
    options[0] = (orm_option_t){orm_view("path"), orm_view(path)};
    options[1] =
        (orm_option_t){orm_view("column_family"), orm_view("orm_cflow")};
    options[2] =
        (orm_option_t){orm_view("max_scan_rows"), orm_view("128")};
    options[3] =
        (orm_option_t){orm_view("max_scan_bytes"), orm_view("1048576")};
    config.driver = orm_view("tidesdb");
    config.options = options;
    config.option_count = 4u;
    orm_error_init(&error);

    check_equal(orm_connect(&config, &connection, &error), ORM_STATUS_OK);
    check_equal(orm_insert(connection, orm_view("people"), &query, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_set(query, orm_view("id"), orm_i64(7), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_set(query, orm_view("score"), orm_i64(19), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_open_command_flow(query, &source, &error),
                ORM_STATUS_OK);
    step = cflow_source_resume(&source, NULL, &command);
    check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(command.affected_rows, (uint64_t)1u);
    cflow_source_destroy(&source);
    source = (cflow_source){0};
    orm_query_destroy(query);
    query = NULL;

    check_equal(orm_query_create(connection, orm_view("people"), &query,
                                 &error),
                ORM_STATUS_OK);
    check_equal(orm_query_add_column(query, orm_view("id"), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_add_column(query, orm_view("score"), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_where(query, orm_view("id"), ORM_COMPARE_EQUAL,
                                orm_i64(7), &error),
                ORM_STATUS_OK);
    orm_flow_config(&flow_config, &orm_tides_public_row_data);
    check_equal(orm_query_open_flow(query, &flow_config, &source, &error),
                ORM_STATUS_OK);
    step = cflow_source_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(row.id, 7L);
    check_equal(row.score, 19L);
    step = cflow_source_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_DONE);

    cflow_source_destroy(&source);
    orm_query_destroy(query);
    orm_disconnect(connection);
    check_equal(tt_remove_tree(path), 0);
    free(path);
  }

  it("updates, deletes, and commits through the pure C transaction backend") {
    char *path = tt_make_temp_dir("orm-tides-public-crud");
    orm_option_t options[2];
    orm_config_t config;
    orm_connection_t *connection = NULL;
    orm_transaction_t *transaction = NULL;
    orm_query_t *query = NULL;
    orm_error_t error;
    cflow_source source = {0};
    orm_command_result_t command = ORM_COMMAND_RESULT_INIT;
    orm_flow_config_t flow_config;
    orm_tides_public_row row = {0};
    cflow_step step;

    check_not_null(path);
    if (path == NULL) return;
    orm_config(&config);
    options[0] = (orm_option_t){orm_view("path"), orm_view(path)};
    options[1] = (orm_option_t){orm_view("column_family"),
                                orm_view("orm_crud")};
    config.driver = orm_view("tidesdb");
    config.options = options;
    config.option_count = 2u;
    orm_error_init(&error);
    check_equal(orm_connect(&config, &connection, &error), ORM_STATUS_OK);

    check_equal(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                      &transaction, &error), ORM_STATUS_OK);
    check_equal(orm_insert(connection, orm_view("people"), &query, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_set(query, orm_view("id"), orm_i64(9), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_set(query, orm_view("score"), orm_i64(10), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_open_command_flow_in_transaction(
                    query, transaction, &source, &error), ORM_STATUS_OK);
    step = cflow_source_resume(&source, NULL, &command);
    check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(command.affected_rows, (uint64_t)1u);
    cflow_source_destroy(&source);
    source = (cflow_source){0};
    orm_query_destroy(query);
    query = NULL;

    check_equal(orm_query_create(connection, orm_view("people"), &query,
                                 &error), ORM_STATUS_OK);
    check_equal(orm_query_add_column(query, orm_view("id"), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_add_column(query, orm_view("score"), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_where(query, orm_view("id"), ORM_COMPARE_EQUAL,
                                orm_i64(9), &error), ORM_STATUS_OK);
    orm_flow_config(&flow_config, &orm_tides_public_row_data);
    check_equal(orm_query_open_flow_in_transaction(
                    query, transaction, &flow_config, &source, &error),
                ORM_STATUS_OK);
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_BUSY);
    step = cflow_source_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(row.score, 10L);
    cflow_source_destroy(&source);
    source = (cflow_source){0};
    orm_query_destroy(query);
    query = NULL;
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    orm_transaction_destroy(transaction);
    transaction = NULL;

    check_equal(orm_update(connection, orm_view("people"), &query, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_set(query, orm_view("score"), orm_i64(20), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_where(query, orm_view("id"), ORM_COMPARE_EQUAL,
                                orm_i64(9), &error), ORM_STATUS_OK);
    check_equal(orm_query_open_command_flow(query, &source, &error),
                ORM_STATUS_OK);
    command = (orm_command_result_t)ORM_COMMAND_RESULT_INIT;
    step = cflow_source_resume(&source, NULL, &command);
    check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(command.affected_rows, (uint64_t)1u);
    cflow_source_destroy(&source);
    source = (cflow_source){0};
    orm_query_destroy(query);
    query = NULL;

    check_equal(orm_query_create(connection, orm_view("people"), &query,
                                 &error), ORM_STATUS_OK);
    check_equal(orm_query_add_column(query, orm_view("id"), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_add_column(query, orm_view("score"), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_where(query, orm_view("id"), ORM_COMPARE_EQUAL,
                                orm_i64(9), &error), ORM_STATUS_OK);
    orm_flow_config(&flow_config, &orm_tides_public_row_data);
    check_equal(orm_query_open_flow(query, &flow_config, &source, &error),
                ORM_STATUS_OK);
    step = cflow_source_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(row.score, 20L);
    cflow_source_destroy(&source);
    source = (cflow_source){0};
    orm_query_destroy(query);
    query = NULL;

    check_equal(orm_delete(connection, orm_view("people"), &query, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_where(query, orm_view("id"), ORM_COMPARE_EQUAL,
                                orm_i64(9), &error), ORM_STATUS_OK);
    check_equal(orm_query_where(query, orm_view("score"), ORM_COMPARE_EQUAL,
                                orm_i64(20), &error), ORM_STATUS_OK);
    check_equal(orm_query_open_command_flow(query, &source, &error),
                ORM_STATUS_OK);
    command = (orm_command_result_t)ORM_COMMAND_RESULT_INIT;
    step = cflow_source_resume(&source, NULL, &command);
    check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(command.affected_rows, (uint64_t)1u);

    cflow_source_destroy(&source);
    orm_query_destroy(query);
    orm_disconnect(connection);
    check_equal(tt_remove_tree(path), 0);
    free(path);
  }
}

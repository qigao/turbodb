#include <orm.h>

#include <cmeta/struct.h>
#include <data_bind.h>
#include <data_bind_message_plan.h>
#include "tinytest.h"

#include <stddef.h>
#include <stdlib.h>
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

static orm_connection_t *orm_public_flow_open_sqlite(
    uint32_t max_predicates, uint64_t max_parameter_bytes,
    orm_error_t *error) {
  orm_config_t config;
  orm_option_t filename;
  orm_connection_t *connection = NULL;

  orm_config(&config);
  filename.keyword = orm_view("filename");
  filename.value = orm_view(":memory:");
  config.driver = orm_view("sqlite");
  config.options = &filename;
  config.option_count = 1u;
  if (max_predicates != 0u)
    config.max_predicates = max_predicates;
  if (max_parameter_bytes != 0u)
    config.max_parameter_bytes = max_parameter_bytes;
  check_equal(orm_connect(&config, &connection, error), ORM_STATUS_OK);
  check_not_null(connection);
  return connection;
}

static void orm_public_flow_execute_sql(orm_connection_t *connection,
                                        const char *sql,
                                        orm_error_t *error) {
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;

  check_equal(orm_raw(connection, orm_view(sql), &query, error), ORM_STATUS_OK);
  check_equal(orm_query_execute(query, &result, error), ORM_STATUS_OK);
  orm_result_destroy(result);
  orm_query_destroy(query);
}

static orm_connection_t *orm_public_flow_composite_fixture(
    uint32_t max_predicates, uint64_t max_parameter_bytes,
    orm_error_t *error) {
  orm_connection_t *connection =
      orm_public_flow_open_sqlite(max_predicates, max_parameter_bytes, error);
  orm_public_flow_execute_sql(
      connection,
      "create table memberships("
      "domain_id text not null,user_id text not null,"
      "group_id text not null,revision integer not null,"
      "primary key(domain_id,user_id,group_id))",
      error);
  orm_public_flow_execute_sql(
      connection,
      "insert into memberships values"
      "('domain-a','user-a','group-a',1),"
      "('domain-a','user-b','group-a',2)",
      error);
  return connection;
}

typedef struct orm_public_object_record {
  int64_t id;
  int64_t score;
} orm_public_object_record;

typedef struct orm_public_object_carrier {
  orm_public_object_record *record;
  cmeta_object_field_provider field_provider;
} orm_public_object_carrier;

typedef struct orm_public_object_factory_state {
  size_t create_count;
  size_t destroy_count;
} orm_public_object_factory_state;

static const cmeta_type_identity orm_public_object_record_identity =
    CMETA_TYPE_ID_ATOM_INIT("orm.test.PublicObjectRow");
static const cmeta_type_desc orm_public_object_record_type = {
    .name = "orm_public_object_record",
    .size = sizeof(orm_public_object_record),
    .align = _Alignof(orm_public_object_record),
    .kind = CMETA_T_OBJECT,
    .pointee = NULL,
    .traits = NULL,
    .identity = &orm_public_object_record_identity
};
static const cmeta_field_desc orm_public_object_layout_fields[] = {
    {"id", "int64_t", CMETA_FIELD_DYNAMIC_OFFSET, sizeof(int64_t),
     _Alignof(int64_t), &cmeta_type_int64, NULL},
    {"score", "int64_t", CMETA_FIELD_DYNAMIC_OFFSET, sizeof(int64_t),
     _Alignof(int64_t), &cmeta_type_int64, NULL}
};
static const cmeta_struct_desc orm_public_object_layout = {
    "PublicObjectRow", sizeof(orm_public_object_record),
    _Alignof(orm_public_object_record),
    orm_public_object_layout_fields, 2u
};
static const cmeta_data_field_desc orm_public_object_fields[] = {
    {"orm.test.PublicObjectRow.id", "id", CMETA_FIELD_DYNAMIC_OFFSET,
     &cmeta_data_int64},
    {"orm.test.PublicObjectRow.score", "score", CMETA_FIELD_DYNAMIC_OFFSET,
     &cmeta_data_int64}
};
static const cmeta_data_struct_shape orm_public_object_shape = {
    &orm_public_object_layout, orm_public_object_fields, 2u
};
static const cmeta_data_desc orm_public_object_data = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orm.test.PublicObjectRow.data",
    .display_name = "PublicObjectRow",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &orm_public_object_record_type,
    .shape = &orm_public_object_shape
};

static const cmeta_type_identity orm_public_object_carrier_identity =
    CMETA_TYPE_ID_ATOM_INIT("orm.test.PublicObjectCarrier");
static const cmeta_type_desc orm_public_object_carrier_type = {
    .name = "orm_public_object_carrier",
    .size = sizeof(orm_public_object_carrier),
    .align = _Alignof(orm_public_object_carrier),
    .kind = CMETA_T_OBJECT,
    .pointee = NULL,
    .traits = NULL,
    .identity = &orm_public_object_carrier_identity
};

static cmeta_status orm_public_object_read(
    void *context, const void *object, const cmeta_data_field_desc *field,
    const void **out_value) {
  const orm_public_object_record *record =
      (const orm_public_object_record *)object;
  (void)context;
  if (!record || !field || !out_value) return CMETA_INVALID_ARGUMENT;
  if (strcmp(field->name, "id") == 0) *out_value = &record->id;
  else if (strcmp(field->name, "score") == 0) *out_value = &record->score;
  else return CMETA_TRAIT_MISSING;
  return CMETA_OK;
}

static cmeta_status orm_public_object_assign(
    void *context, void *object, const cmeta_data_field_desc *field,
    const void *value) {
  orm_public_object_record *record = (orm_public_object_record *)object;
  (void)context;
  if (!record || !field || !value) return CMETA_INVALID_ARGUMENT;
  if (strcmp(field->name, "id") == 0) record->id = *(const int64_t *)value;
  else if (strcmp(field->name, "score") == 0)
    record->score = *(const int64_t *)value;
  else return CMETA_TRAIT_MISSING;
  return CMETA_OK;
}

static orm_status_t orm_public_object_create(
    void *context, void *out_value, cmeta_object_ref *out_object,
    orm_error_t *error) {
  orm_public_object_factory_state *state =
      (orm_public_object_factory_state *)context;
  orm_public_object_carrier *carrier =
      (orm_public_object_carrier *)out_value;
  cmeta_status cmeta_status_code;

  if (!state || !carrier || !out_object) return ORM_STATUS_INVALID_ARGUMENT;
  memset(carrier, 0, sizeof(*carrier));
  carrier->record =
      (orm_public_object_record *)calloc(1u, sizeof(*carrier->record));
  if (!carrier->record) {
    if (error) {
      orm_error_init(error);
      error->status = ORM_STATUS_OUT_OF_MEMORY;
    }
    return ORM_STATUS_OUT_OF_MEMORY;
  }

  carrier->field_provider = (cmeta_object_field_provider){
      .size = sizeof(cmeta_object_field_provider),
      .data = &orm_public_object_data,
      .context = NULL,
      .assign = orm_public_object_assign,
      .read = orm_public_object_read
  };
  cmeta_status_code = cmeta_object_borrow_with_providers(
      out_object, carrier->record, &orm_public_object_data,
      &carrier->field_provider, NULL);
  if (cmeta_status_code != CMETA_OK) {
    free(carrier->record);
    carrier->record = NULL;
    return ORM_STATUS_TYPE_ERROR;
  }
  ++state->create_count;
  return ORM_STATUS_OK;
}

static void orm_public_object_destroy(void *context, void *value) {
  orm_public_object_factory_state *state =
      (orm_public_object_factory_state *)context;
  orm_public_object_carrier *carrier =
      (orm_public_object_carrier *)value;
  if (!carrier) return;
  free(carrier->record);
  carrier->record = NULL;
  if (state) ++state->destroy_count;
}

static DataBindMessagePlan *orm_public_object_plan(DataBind **out_codec) {
  static const char schema[] =
      "message PublicObjectRow { int64 id; int64 score; }";
  DataBind *codec = NULL;
  DataBindMessagePlan *plan = NULL;
  DataBindError error = DATA_BIND_ERROR_INIT;
  DataBindMessagePlanDiagnostic diagnostic =
      DATA_BIND_MESSAGE_PLAN_DIAGNOSTIC_INIT;

  check_equal(
      data_bind_create_from_text(
          schema, sizeof(schema) - 1u, &codec, &error),
      DATA_BIND_OK);
  if (!codec) return NULL;
  check_equal(
      data_bind_message_plan_compile_object(
          codec, "PublicObjectRow", &orm_public_object_data,
          &plan, &diagnostic),
      DATA_BIND_OK);
  if (out_codec) *out_codec = codec;
  else data_bind_free(codec);
  return plan;
}

spec("ORM public reactive flow") {
  it("opens a public provider-backed object Publisher") {
    orm_error_t error;
    orm_connection_t *connection = NULL;
    orm_query_t *query = NULL;
    DataBind *codec = NULL;
    DataBindMessagePlan *plan = NULL;
    orm_public_object_factory_state factory_state = {0};
    orm_object_row_factory_t factory = {
        sizeof(orm_object_row_factory_t), ORM_C_ABI_VERSION,
        &orm_public_object_carrier_type, &factory_state,
        orm_public_object_create, orm_public_object_destroy};
    orm_object_flow_config_t flow_config;
    cflow_publisher source = {0};
    orm_public_object_carrier row = {0};
    cflow_step step;

    orm_error_init(&error);
    connection = orm_public_flow_open_sqlite(0u, 0u, &error);
    check_equal(
        orm_raw(
            connection,
            orm_view("select 7 as id, 19 as score"),
            &query, &error),
        ORM_STATUS_OK);

    plan = orm_public_object_plan(&codec);
    check_not_null(plan);
    if (!plan) {
      orm_query_destroy(query);
      orm_disconnect(connection);
      data_bind_free(codec);
      return;
    }

    orm_object_flow_config(
        &flow_config, &orm_public_object_data, plan, &factory);
    check_equal(
        orm_query_open_object_flow(
            query, &flow_config, &source, &error),
        ORM_STATUS_OK);
    check_true(cmeta_type_equal(
        cflow_publisher_output_type(&source),
        &orm_public_object_carrier_type));

    step = cflow_publisher_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(factory_state.create_count, (size_t)1u);
    check_not_null(row.record);
    if (row.record) {
      check_equal(row.record->id, INT64_C(7));
      check_equal(row.record->score, INT64_C(19));
    }
    orm_public_object_destroy(&factory_state, &row);

    step = cflow_publisher_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_DONE);
    check_equal(factory_state.create_count, (size_t)1u);
    check_equal(factory_state.destroy_count, (size_t)1u);

    cflow_publisher_destroy(&source);
    orm_query_destroy(query);
    orm_disconnect(connection);
    data_bind_message_plan_free(plan);
    data_bind_free(codec);
  }

  it("copies every composite key part before selecting one row") {
    orm_error_t error;
    orm_connection_t *connection;
    orm_query_t *query = NULL;
    orm_result_t *result = NULL;
    char domain[] = "domain-a";
    char user[] = "user-a";
    char group_name[] = "group-a";
    orm_key_part_t key[] = {
        {orm_view("domain_id"), orm_text(domain)},
        {orm_view("user_id"), orm_text(user)},
        {orm_view("group_id"), orm_text(group_name)}};
    uint64_t rows = 0u;
    int64_t revision = 0;

    orm_error_init(&error);
    connection = orm_public_flow_composite_fixture(0u, 0u, &error);
    check_equal(orm_query_create(connection, orm_view("memberships"), &query,
                                 &error),
                ORM_STATUS_OK);
    check_equal(orm_query_add_column(query, orm_view("revision"), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_where_key(query, key, 3u, &error), ORM_STATUS_OK);
    memset(domain, 'x', sizeof(domain) - 1u);
    memset(user, 'x', sizeof(user) - 1u);
    memset(group_name, 'x', sizeof(group_name) - 1u);
    check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
    check_equal(orm_result_row_count(result, &rows, &error), ORM_STATUS_OK);
    check_equal(rows, (uint64_t)1u);
    check_equal(orm_result_get_int64(result, 0u, 0u, &revision, &error),
                ORM_STATUS_OK);
    check_equal(revision, (int64_t)1);

    orm_result_destroy(result);
    orm_query_destroy(query);
    orm_disconnect(connection);
  }

  it("rejects an invalid composite key without retaining an earlier part") {
    orm_error_t error;
    orm_connection_t *connection;
    orm_query_t *query = NULL;
    orm_result_t *result = NULL;
    const orm_key_part_t key[] = {
        {orm_view("domain_id"), orm_text("domain-a")},
        {orm_view("bad;column"), orm_text("user-a")}};
    uint64_t rows = 0u;

    orm_error_init(&error);
    connection = orm_public_flow_composite_fixture(0u, 0u, &error);
    check_equal(orm_query_create(connection, orm_view("memberships"), &query,
                                 &error),
                ORM_STATUS_OK);
    check_equal(orm_query_select_all(query, &error), ORM_STATUS_OK);
    check_equal(orm_query_where_key(query, key, 2u, &error),
                ORM_STATUS_INVALID_ARGUMENT);
    check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
    check_equal(orm_result_row_count(result, &rows, &error), ORM_STATUS_OK);
    check_equal(rows, (uint64_t)2u);

    orm_result_destroy(result);
    orm_query_destroy(query);
    orm_disconnect(connection);
  }

  it("rejects duplicate composite key columns atomically") {
    orm_error_t error;
    orm_connection_t *connection;
    orm_query_t *query = NULL;
    orm_result_t *result = NULL;
    const orm_key_part_t key[] = {
        {orm_view("domain_id"), orm_text("domain-a")},
        {orm_view("domain_id"), orm_text("domain-b")}};
    uint64_t rows = 0u;

    orm_error_init(&error);
    connection = orm_public_flow_composite_fixture(0u, 0u, &error);
    check_equal(orm_query_create(connection, orm_view("memberships"), &query,
                                 &error),
                ORM_STATUS_OK);
    check_equal(orm_query_select_all(query, &error), ORM_STATUS_OK);
    check_equal(orm_query_where_key(query, key, 2u, &error),
                ORM_STATUS_INVALID_ARGUMENT);
    check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
    check_equal(orm_result_row_count(result, &rows, &error), ORM_STATUS_OK);
    check_equal(rows, (uint64_t)2u);

    orm_result_destroy(result);
    orm_query_destroy(query);
    orm_disconnect(connection);
  }

  it("rejects a composite key above the predicate limit atomically") {
    orm_error_t error;
    orm_connection_t *connection;
    orm_query_t *query = NULL;
    orm_result_t *result = NULL;
    const orm_key_part_t key[] = {
        {orm_view("domain_id"), orm_text("domain-a")},
        {orm_view("user_id"), orm_text("user-a")},
        {orm_view("group_id"), orm_text("group-a")}};
    uint64_t rows = 0u;

    orm_error_init(&error);
    connection = orm_public_flow_composite_fixture(2u, 0u, &error);
    check_equal(orm_query_create(connection, orm_view("memberships"), &query,
                                 &error),
                ORM_STATUS_OK);
    check_equal(orm_query_select_all(query, &error), ORM_STATUS_OK);
    check_equal(orm_query_where_key(query, key, 3u, &error),
                ORM_STATUS_LIMIT_EXCEEDED);
    check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
    check_equal(orm_result_row_count(result, &rows, &error), ORM_STATUS_OK);
    check_equal(rows, (uint64_t)2u);

    orm_result_destroy(result);
    orm_query_destroy(query);
    orm_disconnect(connection);
  }

  it("rolls back copied key parts when a later value exceeds the byte limit") {
    orm_error_t error;
    orm_connection_t *connection;
    orm_query_t *query = NULL;
    orm_result_t *result = NULL;
    const orm_key_part_t key[] = {
        {orm_view("domain_id"), orm_text("domain-a")},
        {orm_view("user_id"), orm_text("user-a")}};
    uint64_t rows = 0u;

    orm_error_init(&error);
    connection = orm_public_flow_composite_fixture(0u, 8u, &error);
    check_equal(orm_query_create(connection, orm_view("memberships"), &query,
                                 &error),
                ORM_STATUS_OK);
    check_equal(orm_query_select_all(query, &error), ORM_STATUS_OK);
    check_equal(orm_query_where_key(query, key, 2u, &error),
                ORM_STATUS_LIMIT_EXCEEDED);
    check_equal(orm_query_where(query, orm_view("domain_id"),
                                ORM_COMPARE_EQUAL, orm_text("domain-a"),
                                &error),
                ORM_STATUS_OK);
    check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
    check_equal(orm_result_row_count(result, &rows, &error), ORM_STATUS_OK);
    check_equal(rows, (uint64_t)2u);

    orm_result_destroy(result);
    orm_query_destroy(query);
    orm_disconnect(connection);
  }

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
    cflow_publisher source = {0};
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

    step = cflow_publisher_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    step = cflow_publisher_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_ERROR);
    check_not_null(step.error);

    cflow_publisher_destroy(&source);
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
    cflow_publisher source = {0};
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

    step = cflow_publisher_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_ERROR);
    check_not_null(step.error);

    cflow_publisher_destroy(&source);
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
    cflow_publisher source = {0};
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

    step = cflow_publisher_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(row.id, 7);
    check_equal(row.score, 19L);
    step = cflow_publisher_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_DONE);

    cflow_publisher_destroy(&source);
    orm_query_destroy(query);
    orm_disconnect(connection);
  }

  it("opens a typed SQLite Publisher that advances one row per resume") {
    orm_error_t error;
    orm_config_t connection_config;
    orm_option_t filename;
    orm_connection_t *connection = NULL;
    orm_query_t *query = NULL;
    orm_flow_config_t flow_config;
    cflow_publisher source = {0};
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
    check_true(cflow_publisher_valid(&source));
    check_equal(orm_query_open_flow(query, &flow_config, &source, &error),
                ORM_STATUS_INVALID_STATE);
    check_true(cflow_publisher_valid(&source));

    step = cflow_publisher_resume(&source, NULL, &first);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(first.id, 7);
    check_equal(first.score, 19L);

    step = cflow_publisher_resume(&source, NULL, &second);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(second.id, 11);
    check_equal(second.score, 29L);

    step = cflow_publisher_resume(&source, NULL, &terminal);
    check_equal(step.kind, CFLOW_STEP_DONE);

    cflow_publisher_destroy(&source);
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
    cflow_publisher command_publisher = {0};
    cflow_publisher row_source = {0};
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
    check_equal(orm_query_open_command_flow(command, &command_publisher, &error),
                ORM_STATUS_OK);
    check_equal(orm_raw(connection,
                        orm_view("select id, score from flow_command"),
                        &probe, &error), ORM_STATUS_OK);
    orm_flow_config(&row_config, &orm_public_flow_row_data);
    check_equal(orm_query_open_flow(probe, &row_config, &row_source, &error),
                ORM_STATUS_SQL_ERROR);
    check_false(cflow_publisher_valid(&row_source));
    orm_query_destroy(probe);
    probe = NULL;

    step = cflow_publisher_resume(&command_publisher, NULL, &command_result);
    check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(command_result.affected_rows, (uint64_t)0u);
    cflow_publisher_destroy(&command_publisher);
    command_publisher = (cflow_publisher){0};
    orm_query_destroy(command);
    command = NULL;

    check_equal(orm_raw(connection,
                        orm_view("insert into flow_command values(7, 19)"),
                        &command, &error), ORM_STATUS_OK);
    check_equal(orm_query_open_command_flow(command, &command_publisher, &error),
                ORM_STATUS_OK);
    step = cflow_publisher_resume(&command_publisher, NULL, &command_result);
    check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(command_result.affected_rows, (uint64_t)1u);
    cflow_publisher_destroy(&command_publisher);
    orm_query_destroy(command);

    check_equal(orm_raw(connection,
                        orm_view("select id, score from flow_command"),
                        &probe, &error), ORM_STATUS_OK);
    check_equal(orm_query_open_flow(probe, &row_config, &row_source, &error),
                ORM_STATUS_OK);
    step = cflow_publisher_resume(&row_source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(row.id, 7);
    check_equal(row.score, 19L);
    cflow_publisher_destroy(&row_source);
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

  it("materializes raw mutations with RETURNING including CTEs") {
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
    const char *statements[] = {
      "insert into returned values(7) returning id",
      "update returned set id=7 returning id",
      "delete from returned returning id",
      "with seed(id) as (select 7) insert into returned select id from seed returning id",
      "with seed(id) as (select 7) update returned set id=(select id from seed) returning id",
      "with seed(id) as (select 7) delete from returned where id in (select id from seed) returning id"
    };
    for (size_t i = 0; i < sizeof(statements) / sizeof(statements[0]); ++i) {
      info("SQL: %s", statements[i]);
      result = NULL;
      query = NULL;
      check_equal(orm_raw(connection, orm_view(statements[i]), &query, &error),
                  ORM_STATUS_OK);
      check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
      uint64_t rows = 0u;
      check_equal(orm_result_row_count(result, &rows, &error), ORM_STATUS_OK);
      check_equal(rows, UINT64_C(1));
      check_equal(orm_result_get_int64(result, 0u, 0u, &value, &error),
                  ORM_STATUS_OK);
      check_equal(value, (int64_t)7);
      orm_result_destroy(result);
      orm_query_destroy(query);
    }
    const char *commands[] = {
      "with seed(id) as (select 7) insert into returned select id as `returning` from seed",
      "with seed(id) as (select 7) update returned set id=(select id as [returning] from seed)",
      "with seed(id) as (select 7) delete from returned where id in (select id from seed) /* returning */"
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
      info("SQL: %s", commands[i]);
      result = NULL;
      query = NULL;
      check_equal(orm_raw(connection, orm_view(commands[i]), &query, &error),
                  ORM_STATUS_OK);
      check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
      uint64_t rows = 0u, affected = 0u;
      check_equal(orm_result_row_count(result, &rows, &error), ORM_STATUS_OK);
      check_equal(rows, UINT64_C(0));
      check_equal(orm_result_affected_rows(result, &affected, &error), ORM_STATUS_OK);
      check_equal(affected, UINT64_C(1));
      orm_result_destroy(result);
      orm_query_destroy(query);
    }
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

  it("enforces SQLite foreign keys on connect and classifies constraints") {
    orm_error_t error;
    orm_config_t connection_config;
    orm_option_t filename;
    orm_connection_t *connection = NULL;
    orm_query_t *query = NULL;
    orm_result_t *result = NULL;
    int64_t foreign_keys = 0;

    orm_error_init(&error);
    orm_config(&connection_config);
    filename.keyword = orm_view("filename");
    filename.value = orm_view(":memory:");
    connection_config.driver = orm_view("sqlite");
    connection_config.options = &filename;
    connection_config.option_count = 1u;
    check_equal(orm_connect(&connection_config, &connection, &error), ORM_STATUS_OK);

    check_equal(orm_raw(connection, orm_view("pragma foreign_keys"), &query, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
    check_equal(orm_result_get_int64(result, 0u, 0u, &foreign_keys, &error), ORM_STATUS_OK);
    check_equal(foreign_keys, (int64_t)1);
    orm_result_destroy(result);
    orm_query_destroy(query);
    result = NULL;
    query = NULL;

    check_equal(orm_raw(connection, orm_view("create table parent(id integer primary key)"),
                        &query, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
    orm_result_destroy(result);
    orm_query_destroy(query);
    result = NULL;
    query = NULL;

    check_equal(orm_raw(connection,
                        orm_view("create table child(parent_id integer not null references parent(id))"),
                        &query, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
    orm_result_destroy(result);
    orm_query_destroy(query);
    result = NULL;
    query = NULL;

    check_equal(orm_raw(connection, orm_view("insert into child(parent_id) values(7)"), &query,
                        &error),
                ORM_STATUS_OK);
    check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_CONSTRAINT);
    check_null(result);

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

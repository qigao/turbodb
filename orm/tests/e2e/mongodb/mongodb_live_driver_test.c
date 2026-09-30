#include "orm.h"
#include <orm_runtime.h>

#include <cmeta/struct.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MONGO_LIVE_DATA_PREFIX_SIZE                                         \
  (offsetof(cmeta_data_desc, shape) +                                      \
   sizeof(((cmeta_data_desc *)0)->shape))

Struct(mongo_live_row, (long, id), (long, score));

static const cmeta_type_identity mongo_live_row_identity =
    CMETA_TYPE_ID_ATOM_INIT("orm.test.MongoLiveRow");
static const cmeta_type_traits mongo_live_row_traits = {
    .flags = CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY};
static const cmeta_type_desc mongo_live_row_type = {
    .name = "mongo_live_row",
    .size = sizeof(mongo_live_row),
    .align = _Alignof(mongo_live_row),
    .kind = CMETA_T_OBJECT,
    .traits = &mongo_live_row_traits,
    .identity = &mongo_live_row_identity};
static const cmeta_data_field_desc mongo_live_row_fields[] = {
    {"orm.test.MongoLiveRow.id", "id", offsetof(mongo_live_row, id),
     &cmeta_data_long},
    {"orm.test.MongoLiveRow.score", "score", offsetof(mongo_live_row, score),
     &cmeta_data_long}};
static const cmeta_data_struct_shape mongo_live_row_shape = {
    .layout = StructMeta(mongo_live_row),
    .fields = mongo_live_row_fields,
    .field_count = 2u};
static const cmeta_data_desc mongo_live_row_data = {
    .struct_size = MONGO_LIVE_DATA_PREFIX_SIZE,
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orm.test.MongoLiveRow.data",
    .display_name = "MongoLiveRow",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &mongo_live_row_type,
    .shape = &mongo_live_row_shape};

static int fail_status(const char *operation, orm_status_t got,
                       orm_status_t expected, const orm_error_t *error) {
  if (got == expected)
    return 0;
  fprintf(stderr, "%s: expected=%d got=%d message=%s\n", operation,
          (int)expected, (int)got, error != NULL ? error->message : "");
  return 1;
}

static int run_command(orm_query_t *query, orm_error_t *error,
                       uint64_t *affected) {
  cflow_publisher publisher = {0};
  orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
  cflow_step step;
  orm_status_t status =
      orm_query_open_command_flow(query, &publisher, error);
  if (status != ORM_STATUS_OK)
    return fail_status("open command Publisher", status, ORM_STATUS_OK, error);
  step = cflow_publisher_resume(&publisher, NULL, &result);
  cflow_publisher_destroy(&publisher);
  if (step.kind != CFLOW_STEP_VALUE_AND_DONE) {
    fprintf(stderr, "command Publisher step=%d message=%s\n", (int)step.kind,
            step.error != NULL ? step.error : "");
    return 1;
  }
  if (affected != NULL)
    *affected = result.affected_rows;
  return 0;
}

static int read_score(orm_connection_t *connection, long id, long expected,
                      orm_error_t *error) {
  orm_query_t *query = NULL;
  orm_flow_config_t flow;
  cflow_publisher publisher = {0};
  mongo_live_row row = {0};
  cflow_step step;
  int failed = 0;

  if (fail_status("create MongoDB SELECT",
                  orm_query_create(connection, orm_view("people"), &query, error),
                  ORM_STATUS_OK, error))
    return 1;
  if (fail_status("add id projection",
                  orm_query_add_column(query, orm_view("id"), error),
                  ORM_STATUS_OK, error) ||
      fail_status("add score projection",
                  orm_query_add_column(query, orm_view("score"), error),
                  ORM_STATUS_OK, error) ||
      fail_status("add id predicate",
                  orm_query_where(query, orm_view("id"), ORM_COMPARE_EQUAL,
                                  orm_i64(id), error),
                  ORM_STATUS_OK, error)) {
    orm_query_destroy(query);
    return 1;
  }

  orm_flow_config(&flow, &mongo_live_row_data);
  if (fail_status("open MongoDB row Publisher",
                  orm_query_open_flow(query, &flow, &publisher, error),
                  ORM_STATUS_OK, error)) {
    orm_query_destroy(query);
    return 1;
  }
  step = cflow_publisher_resume(&publisher, NULL, &row);
  if (step.kind != CFLOW_STEP_VALUE || row.id != id || row.score != expected) {
    fprintf(stderr, "MongoDB SELECT mismatch: step=%d id=%ld score=%ld\n",
            (int)step.kind, row.id, row.score);
    failed = 1;
  }
  if (!failed) {
    step = cflow_publisher_resume(&publisher, NULL, &row);
    if (step.kind != CFLOW_STEP_DONE) {
      fprintf(stderr, "MongoDB SELECT expected DONE, got=%d\n", (int)step.kind);
      failed = 1;
    }
  }
  cflow_publisher_destroy(&publisher);
  orm_query_destroy(query);
  return failed;
}

int main(void) {
  const char *module = getenv("ORM_MONGODB_PLUGIN");
  const char *uri = getenv("ORM_MONGODB_URI");
  const long id = 9001001L;
  orm_runtime_config_t runtime_config;
  orm_runtime_t *runtime = NULL;
  orm_driver_load_config_t load;
  orm_config_t config;
  orm_option_t options[2];
  orm_connection_t *connection = NULL;
  orm_query_t *query = NULL;
  orm_error_t error;
  uint64_t affected = 0u;
  int failed = 0;

  if (module == NULL || module[0] == '\0') {
    fprintf(stderr, "ORM_MONGODB_PLUGIN is not configured\n");
    return 1;
  }
  if (uri == NULL || uri[0] == '\0')
    uri = "mongodb://127.0.0.1:27017/?serverSelectionTimeoutMS=3000";

  orm_error_init(&error);
  orm_runtime_config_init(&runtime_config);
  if (fail_status("create runtime",
                  orm_runtime_create(&runtime_config, &runtime, &error),
                  ORM_STATUS_OK, &error))
    return 1;

  memset(&load, 0, sizeof(load));
  load.struct_size = (uint32_t)sizeof(load);
  load.abi_version = ORM_RUNTIME_ABI_VERSION;
  load.module_path = orm_view(module);
  load.expected_driver_id = orm_view("mongodb");
  if (fail_status("load MongoDB Driver",
                  orm_runtime_load_driver(runtime, &load, &error),
                  ORM_STATUS_OK, &error)) {
    orm_runtime_release(runtime);
    return 1;
  }

  orm_config(&config);
  options[0] = (orm_option_t){orm_view("uri"), orm_view(uri)};
  options[1] =
      (orm_option_t){orm_view("database"), orm_view("turbodb_driver_live")};
  config.driver = orm_view("mongodb");
  config.options = options;
  config.option_count = 2u;
  if (fail_status("connect MongoDB Driver",
                  orm_runtime_connect(runtime, &config, &connection, &error),
                  ORM_STATUS_OK, &error)) {
    (void)orm_runtime_close(runtime, &error);
    orm_runtime_release(runtime);
    return 1;
  }

  /* Idempotent cleanup from any interrupted prior run. */
  if (orm_delete(connection, orm_view("people"), &query, &error) != ORM_STATUS_OK ||
      orm_query_where(query, orm_view("id"), ORM_COMPARE_EQUAL, orm_i64(id),
                      &error) != ORM_STATUS_OK ||
      run_command(query, &error, NULL) != 0)
    failed = 1;
  orm_query_destroy(query);
  query = NULL;

  if (!failed) {
    if (orm_insert(connection, orm_view("people"), &query, &error) != ORM_STATUS_OK ||
        orm_query_set(query, orm_view("id"), orm_i64(id), &error) != ORM_STATUS_OK ||
        orm_query_set(query, orm_view("score"), orm_i64(19), &error) != ORM_STATUS_OK ||
        run_command(query, &error, &affected) != 0 || affected != 1u) {
      fprintf(stderr, "MongoDB INSERT failed/affected=%llu: %s\n",
              (unsigned long long)affected, error.message);
      failed = 1;
    }
    orm_query_destroy(query);
    query = NULL;
  }

  if (!failed)
    failed = read_score(connection, id, 19L, &error);

  if (!failed) {
    affected = 0u;
    if (orm_update(connection, orm_view("people"), &query, &error) != ORM_STATUS_OK ||
        orm_query_set(query, orm_view("score"), orm_i64(29), &error) != ORM_STATUS_OK ||
        orm_query_where(query, orm_view("id"), ORM_COMPARE_EQUAL, orm_i64(id),
                        &error) != ORM_STATUS_OK ||
        run_command(query, &error, &affected) != 0 || affected != 1u) {
      fprintf(stderr, "MongoDB UPDATE failed/affected=%llu: %s\n",
              (unsigned long long)affected, error.message);
      failed = 1;
    }
    orm_query_destroy(query);
    query = NULL;
  }

  if (!failed)
    failed = read_score(connection, id, 29L, &error);

  if (!failed) {
    affected = 0u;
    if (orm_delete(connection, orm_view("people"), &query, &error) != ORM_STATUS_OK ||
        orm_query_where(query, orm_view("id"), ORM_COMPARE_EQUAL, orm_i64(id),
                        &error) != ORM_STATUS_OK ||
        run_command(query, &error, &affected) != 0 || affected != 1u) {
      fprintf(stderr, "MongoDB DELETE failed/affected=%llu: %s\n",
              (unsigned long long)affected, error.message);
      failed = 1;
    }
    orm_query_destroy(query);
    query = NULL;
  }

  orm_error_init(&error);
  if (orm_runtime_close(runtime, &error) != ORM_STATUS_BUSY) {
    fprintf(stderr, "live MongoDB connection did not retain Plugin lease: %s\n",
            error.message);
    failed = 1;
  }

  orm_disconnect(connection);
  connection = NULL;
  orm_error_init(&error);
  if (orm_runtime_close(runtime, &error) != ORM_STATUS_OK) {
    fprintf(stderr, "close MongoDB runtime failed: %s\n", error.message);
    failed = 1;
  }
  orm_runtime_release(runtime);
  return failed;
}

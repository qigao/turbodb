#include "orm_cbind_source.h"
#include "orm_mongo_cursor.h"

#include <cmeta/struct.h>
#include "tinymock.h"

#include <stddef.h>
#include <string.h>

#define ORM_MONGO_TEST_DATA_PREFIX_SIZE                                      \
  (offsetof(cmeta_data_desc, shape) +                                         \
   sizeof(((cmeta_data_desc *)0)->shape))

Struct(orm_mongo_test_row,
    (int, id),
    (long, score)
);

static const cmeta_type_identity orm_mongo_test_row_identity =
    CMETA_TYPE_ID_ATOM_INIT("orm.test.MongoRow");
static const cmeta_type_traits orm_mongo_test_row_traits = {
    .flags = CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY};
static const cmeta_type_desc orm_mongo_test_row_type = {
    .name = "orm_mongo_test_row",
    .size = sizeof(orm_mongo_test_row),
    .align = _Alignof(orm_mongo_test_row),
    .kind = CMETA_T_OBJECT,
    .pointee = NULL,
    .traits = &orm_mongo_test_row_traits,
    .identity = &orm_mongo_test_row_identity};
static const cmeta_data_field_desc orm_mongo_test_row_fields[] = {
    {"orm.test.MongoRow.id", "id", offsetof(orm_mongo_test_row, id),
     &cmeta_data_int},
    {"orm.test.MongoRow.score", "score",
     offsetof(orm_mongo_test_row, score), &cmeta_data_long}};
static const cmeta_data_struct_shape orm_mongo_test_row_shape = {
    .layout = StructMeta(orm_mongo_test_row),
    .fields = orm_mongo_test_row_fields,
    .field_count = sizeof(orm_mongo_test_row_fields) /
                   sizeof(orm_mongo_test_row_fields[0])};
static const cmeta_data_desc orm_mongo_test_row_data = {
    .struct_size = ORM_MONGO_TEST_DATA_PREFIX_SIZE,
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orm.test.MongoRow.data",
    .display_name = "MongoRow",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &orm_mongo_test_row_type,
    .shape = &orm_mongo_test_row_shape};

typedef struct orm_mongo_test_document {
  int64_t id;
  int64_t score;
} orm_mongo_test_document;

typedef struct orm_mongo_test_driver_state {
  const orm_mongo_test_document *documents;
  size_t document_count;
  size_t index;
  int fail_next;
} orm_mongo_test_driver_state;

TINYMOCk_MOCK_VOID(orm_mongo_test_destroy, void *)

static orm_mongo_driver_step orm_mongo_test_next(void *context,
                                                  const void **document) {
  orm_mongo_test_driver_state *state =
      (orm_mongo_test_driver_state *)context;
  orm_mongo_driver_step step = ORM_MONGO_DRIVER_STEP_INIT;
  if (state->fail_next) {
    step.kind = ORM_MONGO_DRIVER_ERROR;
    step.status = ORM_STATUS_DATASTORE_ERROR;
    step.message = "fake MongoDB cursor failure";
    return step;
  }
  if (state->index == state->document_count)
    return step;
  *document = &state->documents[state->index++];
  step.kind = ORM_MONGO_DRIVER_ROW;
  return step;
}

static orm_status_t orm_mongo_test_find(void *context, const void *document,
                                         const unsigned char *path,
                                         size_t path_size,
                                         orm_mongo_value *out) {
  const orm_mongo_test_document *row =
      (const orm_mongo_test_document *)document;
  (void)context;
  *out = (orm_mongo_value)ORM_MONGO_VALUE_INIT;
  if (path_size == 3u && memcmp(path, "_id", 3u) == 0) {
    out->kind = ORM_MONGO_VALUE_SINT;
    out->data.sint = row->id;
    return ORM_STATUS_OK;
  }
  if (path_size == 5u && memcmp(path, "score", 5u) == 0) {
    out->kind = ORM_MONGO_VALUE_SINT;
    out->data.sint = row->score;
    return ORM_STATUS_OK;
  }
  out->kind = ORM_MONGO_VALUE_NULL;
  return ORM_STATUS_OK;
}

static const orm_mongo_driver_ops orm_mongo_test_ops = {
    sizeof(orm_mongo_driver_ops), ORM_MONGO_DRIVER_OPS_ABI_VERSION,
    orm_mongo_test_next, orm_mongo_test_find, orm_mongo_test_destroy};

spec("ORM MongoDB CFlow cursor") {
  it("accepts the exact byte budget then rejects the next document") {
    static const unsigned char id[] = "id";
    static const unsigned char native_id[] = "_id";
    static const unsigned char score[] = "score";
    const orm_mongo_test_document documents[] = {{7, 19}, {11, 29}};
    orm_mongo_test_driver_state state = {documents, 2u, 0u, 0};
    orm_mongo_driver driver = {&orm_mongo_test_ops, &state};
    const orm_mongo_field fields[] = {
        {{id, 2u}, {native_id, 3u}}, {{score, 5u}, {score, 5u}}};
    orm_mongo_cursor_config cursor_config = ORM_MONGO_CURSOR_CONFIG_INIT(
        4u, sizeof(int64_t) * 2u, 32u);
    orm_row_cursor cursor = {0};
    orm_error_t error;
    cserde_reader reader = {0};
    orm_row_cursor_step step;

    orm_error_init(&error);
    mock_orm_mongo_test_destroy_reset();
    mock_orm_mongo_test_destroy_expect(TINYMOCk_ARG((void *)&state));
    check_equal(orm_mongo_cursor_start(&cursor, &driver, fields, 2u,
                                       &cursor_config, &error),
                ORM_STATUS_OK);
    check_equal(cursor.ops->next(cursor.context, &reader).kind,
                ORM_ROW_CURSOR_ROW);
    step = cursor.ops->next(cursor.context, &reader);
    check_equal(step.kind, ORM_ROW_CURSOR_ERROR);
    check_equal(step.status, ORM_STATUS_LIMIT_EXCEEDED);
    cursor.ops->destroy(cursor.context);
    mock_orm_mongo_test_destroy_verify();
  }

  it("rejects a document beyond max_result_bytes") {
    static const unsigned char id[] = "id";
    static const unsigned char native_id[] = "_id";
    static const unsigned char score[] = "score";
    const orm_mongo_test_document documents[] = {{7, 19}};
    orm_mongo_test_driver_state state = {documents, 1u, 0u, 0};
    orm_mongo_driver driver = {&orm_mongo_test_ops, &state};
    const orm_mongo_field fields[] = {
        {{id, 2u}, {native_id, 3u}}, {{score, 5u}, {score, 5u}}};
    orm_mongo_cursor_config cursor_config =
        ORM_MONGO_CURSOR_CONFIG_INIT(4u, sizeof(int64_t) * 2u - 1u, 32u);
    orm_row_cursor cursor = {0};
    orm_error_t error;
    cserde_reader reader = {0};
    orm_row_cursor_step step;

    orm_error_init(&error);
    mock_orm_mongo_test_destroy_reset();
    mock_orm_mongo_test_destroy_expect(TINYMOCk_ARG((void *)&state));
    check_equal(orm_mongo_cursor_start(&cursor, &driver, fields, 2u,
                                       &cursor_config, &error),
                ORM_STATUS_OK);
    step = cursor.ops->next(cursor.context, &reader);
    check_equal(step.kind, ORM_ROW_CURSOR_ERROR);
    check_equal(step.status, ORM_STATUS_LIMIT_EXCEEDED);
    cursor.ops->destroy(cursor.context);
    mock_orm_mongo_test_destroy_verify();
  }

  it("advances one native document per resume and destroys the driver once") {
    static const unsigned char id[] = "id";
    static const unsigned char native_id[] = "_id";
    static const unsigned char score[] = "score";
    const orm_mongo_test_document documents[] = {{7, 19}, {11, 29}};
    orm_mongo_test_driver_state state = {documents, 2u, 0u, 0};
    orm_mongo_driver driver = {&orm_mongo_test_ops, &state};
    const orm_mongo_field fields[] = {
        {{id, 2u}, {native_id, 3u}}, {{score, 5u}, {score, 5u}}};
    orm_mongo_cursor_config cursor_config =
        ORM_MONGO_CURSOR_CONFIG_INIT(4u, UINT64_MAX, 32u);
    orm_row_cursor cursor = {0};
    orm_error_t error;
    orm_cbind_source_config source_config = ORM_CBIND_SOURCE_CONFIG_INIT(
        &orm_mongo_test_row_data, 1u, 1u, 2u, 1u);
    cflow_source source = {0};
    orm_mongo_test_row first = {0};
    orm_mongo_test_row second = {0};
    cflow_step step;

    orm_error_init(&error);
    mock_orm_mongo_test_destroy_reset();
    mock_orm_mongo_test_destroy_expect(TINYMOCk_ARG((void *)&state));
    check_equal(orm_mongo_cursor_start(&cursor, &driver, fields, 2u,
                                       &cursor_config, &error),
                ORM_STATUS_OK);
    check_null(driver.ops);
    check_equal(orm_cbind_source_init(&source, &cursor, &source_config,
                                      &error),
                ORM_STATUS_OK);

    step = cflow_source_resume(&source, NULL, &first);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(first.id, 7);
    check_equal(first.score, 19L);
    step = cflow_source_resume(&source, NULL, &second);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(second.id, 11);
    check_equal(second.score, 29L);
    step = cflow_source_resume(&source, NULL, &second);
    check_equal(step.kind, CFLOW_STEP_DONE);

    cflow_source_destroy(&source);
    mock_orm_mongo_test_destroy_verify();
  }

  it("propagates a native cursor failure and destroys the driver once") {
    static const unsigned char id[] = "id";
    static const unsigned char native_id[] = "_id";
    orm_mongo_test_driver_state state = {NULL, 0u, 0u, 1};
    orm_mongo_driver driver = {&orm_mongo_test_ops, &state};
    const orm_mongo_field field = {{id, 2u}, {native_id, 3u}};
    orm_mongo_cursor_config cursor_config =
        ORM_MONGO_CURSOR_CONFIG_INIT(4u, UINT64_MAX, 32u);
    orm_row_cursor cursor = {0};
    orm_error_t error;
    orm_cbind_source_config source_config = ORM_CBIND_SOURCE_CONFIG_INIT(
        &orm_mongo_test_row_data, 1u, 1u, 2u, 1u);
    cflow_source source = {0};
    orm_mongo_test_row row = {0};
    cflow_step step;

    orm_error_init(&error);
    mock_orm_mongo_test_destroy_reset();
    mock_orm_mongo_test_destroy_expect(TINYMOCk_ARG((void *)&state));
    check_equal(orm_mongo_cursor_start(&cursor, &driver, &field, 1u,
                                       &cursor_config, &error),
                ORM_STATUS_OK);
    check_equal(orm_cbind_source_init(&source, &cursor, &source_config,
                                      &error), ORM_STATUS_OK);
    step = cflow_source_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_ERROR);
    check_not_null(step.error);
    cflow_source_destroy(&source);
    mock_orm_mongo_test_destroy_verify();
  }
}

#include "orm_cbind_source.h"
#include "orm_redis_cursor.h"

#include <cmeta/struct.h>
#include "tinymock.h"

#include <stddef.h>

#define ORM_REDIS_TEST_DATA_PREFIX_SIZE                                      \
  (offsetof(cmeta_data_desc, shape) + sizeof(((cmeta_data_desc *)0)->shape))

Struct(orm_redis_test_row,
    (int, id),
    (long, score)
);

static const cmeta_type_identity orm_redis_test_row_identity =
    CMETA_TYPE_ID_ATOM_INIT("orm.test.RedisRow");
static const cmeta_type_traits orm_redis_test_row_traits = {
    .flags = CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY};
static const cmeta_type_desc orm_redis_test_row_type = {
    .name = "orm_redis_test_row",
    .size = sizeof(orm_redis_test_row),
    .align = _Alignof(orm_redis_test_row),
    .kind = CMETA_T_OBJECT,
    .traits = &orm_redis_test_row_traits,
    .identity = &orm_redis_test_row_identity};
static const cmeta_data_field_desc orm_redis_test_row_fields[] = {
    {"orm.test.RedisRow.id", "id", offsetof(orm_redis_test_row, id),
     &cmeta_data_int},
    {"orm.test.RedisRow.score", "score", offsetof(orm_redis_test_row, score),
     &cmeta_data_long}};
static const cmeta_data_struct_shape orm_redis_test_row_shape = {
    .layout = StructMeta(orm_redis_test_row),
    .fields = orm_redis_test_row_fields,
    .field_count = 2u};
static const cmeta_data_desc orm_redis_test_row_data = {
    .struct_size = ORM_REDIS_TEST_DATA_PREFIX_SIZE,
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orm.test.RedisRow.data",
    .display_name = "RedisRow",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &orm_redis_test_row_type,
    .shape = &orm_redis_test_row_shape};

typedef struct orm_redis_test_reply {
  orm_redis_reply_kind kind;
  int64_t integer;
  const unsigned char *bytes;
  size_t byte_count;
  const struct orm_redis_test_reply *const *children;
  size_t child_count;
} orm_redis_test_reply;

typedef struct orm_redis_test_driver {
  const orm_redis_test_reply *rows[2];
  size_t next;
  cflow_waitable waitable;
  size_t waits_remaining;
  size_t cancel_count;
} orm_redis_test_driver;

typedef struct orm_redis_test_waitable {
  size_t arm_count;
  size_t cancel_count;
} orm_redis_test_waitable;

static bool orm_redis_test_waitable_arm(void *context, cflow_waker waker) {
  orm_redis_test_waitable *waitable = (orm_redis_test_waitable *)context;
  (void)waker;
  ++waitable->arm_count;
  return true;
}

static void orm_redis_test_waitable_cancel(void *context) {
  orm_redis_test_waitable *waitable = (orm_redis_test_waitable *)context;
  ++waitable->cancel_count;
}

CMETA_IMPLEMENTS(cflow_waitable, orm_redis_test_waitable, 0,
    .arm = orm_redis_test_waitable_arm,
    .cancel = orm_redis_test_waitable_cancel
);

TINYMOCk_MOCK_VOID(orm_redis_test_release, void *, void *)
TINYMOCk_MOCK_VOID(orm_redis_test_destroy, void *)

static orm_redis_reply_kind orm_redis_test_kind(void *context,
                                                const void *reply) {
  (void)context;
  return ((const orm_redis_test_reply *)reply)->kind;
}

static int64_t orm_redis_test_integer(void *context, const void *reply) {
  (void)context;
  return ((const orm_redis_test_reply *)reply)->integer;
}

static const unsigned char *orm_redis_test_bytes(void *context,
                                                  const void *reply,
                                                  size_t *size) {
  const orm_redis_test_reply *value = (const orm_redis_test_reply *)reply;
  (void)context;
  *size = value->byte_count;
  return value->bytes;
}

static size_t orm_redis_test_child_count(void *context, const void *reply) {
  (void)context;
  return ((const orm_redis_test_reply *)reply)->child_count;
}

static const void *orm_redis_test_child(void *context, const void *reply,
                                        size_t index) {
  (void)context;
  return ((const orm_redis_test_reply *)reply)->children[index];
}

static orm_redis_driver_step orm_redis_test_next(void *context, void **row) {
  orm_redis_test_driver *driver = (orm_redis_test_driver *)context;
  orm_redis_driver_step step = ORM_REDIS_DRIVER_STEP_INIT;
  *row = NULL;
  if (driver->waits_remaining != 0u) {
    --driver->waits_remaining;
    step.kind = ORM_REDIS_DRIVER_WAIT;
    step.waitable = driver->waitable;
    return step;
  }
  if (driver->next == 2u) return step;
  *row = (void *)driver->rows[driver->next++];
  step.kind = ORM_REDIS_DRIVER_ROW;
  return step;
}

static void orm_redis_test_cancel(void *context) {
  orm_redis_test_driver *driver = (orm_redis_test_driver *)context;
  ++driver->cancel_count;
}

static void orm_redis_test_release_call(void *context, void *row) {
  orm_redis_test_release(context, row);
}

static void orm_redis_test_destroy_call(void *context) {
  orm_redis_test_destroy(context);
}

static const orm_redis_reply_ops orm_redis_test_reply_ops = {
    sizeof(orm_redis_reply_ops), ORM_REDIS_REPLY_OPS_ABI_VERSION,
    orm_redis_test_kind, orm_redis_test_integer, orm_redis_test_bytes,
    orm_redis_test_child_count, orm_redis_test_child};

static const orm_redis_row_driver_ops orm_redis_test_driver_ops = {
    sizeof(orm_redis_row_driver_ops), ORM_REDIS_ROW_DRIVER_OPS_ABI_VERSION,
    orm_redis_test_next, orm_redis_test_cancel, orm_redis_test_release_call,
    orm_redis_test_destroy_call};

spec("ORM Redis CFlow cursor") {
  it("requests and releases exactly one owned reply row per resume") {
    static const unsigned char id_name[] = "id";
    static const unsigned char score_name[] = "score";
    const orm_redis_test_reply field_id = {
        ORM_REDIS_REPLY_STRING, 0, id_name, 2u, NULL, 0u};
    const orm_redis_test_reply field_score = {
        ORM_REDIS_REPLY_STRING, 0, score_name, 5u, NULL, 0u};
    const orm_redis_test_reply value_7 = {
        ORM_REDIS_REPLY_INTEGER, 7, NULL, 0u, NULL, 0u};
    const orm_redis_test_reply value_19 = {
        ORM_REDIS_REPLY_INTEGER, 19, NULL, 0u, NULL, 0u};
    const orm_redis_test_reply value_11 = {
        ORM_REDIS_REPLY_INTEGER, 11, NULL, 0u, NULL, 0u};
    const orm_redis_test_reply value_29 = {
        ORM_REDIS_REPLY_INTEGER, 29, NULL, 0u, NULL, 0u};
    const orm_redis_test_reply *row_1_children[] = {
        &field_id, &value_7, &field_score, &value_19};
    const orm_redis_test_reply *row_2_children[] = {
        &field_id, &value_11, &field_score, &value_29};
    const orm_redis_test_reply row_1 = {
        ORM_REDIS_REPLY_ARRAY, 0, NULL, 0u, row_1_children, 4u};
    const orm_redis_test_reply row_2 = {
        ORM_REDIS_REPLY_ARRAY, 0, NULL, 0u, row_2_children, 4u};
    const orm_redis_field_view fields[] = {
        {id_name, 2u}, {score_name, 5u}};
    orm_redis_test_driver driver_context = {
        .rows = {&row_1, &row_2}, .next = 0u};
    orm_redis_row_driver driver = {
        &orm_redis_test_driver_ops, &orm_redis_test_reply_ops, &driver_context};
    orm_redis_cursor_config cursor_config =
        ORM_REDIS_CURSOR_CONFIG_INIT(4u, 16u, UINT64_C(5000000000));
    orm_row_cursor cursor = {0};
    orm_error_t error;
    orm_cbind_source_config source_config = ORM_CBIND_SOURCE_CONFIG_INIT(
        &orm_redis_test_row_data, 1u, 1u, 2u, 1u);
    cflow_source source = {0};
    orm_redis_test_row first = {0};
    orm_redis_test_row second = {0};
    cflow_step step;

    orm_error_init(&error);
    mock_orm_redis_test_release_reset();
    mock_orm_redis_test_destroy_reset();
    mock_orm_redis_test_release_expect(
        TINYMOCk_ARG((void *)&driver_context), TINYMOCk_ARG((void *)&row_1));
    mock_orm_redis_test_release_expect(
        TINYMOCk_ARG((void *)&driver_context), TINYMOCk_ARG((void *)&row_2));
    mock_orm_redis_test_destroy_expect(
        TINYMOCk_ARG((void *)&driver_context));
    check_equal(orm_redis_cursor_start(&cursor, &driver, fields, 2u,
                                       &cursor_config, &error),
                ORM_STATUS_OK);
    check_null(driver.context);
    check_equal(cursor.wait_timeout_ns, UINT64_C(5000000000));
    check_equal(orm_cbind_source_init(&source, &cursor, &source_config, &error),
                ORM_STATUS_OK);
    check_equal(cursor.wait_timeout_ns, 0u);

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
    mock_orm_redis_test_release_verify();
    mock_orm_redis_test_destroy_verify();
  }

  it("propagates driver WAIT and cancellation without manufacturing a row") {
    orm_redis_test_waitable waitable = {0};
    orm_redis_test_driver driver_context = {
        .waitable = orm_redis_test_waitable_as_cflow_waitable(&waitable),
        .waits_remaining = 1u};
    orm_redis_row_driver driver = {
        &orm_redis_test_driver_ops, &orm_redis_test_reply_ops, &driver_context};
    orm_redis_cursor_config cursor_config =
        ORM_REDIS_CURSOR_CONFIG_INIT(1u, 1u, UINT64_C(5000000000));
    orm_row_cursor cursor = {0};
    orm_error_t error;
    orm_cbind_source_config source_config = ORM_CBIND_SOURCE_CONFIG_INIT(
        &orm_redis_test_row_data, 1u, 1u, 2u, 1u);
    cflow_source source = {0};
    orm_redis_test_row row = {0};
    cflow_step step;

    orm_error_init(&error);
    mock_orm_redis_test_release_reset();
    mock_orm_redis_test_destroy_reset();
    mock_orm_redis_test_destroy_expect(
        TINYMOCk_ARG((void *)&driver_context));
    check_equal(orm_redis_cursor_start(&cursor, &driver, NULL, 0u,
                                       &cursor_config, &error),
                ORM_STATUS_OK);
    check_equal(orm_cbind_source_init(&source, &cursor, &source_config, &error),
                ORM_STATUS_OK);

    step = cflow_source_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_WAIT);
    check_true(cflow_waitable_valid(&step.waitable));
    check_true(cflow_waitable_arm(&step.waitable, (cflow_waker){0}));
    cflow_waitable_cancel(&step.waitable);
    check_equal(waitable.arm_count, 1u);
    check_equal(waitable.cancel_count, 1u);

    cflow_source_cancel(&source);
    check_equal(driver_context.cancel_count, 1u);
    cflow_source_destroy(&source);
    mock_orm_redis_test_destroy_verify();
  }
}

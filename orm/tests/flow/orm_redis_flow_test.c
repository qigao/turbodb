#include "orm_cbind_publisher.h"
#include "orm_redis_cursor.h"

#include <cmeta/struct.h>
#include "tinymock.h"

#include <stddef.h>

#define ORM_REDIS_TEST_DATA_PREFIX_SIZE                                      \
  (offsetof(cmeta_data_desc, shape) + sizeof(((cmeta_data_desc *)0)->shape))

Struct(orm_redis_test_row,
    (int, id),
    (long, score),
    (bool, enabled),
    (size_t, count),
    (double, ratio)
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
     &cmeta_data_long},
    {"orm.test.RedisRow.enabled", "enabled",
     offsetof(orm_redis_test_row, enabled), &cmeta_data_bool},
    {"orm.test.RedisRow.count", "count", offsetof(orm_redis_test_row, count),
     &cmeta_data_size},
    {"orm.test.RedisRow.ratio", "ratio", offsetof(orm_redis_test_row, ratio),
     &cmeta_data_double}};
static const cmeta_data_struct_shape orm_redis_test_row_shape = {
    .layout = StructMeta(orm_redis_test_row),
    .fields = orm_redis_test_row_fields,
    .field_count = 5u};
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

static void orm_redis_test_reject_integer_for_shape(
    const unsigned char *field_name, size_t field_name_size, int64_t value) {
  const orm_redis_test_reply field = {
      ORM_REDIS_REPLY_STRING, 0, field_name, field_name_size, NULL, 0u};
  const orm_redis_test_reply integer = {
      ORM_REDIS_REPLY_INTEGER, value, NULL, 0u, NULL, 0u};
  const orm_redis_test_reply *children[] = {&field, &integer};
  const orm_redis_test_reply row = {
      ORM_REDIS_REPLY_ARRAY, 0, NULL, 0u, children, 2u};
  const orm_redis_field_view projection = {field_name, field_name_size};
  orm_redis_test_driver driver_context = {.rows = {&row, NULL}, .next = 0u};
  orm_redis_row_driver driver = {
      &orm_redis_test_driver_ops, &orm_redis_test_reply_ops, &driver_context};
  orm_redis_cursor_config cursor_config =
      ORM_REDIS_CURSOR_CONFIG_INIT(1u, field_name_size, UINT64_C(1024));
  orm_row_cursor cursor = {0};
  orm_error_t error;
  cserde_reader reader = {0};
  cserde_token token = {0};

  orm_error_init(&error);
  mock_orm_redis_test_release_reset();
  mock_orm_redis_test_destroy_reset();
  mock_orm_redis_test_release_expect(
      TINYMOCk_ARG((void *)&driver_context), TINYMOCk_ARG((void *)&row));
  mock_orm_redis_test_destroy_expect(
      TINYMOCk_ARG((void *)&driver_context));
  check_equal(orm_redis_cursor_start(&cursor, &driver, &projection, 1u,
                                     &cursor_config, &error),
              ORM_STATUS_OK);
  check_equal(cursor.ops->configure_shape(cursor.context,
                                           &orm_redis_test_row_data, &error),
              ORM_STATUS_OK);
  check_equal(cursor.ops->next(cursor.context, &reader).kind,
              ORM_ROW_CURSOR_ROW);
  check_equal(cserde_reader_next(&reader, &token), CSERDE_OK);
  check_equal(token.kind, CSERDE_MAP_BEGIN);
  check_equal(cserde_reader_next(&reader, &token), CSERDE_OK);
  check_equal(token.kind, CSERDE_STRING);
  check_equal(cserde_reader_next(&reader, &token), CSERDE_SOURCE_ERROR);
  cursor.ops->destroy(cursor.context);
  mock_orm_redis_test_release_verify();
  mock_orm_redis_test_destroy_verify();
}

spec("ORM Redis CFlow cursor") {
  it("rejects RESP integers outside declared boolean and unsigned domains") {
    static const unsigned char enabled_name[] = "enabled";
    static const unsigned char count_name[] = "count";
    orm_redis_test_reject_integer_for_shape(enabled_name,
                                             sizeof(enabled_name) - 1u, 2);
    orm_redis_test_reject_integer_for_shape(count_name,
                                             sizeof(count_name) - 1u, -1);
  }

  it("rejects a projection missing from the configured row shape") {
    static const unsigned char missing_name[] = "missing";
    const orm_redis_field_view projection = {
        missing_name, sizeof(missing_name) - 1u};
    orm_redis_test_driver driver_context = {0};
    orm_redis_row_driver driver = {
        &orm_redis_test_driver_ops, &orm_redis_test_reply_ops, &driver_context};
    orm_redis_cursor_config cursor_config =
        ORM_REDIS_CURSOR_CONFIG_INIT(1u, sizeof(missing_name) - 1u,
                                     UINT64_C(1024));
    orm_row_cursor cursor = {0};
    orm_error_t error;

    orm_error_init(&error);
    mock_orm_redis_test_destroy_reset();
    mock_orm_redis_test_destroy_expect(
        TINYMOCk_ARG((void *)&driver_context));
    check_equal(orm_redis_cursor_start(&cursor, &driver, &projection, 1u,
                                       &cursor_config, &error),
                ORM_STATUS_OK);
    check_equal(cursor.ops->configure_shape(cursor.context,
                                             &orm_redis_test_row_data,
                                             &error),
                ORM_STATUS_TYPE_ERROR);
    cursor.ops->destroy(cursor.context);
    mock_orm_redis_test_destroy_verify();
  }

  it("requests and releases exactly one owned reply row per resume") {
    static const unsigned char id_name[] = "id";
    static const unsigned char score_name[] = "score";
    static const unsigned char enabled_name[] = "enabled";
    static const unsigned char count_name[] = "count";
    static const unsigned char ratio_name[] = "ratio";
    static const unsigned char value_7_text[] = "7";
    static const unsigned char value_19_text[] = "19";
    static const unsigned char value_11_text[] = "11";
    static const unsigned char value_29_text[] = "29";
    static const unsigned char enabled_1_text[] = "1";
    static const unsigned char count_42_text[] = "42";
    static const unsigned char count_84_text[] = "84";
    static const unsigned char ratio_125_text[] = "1.25";
    static const unsigned char ratio_25_text[] = "2.5";
    const orm_redis_test_reply field_id = {
        ORM_REDIS_REPLY_STRING, 0, id_name, 2u, NULL, 0u};
    const orm_redis_test_reply field_score = {
        ORM_REDIS_REPLY_STRING, 0, score_name, 5u, NULL, 0u};
    const orm_redis_test_reply field_enabled = {
        ORM_REDIS_REPLY_STRING, 0, enabled_name, 7u, NULL, 0u};
    const orm_redis_test_reply field_count = {
        ORM_REDIS_REPLY_STRING, 0, count_name, 5u, NULL, 0u};
    const orm_redis_test_reply field_ratio = {
        ORM_REDIS_REPLY_STRING, 0, ratio_name, 5u, NULL, 0u};
    const orm_redis_test_reply value_7 = {
        ORM_REDIS_REPLY_STRING, 0, value_7_text, 1u, NULL, 0u};
    const orm_redis_test_reply value_19 = {
        ORM_REDIS_REPLY_STRING, 0, value_19_text, 2u, NULL, 0u};
    const orm_redis_test_reply value_11 = {
        ORM_REDIS_REPLY_STRING, 0, value_11_text, 2u, NULL, 0u};
    const orm_redis_test_reply value_29 = {
        ORM_REDIS_REPLY_STRING, 0, value_29_text, 2u, NULL, 0u};
    const orm_redis_test_reply enabled_1 = {
        ORM_REDIS_REPLY_STRING, 0, enabled_1_text, 1u, NULL, 0u};
    const orm_redis_test_reply enabled_0 = {
        ORM_REDIS_REPLY_INTEGER, 0, NULL, 0u, NULL, 0u};
    const orm_redis_test_reply count_42 = {
        ORM_REDIS_REPLY_STRING, 0, count_42_text, 2u, NULL, 0u};
    const orm_redis_test_reply count_84 = {
        ORM_REDIS_REPLY_STRING, 0, count_84_text, 2u, NULL, 0u};
    const orm_redis_test_reply ratio_125 = {
        ORM_REDIS_REPLY_STRING, 0, ratio_125_text, 4u, NULL, 0u};
    const orm_redis_test_reply ratio_25 = {
        ORM_REDIS_REPLY_STRING, 0, ratio_25_text, 3u, NULL, 0u};
    const orm_redis_test_reply *row_1_children[] = {
        &field_id, &value_7, &field_score, &value_19,
        &field_enabled, &enabled_1, &field_count, &count_42,
        &field_ratio, &ratio_125};
    const orm_redis_test_reply *row_2_children[] = {
        &field_id, &value_11, &field_score, &value_29,
        &field_enabled, &enabled_0, &field_count, &count_84,
        &field_ratio, &ratio_25};
    const orm_redis_test_reply row_1 = {
        ORM_REDIS_REPLY_ARRAY, 0, NULL, 0u, row_1_children, 10u};
    const orm_redis_test_reply row_2 = {
        ORM_REDIS_REPLY_ARRAY, 0, NULL, 0u, row_2_children, 10u};
    const orm_redis_field_view fields[] = {
        {id_name, 2u}, {score_name, 5u}, {enabled_name, 7u},
        {count_name, 5u}, {ratio_name, 5u}};
    orm_redis_test_driver driver_context = {
        .rows = {&row_1, &row_2}, .next = 0u};
    orm_redis_row_driver driver = {
        &orm_redis_test_driver_ops, &orm_redis_test_reply_ops, &driver_context};
    orm_redis_cursor_config cursor_config =
        ORM_REDIS_CURSOR_CONFIG_INIT(4u, 32u, UINT64_C(5000000000));
    orm_row_cursor cursor = {0};
    orm_error_t error;
    orm_cbind_publisher_config publisher_config = ORM_CBIND_PUBLISHER_CONFIG_INIT(
        &orm_redis_test_row_data, 1u, 1u, 5u, 1u);
    cflow_publisher source = {0};
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
    check_equal(orm_redis_cursor_start(&cursor, &driver, fields, 5u,
                                       &cursor_config, &error),
                ORM_STATUS_OK);
    check_null(driver.context);
    check_equal(cursor.wait_timeout_ns, UINT64_C(5000000000));
    check_equal(orm_cbind_publisher_init(&source, &cursor, &publisher_config, &error),
                ORM_STATUS_OK);
    check_equal(cursor.wait_timeout_ns, 0u);

    step = cflow_publisher_resume(&source, NULL, &first);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(first.id, 7);
    check_equal(first.score, 19L);
    check_true(first.enabled);
    check_equal(first.count, (size_t)42u);
    check_equal(first.ratio, 1.25);
    step = cflow_publisher_resume(&source, NULL, &second);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(second.id, 11);
    check_equal(second.score, 29L);
    check_false(second.enabled);
    check_equal(second.count, (size_t)84u);
    check_equal(second.ratio, 2.5);
    step = cflow_publisher_resume(&source, NULL, &second);
    check_equal(step.kind, CFLOW_STEP_DONE);

    cflow_publisher_destroy(&source);
    mock_orm_redis_test_release_verify();
    mock_orm_redis_test_destroy_verify();
  }

  it("rejects non-canonical numeric bulk strings at the Redis boundary") {
    static const unsigned char ratio_name[] = "ratio";
    static const unsigned char padded_ratio_text[] = " 1.25";
    const orm_redis_test_reply field_ratio = {
        ORM_REDIS_REPLY_STRING, 0, ratio_name, 5u, NULL, 0u};
    const orm_redis_test_reply padded_ratio = {
        ORM_REDIS_REPLY_STRING, 0, padded_ratio_text, 5u, NULL, 0u};
    const orm_redis_test_reply *children[] = {&field_ratio, &padded_ratio};
    const orm_redis_test_reply row = {
        ORM_REDIS_REPLY_ARRAY, 0, NULL, 0u, children, 2u};
    const orm_redis_field_view field = {ratio_name, 5u};
    orm_redis_test_driver driver_context = {
        .rows = {&row, NULL}, .next = 0u};
    orm_redis_row_driver driver = {
        &orm_redis_test_driver_ops, &orm_redis_test_reply_ops, &driver_context};
    orm_redis_cursor_config cursor_config =
        ORM_REDIS_CURSOR_CONFIG_INIT(1u, 5u, UINT64_C(5000000000));
    orm_row_cursor cursor = {0};
    orm_error_t error;
    cserde_reader reader = {0};
    cserde_token token = {0};

    orm_error_init(&error);
    mock_orm_redis_test_release_reset();
    mock_orm_redis_test_destroy_reset();
    mock_orm_redis_test_release_expect(
        TINYMOCk_ARG((void *)&driver_context), TINYMOCk_ARG((void *)&row));
    mock_orm_redis_test_destroy_expect(
        TINYMOCk_ARG((void *)&driver_context));
    check_equal(orm_redis_cursor_start(&cursor, &driver, &field, 1u,
                                       &cursor_config, &error),
                ORM_STATUS_OK);
    check_equal(cursor.ops->configure_shape(cursor.context,
                                             &orm_redis_test_row_data,
                                             &error),
                ORM_STATUS_OK);
    check_equal(cursor.ops->next(cursor.context, &reader).kind,
                ORM_ROW_CURSOR_ROW);
    check_equal(cserde_reader_next(&reader, &token), CSERDE_OK);
    check_equal(token.kind, CSERDE_MAP_BEGIN);
    check_equal(cserde_reader_next(&reader, &token), CSERDE_OK);
    check_equal(token.kind, CSERDE_STRING);
    check_equal(cserde_reader_next(&reader, &token), CSERDE_SOURCE_ERROR);

    cursor.ops->destroy(cursor.context);
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
    orm_cbind_publisher_config publisher_config = ORM_CBIND_PUBLISHER_CONFIG_INIT(
        &orm_redis_test_row_data, 1u, 1u, 2u, 1u);
    cflow_publisher source = {0};
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
    check_equal(orm_cbind_publisher_init(&source, &cursor, &publisher_config, &error),
                ORM_STATUS_OK);

    step = cflow_publisher_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_WAIT);
    check_true(cflow_waitable_valid(&step.waitable));
    check_true(cflow_waitable_arm(&step.waitable, (cflow_waker){0}));
    cflow_waitable_cancel(&step.waitable);
    check_equal(waitable.arm_count, 1u);
    check_equal(waitable.cancel_count, 1u);

    cflow_publisher_cancel(&source);
    check_equal(driver_context.cancel_count, 1u);
    cflow_publisher_destroy(&source);
    mock_orm_redis_test_destroy_verify();
  }
}

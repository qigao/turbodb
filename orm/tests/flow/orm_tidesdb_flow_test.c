#include "orm_cbind_source.h"
#include "orm_tidesdb_cursor.h"

#include <cmeta/struct.h>
#include "tinymock.h"

#include <stddef.h>

#define ORM_TIDES_TEST_DATA_PREFIX_SIZE                                      \
  (offsetof(cmeta_data_desc, shape) +                                         \
   sizeof(((cmeta_data_desc *)0)->shape))

Struct(orm_tides_test_row,
    (int, id),
    (long, score)
);

static const cmeta_type_identity orm_tides_test_row_identity =
    CMETA_TYPE_ID_ATOM_INIT("orm.test.TidesRow");
static const cmeta_type_traits orm_tides_test_row_traits = {
    .flags = CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY};
static const cmeta_type_desc orm_tides_test_row_type = {
    .name = "orm_tides_test_row", .size = sizeof(orm_tides_test_row),
    .align = _Alignof(orm_tides_test_row), .kind = CMETA_T_OBJECT,
    .pointee = NULL, .traits = &orm_tides_test_row_traits,
    .identity = &orm_tides_test_row_identity};
static const cmeta_data_field_desc orm_tides_test_row_fields[] = {
    {"orm.test.TidesRow.id", "id", offsetof(orm_tides_test_row, id),
     &cmeta_data_int},
    {"orm.test.TidesRow.score", "score",
     offsetof(orm_tides_test_row, score), &cmeta_data_long}};
static const cmeta_data_struct_shape orm_tides_test_row_shape = {
    .layout = StructMeta(orm_tides_test_row),
    .fields = orm_tides_test_row_fields,
    .field_count = 2u};
static const cmeta_data_desc orm_tides_test_row_data = {
    .struct_size = ORM_TIDES_TEST_DATA_PREFIX_SIZE,
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orm.test.TidesRow.data", .display_name = "TidesRow",
    .kind = CMETA_DATA_STRUCT, .storage_type = &orm_tides_test_row_type,
    .shape = &orm_tides_test_row_shape};

typedef struct orm_tides_test_state {
  const unsigned char *row;
  size_t row_size;
  size_t next_count;
} orm_tides_test_state;

TINYMOCk_MOCK_VOID(orm_tides_test_release_call, void *, void *)
TINYMOCk_MOCK_VOID(orm_tides_test_destroy, void *)

static void orm_tides_test_release(void *context,
                                   const unsigned char *row) {
  orm_tides_test_release_call(context, (void *)row);
}

static orm_tidesdb_driver_step orm_tides_test_next(
    void *context, const unsigned char **row, size_t *row_size) {
  orm_tides_test_state *state = (orm_tides_test_state *)context;
  orm_tidesdb_driver_step step = ORM_TIDESDB_DRIVER_STEP_INIT;
  if (state->next_count++ != 0u)
    return step;
  *row = state->row;
  *row_size = state->row_size;
  step.kind = ORM_TIDESDB_DRIVER_ROW;
  return step;
}

static const orm_tidesdb_driver_ops orm_tides_test_ops = {
    sizeof(orm_tidesdb_driver_ops), ORM_TIDESDB_DRIVER_OPS_ABI_VERSION,
    orm_tides_test_next, orm_tides_test_release, orm_tides_test_destroy};

spec("ORM TidesDB CFlow cursor") {
  it("decodes one stored row without constructing a result matrix") {
    static const unsigned char encoded[] = {
        'O','R','M','T','D','B',1,0, 2,0,0,0,
        2,0, ORM_VALUE_INT64,0, 1,0,0,0, 'i','d','7',
        5,0, ORM_VALUE_INT64,0, 2,0,0,0, 's','c','o','r','e','1','9'};
    orm_tides_test_state state = {encoded, sizeof(encoded), 0u};
    orm_tidesdb_driver driver = {&orm_tides_test_ops, &state};
    orm_tidesdb_cursor_config cursor_config =
        ORM_TIDESDB_CURSOR_CONFIG_INIT(4u, 256u, 8u);
    orm_row_cursor cursor = {0};
    orm_error_t error;
    orm_cbind_source_config source_config = ORM_CBIND_SOURCE_CONFIG_INIT(
        &orm_tides_test_row_data, 1u, 1u, 2u, 1u);
    cflow_source source = {0};
    orm_tides_test_row row = {0};
    cflow_step step;

    orm_error_init(&error);
    mock_orm_tides_test_release_call_reset();
    mock_orm_tides_test_destroy_reset();
    mock_orm_tides_test_release_call_expect(TINYMOCk_ARG((void *)&state),
                                            TINYMOCk_ARG((void *)encoded));
    mock_orm_tides_test_destroy_expect(TINYMOCk_ARG((void *)&state));
    check_equal(orm_tidesdb_cursor_start(&cursor, &driver, &cursor_config,
                                         &error), ORM_STATUS_OK);
    check_null(driver.ops);
    check_equal(orm_cbind_source_init(&source, &cursor, &source_config,
                                      &error), ORM_STATUS_OK);
    step = cflow_source_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(row.id, 7);
    check_equal(row.score, 19L);
    step = cflow_source_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_DONE);
    cflow_source_destroy(&source);
    mock_orm_tides_test_release_call_verify();
    mock_orm_tides_test_destroy_verify();
  }

  it("fails fast on a truncated stored row and still releases ownership") {
    static const unsigned char truncated[] = {
        'O','R','M','T','D','B',1,0, 1,0,0,0,
        2,0, ORM_VALUE_INT64,0, 4,0,0,0, 'i','d','7'};
    orm_tides_test_state state = {truncated, sizeof(truncated), 0u};
    orm_tidesdb_driver driver = {&orm_tides_test_ops, &state};
    orm_tidesdb_cursor_config cursor_config =
        ORM_TIDESDB_CURSOR_CONFIG_INIT(4u, 256u, 8u);
    orm_row_cursor cursor = {0};
    orm_error_t error;
    orm_cbind_source_config source_config = ORM_CBIND_SOURCE_CONFIG_INIT(
        &orm_tides_test_row_data, 1u, 1u, 2u, 1u);
    cflow_source source = {0};
    orm_tides_test_row row = {0};
    cflow_step step;

    orm_error_init(&error);
    mock_orm_tides_test_release_call_reset();
    mock_orm_tides_test_destroy_reset();
    mock_orm_tides_test_release_call_expect(TINYMOCk_ARG((void *)&state),
                                            TINYMOCk_ARG((void *)truncated));
    mock_orm_tides_test_destroy_expect(TINYMOCk_ARG((void *)&state));
    check_equal(orm_tidesdb_cursor_start(&cursor, &driver, &cursor_config,
                                         &error), ORM_STATUS_OK);
    check_equal(orm_cbind_source_init(&source, &cursor, &source_config,
                                      &error), ORM_STATUS_OK);
    step = cflow_source_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_ERROR);
    check_not_null(step.error);
    cflow_source_destroy(&source);
    mock_orm_tides_test_release_call_verify();
    mock_orm_tides_test_destroy_verify();
  }
}

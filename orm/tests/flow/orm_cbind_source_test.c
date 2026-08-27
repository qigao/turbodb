#include "orm_cbind_source.h"

#include <cmeta/struct.h>
#include "tinytest.h"

#include <stddef.h>
#include <string.h>

#define ORM_TEST_DATA_PREFIX_SIZE \
  (offsetof(cmeta_data_desc, shape) + sizeof(((cmeta_data_desc *)0)->shape))

Struct(orm_flow_test_row,
    (int, id),
    (long, score)
);

static const cmeta_type_identity orm_flow_test_row_identity =
    CMETA_TYPE_ID_ATOM_INIT("orm.test.FlowRow");
static const cmeta_type_traits orm_flow_test_row_traits = {
    .flags = CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY
};
static const cmeta_type_desc orm_flow_test_row_type = {
    .name = "orm_flow_test_row",
    .size = sizeof(orm_flow_test_row),
    .align = _Alignof(orm_flow_test_row),
    .kind = CMETA_T_OBJECT,
    .pointee = NULL,
    .traits = &orm_flow_test_row_traits,
    .identity = &orm_flow_test_row_identity
};
static const cmeta_data_field_desc orm_flow_test_row_fields[] = {
    {"orm.test.FlowRow.id", "id", offsetof(orm_flow_test_row, id),
     &cmeta_data_int},
    {"orm.test.FlowRow.score", "score", offsetof(orm_flow_test_row, score),
     &cmeta_data_long}
};
static const cmeta_data_struct_shape orm_flow_test_row_shape = {
    .layout = StructMeta(orm_flow_test_row),
    .fields = orm_flow_test_row_fields,
    .field_count = sizeof(orm_flow_test_row_fields) /
                   sizeof(orm_flow_test_row_fields[0])
};
static const cmeta_data_desc orm_flow_test_row_data = {
    .struct_size = ORM_TEST_DATA_PREFIX_SIZE,
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orm.test.FlowRow.data",
    .display_name = "FlowRow",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &orm_flow_test_row_type,
    .shape = &orm_flow_test_row_shape
};

typedef struct orm_flow_test_reader_state {
  const cserde_token *tokens;
  size_t count;
  size_t index;
} orm_flow_test_reader_state;

static cserde_status orm_flow_test_reader_next(void *context,
                                                cserde_token *out) {
  orm_flow_test_reader_state *state = (orm_flow_test_reader_state *)context;
  if (state == NULL || out == NULL)
    return CSERDE_INVALID_ARGUMENT;
  if (state->index == state->count)
    return CSERDE_DONE;
  *out = state->tokens[state->index++];
  return CSERDE_OK;
}

static const cserde_reader_ops orm_flow_test_reader_ops = {
    sizeof(cserde_reader_ops), CSERDE_READER_OPS_ABI_VERSION,
    orm_flow_test_reader_next};

typedef struct orm_flow_test_cursor_state {
  orm_flow_test_reader_state reader_state;
  orm_row_cursor_step_kind next_kind;
  cflow_waitable waitable;
  size_t next_count;
  size_t cancel_count;
  size_t destroy_count;
} orm_flow_test_cursor_state;

typedef struct orm_flow_test_waitable_state {
  size_t arm_count;
  size_t cancel_count;
} orm_flow_test_waitable_state;

static bool orm_flow_test_waitable_arm(void *context, cflow_waker waker) {
  orm_flow_test_waitable_state *state =
      (orm_flow_test_waitable_state *)context;
  (void)waker;
  ++state->arm_count;
  return true;
}

static void orm_flow_test_waitable_cancel(void *context) {
  orm_flow_test_waitable_state *state =
      (orm_flow_test_waitable_state *)context;
  ++state->cancel_count;
}

CMETA_IMPLEMENTS(cflow_waitable, orm_flow_test_waitable, 0,
    .arm = orm_flow_test_waitable_arm,
    .cancel = orm_flow_test_waitable_cancel
);

static orm_row_cursor_step orm_flow_test_cursor_next(void *context,
                                                      cserde_reader *out_row) {
  orm_flow_test_cursor_state *state =
      (orm_flow_test_cursor_state *)context;
  orm_row_cursor_step step = ORM_ROW_CURSOR_STEP_INIT;

  ++state->next_count;
  step.kind = state->next_kind;
  step.waitable = state->waitable;
  if (step.kind != ORM_ROW_CURSOR_ROW &&
      step.kind != ORM_ROW_CURSOR_ROW_AND_DONE)
    return step;
  if (cserde_reader_init(out_row, &orm_flow_test_reader_ops,
                        &state->reader_state) != CSERDE_OK) {
    step.kind = ORM_ROW_CURSOR_ERROR;
    step.status = ORM_STATUS_INTERNAL_ERROR;
    step.message = "reader initialization failed";
    return step;
  }
  return step;
}

static void orm_flow_test_cursor_cancel(void *context) {
  orm_flow_test_cursor_state *state =
      (orm_flow_test_cursor_state *)context;
  ++state->cancel_count;
}

static void orm_flow_test_cursor_destroy(void *context) {
  orm_flow_test_cursor_state *state =
      (orm_flow_test_cursor_state *)context;
  ++state->destroy_count;
}

static const orm_row_cursor_ops orm_flow_test_cursor_ops = {
    sizeof(orm_row_cursor_ops), ORM_ROW_CURSOR_OPS_ABI_VERSION,
    "flow-test", orm_flow_test_cursor_next,
    orm_flow_test_cursor_cancel, orm_flow_test_cursor_destroy, NULL};

static const orm_row_cursor_ops orm_flow_test_partial_cursor_ops = {
    sizeof(orm_row_cursor_ops), ORM_ROW_CURSOR_OPS_ABI_VERSION,
    "partial-flow-test", NULL, orm_flow_test_cursor_cancel,
    orm_flow_test_cursor_destroy, NULL};

typedef struct orm_flow_test_sink_state {
  orm_flow_test_row row;
  size_t value_count;
  size_t done_count;
  const char *error;
} orm_flow_test_sink_state;

static bool orm_flow_test_sink_value(void *context,
                                     const cmeta_type_desc *type,
                                     const void *value) {
  orm_flow_test_sink_state *state = (orm_flow_test_sink_state *)context;
  if (!cmeta_type_equal(type, &orm_flow_test_row_type) || value == NULL)
    return false;
  state->row = *(const orm_flow_test_row *)value;
  ++state->value_count;
  return true;
}

static void orm_flow_test_sink_error(void *context, const char *message) {
  orm_flow_test_sink_state *state = (orm_flow_test_sink_state *)context;
  state->error = message;
}

static void orm_flow_test_sink_done(void *context) {
  orm_flow_test_sink_state *state = (orm_flow_test_sink_state *)context;
  ++state->done_count;
}

spec("ORM CBind CFlow source") {
  it("disposes a partially initialized cursor from a failed backend contract") {
    orm_flow_test_cursor_state state = {0};
    orm_row_cursor cursor = {.ops = &orm_flow_test_partial_cursor_ops,
                             .context = &state,
                             .wait_timeout_ns = 0u};

    check_false(orm_row_cursor_valid(&cursor));
    orm_row_cursor_dispose(&cursor);
    check_equal(state.destroy_count, (size_t)1u);
    check_null(cursor.ops);
    check_null(cursor.context);
  }

  it("decodes a final cursor row into one owning typed value") {
    static const unsigned char id_name[] = "id";
    static const unsigned char score_name[] = "score";
    const cserde_token tokens[] = {
        {.kind = CSERDE_MAP_BEGIN},
        {.kind = CSERDE_STRING,
         .value.slice = {id_name, sizeof(id_name) - 1u, CSERDE_VIEW_STABLE}},
        {.kind = CSERDE_SINT, .value.sint = 7},
        {.kind = CSERDE_STRING,
         .value.slice = {score_name, sizeof(score_name) - 1u,
                         CSERDE_VIEW_STABLE}},
        {.kind = CSERDE_SINT, .value.sint = 19},
        {.kind = CSERDE_MAP_END}};
    orm_flow_test_cursor_state state = {
        .reader_state = {tokens, sizeof(tokens) / sizeof(tokens[0]), 0u},
        .next_kind = ORM_ROW_CURSOR_ROW_AND_DONE,
        .cancel_count = 0u,
        .destroy_count = 0u};
    orm_row_cursor cursor = {.ops = &orm_flow_test_cursor_ops,
                             .context = &state,
                             .wait_timeout_ns = 0u};
    orm_cbind_source_config config = ORM_CBIND_SOURCE_CONFIG_INIT(
        &orm_flow_test_row_data, 1u, 1u, 64u, 1u);
    orm_error_t error;
    cflow_source source = {0};
    orm_flow_test_row row = {0};
    cflow_step step;

    orm_error_init(&error);
    check_equal(orm_cbind_source_init(&source, &cursor, &config, &error),
                ORM_STATUS_OK);
    check_equal(error.status, ORM_STATUS_OK);
    check_equal(error.message[0], '\0');
    check_null(cursor.ops);
    check_null(cursor.context);

    step = cflow_source_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(row.id, 7);
    check_equal(row.score, 19L);

    cflow_source_destroy(&source);
    check_equal(state.cancel_count, (size_t)0u);
    check_equal(state.destroy_count, (size_t)1u);
  }

  it("rejects WAIT without a waitable and cancels the cursor") {
    orm_flow_test_cursor_state state = {
        .reader_state = {NULL, 0u, 0u},
        .next_kind = ORM_ROW_CURSOR_WAIT,
        .cancel_count = 0u,
        .destroy_count = 0u};
    orm_row_cursor cursor = {.ops = &orm_flow_test_cursor_ops,
                             .context = &state,
                             .wait_timeout_ns = 0u};
    orm_cbind_source_config config = ORM_CBIND_SOURCE_CONFIG_INIT(
        &orm_flow_test_row_data, 1u, 1u, 64u, 1u);
    orm_error_t error;
    cflow_source source = {0};
    orm_flow_test_row row = {0};
    cflow_step step;

    orm_error_init(&error);
    check_equal(orm_cbind_source_init(&source, &cursor, &config, &error),
                ORM_STATUS_OK);

    step = cflow_source_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_ERROR);
    check_equal(state.cancel_count, (size_t)1u);

    cflow_source_destroy(&source);
    check_equal(state.cancel_count, (size_t)1u);
    check_equal(state.destroy_count, (size_t)1u);
  }

  it("propagates a valid WAIT without advancing or destroying the cursor") {
    orm_flow_test_waitable_state wait_state = {0};
    orm_flow_test_cursor_state state = {
        .reader_state = {NULL, 0u, 0u},
        .next_kind = ORM_ROW_CURSOR_WAIT,
        .waitable = orm_flow_test_waitable_as_cflow_waitable(&wait_state),
        .cancel_count = 0u,
        .destroy_count = 0u};
    orm_row_cursor cursor = {.ops = &orm_flow_test_cursor_ops,
                             .context = &state,
                             .wait_timeout_ns = 0u};
    orm_cbind_source_config config = ORM_CBIND_SOURCE_CONFIG_INIT(
        &orm_flow_test_row_data, 1u, 1u, 64u, 1u);
    orm_error_t error;
    cflow_source source = {0};
    orm_flow_test_row row = {0};
    cflow_step step;

    orm_error_init(&error);
    check_equal(orm_cbind_source_init(&source, &cursor, &config, &error),
                ORM_STATUS_OK);
    step = cflow_source_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_WAIT);
    check_true(cflow_waitable_valid(&step.waitable));
    check_equal(state.cancel_count, (size_t)0u);
    check_equal(state.destroy_count, (size_t)0u);

    cflow_waitable_cancel(&step.waitable);
    cflow_source_destroy(&source);
    check_equal(wait_state.cancel_count, (size_t)1u);
    check_equal(state.cancel_count, (size_t)1u);
    check_equal(state.destroy_count, (size_t)1u);
  }

  it("keeps cursor ownership when source configuration is rejected") {
    orm_flow_test_cursor_state state = {
        .reader_state = {NULL, 0u, 0u},
        .next_kind = ORM_ROW_CURSOR_DONE,
        .cancel_count = 0u,
        .destroy_count = 0u};
    orm_row_cursor cursor = {.ops = &orm_flow_test_cursor_ops,
                             .context = &state,
                             .wait_timeout_ns = 0u};
    orm_cbind_source_config config = ORM_CBIND_SOURCE_CONFIG_INIT(
        &orm_flow_test_row_data, 1u, 0u, 64u, 1u);
    orm_error_t error;
    cflow_source source = {0};

    orm_error_init(&error);
    check_equal(orm_cbind_source_init(&source, &cursor, &config, &error),
                ORM_STATUS_INVALID_ARGUMENT);
    check_not_null(cursor.ops);
    check_not_null(cursor.context);
    check_null(source.self);
    check_equal(state.cancel_count, (size_t)0u);
    check_equal(state.destroy_count, (size_t)0u);

    cursor.ops->destroy(cursor.context);
    check_equal(state.destroy_count, (size_t)1u);
  }

  it("cancels the cursor and restores zero when row binding fails") {
    static const unsigned char id_name[] = "id";
    const cserde_token tokens[] = {
        {.kind = CSERDE_MAP_BEGIN},
        {.kind = CSERDE_STRING,
         .value.slice = {id_name, sizeof(id_name) - 1u, CSERDE_VIEW_STABLE}},
        {.kind = CSERDE_SINT, .value.sint = 7},
        {.kind = CSERDE_MAP_END}};
    orm_flow_test_cursor_state state = {
        .reader_state = {tokens, sizeof(tokens) / sizeof(tokens[0]), 0u},
        .next_kind = ORM_ROW_CURSOR_ROW,
        .cancel_count = 0u,
        .destroy_count = 0u};
    orm_row_cursor cursor = {.ops = &orm_flow_test_cursor_ops,
                             .context = &state,
                             .wait_timeout_ns = 0u};
    orm_cbind_source_config config = ORM_CBIND_SOURCE_CONFIG_INIT(
        &orm_flow_test_row_data, 1u, 1u, 64u, 1u);
    orm_error_t error;
    cflow_source source = {0};
    orm_flow_test_row row = {.id = 91, .score = 92};
    cflow_step step;

    orm_error_init(&error);
    check_equal(orm_cbind_source_init(&source, &cursor, &config, &error),
                ORM_STATUS_OK);
    step = cflow_source_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_ERROR);
    check_not_null(step.error);
    check_equal(row.id, 0);
    check_equal(row.score, 0L);
    check_equal(state.cancel_count, (size_t)1u);

    cflow_source_destroy(&source);
    check_equal(state.cancel_count, (size_t)1u);
    check_equal(state.destroy_count, (size_t)1u);
  }

  it("makes repeated cancellation idempotent before destruction") {
    orm_flow_test_cursor_state state = {
        .reader_state = {NULL, 0u, 0u},
        .next_kind = ORM_ROW_CURSOR_DONE,
        .cancel_count = 0u,
        .destroy_count = 0u};
    orm_row_cursor cursor = {.ops = &orm_flow_test_cursor_ops,
                             .context = &state,
                             .wait_timeout_ns = 0u};
    orm_cbind_source_config config = ORM_CBIND_SOURCE_CONFIG_INIT(
        &orm_flow_test_row_data, 1u, 1u, 64u, 1u);
    orm_error_t error;
    cflow_source source = {0};
    const char *terminal_error = "not cleared";
    orm_flow_test_row row = {0};
    cflow_step step;

    orm_error_init(&error);
    check_equal(orm_cbind_source_init(&source, &cursor, &config, &error),
                ORM_STATUS_OK);
    cflow_source_cancel(&source);
    cflow_source_cancel(&source);
    check_equal(state.cancel_count, (size_t)1u);
    check_equal(cflow_source_poll_terminal(&source, &terminal_error),
                CFLOW_SOURCE_DONE);
    check_null(terminal_error);
    step = cflow_source_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_DONE);
    check_equal(state.next_count, (size_t)0u);

    cflow_source_destroy(&source);
    check_equal(state.cancel_count, (size_t)1u);
    check_equal(state.destroy_count, (size_t)1u);
  }

  it("polls the cursor only after CFlow downstream demand") {
    static const unsigned char id_name[] = "id";
    static const unsigned char score_name[] = "score";
    const cserde_token tokens[] = {
        {.kind = CSERDE_MAP_BEGIN},
        {.kind = CSERDE_STRING,
         .value.slice = {id_name, sizeof(id_name) - 1u, CSERDE_VIEW_STABLE}},
        {.kind = CSERDE_SINT, .value.sint = 11},
        {.kind = CSERDE_STRING,
         .value.slice = {score_name, sizeof(score_name) - 1u,
                         CSERDE_VIEW_STABLE}},
        {.kind = CSERDE_SINT, .value.sint = 29},
        {.kind = CSERDE_MAP_END}};
    orm_flow_test_cursor_state cursor_state = {
        .reader_state = {tokens, sizeof(tokens) / sizeof(tokens[0]), 0u},
        .next_kind = ORM_ROW_CURSOR_ROW_AND_DONE};
    orm_row_cursor cursor = {.ops = &orm_flow_test_cursor_ops,
                             .context = &cursor_state,
                             .wait_timeout_ns = 0u};
    orm_cbind_source_config config = ORM_CBIND_SOURCE_CONFIG_INIT(
        &orm_flow_test_row_data, 1u, 1u, 64u, 1u);
    orm_flow_test_sink_state sink_state = {0};
    cflow_sink_callbacks callbacks = {
        orm_flow_test_sink_value, orm_flow_test_sink_error,
        orm_flow_test_sink_done, &sink_state};
    cflow_sink sink = cflow_sink_from_callbacks(&callbacks);
    orm_error_t error;
    cflow_graph surface = {0};
    cflow_graph normalized = {0};
    cflow_scheduler scheduler = {0};
    cflow_source source = {0};
    cflow_run run = {0};

    normalized.root = CMETA_INVALID_ID;
    orm_error_init(&error);
    cflow_graph_init(&surface, &orm_flow_test_row_type);
    check_true(cflow_graph_normalize(&normalized, &surface));
    check_true(cflow_scheduler_test_init(&scheduler));
    check_equal(orm_cbind_source_init(&source, &cursor, &config, &error),
                ORM_STATUS_OK);
    check_true(cflow_run_open(&run, &normalized, &source, &scheduler, &sink));
    check_null(source.self);
    check_equal(cursor_state.next_count, (size_t)0u);

    check_true(cflow_run_request(&run, 1u));
    (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
    check_equal(cursor_state.next_count, (size_t)1u);
    check_equal(sink_state.value_count, (size_t)1u);
    check_equal(sink_state.row.id, 11);
    check_equal(sink_state.row.score, 29L);
    check_equal(sink_state.done_count, (size_t)1u);
    check_null(sink_state.error);

    cflow_run_close(&run);
    check_equal(cursor_state.destroy_count, (size_t)1u);
    cflow_scheduler_destroy(&scheduler);
    cflow_graph_destroy(&normalized);
    cflow_graph_destroy(&surface);
  }
}

#include "orm_row_publisher.h"

#include <cmeta/struct.h>
#include <data_bind.h>
#include <data_bind_message_plan.h>
#include <data_bind_native_binding.h>
#include "tinytest.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

typedef struct orm_flow_validated_row {
  uint32_t id;
} orm_flow_validated_row;

static const cmeta_type_identity orm_flow_validated_row_identity =
    CMETA_TYPE_ID_ATOM_INIT("orm.test.ValidatedRow");
static const cmeta_type_desc orm_flow_validated_row_type = {
    "orm_flow_validated_row",
    sizeof(orm_flow_validated_row),
    _Alignof(orm_flow_validated_row),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &orm_flow_validated_row_identity
};
static const cmeta_field_desc orm_flow_validated_row_layout_fields[] = {
    {"id", "uint32_t", offsetof(orm_flow_validated_row, id),
     sizeof(uint32_t), _Alignof(uint32_t), &cmeta_type_uint32, NULL}
};
static const cmeta_struct_desc orm_flow_validated_row_layout = {
    "ValidatedRow",
    sizeof(orm_flow_validated_row),
    _Alignof(orm_flow_validated_row),
    orm_flow_validated_row_layout_fields,
    1u
};
static const cmeta_data_field_desc orm_flow_validated_row_fields[] = {
    {"orm.test.ValidatedRow.id", "id",
     offsetof(orm_flow_validated_row, id), &cmeta_data_uint32}
};
static const cmeta_data_struct_shape orm_flow_validated_row_shape = {
    &orm_flow_validated_row_layout,
    orm_flow_validated_row_fields,
    1u
};
static const cmeta_data_desc orm_flow_validated_row_data = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orm.test.ValidatedRow.data",
    .display_name = "ValidatedRow",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &orm_flow_validated_row_type,
    .shape = &orm_flow_validated_row_shape
};

static DataBindMessagePlan *orm_flow_validation_plan(
    DataBindNativeTypeBinding *native) {
  static const char schema[] =
      "message ValidatedRow { @Min(10) uint32 id; }";
  DataBind *codec = NULL;
  DataBindMessagePlan *plan = NULL;
  DataBindError error = DATA_BIND_ERROR_INIT;
  DataBindMessagePlanDiagnostic diagnostic =
      DATA_BIND_MESSAGE_PLAN_DIAGNOSTIC_INIT;

  *native = (DataBindNativeTypeBinding)
      DATA_BIND_NATIVE_TYPE_BINDING_INIT(
          "ValidatedRow", &orm_flow_validated_row_data);
  check_equal(
      data_bind_create_from_text(
          schema, sizeof(schema) - 1u, &codec, &error),
      DATA_BIND_OK);
  if (codec == NULL)
    return NULL;
  check_equal(
      data_bind_message_plan_compile(
          codec, "ValidatedRow", native, &plan, &diagnostic),
      DATA_BIND_OK);
  /* MessagePlan owns logical validation facts after control-plane compile. */
  data_bind_free(codec);
  return plan;
}

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
    orm_flow_test_cursor_cancel, orm_flow_test_cursor_destroy, NULL, NULL};

static const orm_row_cursor_ops orm_flow_test_partial_cursor_ops = {
    sizeof(orm_row_cursor_ops), ORM_ROW_CURSOR_OPS_ABI_VERSION,
    "partial-flow-test", NULL, orm_flow_test_cursor_cancel,
    orm_flow_test_cursor_destroy, NULL, NULL};

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

typedef struct orm_flow_dynamic_record {
  int64_t id;
  double score;
} orm_flow_dynamic_record;

typedef struct orm_flow_dynamic_carrier {
  orm_flow_dynamic_record *record;
  cmeta_object_field_provider field_provider;
} orm_flow_dynamic_carrier;

typedef struct orm_flow_dynamic_factory_state {
  size_t create_count;
  size_t destroy_count;
} orm_flow_dynamic_factory_state;

static const cmeta_type_identity orm_flow_dynamic_record_identity =
    CMETA_TYPE_ID_ATOM_INIT("orm.test.DynamicRow");
static const cmeta_type_desc orm_flow_dynamic_record_type = {
    .name = "orm_flow_dynamic_record",
    .size = sizeof(orm_flow_dynamic_record),
    .align = _Alignof(orm_flow_dynamic_record),
    .kind = CMETA_T_OBJECT,
    .pointee = NULL,
    .traits = NULL,
    .identity = &orm_flow_dynamic_record_identity
};
static const cmeta_field_desc orm_flow_dynamic_layout_fields[] = {
    {"id", "int64_t", CMETA_FIELD_DYNAMIC_OFFSET, sizeof(int64_t),
     _Alignof(int64_t), &cmeta_type_int64, NULL},
    {"score", "double", CMETA_FIELD_DYNAMIC_OFFSET, sizeof(double),
     _Alignof(double), &cmeta_type_double, NULL}
};
static const cmeta_struct_desc orm_flow_dynamic_layout = {
    "DynamicRow", sizeof(orm_flow_dynamic_record),
    _Alignof(orm_flow_dynamic_record),
    orm_flow_dynamic_layout_fields, 2u
};
static const cmeta_data_field_desc orm_flow_dynamic_fields[] = {
    {"orm.test.DynamicRow.id", "id", CMETA_FIELD_DYNAMIC_OFFSET,
     &cmeta_data_int64},
    {"orm.test.DynamicRow.score", "score", CMETA_FIELD_DYNAMIC_OFFSET,
     &cmeta_data_double}
};
static const cmeta_data_struct_shape orm_flow_dynamic_shape = {
    &orm_flow_dynamic_layout, orm_flow_dynamic_fields, 2u
};
static const cmeta_data_desc orm_flow_dynamic_data = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orm.test.DynamicRow.data",
    .display_name = "DynamicRow",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &orm_flow_dynamic_record_type,
    .shape = &orm_flow_dynamic_shape
};

static const cmeta_type_identity orm_flow_dynamic_carrier_identity =
    CMETA_TYPE_ID_ATOM_INIT("orm.test.DynamicRowCarrier");
static const cmeta_type_desc orm_flow_dynamic_carrier_type = {
    .name = "orm_flow_dynamic_carrier",
    .size = sizeof(orm_flow_dynamic_carrier),
    .align = _Alignof(orm_flow_dynamic_carrier),
    .kind = CMETA_T_OBJECT,
    .pointee = NULL,
    .traits = NULL,
    .identity = &orm_flow_dynamic_carrier_identity
};

static cmeta_status orm_flow_dynamic_read(
    void *context, const void *object, const cmeta_data_field_desc *field,
    const void **out_value) {
  const orm_flow_dynamic_record *record =
      (const orm_flow_dynamic_record *)object;
  (void)context;
  if (!record || !field || !out_value) return CMETA_INVALID_ARGUMENT;
  if (strcmp(field->name, "id") == 0)
    *out_value = &record->id;
  else if (strcmp(field->name, "score") == 0)
    *out_value = &record->score;
  else
    return CMETA_TRAIT_MISSING;
  return CMETA_OK;
}

static cmeta_status orm_flow_dynamic_assign(
    void *context, void *object, const cmeta_data_field_desc *field,
    const void *value) {
  orm_flow_dynamic_record *record = (orm_flow_dynamic_record *)object;
  (void)context;
  if (!record || !field || !value) return CMETA_INVALID_ARGUMENT;
  if (strcmp(field->name, "id") == 0)
    record->id = *(const int64_t *)value;
  else if (strcmp(field->name, "score") == 0)
    record->score = *(const double *)value;
  else
    return CMETA_TRAIT_MISSING;
  return CMETA_OK;
}

static orm_status_t orm_flow_dynamic_create(
    void *context, void *out_value, cmeta_object_ref *out_object,
    orm_error_t *error) {
  orm_flow_dynamic_factory_state *state =
      (orm_flow_dynamic_factory_state *)context;
  orm_flow_dynamic_carrier *carrier =
      (orm_flow_dynamic_carrier *)out_value;
  cmeta_status status;

  if (!state || !carrier || !out_object) return ORM_STATUS_INVALID_ARGUMENT;
  memset(carrier, 0, sizeof(*carrier));
  carrier->record =
      (orm_flow_dynamic_record *)calloc(1u, sizeof(*carrier->record));
  if (!carrier->record) {
    if (error) {
      orm_error_init(error);
      error->status = ORM_STATUS_OUT_OF_MEMORY;
      snprintf(error->message, sizeof(error->message),
               "allocate dynamic row fixture");
    }
    return ORM_STATUS_OUT_OF_MEMORY;
  }

  carrier->field_provider = (cmeta_object_field_provider){
      .size = sizeof(cmeta_object_field_provider),
      .data = &orm_flow_dynamic_data,
      .context = NULL,
      .assign = orm_flow_dynamic_assign,
      .read = orm_flow_dynamic_read
  };
  status = cmeta_object_borrow_with_providers(
      out_object, carrier->record, &orm_flow_dynamic_data,
      &carrier->field_provider, NULL);
  if (status != CMETA_OK) {
    free(carrier->record);
    carrier->record = NULL;
    return ORM_STATUS_TYPE_ERROR;
  }
  ++state->create_count;
  return ORM_STATUS_OK;
}

static void orm_flow_dynamic_destroy(void *context, void *value) {
  orm_flow_dynamic_factory_state *state =
      (orm_flow_dynamic_factory_state *)context;
  orm_flow_dynamic_carrier *carrier =
      (orm_flow_dynamic_carrier *)value;
  if (!carrier) return;
  free(carrier->record);
  carrier->record = NULL;
  memset(&carrier->field_provider, 0, sizeof(carrier->field_provider));
  if (state) ++state->destroy_count;
}

static DataBindMessagePlan *orm_flow_dynamic_plan(
    int validated, DataBind **out_codec) {
  static const char plain_schema[] =
      "message DynamicRow { int64 id; double score; }";
  static const char validated_schema[] =
      "message DynamicRow { @Min(10) int64 id; double score; }";
  const char *schema = validated ? validated_schema : plain_schema;
  const size_t schema_len =
      validated ? sizeof(validated_schema) - 1u : sizeof(plain_schema) - 1u;
  DataBind *codec = NULL;
  DataBindMessagePlan *plan = NULL;
  DataBindError error = DATA_BIND_ERROR_INIT;
  DataBindMessagePlanDiagnostic diagnostic =
      DATA_BIND_MESSAGE_PLAN_DIAGNOSTIC_INIT;

  check_equal(
      data_bind_create_from_text(schema, schema_len, &codec, &error),
      DATA_BIND_OK);
  if (!codec) return NULL;
  check_equal(
      data_bind_message_plan_compile_object(
          codec, "DynamicRow", &orm_flow_dynamic_data, &plan, &diagnostic),
      DATA_BIND_OK);
  if (out_codec) *out_codec = codec;
  else data_bind_free(codec);
  return plan;
}

spec("ORM DataBind CFlow publisher") {
  it("decodes one provider-backed object into a caller-defined carrier") {
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
        {.kind = CSERDE_FLOAT, .value.floating = 2.5},
        {.kind = CSERDE_MAP_END}};
    orm_flow_test_cursor_state cursor_state = {
        .reader_state = {tokens, sizeof(tokens) / sizeof(tokens[0]), 0u},
        .next_kind = ORM_ROW_CURSOR_ROW_AND_DONE};
    orm_row_cursor cursor = {
        .ops = &orm_flow_test_cursor_ops, .context = &cursor_state};
    orm_flow_dynamic_factory_state factory_state = {0};
    orm_object_row_factory_t factory = {
        sizeof(orm_object_row_factory_t), ORM_C_ABI_VERSION,
        &orm_flow_dynamic_carrier_type, &factory_state,
        orm_flow_dynamic_create, orm_flow_dynamic_destroy};
    DataBind *codec = NULL;
    DataBindMessagePlan *plan = orm_flow_dynamic_plan(0, &codec);
    orm_row_publisher_config config = ORM_ROW_PUBLISHER_CONFIG_INIT(
        &orm_flow_dynamic_data, 2048u, 8u, 64u, 1024u);
    cflow_publisher source = {0};
    orm_flow_dynamic_carrier output = {0};
    orm_error_t error;
    cflow_step step;

    check_not_null(plan);
    if (!plan) {
      data_bind_free(codec);
      return;
    }
    config.message_plan = plan;
    config.object_factory = &factory;

    orm_error_init(&error);
    check_equal(
        orm_row_publisher_init(&source, &cursor, &config, &error),
        ORM_STATUS_OK);
    check_true(cmeta_type_equal(
        cflow_publisher_output_type(&source),
        &orm_flow_dynamic_carrier_type));

    step = cflow_publisher_resume(&source, NULL, &output);
    check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(factory_state.create_count, (size_t)1u);
    check_equal(factory_state.destroy_count, (size_t)0u);
    check_not_null(output.record);
    if (output.record) {
      check_equal(output.record->id, INT64_C(7));
      check_true(output.record->score == 2.5);
    }

    orm_flow_dynamic_destroy(&factory_state, &output);
    check_equal(factory_state.destroy_count, (size_t)1u);
    cflow_publisher_destroy(&source);
    check_equal(cursor_state.destroy_count, (size_t)1u);
    data_bind_message_plan_free(plan);
    data_bind_free(codec);
  }

  it("destroys object staging when DataBind validation rejects a row") {
    static const unsigned char id_name[] = "id";
    static const unsigned char score_name[] = "score";
    const cserde_token tokens[] = {
        {.kind = CSERDE_MAP_BEGIN},
        {.kind = CSERDE_STRING,
         .value.slice = {id_name, sizeof(id_name) - 1u, CSERDE_VIEW_STABLE}},
        {.kind = CSERDE_SINT, .value.sint = 5},
        {.kind = CSERDE_STRING,
         .value.slice = {score_name, sizeof(score_name) - 1u,
                         CSERDE_VIEW_STABLE}},
        {.kind = CSERDE_FLOAT, .value.floating = 2.5},
        {.kind = CSERDE_MAP_END}};
    orm_flow_test_cursor_state cursor_state = {
        .reader_state = {tokens, sizeof(tokens) / sizeof(tokens[0]), 0u},
        .next_kind = ORM_ROW_CURSOR_ROW};
    orm_row_cursor cursor = {
        .ops = &orm_flow_test_cursor_ops, .context = &cursor_state};
    orm_flow_dynamic_factory_state factory_state = {0};
    orm_object_row_factory_t factory = {
        sizeof(orm_object_row_factory_t), ORM_C_ABI_VERSION,
        &orm_flow_dynamic_carrier_type, &factory_state,
        orm_flow_dynamic_create, orm_flow_dynamic_destroy};
    DataBind *codec = NULL;
    DataBindMessagePlan *plan = orm_flow_dynamic_plan(1, &codec);
    orm_row_publisher_config config = ORM_ROW_PUBLISHER_CONFIG_INIT(
        &orm_flow_dynamic_data, 2048u, 8u, 64u, 1024u);
    cflow_publisher source = {0};
    orm_flow_dynamic_carrier output = {0};
    orm_error_t error;
    cflow_step step;

    check_not_null(plan);
    if (!plan) {
      data_bind_free(codec);
      return;
    }
    config.message_plan = plan;
    config.object_factory = &factory;

    orm_error_init(&error);
    check_equal(
        orm_row_publisher_init(&source, &cursor, &config, &error),
        ORM_STATUS_OK);
    step = cflow_publisher_resume(&source, NULL, &output);
    check_equal(step.kind, CFLOW_STEP_ERROR);
    check_not_null(step.error);
    check_contains(step.error, "row validation failed");
    check_contains(step.error, "id");
    check_equal(factory_state.create_count, (size_t)1u);
    check_equal(factory_state.destroy_count, (size_t)1u);
    check_null(output.record);
    check_equal(cursor_state.cancel_count, (size_t)1u);

    cflow_publisher_destroy(&source);
    data_bind_message_plan_free(plan);
    data_bind_free(codec);
  }

  it("does not construct provider-backed outputs for WAIT or DONE") {
    orm_flow_test_waitable_state wait_state = {0};
    orm_flow_test_cursor_state wait_cursor_state = {
        .next_kind = ORM_ROW_CURSOR_WAIT,
        .waitable = orm_flow_test_waitable_as_cflow_waitable(&wait_state)};
    orm_flow_test_cursor_state done_cursor_state = {
        .next_kind = ORM_ROW_CURSOR_DONE};
    orm_row_cursor wait_cursor = {
        .ops = &orm_flow_test_cursor_ops, .context = &wait_cursor_state};
    orm_row_cursor done_cursor = {
        .ops = &orm_flow_test_cursor_ops, .context = &done_cursor_state};
    orm_flow_dynamic_factory_state factory_state = {0};
    orm_object_row_factory_t factory = {
        sizeof(orm_object_row_factory_t), ORM_C_ABI_VERSION,
        &orm_flow_dynamic_carrier_type, &factory_state,
        orm_flow_dynamic_create, orm_flow_dynamic_destroy};
    DataBind *codec = NULL;
    DataBindMessagePlan *plan = orm_flow_dynamic_plan(0, &codec);
    orm_row_publisher_config config = ORM_ROW_PUBLISHER_CONFIG_INIT(
        &orm_flow_dynamic_data, 2048u, 8u, 64u, 1024u);
    cflow_publisher wait_source = {0};
    cflow_publisher done_source = {0};
    orm_flow_dynamic_carrier output = {0};
    orm_error_t error;
    cflow_step step;

    check_not_null(plan);
    if (!plan) {
      data_bind_free(codec);
      return;
    }
    config.message_plan = plan;
    config.object_factory = &factory;

    orm_error_init(&error);
    check_equal(
        orm_row_publisher_init(
            &wait_source, &wait_cursor, &config, &error),
        ORM_STATUS_OK);
    step = cflow_publisher_resume(&wait_source, NULL, &output);
    check_equal(step.kind, CFLOW_STEP_WAIT);
    check_equal(factory_state.create_count, (size_t)0u);
    cflow_publisher_destroy(&wait_source);

    orm_error_init(&error);
    check_equal(
        orm_row_publisher_init(
            &done_source, &done_cursor, &config, &error),
        ORM_STATUS_OK);
    step = cflow_publisher_resume(&done_source, NULL, &output);
    check_equal(step.kind, CFLOW_STEP_DONE);
    check_equal(factory_state.create_count, (size_t)0u);
    check_equal(factory_state.destroy_count, (size_t)0u);
    cflow_publisher_destroy(&done_source);

    data_bind_message_plan_free(plan);
    data_bind_free(codec);
  }

  it("executes one immutable DataBind ValidationPlan on row reads") {
    static const unsigned char id_name[] = "id";
    const cserde_token valid_tokens[] = {
        {.kind = CSERDE_MAP_BEGIN},
        {.kind = CSERDE_STRING,
         .value.slice = {id_name, sizeof(id_name) - 1u,
                         CSERDE_VIEW_STABLE}},
        {.kind = CSERDE_UINT, .value.uint = 11u},
        {.kind = CSERDE_MAP_END}};
    const cserde_token invalid_tokens[] = {
        {.kind = CSERDE_MAP_BEGIN},
        {.kind = CSERDE_STRING,
         .value.slice = {id_name, sizeof(id_name) - 1u,
                         CSERDE_VIEW_STABLE}},
        {.kind = CSERDE_UINT, .value.uint = 5u},
        {.kind = CSERDE_MAP_END}};
    DataBindNativeTypeBinding native = {0};
    DataBindMessagePlan *plan = orm_flow_validation_plan(&native);
    orm_flow_test_cursor_state valid_state = {
        .reader_state = {valid_tokens,
                         sizeof(valid_tokens) / sizeof(valid_tokens[0]), 0u},
        .next_kind = ORM_ROW_CURSOR_ROW_AND_DONE};
    orm_flow_test_cursor_state invalid_state = {
        .reader_state = {invalid_tokens,
                         sizeof(invalid_tokens) / sizeof(invalid_tokens[0]), 0u},
        .next_kind = ORM_ROW_CURSOR_ROW_AND_DONE};
    orm_row_cursor valid_cursor = {
        .ops = &orm_flow_test_cursor_ops, .context = &valid_state};
    orm_row_cursor invalid_cursor = {
        .ops = &orm_flow_test_cursor_ops, .context = &invalid_state};
    orm_row_publisher_config config = ORM_ROW_PUBLISHER_CONFIG_INIT(
        &orm_flow_validated_row_data, 64u, 1u, 64u, 64u);
    cflow_publisher valid_source = {0};
    cflow_publisher invalid_source = {0};
    orm_flow_validated_row valid_row = {0};
    orm_flow_validated_row invalid_row = {UINT32_MAX};
    orm_error_t error;
    cflow_step step;

    check_not_null(plan);
    if (plan == NULL)
      return;
    config.message_plan = plan;

    orm_error_init(&error);
    check_equal(
        orm_row_publisher_init(
            &valid_source, &valid_cursor, &config, &error),
        ORM_STATUS_OK);
    step = cflow_publisher_resume(&valid_source, NULL, &valid_row);
    check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(valid_row.id, UINT32_C(11));
    cflow_publisher_destroy(&valid_source);
    check_equal(valid_state.cancel_count, (size_t)0u);
    check_equal(valid_state.destroy_count, (size_t)1u);

    orm_error_init(&error);
    check_equal(
        orm_row_publisher_init(
            &invalid_source, &invalid_cursor, &config, &error),
        ORM_STATUS_OK);
    step = cflow_publisher_resume(&invalid_source, NULL, &invalid_row);
    check_equal(step.kind, CFLOW_STEP_ERROR);
    check_not_null(step.error);
    check_contains(step.error, "row validation failed");
    check_equal(invalid_row.id, UINT32_C(0));
    check_equal(invalid_state.cancel_count, (size_t)1u);
    cflow_publisher_destroy(&invalid_source);
    check_equal(invalid_state.destroy_count, (size_t)1u);

    data_bind_message_plan_free(plan);
  }

  it("rejects a MessagePlan with the wrong native row shape before cursor work") {
    DataBindNativeTypeBinding native = {0};
    DataBindMessagePlan *plan = orm_flow_validation_plan(&native);
    orm_flow_test_cursor_state state = {
        .next_kind = ORM_ROW_CURSOR_DONE};
    orm_row_cursor cursor = {
        .ops = &orm_flow_test_cursor_ops, .context = &state};
    orm_row_publisher_config config = ORM_ROW_PUBLISHER_CONFIG_INIT(
        &orm_flow_test_row_data, 64u, 1u, 64u, 64u);
    cflow_publisher source = {0};
    orm_error_t error;

    check_not_null(plan);
    if (plan == NULL)
      return;
    config.message_plan = plan;
    orm_error_init(&error);
    check_equal(
        orm_row_publisher_init(&source, &cursor, &config, &error),
        ORM_STATUS_TYPE_ERROR);
    check_contains(error.message, "does not match ORM row_shape");
    check_equal(state.next_count, (size_t)0u);
    check_equal(state.cancel_count, (size_t)0u);
    check_equal(state.destroy_count, (size_t)0u);
    check_not_null(cursor.context);
    check_null(source.self);

    orm_row_cursor_dispose(&cursor);
    check_equal(state.destroy_count, (size_t)1u);
    data_bind_message_plan_free(plan);
  }

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
    orm_row_publisher_config config = ORM_ROW_PUBLISHER_CONFIG_INIT(
        &orm_flow_test_row_data, 1u, 1u, 64u, 1u);
    orm_error_t error;
    cflow_publisher source = {0};
    orm_flow_test_row row = {0};
    cflow_step step;

    orm_error_init(&error);
    check_equal(orm_row_publisher_init(&source, &cursor, &config, &error),
                ORM_STATUS_OK);
    check_equal(error.status, ORM_STATUS_OK);
    check_equal(error.message[0], '\0');
    check_null(cursor.ops);
    check_null(cursor.context);

    step = cflow_publisher_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(row.id, 7);
    check_equal(row.score, 19L);

    cflow_publisher_destroy(&source);
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
    orm_row_publisher_config config = ORM_ROW_PUBLISHER_CONFIG_INIT(
        &orm_flow_test_row_data, 1u, 1u, 64u, 1u);
    orm_error_t error;
    cflow_publisher source = {0};
    orm_flow_test_row row = {0};
    cflow_step step;

    orm_error_init(&error);
    check_equal(orm_row_publisher_init(&source, &cursor, &config, &error),
                ORM_STATUS_OK);

    step = cflow_publisher_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_ERROR);
    check_equal(state.cancel_count, (size_t)1u);

    cflow_publisher_destroy(&source);
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
    orm_row_publisher_config config = ORM_ROW_PUBLISHER_CONFIG_INIT(
        &orm_flow_test_row_data, 1u, 1u, 64u, 1u);
    orm_error_t error;
    cflow_publisher source = {0};
    orm_flow_test_row row = {0};
    cflow_step step;

    orm_error_init(&error);
    check_equal(orm_row_publisher_init(&source, &cursor, &config, &error),
                ORM_STATUS_OK);
    step = cflow_publisher_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_WAIT);
    check_true(cflow_waitable_valid(&step.waitable));
    check_equal(state.cancel_count, (size_t)0u);
    check_equal(state.destroy_count, (size_t)0u);

    cflow_waitable_cancel(&step.waitable);
    cflow_publisher_destroy(&source);
    check_equal(wait_state.cancel_count, (size_t)1u);
    check_equal(state.cancel_count, (size_t)1u);
    check_equal(state.destroy_count, (size_t)1u);
  }

  it("keeps cursor ownership when publisher configuration is rejected") {
    orm_flow_test_cursor_state state = {
        .reader_state = {NULL, 0u, 0u},
        .next_kind = ORM_ROW_CURSOR_DONE,
        .cancel_count = 0u,
        .destroy_count = 0u};
    orm_row_cursor cursor = {.ops = &orm_flow_test_cursor_ops,
                             .context = &state,
                             .wait_timeout_ns = 0u};
    orm_row_publisher_config config = ORM_ROW_PUBLISHER_CONFIG_INIT(
        &orm_flow_test_row_data, 1u, 0u, 64u, 1u);
    orm_error_t error;
    cflow_publisher source = {0};

    orm_error_init(&error);
    check_equal(orm_row_publisher_init(&source, &cursor, &config, &error),
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
    orm_row_publisher_config config = ORM_ROW_PUBLISHER_CONFIG_INIT(
        &orm_flow_test_row_data, 1u, 1u, 64u, 1u);
    orm_error_t error;
    cflow_publisher source = {0};
    orm_flow_test_row row = {.id = 91, .score = 92};
    cflow_step step;

    orm_error_init(&error);
    check_equal(orm_row_publisher_init(&source, &cursor, &config, &error),
                ORM_STATUS_OK);
    step = cflow_publisher_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_ERROR);
    check_not_null(step.error);
    check_equal(row.id, 0);
    check_equal(row.score, 0L);
    check_equal(state.cancel_count, (size_t)1u);

    cflow_publisher_destroy(&source);
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
    orm_row_publisher_config config = ORM_ROW_PUBLISHER_CONFIG_INIT(
        &orm_flow_test_row_data, 1u, 1u, 64u, 1u);
    orm_error_t error;
    cflow_publisher source = {0};
    const char *terminal_error = "not cleared";
    orm_flow_test_row row = {0};
    cflow_step step;

    orm_error_init(&error);
    check_equal(orm_row_publisher_init(&source, &cursor, &config, &error),
                ORM_STATUS_OK);
    cflow_publisher_cancel(&source);
    cflow_publisher_cancel(&source);
    check_equal(state.cancel_count, (size_t)1u);
    check_equal(cflow_publisher_poll_terminal(&source, &terminal_error),
                CFLOW_PUBLISHER_DONE);
    check_null(terminal_error);
    step = cflow_publisher_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_DONE);
    check_equal(state.next_count, (size_t)0u);

    cflow_publisher_destroy(&source);
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
    orm_row_publisher_config config = ORM_ROW_PUBLISHER_CONFIG_INIT(
        &orm_flow_test_row_data, 1u, 1u, 64u, 1u);
    orm_flow_test_sink_state sink_state = {0};
    cflow_subscriber_callbacks callbacks = {
        orm_flow_test_sink_value, orm_flow_test_sink_error,
        orm_flow_test_sink_done, &sink_state};
    cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);
    orm_error_t error;
    cflow_graph surface = {0};
    cflow_graph normalized = {0};
    cflow_scheduler scheduler = {0};
    cflow_publisher source = {0};
    cflow_subscription run = {0};

    normalized.root = CMETA_INVALID_ID;
    orm_error_init(&error);
    cflow_graph_init(&surface, &orm_flow_test_row_type);
    check_true(cflow_graph_normalize(&normalized, &surface));
    check_true(cflow_scheduler_test_init(&scheduler));
    check_equal(orm_row_publisher_init(&source, &cursor, &config, &error),
                ORM_STATUS_OK);
    check_true(cflow_subscribe(&run, &normalized, &source, &scheduler, &sink));
    check_null(source.self);
    check_equal(cursor_state.next_count, (size_t)0u);

    check_true(cflow_subscription_request(&run, 1u));
    (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
    check_equal(cursor_state.next_count, (size_t)1u);
    check_equal(sink_state.value_count, (size_t)1u);
    check_equal(sink_state.row.id, 11);
    check_equal(sink_state.row.score, 29L);
    check_equal(sink_state.done_count, (size_t)1u);
    check_null(sink_state.error);

    cflow_subscription_close(&run);
    check_equal(cursor_state.destroy_count, (size_t)1u);
    cflow_scheduler_destroy(&scheduler);
    cflow_graph_destroy(&normalized);
    cflow_graph_destroy(&surface);
  }
}

static size_t orm_budget_configure_calls;
static orm_status_t orm_budget_configure_status;
static orm_status_t orm_budget_configure(void *context,
    const cmeta_data_desc *shape, orm_error_t *error) {
  (void)context; (void)shape; (void)error;
  ++orm_budget_configure_calls;
  return orm_budget_configure_status;
}

spec("ORM row budgets are checked before native shape configuration") {
  before_each() {
    orm_budget_configure_calls = 0u;
    orm_budget_configure_status = ORM_STATUS_OK;
  }
  it("rejects one-byte-short field scratch without any native callback") {
    orm_flow_test_cursor_state state = {0};
    orm_row_cursor_ops ops = orm_flow_test_cursor_ops;
    ops.configure_shape = orm_budget_configure;
    orm_row_cursor cursor = {.ops = &ops, .context = &state};
    orm_row_publisher_config config = ORM_ROW_PUBLISHER_CONFIG_INIT(
        &orm_flow_test_row_data, 0u, 1u, 0u, 0u);
    cflow_publisher source = {0};
    orm_error_t error;
    orm_error_init(&error);
    check_equal(orm_row_publisher_init(&source, &cursor, &config, &error), ORM_STATUS_LIMIT_EXCEEDED);
    check_equal(orm_budget_configure_calls, 0u);
    check_equal(state.next_count, 0u);
    check_equal(state.cancel_count, 0u);
    check_equal(state.destroy_count, 0u);
    check_null(source.self);
    check_true(cursor.context == &state);
    orm_row_cursor_dispose(&cursor);
    check_equal(state.destroy_count, 1u);
  }
  it("rejects native-depth addition overflow before allocating or configuring") {
    orm_row_publisher_config config = ORM_ROW_PUBLISHER_CONFIG_INIT(
        &orm_flow_test_row_data, 1u, SIZE_MAX, 0u, 0u);
    orm_row_publisher_prepared *prepared = NULL;
    orm_error_t error;
    orm_error_init(&error);
    check_equal(orm_row_publisher_prepare(&config, &prepared, &error), ORM_STATUS_LIMIT_EXCEEDED);
    check_null(prepared);
    check_equal(orm_budget_configure_calls, 0u);
  }
  it("does not treat static Struct fields as dynamically collected items") {
    orm_row_publisher_config config = ORM_ROW_PUBLISHER_CONFIG_INIT(
        &orm_flow_test_row_data, 1u, 1u, 0u, 0u);
    orm_row_publisher_prepared *prepared = NULL;
    orm_error_t error;
    orm_error_init(&error);
    check_equal(orm_row_publisher_prepare(&config, &prepared, &error), ORM_STATUS_OK);
    check_not_null(prepared);
    check_equal(orm_budget_configure_calls, 0u);
    orm_row_publisher_prepared_destroy(prepared);
  }
  it("keeps preparation and cursor caller-owned when native configuration fails") {
    orm_flow_test_cursor_state state = {0};
    orm_row_cursor_ops ops = orm_flow_test_cursor_ops;
    ops.configure_shape = orm_budget_configure;
    orm_row_cursor cursor = {.ops = &ops, .context = &state};
    orm_row_publisher_config config = ORM_ROW_PUBLISHER_CONFIG_INIT(
        &orm_flow_test_row_data, 1u, 1u, 0u, 0u);
    orm_row_publisher_prepared *prepared = NULL;
    cflow_publisher source = {0};
    orm_error_t error;
    orm_error_init(&error);
    check_equal(orm_row_publisher_prepare(&config, &prepared, &error), ORM_STATUS_OK);
    orm_budget_configure_status = ORM_STATUS_TYPE_ERROR;
    check_equal(orm_row_publisher_publish(&source, &cursor, prepared, &error), ORM_STATUS_TYPE_ERROR);
    check_equal(orm_budget_configure_calls, 1u);
    check_equal(state.destroy_count, 0u);
    check_true(cursor.context == &state);
    check_null(source.self);
    orm_row_publisher_prepared_destroy(prepared);
    orm_row_cursor_dispose(&cursor);
    check_equal(state.destroy_count, 1u);
  }
}

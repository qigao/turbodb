#include "data/dbtool_transfer.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tinytest.h"

enum { FAKE_BYTES_CAPACITY = 64 };

typedef struct fake_transfer_state {
  const char *chunks[3];
  size_t chunk_sizes[3];
  size_t chunk_count;
  size_t chunk_index;
  size_t cell_count;
  size_t cell_size;
  uint64_t source_rows;
  uint64_t source_index;
  int decoder_fail;
  int sink_fail_call;
  int commit_fail;
  int rollback_fail;
  int source_fail_call;
  int huge_encoder_write;
  int oversized_input_read;
  int input_close_calls;
  int decoder_close_calls;
  int sink_begin_calls;
  int sink_write_calls;
  int sink_commit_calls;
  int sink_rollback_calls;
  int sink_close_calls;
  int source_open_calls;
  int source_next_calls;
  int source_close_calls;
  int encoder_close_calls;
  int output_close_calls;
  unsigned char observed[FAKE_BYTES_CAPACITY];
  size_t observed_size;
  dbtool_write_bytes_fn writer;
  void *writer_context;
  unsigned char borrowed_source_bytes[3];
} fake_transfer_state;

typedef struct fake_decoder_context {
  fake_transfer_state *state;
  dbtool_record_emit_fn emit;
  void *emit_context;
} fake_decoder_context;

static fake_transfer_state fake_state;

static const dbtool_column_v1 fake_columns[] = {
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 0u, 0u,
     DBTOOL_SCALAR_TEXT, DBTOOL_STORAGE_TEXT, "value", "value"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 1u,
     DBTOOL_COLUMN_OPTIONAL, DBTOOL_SCALAR_TEXT, DBTOOL_STORAGE_TEXT, "note",
     "note"}};
static const dbtool_table_v1 fake_table = {
    sizeof(dbtool_table_v1), DBTOOL_MODEL_ABI_VERSION, 0u,
    "Record",               "records",                fake_columns,
    2u};

static void fake_fail(dbtool_error *error, const char *message) {
  dbtool_error_set(error, DBTOOL_STATUS_SQL_ERROR, "fake-transfer", 0,
                   message);
}

static dbtool_status fake_input_read(void *context, unsigned char *destination,
                                     size_t capacity, size_t *out_size,
                                     dbtool_error *error) {
  fake_transfer_state *state = (fake_transfer_state *)context;
  const size_t index = state->chunk_index;
  (void)error;
  *out_size = 0u;
  if (index >= state->chunk_count)
    return DBTOOL_STATUS_OK;
  if (state->oversized_input_read) {
    *out_size = capacity + 1u;
    ++state->chunk_index;
    return DBTOOL_STATUS_OK;
  }
  if (state->chunk_sizes[index] > capacity)
    return DBTOOL_STATUS_INTERNAL_ERROR;
  memcpy(destination, state->chunks[index], state->chunk_sizes[index]);
  *out_size = state->chunk_sizes[index];
  ++state->chunk_index;
  return DBTOOL_STATUS_OK;
}

static void fake_input_close(void *context) {
  ++((fake_transfer_state *)context)->input_close_calls;
}

static const dbtool_byte_source_ops fake_input_ops = {
    sizeof(dbtool_byte_source_ops), DBTOOL_RECORD_ABI_VERSION,
    fake_input_read, fake_input_close};

static dbtool_status fake_decoder_feed(void *context,
                                       const unsigned char *data, size_t size,
                                       int final, dbtool_error *error) {
  fake_decoder_context *decoder = (fake_decoder_context *)context;
  dbtool_cell cells[2] = {0};
  dbtool_record_view record;
  size_t index;
  if (decoder->state->decoder_fail) {
    fake_fail(error, "injected decode failure");
    return DBTOOL_STATUS_SQL_ERROR;
  }
  if (final || size == 0u)
    return DBTOOL_STATUS_OK;
  for (index = 0u; index < decoder->state->cell_count && index < 2u; ++index) {
    cells[index].kind = DBTOOL_VALUE_TEXT;
    cells[index].data.bytes.data = data;
    cells[index].data.bytes.size = decoder->state->cell_size != 0u
                                       ? decoder->state->cell_size
                                       : size;
  }
  record.cells = cells;
  record.cell_count = decoder->state->cell_count;
  return decoder->emit(decoder->emit_context, &record, error);
}

static void fake_decoder_close(void *context) {
  fake_decoder_context *decoder = (fake_decoder_context *)context;
  ++decoder->state->decoder_close_calls;
}

static const dbtool_decoder_ops fake_decoder_ops = {
    sizeof(dbtool_decoder_ops), DBTOOL_MODEL_ABI_VERSION,
    fake_decoder_feed, fake_decoder_close};

static dbtool_status fake_open_decoder(
    const void *model_context, size_t table_index, dbtool_format format,
    const dbtool_transfer_limits *limits, dbtool_record_emit_fn emit,
    void *emit_context, dbtool_decoder *out, dbtool_error *error) {
  static fake_decoder_context decoder;
  (void)table_index;
  (void)format;
  (void)limits;
  (void)error;
  decoder.state = (fake_transfer_state *)model_context;
  decoder.emit = emit;
  decoder.emit_context = emit_context;
  out->ops = &fake_decoder_ops;
  out->context = &decoder;
  return DBTOOL_STATUS_OK;
}

static dbtool_status fake_encoder_write(void *context,
                                        const dbtool_record_view *record,
                                        dbtool_error *error) {
  fake_transfer_state *state = (fake_transfer_state *)context;
  if (state->huge_encoder_write) {
    static const unsigned char byte = 1u;
    dbtool_status status =
        state->writer(state->writer_context, &byte, 1u, error);
    if (status != DBTOOL_STATUS_OK)
      return status;
    return state->writer(state->writer_context, &byte, SIZE_MAX, error);
  }
  return state->writer(state->writer_context,
                       record->cells[0].data.bytes.data,
                       record->cells[0].data.bytes.size, error);
}

static dbtool_status fake_encoder_finish(void *context, dbtool_error *error) {
  (void)context;
  (void)error;
  return DBTOOL_STATUS_OK;
}

static void fake_encoder_close(void *context) {
  ++((fake_transfer_state *)context)->encoder_close_calls;
}

static const dbtool_encoder_ops fake_encoder_ops = {
    sizeof(dbtool_encoder_ops), DBTOOL_MODEL_ABI_VERSION,
    fake_encoder_write, fake_encoder_finish, fake_encoder_close};

static dbtool_status fake_open_encoder(
    const void *model_context, size_t table_index, dbtool_format format,
    const dbtool_transfer_limits *limits, dbtool_write_bytes_fn writer,
    void *writer_context, dbtool_encoder *out, dbtool_error *error) {
  fake_transfer_state *state = (fake_transfer_state *)model_context;
  (void)table_index;
  (void)format;
  (void)limits;
  (void)error;
  state->writer = writer;
  state->writer_context = writer_context;
  out->ops = &fake_encoder_ops;
  out->context = state;
  return DBTOOL_STATUS_OK;
}

static const dbtool_model_v1 fake_model = {
    sizeof(dbtool_model_v1), DBTOOL_MODEL_ABI_VERSION, &fake_state,
    &fake_table,              1u,                       fake_open_decoder,
    fake_open_encoder};

static dbtool_status fake_sink_begin(void *factory_context, void **out_context,
                                     const dbtool_model_v1 *model,
                                     size_t table_index,
                                     dbtool_error *error) {
  fake_transfer_state *state = (fake_transfer_state *)factory_context;
  (void)model;
  (void)table_index;
  (void)error;
  ++state->sink_begin_calls;
  *out_context = state;
  return DBTOOL_STATUS_OK;
}

static dbtool_status fake_sink_write(void *context,
                                     const dbtool_record_view *record,
                                     dbtool_error *error) {
  fake_transfer_state *state = (fake_transfer_state *)context;
  size_t copy_size;
  ++state->sink_write_calls;
  if (state->sink_fail_call == state->sink_write_calls) {
    fake_fail(error, "injected sink failure");
    return DBTOOL_STATUS_SQL_ERROR;
  }
  copy_size = record->cells[0].data.bytes.size;
  if (copy_size > sizeof(state->observed) - state->observed_size)
    copy_size = sizeof(state->observed) - state->observed_size;
  memcpy(state->observed + state->observed_size,
         record->cells[0].data.bytes.data, copy_size);
  state->observed_size += copy_size;
  return DBTOOL_STATUS_OK;
}

static dbtool_status fake_sink_commit(void *context, dbtool_error *error) {
  fake_transfer_state *state = (fake_transfer_state *)context;
  ++state->sink_commit_calls;
  if (state->commit_fail) {
    fake_fail(error, "injected commit failure");
    return DBTOOL_STATUS_SQL_ERROR;
  }
  return DBTOOL_STATUS_OK;
}

static dbtool_status fake_sink_rollback(void *context, dbtool_error *error) {
  fake_transfer_state *state = (fake_transfer_state *)context;
  ++state->sink_rollback_calls;
  if (state->rollback_fail) {
    fake_fail(error, "injected rollback failure");
    return DBTOOL_STATUS_SQL_ERROR;
  }
  return DBTOOL_STATUS_OK;
}

static void fake_sink_close(void *context) {
  ++((fake_transfer_state *)context)->sink_close_calls;
}

static const dbtool_record_sink_ops fake_sink_ops = {
    sizeof(dbtool_record_sink_ops), DBTOOL_RECORD_ABI_VERSION,
    fake_sink_begin, fake_sink_write, fake_sink_commit,
    fake_sink_rollback, fake_sink_close};

static dbtool_status fake_source_open(void *factory_context,
                                      void **out_context,
                                      const dbtool_model_v1 *model,
                                      size_t table_index,
                                      dbtool_error *error) {
  fake_transfer_state *state = (fake_transfer_state *)factory_context;
  (void)model;
  (void)table_index;
  (void)error;
  ++state->source_open_calls;
  *out_context = state;
  return DBTOOL_STATUS_OK;
}

static dbtool_record_step fake_source_next(void *context,
                                           dbtool_record_view *out,
                                           dbtool_error *error) {
  static const unsigned char values[][3] = {{'o', 'n', 'e'},
                                             {'t', 'w', 'o'}};
  static dbtool_cell cells[2];
  fake_transfer_state *state = (fake_transfer_state *)context;
  size_t index;
  ++state->source_next_calls;
  if (state->source_fail_call == state->source_next_calls) {
    fake_fail(error, "injected source failure");
    return DBTOOL_RECORD_ERROR;
  }
  if (state->source_index >= state->source_rows)
    return DBTOOL_RECORD_DONE;
  memcpy(state->borrowed_source_bytes, values[state->source_index % 2u], 3u);
  for (index = 0u; index < state->cell_count && index < 2u; ++index) {
    cells[index].kind = DBTOOL_VALUE_TEXT;
    cells[index].data.bytes.data = state->borrowed_source_bytes;
    cells[index].data.bytes.size = state->cell_size != 0u
                                       ? state->cell_size
                                       : 3u;
  }
  out->cells = cells;
  out->cell_count = state->cell_count;
  ++state->source_index;
  return DBTOOL_RECORD_ROW;
}

static void fake_source_close(void *context) {
  ++((fake_transfer_state *)context)->source_close_calls;
}

static const dbtool_record_source_ops fake_source_ops = {
    sizeof(dbtool_record_source_ops), DBTOOL_RECORD_ABI_VERSION,
    fake_source_open, fake_source_next, fake_source_close};

static dbtool_status fake_output_write(void *context,
                                       const unsigned char *data, size_t size,
                                       dbtool_error *error) {
  fake_transfer_state *state = (fake_transfer_state *)context;
  (void)error;
  if (size > sizeof(state->observed) - state->observed_size)
    return DBTOOL_STATUS_LIMIT_EXCEEDED;
  memcpy(state->observed + state->observed_size, data, size);
  state->observed_size += size;
  return DBTOOL_STATUS_OK;
}

static void fake_output_close(void *context) {
  ++((fake_transfer_state *)context)->output_close_calls;
}

static const dbtool_byte_sink_ops fake_output_ops = {
    sizeof(dbtool_byte_sink_ops), DBTOOL_RECORD_ABI_VERSION,
    fake_output_write, fake_output_close};

static dbtool_transfer_limits fake_limits(void) {
  dbtool_transfer_limits limits = DBTOOL_TRANSFER_LIMITS_INIT;
  limits.chunk_bytes = 8u;
  limits.max_input_bytes = 32u;
  limits.max_rows = 4u;
  limits.max_columns = 2u;
  limits.max_cell_bytes = 16u;
  limits.max_record_bytes = 64u;
  limits.max_output_bytes = 32u;
  return limits;
}

static dbtool_status run_import(dbtool_transfer_limits limits,
                                dbtool_transfer_result *result,
                                dbtool_error *error) {
  const dbtool_import_request request = {
      sizeof(dbtool_import_request), DBTOOL_TRANSFER_ABI_VERSION,
      &fake_model,                    0u,
      DBTOOL_FORMAT_JSON,             limits,
      {&fake_input_ops, &fake_state}, &fake_sink_ops,
      &fake_state};
  return dbtool_transfer_import(&request, result, error);
}

static dbtool_status run_export(dbtool_transfer_limits limits,
                                dbtool_transfer_result *result,
                                dbtool_error *error) {
  const dbtool_export_request request = {
      sizeof(dbtool_export_request), DBTOOL_TRANSFER_ABI_VERSION,
      &fake_model,                    0u,
      DBTOOL_FORMAT_JSON,             limits,
      &fake_source_ops,               &fake_state,
      {&fake_output_ops, &fake_state}};
  return dbtool_transfer_export(&request, result, error);
}

static void fake_reset(void) {
  memset(&fake_state, 0, sizeof(fake_state));
  fake_state.cell_count = 2u;
}

spec("generated model transfer core") {
  before_each() { fake_reset(); }

  it("commits one import transaction and consumes borrowed chunk rows") {
    dbtool_transfer_limits limits = fake_limits();
    dbtool_transfer_result result = DBTOOL_TRANSFER_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;
    fake_state.chunks[0] = "one";
    fake_state.chunk_sizes[0] = 3u;
    fake_state.chunks[1] = "two";
    fake_state.chunk_sizes[1] = 3u;
    fake_state.chunk_count = 2u;

    check_equal(run_import(limits, &result, &error), DBTOOL_STATUS_OK);
    check_equal(result.rows, (uint64_t)2u);
    check_equal(result.input_bytes, (uint64_t)6u);
    check_equal(fake_state.observed_size, (size_t)6u);
    check_equal(memcmp(fake_state.observed, "onetwo", 6u), 0);
    check_equal(fake_state.sink_begin_calls, 1);
    check_equal(fake_state.sink_commit_calls, 1);
    check_equal(fake_state.sink_rollback_calls, 0);
    check_equal(fake_state.sink_close_calls, 1);
    check_equal(fake_state.decoder_close_calls, 1);
    check_equal(fake_state.input_close_calls, 1);
  }

  it("rolls back a parse failure before the first row") {
    dbtool_transfer_limits limits = fake_limits();
    dbtool_transfer_result result = DBTOOL_TRANSFER_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;
    fake_state.chunks[0] = "bad";
    fake_state.chunk_sizes[0] = 3u;
    fake_state.chunk_count = 1u;
    fake_state.decoder_fail = 1;

    check_equal(run_import(limits, &result, &error),
                DBTOOL_STATUS_SQL_ERROR);
    check_equal(fake_state.sink_write_calls, 0);
    check_equal(fake_state.sink_commit_calls, 0);
    check_equal(fake_state.sink_rollback_calls, 1);
    check_equal(fake_state.sink_close_calls, 1);
  }

  it("preserves a later sink failure when rollback also fails") {
    dbtool_transfer_limits limits = fake_limits();
    dbtool_transfer_result result = DBTOOL_TRANSFER_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;
    fake_state.chunks[0] = "one";
    fake_state.chunk_sizes[0] = 3u;
    fake_state.chunks[1] = "two";
    fake_state.chunk_sizes[1] = 3u;
    fake_state.chunk_count = 2u;
    fake_state.sink_fail_call = 2;
    fake_state.rollback_fail = 1;

    check_equal(run_import(limits, &result, &error),
                DBTOOL_STATUS_SQL_ERROR);
    check_equal(fake_state.sink_write_calls, 2);
    check_equal(fake_state.sink_rollback_calls, 1);
    check_contains(error.message, "sink failure");
  }

  it("rolls back a failed commit and preserves the commit error") {
    dbtool_transfer_limits limits = fake_limits();
    dbtool_transfer_result result = DBTOOL_TRANSFER_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;
    fake_state.chunks[0] = "one";
    fake_state.chunk_sizes[0] = 3u;
    fake_state.chunk_count = 1u;
    fake_state.commit_fail = 1;
    fake_state.rollback_fail = 1;

    check_equal(run_import(limits, &result, &error),
                DBTOOL_STATUS_SQL_ERROR);
    check_equal(fake_state.sink_commit_calls, 1);
    check_equal(fake_state.sink_rollback_calls, 1);
    check_contains(error.message, "commit failure");
  }

  it("closes export resources after a source failure") {
    dbtool_transfer_limits limits = fake_limits();
    dbtool_transfer_result result = DBTOOL_TRANSFER_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;
    fake_state.source_rows = 2u;
    fake_state.source_fail_call = 2;

    check_equal(run_export(limits, &result, &error),
                DBTOOL_STATUS_SQL_ERROR);
    check_equal(fake_state.source_open_calls, 1);
    check_equal(fake_state.source_close_calls, 1);
    check_equal(fake_state.encoder_close_calls, 1);
    check_equal(fake_state.output_close_calls, 1);
    check_contains(error.message, "source failure");
  }

  it("encodes each borrowed source row before advancing the cursor") {
    dbtool_transfer_limits limits = fake_limits();
    dbtool_transfer_result result = DBTOOL_TRANSFER_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;
    fake_state.source_rows = 2u;

    check_equal(run_export(limits, &result, &error), DBTOOL_STATUS_OK);
    check_equal(result.rows, (uint64_t)2u);
    check_equal(result.output_bytes, (uint64_t)6u);
    check_equal(fake_state.observed_size, (size_t)6u);
    check_equal(memcmp(fake_state.observed, "onetwo", 6u), 0);
  }

  it("rejects each zero transfer limit before acquiring resources") {
    dbtool_transfer_limits limits = fake_limits();
    dbtool_transfer_result result = DBTOOL_TRANSFER_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;

    limits.chunk_bytes = 0u;
    check_equal(run_import(limits, &result, &error),
                DBTOOL_STATUS_INVALID_ARGUMENT);
    limits = fake_limits();
    limits.max_input_bytes = 0u;
    check_equal(run_import(limits, &result, &error),
                DBTOOL_STATUS_INVALID_ARGUMENT);
    limits = fake_limits();
    limits.max_rows = 0u;
    check_equal(run_import(limits, &result, &error),
                DBTOOL_STATUS_INVALID_ARGUMENT);
    limits = fake_limits();
    limits.max_columns = 0u;
    check_equal(run_import(limits, &result, &error),
                DBTOOL_STATUS_INVALID_ARGUMENT);
    limits = fake_limits();
    limits.max_cell_bytes = 0u;
    check_equal(run_import(limits, &result, &error),
                DBTOOL_STATUS_INVALID_ARGUMENT);
    limits = fake_limits();
    limits.max_record_bytes = 0u;
    check_equal(run_import(limits, &result, &error),
                DBTOOL_STATUS_INVALID_ARGUMENT);
    limits = fake_limits();
    limits.max_output_bytes = 0u;
    check_equal(run_import(limits, &result, &error),
                DBTOOL_STATUS_INVALID_ARGUMENT);
    check_equal(fake_state.input_close_calls, 0);
    check_equal(fake_state.sink_begin_calls, 0);
  }

  it("enforces chunk and input byte capacity boundaries") {
    dbtool_transfer_limits limits = fake_limits();
    dbtool_transfer_result result = DBTOOL_TRANSFER_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;

    fake_state.chunks[0] = "x";
    fake_state.chunk_sizes[0] = 1u;
    fake_state.chunk_count = 1u;
    limits.chunk_bytes = 1u;
    limits.max_input_bytes = 1u;
    check_equal(run_import(limits, &result, &error), DBTOOL_STATUS_OK);
    check_equal(result.input_bytes, (uint64_t)1u);

    fake_reset();
    limits = fake_limits();
    fake_state.chunks[0] = "one";
    fake_state.chunk_sizes[0] = 3u;
    fake_state.chunk_count = 1u;
    limits.chunk_bytes = 3u;
    limits.max_input_bytes = 3u;
    check_equal(run_import(limits, &result, &error), DBTOOL_STATUS_OK);

    fake_reset();
    limits = fake_limits();
    fake_state.chunk_count = 1u;
    fake_state.oversized_input_read = 1;
    limits.chunk_bytes = 3u;
    check_equal(run_import(limits, &result, &error),
                DBTOOL_STATUS_INTERNAL_ERROR);

    fake_reset();
    limits = fake_limits();
    fake_state.chunks[0] = "one";
    fake_state.chunk_sizes[0] = 3u;
    fake_state.chunks[1] = "x";
    fake_state.chunk_sizes[1] = 1u;
    fake_state.chunk_count = 2u;
    limits.max_input_bytes = 3u;
    check_equal(run_import(limits, &result, &error),
                DBTOOL_STATUS_LIMIT_EXCEEDED);

    fake_reset();
    limits = fake_limits();
    fake_state.chunks[0] = "one";
    fake_state.chunk_sizes[0] = 3u;
    fake_state.chunk_count = 1u;
    limits.max_input_bytes = UINT64_MAX;
    check_equal(run_import(limits, &result, &error), DBTOOL_STATUS_OK);

    fake_reset();
    limits = fake_limits();
    limits.chunk_bytes = SIZE_MAX;
    check_equal(run_import(limits, &result, &error),
                DBTOOL_STATUS_OUT_OF_MEMORY);
    check_equal(fake_state.input_close_calls, 1);
  }

  it("enforces row, column, cell, and record capacity boundaries") {
    dbtool_transfer_limits limits = fake_limits();
    dbtool_transfer_result result = DBTOOL_TRANSFER_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;
    const size_t exact_record_bytes = (2u * sizeof(dbtool_cell)) + 6u;

    fake_state.chunks[0] = "one";
    fake_state.chunk_sizes[0] = 3u;
    fake_state.chunk_count = 1u;
    limits.max_rows = 1u;
    limits.max_columns = 2u;
    limits.max_cell_bytes = 3u;
    limits.max_record_bytes = exact_record_bytes;
    check_equal(run_import(limits, &result, &error), DBTOOL_STATUS_OK);
    check_equal(result.rows, (uint64_t)1u);

    fake_reset();
    limits = fake_limits();
    fake_state.chunks[0] = "one";
    fake_state.chunk_sizes[0] = 3u;
    fake_state.chunks[1] = "two";
    fake_state.chunk_sizes[1] = 3u;
    fake_state.chunk_count = 2u;
    limits.max_rows = 1u;
    check_equal(run_import(limits, &result, &error),
                DBTOOL_STATUS_LIMIT_EXCEEDED);

    fake_reset();
    limits = fake_limits();
    fake_state.chunks[0] = "one";
    fake_state.chunk_sizes[0] = 3u;
    fake_state.chunk_count = 1u;
    limits.max_columns = 1u;
    check_equal(run_import(limits, &result, &error),
                DBTOOL_STATUS_LIMIT_EXCEEDED);

    fake_reset();
    limits = fake_limits();
    fake_state.chunks[0] = "one";
    fake_state.chunk_sizes[0] = 3u;
    fake_state.chunk_count = 1u;
    limits.max_cell_bytes = 2u;
    check_equal(run_import(limits, &result, &error),
                DBTOOL_STATUS_LIMIT_EXCEEDED);

    fake_reset();
    limits = fake_limits();
    fake_state.chunks[0] = "one";
    fake_state.chunk_sizes[0] = 3u;
    fake_state.chunk_count = 1u;
    limits.max_record_bytes = exact_record_bytes - 1u;
    check_equal(run_import(limits, &result, &error),
                DBTOOL_STATUS_LIMIT_EXCEEDED);

    fake_reset();
    limits = fake_limits();
    fake_state.chunks[0] = "x";
    fake_state.chunk_sizes[0] = 1u;
    fake_state.chunk_count = 1u;
    limits.max_rows = UINT64_MAX;
    limits.max_columns = SIZE_MAX;
    limits.max_cell_bytes = SIZE_MAX;
    limits.max_record_bytes = SIZE_MAX;
    check_equal(run_import(limits, &result, &error), DBTOOL_STATUS_OK);

    fake_reset();
    limits = fake_limits();
    fake_state.chunks[0] = "x";
    fake_state.chunk_sizes[0] = 1u;
    fake_state.chunk_count = 1u;
    fake_state.cell_count = SIZE_MAX;
    limits.max_columns = SIZE_MAX;
    limits.max_record_bytes = SIZE_MAX;
    check_equal(run_import(limits, &result, &error),
                DBTOOL_STATUS_LIMIT_EXCEEDED);
    check_contains(error.message, "metadata size overflows");
  }

  it("enforces exact and maximum output limits with checked addition") {
    dbtool_transfer_limits limits = fake_limits();
    dbtool_transfer_result result = DBTOOL_TRANSFER_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;

    fake_state.source_rows = 1u;
    limits.max_output_bytes = 3u;
    check_equal(run_export(limits, &result, &error), DBTOOL_STATUS_OK);
    check_equal(result.output_bytes, (uint64_t)3u);

    fake_reset();
    limits = fake_limits();
    fake_state.source_rows = 2u;
    limits.max_output_bytes = 5u;
    check_equal(run_export(limits, &result, &error),
                DBTOOL_STATUS_LIMIT_EXCEEDED);

    fake_reset();
    limits = fake_limits();
    fake_state.source_rows = 2u;
    limits.max_rows = UINT64_MAX;
    limits.max_output_bytes = UINT64_MAX;
    check_equal(run_export(limits, &result, &error), DBTOOL_STATUS_OK);
    check_equal(result.output_bytes, (uint64_t)6u);

    fake_reset();
    limits = fake_limits();
    limits.max_output_bytes = UINT64_MAX;
    fake_state.source_rows = 1u;
    fake_state.huge_encoder_write = 1;
    check_equal(run_export(limits, &result, &error),
                DBTOOL_STATUS_LIMIT_EXCEEDED);
  }
}

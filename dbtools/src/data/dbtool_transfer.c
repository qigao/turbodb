#include "data/dbtool_transfer.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct dbtool_import_state {
  const dbtool_import_request *request;
  const dbtool_table_v1 *table;
  void *sink_context;
  uint64_t rows;
} dbtool_import_state;

typedef struct dbtool_output_state {
  const dbtool_export_request *request;
  uint64_t bytes;
} dbtool_output_state;

static dbtool_status dbtool_transfer_fail(dbtool_error *error,
                                          dbtool_status status,
                                          const char *stage,
                                          const char *message) {
  dbtool_error_set(error, status, stage, 0, message);
  return status;
}

static dbtool_status dbtool_transfer_preserve_callback_error(
    dbtool_status status, const char *stage, const char *message,
    dbtool_error *error) {
  if (status != DBTOOL_STATUS_OK &&
      (error == NULL || error->status == DBTOOL_STATUS_OK))
    return dbtool_transfer_fail(error, status, stage, message);
  return status;
}

static int dbtool_format_valid(dbtool_format format) {
  return format >= DBTOOL_FORMAT_JSON && format <= DBTOOL_FORMAT_TBE_BINARY;
}

static int dbtool_limits_valid(const dbtool_transfer_limits *limits) {
  return limits != NULL && limits->struct_size >= sizeof(*limits) &&
         limits->abi_version == DBTOOL_MODEL_ABI_VERSION &&
         limits->chunk_bytes != 0u && limits->max_input_bytes != 0u &&
         limits->max_rows != 0u && limits->max_columns != 0u &&
         limits->max_cell_bytes != 0u && limits->max_record_bytes != 0u &&
         limits->max_output_bytes != 0u;
}

static int dbtool_model_valid(const dbtool_model_v1 *model,
                              size_t table_index) {
  const dbtool_table_v1 *table;
  if (model == NULL || model->struct_size < sizeof(*model) ||
      model->abi_version != DBTOOL_MODEL_ABI_VERSION || model->tables == NULL ||
      model->table_count == 0u || table_index >= model->table_count ||
      model->open_decoder == NULL || model->open_encoder == NULL)
    return 0;
  table = &model->tables[table_index];
  return table->struct_size >= sizeof(*table) &&
         table->abi_version == DBTOOL_MODEL_ABI_VERSION &&
         table->index == table_index && table->name != NULL &&
         table->database_name != NULL && table->columns != NULL &&
         table->column_count != 0u;
}

static int dbtool_decoder_valid(const dbtool_decoder *decoder) {
  return decoder != NULL && decoder->context != NULL && decoder->ops != NULL &&
         decoder->ops->struct_size >= sizeof(*decoder->ops) &&
         decoder->ops->abi_version == DBTOOL_MODEL_ABI_VERSION &&
         decoder->ops->feed != NULL && decoder->ops->close != NULL;
}

static int dbtool_encoder_valid(const dbtool_encoder *encoder) {
  return encoder != NULL && encoder->context != NULL && encoder->ops != NULL &&
         encoder->ops->struct_size >= sizeof(*encoder->ops) &&
         encoder->ops->abi_version == DBTOOL_MODEL_ABI_VERSION &&
         encoder->ops->write != NULL && encoder->ops->finish != NULL &&
         encoder->ops->close != NULL;
}

static dbtool_status dbtool_record_validate(
    const dbtool_record_view *record, const dbtool_table_v1 *table,
    const dbtool_transfer_limits *limits, dbtool_error *error) {
  size_t record_bytes;
  size_t index;
  if (record == NULL || record->cells == NULL || record->cell_count == 0u) {
    return dbtool_transfer_fail(error, DBTOOL_STATUS_INTERNAL_ERROR,
                                "validate-record", "record view is invalid");
  }
  if (record->cell_count > limits->max_columns) {
    return dbtool_transfer_fail(error, DBTOOL_STATUS_LIMIT_EXCEEDED,
                                "validate-record",
                                "record exceeds max_columns");
  }
  if (record->cell_count > SIZE_MAX / sizeof(*record->cells)) {
    return dbtool_transfer_fail(error, DBTOOL_STATUS_LIMIT_EXCEEDED,
                                "validate-record",
                                "record metadata size overflows");
  }
  if (record->cell_count != table->column_count) {
    return dbtool_transfer_fail(error, DBTOOL_STATUS_INTERNAL_ERROR,
                                "validate-record",
                                "record column count does not match model");
  }
  record_bytes = record->cell_count * sizeof(*record->cells);
  if (record_bytes > limits->max_record_bytes) {
    return dbtool_transfer_fail(error, DBTOOL_STATUS_LIMIT_EXCEEDED,
                                "validate-record",
                                "record exceeds max_record_bytes");
  }
  for (index = 0u; index < record->cell_count; ++index) {
    const dbtool_cell *cell = &record->cells[index];
    size_t bytes = 0u;
    if (cell->kind == DBTOOL_VALUE_TEXT ||
        cell->kind == DBTOOL_VALUE_BYTES || cell->kind == DBTOOL_VALUE_UUID) {
      bytes = cell->data.bytes.size;
      if (bytes != 0u && cell->data.bytes.data == NULL) {
        return dbtool_transfer_fail(error, DBTOOL_STATUS_INTERNAL_ERROR,
                                    "validate-record",
                                    "cell byte view is invalid");
      }
      if (bytes > limits->max_cell_bytes) {
        return dbtool_transfer_fail(error, DBTOOL_STATUS_LIMIT_EXCEEDED,
                                    "validate-record",
                                    "cell exceeds max_cell_bytes");
      }
    } else if (cell->kind < DBTOOL_VALUE_ABSENT ||
               cell->kind > DBTOOL_VALUE_BOOLEAN) {
      return dbtool_transfer_fail(error, DBTOOL_STATUS_INTERNAL_ERROR,
                                  "validate-record",
                                  "cell value kind is invalid");
    }
    if (bytes > limits->max_record_bytes - record_bytes) {
      return dbtool_transfer_fail(error, DBTOOL_STATUS_LIMIT_EXCEEDED,
                                  "validate-record",
                                  "record exceeds max_record_bytes");
    }
    record_bytes += bytes;
  }
  return DBTOOL_STATUS_OK;
}

static dbtool_status dbtool_import_emit(void *context,
                                        const dbtool_record_view *record,
                                        dbtool_error *error) {
  dbtool_import_state *state = (dbtool_import_state *)context;
  dbtool_status status;
  if (state->rows >= state->request->limits.max_rows) {
    return dbtool_transfer_fail(error, DBTOOL_STATUS_LIMIT_EXCEEDED,
                                "write-record", "import exceeds max_rows");
  }
  status = dbtool_record_validate(record, state->table,
                                  &state->request->limits, error);
  if (status != DBTOOL_STATUS_OK)
    return status;
  status = state->request->sink->write(state->sink_context, record, error);
  status = dbtool_transfer_preserve_callback_error(
      status, "write-record", "record sink write failed", error);
  if (status == DBTOOL_STATUS_OK)
    ++state->rows;
  return status;
}

static int dbtool_import_request_valid(const dbtool_import_request *request) {
  const dbtool_byte_source_ops *input;
  const dbtool_record_sink_ops *sink;
  if (request == NULL || request->struct_size < sizeof(*request) ||
      request->abi_version != DBTOOL_TRANSFER_ABI_VERSION ||
      !dbtool_model_valid(request->model, request->table_index) ||
      !dbtool_format_valid(request->format) ||
      !dbtool_limits_valid(&request->limits))
    return 0;
  input = request->input.ops;
  sink = request->sink;
  return request->input.context != NULL && input != NULL &&
         input->struct_size >= sizeof(*input) &&
         input->abi_version == DBTOOL_RECORD_ABI_VERSION &&
         input->read != NULL && input->close != NULL && sink != NULL &&
         sink->struct_size >= sizeof(*sink) &&
         sink->abi_version == DBTOOL_RECORD_ABI_VERSION &&
         sink->begin != NULL && sink->write != NULL && sink->commit != NULL &&
         sink->rollback != NULL && sink->close != NULL;
}

dbtool_status dbtool_transfer_import(const dbtool_import_request *request,
                                     dbtool_transfer_result *result,
                                     dbtool_error *error) {
  dbtool_import_state state;
  dbtool_decoder decoder = {0};
  unsigned char *chunk = NULL;
  uint64_t input_bytes = 0u;
  dbtool_status status;
  int decoder_open = 0;
  int transaction_active = 0;
  if (result != NULL)
    *result = (dbtool_transfer_result)DBTOOL_TRANSFER_RESULT_INIT;
  dbtool_error_init(error);
  if (result == NULL || !dbtool_import_request_valid(request))
    return dbtool_transfer_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT,
                                "validate-transfer",
                                "invalid import transfer request");

  memset(&state, 0, sizeof(state));
  state.request = request;
  state.table = &request->model->tables[request->table_index];
  chunk = (unsigned char *)malloc(request->limits.chunk_bytes);
  if (chunk == NULL) {
    status = dbtool_transfer_fail(error, DBTOOL_STATUS_OUT_OF_MEMORY,
                                  "open-transfer",
                                  "allocate import chunk buffer");
    goto cleanup;
  }
  status = request->sink->begin(request->sink_factory_context,
                                &state.sink_context, request->model,
                                request->table_index, error);
  status = dbtool_transfer_preserve_callback_error(
      status, "begin-import", "record sink begin failed", error);
  if (status != DBTOOL_STATUS_OK)
    goto cleanup;
  if (state.sink_context == NULL) {
    status = dbtool_transfer_fail(error, DBTOOL_STATUS_INTERNAL_ERROR,
                                  "begin-import",
                                  "record sink returned no context");
    goto cleanup;
  }
  transaction_active = 1;
  status = request->model->open_decoder(
      request->model->context, request->table_index, request->format,
      &request->limits, dbtool_import_emit, &state, &decoder, error);
  status = dbtool_transfer_preserve_callback_error(
      status, "open-decoder", "model decoder open failed", error);
  if (status != DBTOOL_STATUS_OK)
    goto rollback;
  if (!dbtool_decoder_valid(&decoder)) {
    status = dbtool_transfer_fail(error, DBTOOL_STATUS_INTERNAL_ERROR,
                                  "open-decoder",
                                  "model returned an invalid decoder");
    goto rollback;
  }
  decoder_open = 1;

  for (;;) {
    size_t size = 0u;
    status = request->input.ops->read(request->input.context, chunk,
                                      request->limits.chunk_bytes, &size,
                                      error);
    status = dbtool_transfer_preserve_callback_error(
        status, "read-input", "input read failed", error);
    if (status != DBTOOL_STATUS_OK)
      goto rollback;
    if (size > request->limits.chunk_bytes) {
      status = dbtool_transfer_fail(error, DBTOOL_STATUS_INTERNAL_ERROR,
                                    "read-input",
                                    "input returned more than chunk capacity");
      goto rollback;
    }
    if (size == 0u)
      break;
    if ((uint64_t)size > request->limits.max_input_bytes - input_bytes) {
      status = dbtool_transfer_fail(error, DBTOOL_STATUS_LIMIT_EXCEEDED,
                                    "read-input",
                                    "import exceeds max_input_bytes");
      goto rollback;
    }
    input_bytes += (uint64_t)size;
    status = decoder.ops->feed(decoder.context, chunk, size, 0, error);
    status = dbtool_transfer_preserve_callback_error(
        status, "decode-input", "model decoder feed failed", error);
    if (status != DBTOOL_STATUS_OK)
      goto rollback;
  }
  status = decoder.ops->feed(decoder.context, NULL, 0u, 1, error);
  status = dbtool_transfer_preserve_callback_error(
      status, "decode-input", "model decoder finish failed", error);
  if (status != DBTOOL_STATUS_OK)
    goto rollback;
  status = request->sink->commit(state.sink_context, error);
  status = dbtool_transfer_preserve_callback_error(
      status, "commit-import", "record sink commit failed", error);
  if (status != DBTOOL_STATUS_OK)
    goto rollback;
  transaction_active = 0;
  result->rows = state.rows;
  result->input_bytes = input_bytes;
  goto cleanup;

rollback:
  if (transaction_active) {
    dbtool_error primary_error = error != NULL ? *error
                                               : (dbtool_error)DBTOOL_ERROR_INIT;
    dbtool_error rollback_error = DBTOOL_ERROR_INIT;
    (void)request->sink->rollback(state.sink_context, &rollback_error);
    if (error != NULL)
      *error = primary_error;
    transaction_active = 0;
  }

cleanup:
  if (decoder_open)
    decoder.ops->close(decoder.context);
  if (state.sink_context != NULL)
    request->sink->close(state.sink_context);
  request->input.ops->close(request->input.context);
  free(chunk);
  return status;
}

static int dbtool_export_request_valid(const dbtool_export_request *request) {
  const dbtool_record_source_ops *source;
  const dbtool_byte_sink_ops *output;
  if (request == NULL || request->struct_size < sizeof(*request) ||
      request->abi_version != DBTOOL_TRANSFER_ABI_VERSION ||
      !dbtool_model_valid(request->model, request->table_index) ||
      !dbtool_format_valid(request->format) ||
      !dbtool_limits_valid(&request->limits))
    return 0;
  source = request->source;
  output = request->output.ops;
  return source != NULL && source->struct_size >= sizeof(*source) &&
         source->abi_version == DBTOOL_RECORD_ABI_VERSION &&
         source->open != NULL && source->next != NULL &&
         source->close != NULL && request->output.context != NULL &&
         output != NULL && output->struct_size >= sizeof(*output) &&
         output->abi_version == DBTOOL_RECORD_ABI_VERSION &&
         output->write != NULL && output->close != NULL;
}

static dbtool_status dbtool_output_write(void *context,
                                         const unsigned char *data,
                                         size_t size, dbtool_error *error) {
  dbtool_output_state *state = (dbtool_output_state *)context;
  dbtool_status status;
  if (size != 0u && data == NULL)
    return dbtool_transfer_fail(error, DBTOOL_STATUS_INTERNAL_ERROR,
                                "write-output", "output byte view is invalid");
  if ((uint64_t)size > state->request->limits.max_output_bytes - state->bytes)
    return dbtool_transfer_fail(error, DBTOOL_STATUS_LIMIT_EXCEEDED,
                                "write-output",
                                "export exceeds max_output_bytes");
  status = state->request->output.ops->write(
      state->request->output.context, data, size, error);
  status = dbtool_transfer_preserve_callback_error(
      status, "write-output", "output write failed", error);
  if (status == DBTOOL_STATUS_OK)
    state->bytes += (uint64_t)size;
  return status;
}

dbtool_status dbtool_transfer_export(const dbtool_export_request *request,
                                     dbtool_transfer_result *result,
                                     dbtool_error *error) {
  const dbtool_table_v1 *table;
  dbtool_output_state output_state;
  dbtool_encoder encoder = {0};
  void *source_context = NULL;
  uint64_t rows = 0u;
  dbtool_status status;
  int encoder_open = 0;
  if (result != NULL)
    *result = (dbtool_transfer_result)DBTOOL_TRANSFER_RESULT_INIT;
  dbtool_error_init(error);
  if (result == NULL || !dbtool_export_request_valid(request))
    return dbtool_transfer_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT,
                                "validate-transfer",
                                "invalid export transfer request");
  table = &request->model->tables[request->table_index];
  output_state.request = request;
  output_state.bytes = 0u;

  status = request->source->open(request->source_factory_context,
                                 &source_context, request->model,
                                 request->table_index, error);
  status = dbtool_transfer_preserve_callback_error(
      status, "open-source", "record source open failed", error);
  if (status != DBTOOL_STATUS_OK)
    goto cleanup;
  if (source_context == NULL) {
    status = dbtool_transfer_fail(error, DBTOOL_STATUS_INTERNAL_ERROR,
                                  "open-source",
                                  "record source returned no context");
    goto cleanup;
  }
  status = request->model->open_encoder(
      request->model->context, request->table_index, request->format,
      &request->limits, dbtool_output_write, &output_state, &encoder, error);
  status = dbtool_transfer_preserve_callback_error(
      status, "open-encoder", "model encoder open failed", error);
  if (status != DBTOOL_STATUS_OK)
    goto cleanup;
  if (!dbtool_encoder_valid(&encoder)) {
    status = dbtool_transfer_fail(error, DBTOOL_STATUS_INTERNAL_ERROR,
                                  "open-encoder",
                                  "model returned an invalid encoder");
    goto cleanup;
  }
  encoder_open = 1;

  for (;;) {
    dbtool_record_view record = {0};
    const dbtool_record_step step =
        request->source->next(source_context, &record, error);
    if (step == DBTOOL_RECORD_DONE)
      break;
    if (step == DBTOOL_RECORD_ERROR) {
      status = error != NULL && error->status != DBTOOL_STATUS_OK
                   ? error->status
                   : DBTOOL_STATUS_INTERNAL_ERROR;
      status = dbtool_transfer_preserve_callback_error(
          status, "read-record", "record source failed", error);
      goto cleanup;
    }
    if (step != DBTOOL_RECORD_ROW) {
      status = dbtool_transfer_fail(error, DBTOOL_STATUS_INTERNAL_ERROR,
                                    "read-record",
                                    "record source returned an invalid step");
      goto cleanup;
    }
    if (rows >= request->limits.max_rows) {
      status = dbtool_transfer_fail(error, DBTOOL_STATUS_LIMIT_EXCEEDED,
                                    "read-record", "export exceeds max_rows");
      goto cleanup;
    }
    status = dbtool_record_validate(&record, table, &request->limits, error);
    if (status != DBTOOL_STATUS_OK)
      goto cleanup;
    status = encoder.ops->write(encoder.context, &record, error);
    status = dbtool_transfer_preserve_callback_error(
        status, "encode-record", "model encoder write failed", error);
    if (status != DBTOOL_STATUS_OK)
      goto cleanup;
    ++rows;
  }
  status = encoder.ops->finish(encoder.context, error);
  status = dbtool_transfer_preserve_callback_error(
      status, "finish-output", "model encoder finish failed", error);
  if (status == DBTOOL_STATUS_OK) {
    result->rows = rows;
    result->output_bytes = output_state.bytes;
  }

cleanup:
  if (encoder_open)
    encoder.ops->close(encoder.context);
  if (source_context != NULL)
    request->source->close(source_context);
  request->output.ops->close(request->output.context);
  return status;
}

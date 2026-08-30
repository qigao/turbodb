#include <turbodb/dbtool_format.h>

#include "data_bind.h"
#include "turbo_parser_json.h"
#include "turbo_uuid.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct dbtool_json_decoder_s {
  DataBind *codec;
  data_bind_stream_t *stream;
  DataBindError bind_error;
  const dbtool_table_v1 *table;
  dbtool_transfer_limits limits;
  dbtool_record_emit_fn emit;
  void *emit_context;
  dbtool_cell *cells;
  unsigned char *uuid_storage;
  dbtool_error *active_error;
  dbtool_status callback_status;
  uint64_t rows;
  int final_seen;
  int failed;
} dbtool_json_decoder_t;

typedef struct dbtool_json_encoder_s {
  DataBind *codec;
  DataBindError bind_error;
  const dbtool_table_v1 *table;
  dbtool_transfer_limits limits;
  dbtool_write_bytes_fn writer;
  void *writer_context;
  uint64_t rows;
  uint64_t output_bytes;
  int finished;
  int failed;
} dbtool_json_encoder_t;

static dbtool_status dbtool_format_fail(dbtool_error *error,
                                        dbtool_status status,
                                        const char *stage,
                                        int native_code,
                                        const char *message) {
  dbtool_error_set(error, status, stage, native_code, message);
  return status;
}

static dbtool_status dbtool_format_data_bind_status(DataBindStatus status,
                                                     const char *stage,
                                                     const DataBindError *source,
                                                     dbtool_error *error) {
  dbtool_status mapped;
  if (status == DATA_BIND_OK) return DBTOOL_STATUS_OK;
  switch (status) {
    case DATA_BIND_ERR_OOM:
      mapped = DBTOOL_STATUS_OUT_OF_MEMORY;
      break;
    case DATA_BIND_ERR_LIMIT:
    case DATA_BIND_ERR_BUFFER_TOO_SMALL:
      mapped = DBTOOL_STATUS_LIMIT_EXCEEDED;
      break;
    case DATA_BIND_ERR_IO:
      mapped = DBTOOL_STATUS_FILE_ERROR;
      break;
    case DATA_BIND_ERR_INVALID_ARG:
    case DATA_BIND_ERR_PARSE:
    case DATA_BIND_ERR_SCHEMA:
    case DATA_BIND_ERR_TYPE_NOT_FOUND:
    case DATA_BIND_ERR_TYPE_MISMATCH:
      mapped = DBTOOL_STATUS_INVALID_ARGUMENT;
      break;
    default:
      mapped = DBTOOL_STATUS_INTERNAL_ERROR;
      break;
  }
  return dbtool_format_fail(
      error, mapped, stage, (int)status,
      source != NULL && source->message[0] != '\0'
          ? source->message
          : "DataBind format operation failed");
}

static int dbtool_format_limits_valid(const dbtool_transfer_limits *limits) {
  return limits != NULL && limits->struct_size >= sizeof(*limits) &&
         limits->abi_version == DBTOOL_MODEL_ABI_VERSION &&
         limits->max_input_bytes != 0u && limits->max_rows != 0u &&
         limits->max_columns != 0u && limits->max_cell_bytes != 0u &&
         limits->max_record_bytes != 0u && limits->max_output_bytes != 0u;
}

static int dbtool_format_scalar_valid(dbtool_scalar_kind kind) {
  return kind >= DBTOOL_SCALAR_INT64 && kind <= DBTOOL_SCALAR_UUID;
}

static int dbtool_format_storage_matches(const dbtool_column_v1 *column) {
  switch (column->scalar_kind) {
    case DBTOOL_SCALAR_INT64:
      return column->storage_kind >= DBTOOL_STORAGE_INTEGER16 &&
             column->storage_kind <= DBTOOL_STORAGE_INTEGER64;
    case DBTOOL_SCALAR_UINT64:
      return (column->storage_kind >= DBTOOL_STORAGE_INTEGER16 &&
              column->storage_kind <= DBTOOL_STORAGE_INTEGER64) ||
             column->storage_kind == DBTOOL_STORAGE_UINT64_DECIMAL;
    case DBTOOL_SCALAR_DOUBLE:
      return column->storage_kind == DBTOOL_STORAGE_FLOAT32 ||
             column->storage_kind == DBTOOL_STORAGE_FLOAT64;
    case DBTOOL_SCALAR_BOOLEAN:
      return column->storage_kind == DBTOOL_STORAGE_BOOLEAN;
    case DBTOOL_SCALAR_TEXT:
      return column->storage_kind == DBTOOL_STORAGE_TEXT;
    case DBTOOL_SCALAR_BYTES:
      return column->storage_kind == DBTOOL_STORAGE_BYTES;
    case DBTOOL_SCALAR_UUID:
      return column->storage_kind == DBTOOL_STORAGE_UUID;
    default:
      return 0;
  }
}

static int dbtool_format_columns_valid(const dbtool_table_v1 *table) {
  static const uint32_t known_flags =
      DBTOOL_COLUMN_OPTIONAL | DBTOOL_COLUMN_HAS_DEFAULT |
      DBTOOL_COLUMN_GENERATED;
  size_t index;
  for (index = 0u; index < table->column_count; ++index) {
    const dbtool_column_v1 *column = &table->columns[index];
    if (column->struct_size < sizeof(*column) ||
        column->abi_version != DBTOOL_MODEL_ABI_VERSION ||
        column->index != index || column->name == NULL ||
        column->name[0] == '\0' || column->database_name == NULL ||
        column->database_name[0] == '\0' ||
        !dbtool_format_scalar_valid(column->scalar_kind) ||
        !dbtool_format_storage_matches(column) ||
        (column->flags & ~known_flags) != 0u ||
        ((column->flags & DBTOOL_COLUMN_GENERATED) != 0u &&
         (column->flags &
          (DBTOOL_COLUMN_OPTIONAL | DBTOOL_COLUMN_HAS_DEFAULT)) != 0u))
      return 0;
  }
  return 1;
}

static const dbtool_table_v1 *dbtool_format_validate_model(
    const void *opaque_context, size_t table_index,
    const dbtool_transfer_limits *limits, dbtool_error *error) {
  const dbtool_format_model_v1 *context =
      (const dbtool_format_model_v1 *)opaque_context;
  const dbtool_table_v1 *table;
  if (context == NULL || context->struct_size < sizeof(*context) ||
      context->abi_version != DBTOOL_FORMAT_MODEL_ABI_VERSION ||
      (context->dialect != DBTOOL_DIALECT_SQLITE &&
       context->dialect != DBTOOL_DIALECT_POSTGRESQL) ||
      context->schema_text == NULL || context->schema_size == 0u ||
      context->tables == NULL || context->table_count == 0u ||
      table_index >= context->table_count || !dbtool_format_limits_valid(limits)) {
    dbtool_format_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT, "open-format", 0,
                       "invalid generated format model or limits");
    return NULL;
  }
  table = &context->tables[table_index];
  if (table->struct_size < sizeof(*table) ||
      table->abi_version != DBTOOL_MODEL_ABI_VERSION ||
      table->index != table_index || table->name == NULL ||
      table->database_name == NULL || table->columns == NULL ||
      table->column_count == 0u || table->column_count > limits->max_columns ||
      table->column_count > SIZE_MAX / sizeof(dbtool_cell) ||
      table->column_count > SIZE_MAX / TURBO_UUID_SIZE ||
      !dbtool_format_columns_valid(table)) {
    dbtool_format_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT, "open-format", 0,
                       "generated table metadata is invalid or exceeds limits");
    return NULL;
  }
  return table;
}

static int dbtool_format_column_allows_null(const dbtool_column_v1 *column) {
  return (column->flags & DBTOOL_COLUMN_OPTIONAL) != 0u;
}

static dbtool_status dbtool_json_cell_from_value(
    dbtool_json_decoder_t *decoder, size_t index, const DataBindValue *value,
    size_t *record_bytes) {
  const dbtool_column_v1 *column = &decoder->table->columns[index];
  dbtool_cell *cell = &decoder->cells[index];
  DataBindStatus status = DATA_BIND_OK;
  const char *text = NULL;
  const uint8_t *bytes = NULL;
  size_t size = 0u;
  if (value == NULL) {
    if ((column->flags & (DBTOOL_COLUMN_OPTIONAL |
                          DBTOOL_COLUMN_HAS_DEFAULT |
                          DBTOOL_COLUMN_GENERATED)) == 0u)
      return dbtool_format_fail(decoder->active_error,
                                DBTOOL_STATUS_INVALID_ARGUMENT, "decode-json",
                                0, "required database field is missing");
    cell->kind = DBTOOL_VALUE_ABSENT;
    return DBTOOL_STATUS_OK;
  }
  if (data_bind_value_kind(value) == DATA_BIND_VALUE_NULL) {
    if (!dbtool_format_column_allows_null(column))
      return dbtool_format_fail(decoder->active_error,
                                DBTOOL_STATUS_INVALID_ARGUMENT, "decode-json",
                                0, "non-optional database field is null");
    cell->kind = DBTOOL_VALUE_NULL;
    return DBTOOL_STATUS_OK;
  }
  switch (column->scalar_kind) {
    case DBTOOL_SCALAR_INT64:
      status = data_bind_value_get_int64(value, &cell->data.int64_value);
      cell->kind = DBTOOL_VALUE_INT64;
      break;
    case DBTOOL_SCALAR_UINT64:
      status = data_bind_value_get_uint64(value, &cell->data.uint64_value);
      cell->kind = DBTOOL_VALUE_UINT64;
      break;
    case DBTOOL_SCALAR_DOUBLE:
      status = data_bind_value_get_double(value, &cell->data.double_value);
      cell->kind = DBTOOL_VALUE_DOUBLE;
      break;
    case DBTOOL_SCALAR_BOOLEAN:
      status = data_bind_value_get_bool(value, &cell->data.boolean_value);
      cell->kind = DBTOOL_VALUE_BOOLEAN;
      break;
    case DBTOOL_SCALAR_TEXT:
      status = data_bind_value_get_string(value, &text, &size);
      cell->kind = DBTOOL_VALUE_TEXT;
      cell->data.bytes.data = (const unsigned char *)text;
      cell->data.bytes.size = size;
      break;
    case DBTOOL_SCALAR_BYTES:
      status = data_bind_value_get_bytes(value, &bytes, &size);
      cell->kind = DBTOOL_VALUE_BYTES;
      cell->data.bytes.data = bytes;
      cell->data.bytes.size = size;
      break;
    case DBTOOL_SCALAR_UUID:
      status = data_bind_value_get_uuid(
          value, decoder->uuid_storage + index * TURBO_UUID_SIZE);
      cell->kind = DBTOOL_VALUE_UUID;
      cell->data.bytes.data = decoder->uuid_storage + index * TURBO_UUID_SIZE;
      cell->data.bytes.size = TURBO_UUID_SIZE;
      size = TURBO_UUID_SIZE;
      break;
    default:
      return dbtool_format_fail(decoder->active_error,
                                DBTOOL_STATUS_INTERNAL_ERROR, "decode-json",
                                0, "generated scalar kind is invalid");
  }
  if (status != DATA_BIND_OK)
    return dbtool_format_data_bind_status(status, "decode-json", NULL,
                                          decoder->active_error);
  if (size > decoder->limits.max_cell_bytes ||
      size > decoder->limits.max_record_bytes - *record_bytes)
    return dbtool_format_fail(decoder->active_error,
                              DBTOOL_STATUS_LIMIT_EXCEEDED, "decode-json", 0,
                              "decoded record exceeds byte limits");
  *record_bytes += size;
  return DBTOOL_STATUS_OK;
}

static DataBindRecordAction dbtool_json_record_callback(void *opaque_decoder,
                                                        const DataBindValue *value,
                                                        uint64_t record_index) {
  dbtool_json_decoder_t *decoder = (dbtool_json_decoder_t *)opaque_decoder;
  dbtool_record_view record;
  size_t record_bytes = decoder->table->column_count * sizeof(dbtool_cell);
  size_t index;
  (void)record_index;
  if (decoder->rows >= decoder->limits.max_rows) {
    decoder->callback_status = dbtool_format_fail(
        decoder->active_error, DBTOOL_STATUS_LIMIT_EXCEEDED, "decode-json", 0,
        "decoded JSON exceeds max_rows");
    return DATA_BIND_RECORD_ERROR;
  }
  if (record_bytes > decoder->limits.max_record_bytes) {
    decoder->callback_status = dbtool_format_fail(
        decoder->active_error, DBTOOL_STATUS_LIMIT_EXCEEDED, "decode-json", 0,
        "decoded record metadata exceeds max_record_bytes");
    return DATA_BIND_RECORD_ERROR;
  }
  memset(decoder->cells, 0,
         decoder->table->column_count * sizeof(*decoder->cells));
  for (index = 0u; index < decoder->table->column_count; ++index) {
    decoder->callback_status = dbtool_json_cell_from_value(
        decoder, index,
        data_bind_value_get(value, decoder->table->columns[index].name),
        &record_bytes);
    if (decoder->callback_status != DBTOOL_STATUS_OK)
      return DATA_BIND_RECORD_ERROR;
  }
  record.cells = decoder->cells;
  record.cell_count = decoder->table->column_count;
  decoder->callback_status =
      decoder->emit(decoder->emit_context, &record, decoder->active_error);
  if (decoder->callback_status != DBTOOL_STATUS_OK) {
    if (decoder->active_error != NULL &&
        decoder->active_error->status == DBTOOL_STATUS_OK)
      dbtool_format_fail(decoder->active_error, decoder->callback_status,
                         "decode-json", 0, "record callback failed");
    return DATA_BIND_RECORD_ERROR;
  }
  ++decoder->rows;
  return DATA_BIND_RECORD_CONTINUE;
}

static dbtool_status dbtool_json_decoder_feed(void *opaque_decoder,
                                              const unsigned char *data,
                                              size_t size, int final,
                                              dbtool_error *error) {
  dbtool_json_decoder_t *decoder = (dbtool_json_decoder_t *)opaque_decoder;
  DataBindStatus status;
  if (decoder == NULL || (size != 0u && data == NULL) ||
      (final != 0 && final != 1) || decoder->final_seen ||
      decoder->failed || (final == 0 && size == 0u))
    return dbtool_format_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT,
                              "decode-json", 0,
                              "invalid JSON decoder feed state");
  decoder->active_error = error;
  decoder->callback_status = DBTOOL_STATUS_OK;
  if (size != 0u) {
    status = data_bind_stream_feed(decoder->stream, data, size);
    if (decoder->callback_status != DBTOOL_STATUS_OK) {
      decoder->failed = 1;
      return decoder->callback_status;
    }
    if (status != DATA_BIND_OK) {
      decoder->failed = 1;
      return dbtool_format_data_bind_status(status, "decode-json",
                                            &decoder->bind_error, error);
    }
  }
  if (final) {
    decoder->final_seen = 1;
    status = data_bind_stream_finish(decoder->stream);
    if (decoder->callback_status != DBTOOL_STATUS_OK) {
      decoder->failed = 1;
      return decoder->callback_status;
    }
    if (status != DATA_BIND_OK) {
      decoder->failed = 1;
      return dbtool_format_data_bind_status(status, "decode-json",
                                            &decoder->bind_error, error);
    }
  }
  return DBTOOL_STATUS_OK;
}

static void dbtool_json_decoder_close(void *opaque_decoder) {
  dbtool_json_decoder_t *decoder = (dbtool_json_decoder_t *)opaque_decoder;
  if (decoder == NULL) return;
  data_bind_stream_destroy(decoder->stream);
  data_bind_free(decoder->codec);
  free(decoder->uuid_storage);
  free(decoder->cells);
  free(decoder);
}

static const dbtool_decoder_ops DBTOOL_JSON_DECODER_OPS = {
    sizeof(dbtool_decoder_ops), DBTOOL_MODEL_ABI_VERSION,
    dbtool_json_decoder_feed, dbtool_json_decoder_close};

static dbtool_status dbtool_json_encoder_output(dbtool_json_encoder_t *encoder,
                                                const unsigned char *data,
                                                size_t size,
                                                dbtool_error *error) {
  dbtool_status status;
  if ((uint64_t)size > encoder->limits.max_output_bytes - encoder->output_bytes) {
    encoder->failed = 1;
    return dbtool_format_fail(error, DBTOOL_STATUS_LIMIT_EXCEEDED,
                              "encode-json", 0,
                              "encoded JSON exceeds max_output_bytes");
  }
  status = encoder->writer(encoder->writer_context, data, size, error);
  if (status != DBTOOL_STATUS_OK) {
    encoder->failed = 1;
    if (error != NULL && error->status == DBTOOL_STATUS_OK)
      dbtool_format_fail(error, status, "encode-json", 0,
                         "output writer failed");
    return status;
  }
  encoder->output_bytes += size;
  return DBTOOL_STATUS_OK;
}

static json_value_t *dbtool_json_value_from_cell(
    const dbtool_column_v1 *column, const dbtool_cell *cell,
    dbtool_error *error) {
  turbo_uuid_t uuid;
  char uuid_text[TURBO_UUID_STRING_SIZE];
  const char *text;
  if (cell->kind == DBTOOL_VALUE_NULL) {
    if (!dbtool_format_column_allows_null(column)) {
      dbtool_format_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT, "encode-json",
                         0, "non-optional database field is null");
      return NULL;
    }
    return turbo_json_create_null();
  }
  switch (column->scalar_kind) {
    case DBTOOL_SCALAR_INT64:
      if (cell->kind == DBTOOL_VALUE_INT64)
        return turbo_json_create_int64(cell->data.int64_value);
      break;
    case DBTOOL_SCALAR_UINT64:
      if (cell->kind == DBTOOL_VALUE_UINT64)
        return turbo_json_create_uint64(cell->data.uint64_value);
      break;
    case DBTOOL_SCALAR_DOUBLE:
      if (cell->kind == DBTOOL_VALUE_DOUBLE &&
          isfinite(cell->data.double_value))
        return turbo_json_create_number(cell->data.double_value);
      break;
    case DBTOOL_SCALAR_BOOLEAN:
      if (cell->kind == DBTOOL_VALUE_BOOLEAN &&
          (cell->data.boolean_value == 0 || cell->data.boolean_value == 1))
        return turbo_json_create_bool(cell->data.boolean_value != 0);
      break;
    case DBTOOL_SCALAR_TEXT:
    case DBTOOL_SCALAR_BYTES:
      if ((column->scalar_kind == DBTOOL_SCALAR_TEXT &&
           cell->kind == DBTOOL_VALUE_TEXT) ||
          (column->scalar_kind == DBTOOL_SCALAR_BYTES &&
           cell->kind == DBTOOL_VALUE_BYTES)) {
        if (cell->data.bytes.size != 0u && cell->data.bytes.data == NULL)
          break;
        text = cell->data.bytes.size == 0u
                   ? ""
                   : (const char *)cell->data.bytes.data;
        return turbo_json_create_string_n(text, cell->data.bytes.size);
      }
      break;
    case DBTOOL_SCALAR_UUID:
      if (cell->kind == DBTOOL_VALUE_UUID &&
          cell->data.bytes.size == TURBO_UUID_SIZE &&
          cell->data.bytes.data != NULL) {
        memcpy(uuid.bytes, cell->data.bytes.data, TURBO_UUID_SIZE);
        if (turbo_uuid_format(&uuid, uuid_text, sizeof(uuid_text)) == TURBO_OK)
          return turbo_json_create_string(uuid_text);
      }
      break;
    default:
      break;
  }
  dbtool_format_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT, "encode-json", 0,
                     "record cell does not match generated scalar kind");
  return NULL;
}

static dbtool_status dbtool_json_encoder_write(void *opaque_encoder,
                                               const dbtool_record_view *record,
                                               dbtool_error *error) {
  static const unsigned char array_start[] = "[";
  static const unsigned char separator[] = ",";
  dbtool_json_encoder_t *encoder = (dbtool_json_encoder_t *)opaque_encoder;
  turbo_json_doc_t *json = NULL;
  DataBindObject *object = NULL;
  char *serialized = NULL;
  size_t serialized_size = 0u;
  size_t index;
  dbtool_status result = DBTOOL_STATUS_OK;
  DataBindStatus bind_status;
  if (encoder == NULL || record == NULL || record->cells == NULL ||
      record->cell_count != encoder->table->column_count || encoder->finished ||
      encoder->failed)
    return dbtool_format_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT,
                              "encode-json", 0,
                              "invalid JSON encoder record or state");
  if (encoder->rows >= encoder->limits.max_rows)
    return dbtool_format_fail(error, DBTOOL_STATUS_LIMIT_EXCEEDED,
                              "encode-json", 0,
                              "encoded JSON exceeds max_rows");
  json = turbo_json_create_object();
  if (json == NULL)
    return dbtool_format_fail(error, DBTOOL_STATUS_OUT_OF_MEMORY,
                              "encode-json", 0,
                              "allocate JSON record object");
  for (index = 0u; index < record->cell_count; ++index) {
    const dbtool_column_v1 *column = &encoder->table->columns[index];
    const dbtool_cell *cell = &record->cells[index];
    json_value_t *value;
    if (cell->kind == DBTOOL_VALUE_ABSENT) {
      if ((column->flags &
           (DBTOOL_COLUMN_HAS_DEFAULT | DBTOOL_COLUMN_GENERATED)) == 0u) {
        result = dbtool_format_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT,
                                    "encode-json", 0,
                                    "record omits a database-owned value");
        goto cleanup;
      }
      continue;
    }
    value = dbtool_json_value_from_cell(column, cell, error);
    if (value == NULL) {
      result = error != NULL ? error->status : DBTOOL_STATUS_INVALID_ARGUMENT;
      goto cleanup;
    }
    if (!turbo_json_object_add_checked(json, column->name, value)) {
      turbo_free_json((turbo_json_doc_t **)&value);
      result = dbtool_format_fail(error, DBTOOL_STATUS_OUT_OF_MEMORY,
                                  "encode-json", 0,
                                  "add JSON record field");
      goto cleanup;
    }
  }
  bind_status = data_bind_object_from_json_value(
      encoder->codec, encoder->table->name, json, &object, &encoder->bind_error);
  if (bind_status != DATA_BIND_OK) {
    result = dbtool_format_data_bind_status(bind_status, "encode-json",
                                            &encoder->bind_error, error);
    goto cleanup;
  }
  bind_status = data_bind_object_serialize_json(
      encoder->codec, object, &serialized, &serialized_size,
      &encoder->bind_error);
  if (bind_status != DATA_BIND_OK) {
    result = dbtool_format_data_bind_status(bind_status, "encode-json",
                                            &encoder->bind_error, error);
    goto cleanup;
  }
  if (serialized_size > encoder->limits.max_record_bytes) {
    result = dbtool_format_fail(error, DBTOOL_STATUS_LIMIT_EXCEEDED,
                                "encode-json", 0,
                                "encoded JSON record exceeds max_record_bytes");
    goto cleanup;
  }
  result = dbtool_json_encoder_output(
      encoder, encoder->rows == 0u ? array_start : separator, 1u, error);
  if (result == DBTOOL_STATUS_OK)
    result = dbtool_json_encoder_output(
        encoder, (const unsigned char *)serialized, serialized_size, error);
  if (result == DBTOOL_STATUS_OK) ++encoder->rows;

cleanup:
  if (result != DBTOOL_STATUS_OK) encoder->failed = 1;
  data_bind_serialized_free(serialized);
  data_bind_object_free(object);
  turbo_free_json(&json);
  return result;
}

static dbtool_status dbtool_json_encoder_finish(void *opaque_encoder,
                                                dbtool_error *error) {
  static const unsigned char empty_array[] = "[]";
  static const unsigned char array_end[] = "]";
  dbtool_json_encoder_t *encoder = (dbtool_json_encoder_t *)opaque_encoder;
  dbtool_status status;
  if (encoder == NULL || encoder->finished || encoder->failed)
    return dbtool_format_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT,
                              "encode-json", 0,
                              "invalid JSON encoder finish state");
  status = dbtool_json_encoder_output(
      encoder, encoder->rows == 0u ? empty_array : array_end,
      encoder->rows == 0u ? 2u : 1u, error);
  if (status == DBTOOL_STATUS_OK) encoder->finished = 1;
  return status;
}

static void dbtool_json_encoder_close(void *opaque_encoder) {
  dbtool_json_encoder_t *encoder = (dbtool_json_encoder_t *)opaque_encoder;
  if (encoder == NULL) return;
  data_bind_free(encoder->codec);
  free(encoder);
}

static const dbtool_encoder_ops DBTOOL_JSON_ENCODER_OPS = {
    sizeof(dbtool_encoder_ops), DBTOOL_MODEL_ABI_VERSION,
    dbtool_json_encoder_write, dbtool_json_encoder_finish,
    dbtool_json_encoder_close};

dbtool_status dbtool_format_open_decoder_v1(
    const void *model_context, size_t table_index, dbtool_format format,
    const dbtool_transfer_limits *limits, dbtool_record_emit_fn emit,
    void *emit_context, dbtool_decoder *out, dbtool_error *error) {
  const dbtool_format_model_v1 *model =
      (const dbtool_format_model_v1 *)model_context;
  const dbtool_table_v1 *table;
  dbtool_json_decoder_t *decoder;
  DataBindStreamConfig config = DATA_BIND_STREAM_CONFIG_INIT;
  DataBindStatus status;
  if (out != NULL) *out = (dbtool_decoder){0};
  dbtool_error_init(error);
  table = dbtool_format_validate_model(model_context, table_index, limits, error);
  if (table == NULL || emit == NULL || out == NULL)
    return table == NULL
               ? (error != NULL ? error->status : DBTOOL_STATUS_INVALID_ARGUMENT)
               : dbtool_format_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT,
                                    "open-format", 0,
                                    "decoder callback or output is missing");
  if (format != DBTOOL_FORMAT_JSON)
    return dbtool_format_fail(error, DBTOOL_STATUS_UNSUPPORTED, "open-format",
                              0, "format decoder is not enabled");
  decoder = (dbtool_json_decoder_t *)calloc(1u, sizeof(*decoder));
  if (decoder == NULL)
    return dbtool_format_fail(error, DBTOOL_STATUS_OUT_OF_MEMORY,
                              "open-format", 0,
                              "allocate JSON decoder context");
  decoder->bind_error = (DataBindError)DATA_BIND_ERROR_INIT;
  decoder->table = table;
  decoder->limits = *limits;
  decoder->emit = emit;
  decoder->emit_context = emit_context;
  decoder->cells =
      (dbtool_cell *)calloc(table->column_count, sizeof(*decoder->cells));
  decoder->uuid_storage =
      (unsigned char *)calloc(table->column_count, TURBO_UUID_SIZE);
  if (decoder->cells == NULL || decoder->uuid_storage == NULL) {
    dbtool_json_decoder_close(decoder);
    return dbtool_format_fail(error, DBTOOL_STATUS_OUT_OF_MEMORY,
                              "open-format", 0,
                              "allocate JSON decoder record storage");
  }
  status = data_bind_create_from_text(model->schema_text, model->schema_size,
                                      &decoder->codec, &decoder->bind_error);
  if (status != DATA_BIND_OK) {
    dbtool_status mapped = dbtool_format_data_bind_status(
        status, "open-format", &decoder->bind_error, error);
    dbtool_json_decoder_close(decoder);
    return mapped;
  }
  config.format = DATA_BIND_FORMAT_JSON;
  config.selection = DATA_BIND_STREAM_SELECT_ALL;
  config.type_name = table->name;
  config.output_mode = DATA_BIND_STREAM_OUTPUT_CALLBACK_ONLY;
  config.record_callback = dbtool_json_record_callback;
  config.record_callback_user = decoder;
  config.limits.max_input_bytes =
      limits->max_input_bytes > SIZE_MAX ? SIZE_MAX
                                         : (size_t)limits->max_input_bytes;
  config.limits.max_record_bytes = limits->max_record_bytes;
  config.limits.max_field_bytes = limits->max_cell_bytes;
  config.limits.max_result_count =
      limits->max_rows > SIZE_MAX ? SIZE_MAX : (size_t)limits->max_rows;
  status = data_bind_stream_create(decoder->codec, &config, &decoder->stream,
                                   &decoder->bind_error);
  if (status != DATA_BIND_OK) {
    dbtool_status mapped = dbtool_format_data_bind_status(
        status, "open-format", &decoder->bind_error, error);
    dbtool_json_decoder_close(decoder);
    return mapped;
  }
  out->ops = &DBTOOL_JSON_DECODER_OPS;
  out->context = decoder;
  return DBTOOL_STATUS_OK;
}

dbtool_status dbtool_format_open_encoder_v1(
    const void *model_context, size_t table_index, dbtool_format format,
    const dbtool_transfer_limits *limits, dbtool_write_bytes_fn writer,
    void *writer_context, dbtool_encoder *out, dbtool_error *error) {
  const dbtool_format_model_v1 *model =
      (const dbtool_format_model_v1 *)model_context;
  const dbtool_table_v1 *table;
  dbtool_json_encoder_t *encoder;
  DataBindStatus status;
  if (out != NULL) *out = (dbtool_encoder){0};
  dbtool_error_init(error);
  table = dbtool_format_validate_model(model_context, table_index, limits, error);
  if (table == NULL || writer == NULL || out == NULL)
    return table == NULL
               ? (error != NULL ? error->status : DBTOOL_STATUS_INVALID_ARGUMENT)
               : dbtool_format_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT,
                                    "open-format", 0,
                                    "encoder writer or output is missing");
  if (format != DBTOOL_FORMAT_JSON)
    return dbtool_format_fail(error, DBTOOL_STATUS_UNSUPPORTED, "open-format",
                              0, "format encoder is not enabled");
  encoder = (dbtool_json_encoder_t *)calloc(1u, sizeof(*encoder));
  if (encoder == NULL)
    return dbtool_format_fail(error, DBTOOL_STATUS_OUT_OF_MEMORY,
                              "open-format", 0,
                              "allocate JSON encoder context");
  encoder->bind_error = (DataBindError)DATA_BIND_ERROR_INIT;
  encoder->table = table;
  encoder->limits = *limits;
  encoder->writer = writer;
  encoder->writer_context = writer_context;
  status = data_bind_create_from_text(model->schema_text, model->schema_size,
                                      &encoder->codec, &encoder->bind_error);
  if (status != DATA_BIND_OK) {
    dbtool_status mapped = dbtool_format_data_bind_status(
        status, "open-format", &encoder->bind_error, error);
    dbtool_json_encoder_close(encoder);
    return mapped;
  }
  out->ops = &DBTOOL_JSON_ENCODER_OPS;
  out->context = encoder;
  return DBTOOL_STATUS_OK;
}

#ifndef TURBODB_DBTOOL_FORMAT_H
#define TURBODB_DBTOOL_FORMAT_H

#include <turbodb/dbtool_model.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DBTOOL_FORMAT_MODEL_ABI_VERSION 1u

typedef enum dbtool_database_dialect {
  DBTOOL_DIALECT_SQLITE = 1,
  DBTOOL_DIALECT_POSTGRESQL
} dbtool_database_dialect;

/**
 * Immutable context emitted by tbe_compiler for the generic DataBind adapter.
 * All pointers have static storage duration in the generated translation unit.
 */
typedef struct dbtool_format_model_v1 {
  size_t struct_size;
  uint32_t abi_version;
  dbtool_database_dialect dialect;
  const char *schema_text;
  size_t schema_size;
  const dbtool_table_v1 *tables;
  size_t table_count;
} dbtool_format_model_v1;

dbtool_status dbtool_format_open_decoder_v1(
    const void *model_context, size_t table_index, dbtool_format format,
    const dbtool_transfer_limits *limits, dbtool_record_emit_fn emit,
    void *emit_context, dbtool_decoder *out, dbtool_error *error);

dbtool_status dbtool_format_open_encoder_v1(
    const void *model_context, size_t table_index, dbtool_format format,
    const dbtool_transfer_limits *limits, dbtool_write_bytes_fn writer,
    void *writer_context, dbtool_encoder *out, dbtool_error *error);

#ifdef __cplusplus
}
#endif

#endif /* TURBODB_DBTOOL_FORMAT_H */

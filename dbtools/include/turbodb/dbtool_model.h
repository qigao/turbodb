#ifndef TURBODB_DBTOOL_MODEL_H
#define TURBODB_DBTOOL_MODEL_H

#include <turbodb/dbtool_error.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DBTOOL_MODEL_ABI_VERSION 1u

typedef enum dbtool_scalar_kind {
  DBTOOL_SCALAR_INT64 = 1,
  DBTOOL_SCALAR_UINT64,
  DBTOOL_SCALAR_DOUBLE,
  DBTOOL_SCALAR_BOOLEAN,
  DBTOOL_SCALAR_TEXT,
  DBTOOL_SCALAR_BYTES,
  DBTOOL_SCALAR_UUID
} dbtool_scalar_kind;

typedef enum dbtool_column_flags {
  DBTOOL_COLUMN_OPTIONAL = 1u << 0u,
  DBTOOL_COLUMN_HAS_DEFAULT = 1u << 1u,
  DBTOOL_COLUMN_GENERATED = 1u << 2u
} dbtool_column_flags;

typedef enum dbtool_value_kind {
  DBTOOL_VALUE_ABSENT = 0,
  DBTOOL_VALUE_NULL,
  DBTOOL_VALUE_INT64,
  DBTOOL_VALUE_UINT64,
  DBTOOL_VALUE_DOUBLE,
  DBTOOL_VALUE_BOOLEAN,
  DBTOOL_VALUE_TEXT,
  DBTOOL_VALUE_BYTES,
  DBTOOL_VALUE_UUID
} dbtool_value_kind;

typedef enum dbtool_format {
  DBTOOL_FORMAT_JSON = 1,
  DBTOOL_FORMAT_CSV,
  DBTOOL_FORMAT_YAML,
  DBTOOL_FORMAT_XML,
  DBTOOL_FORMAT_TBE_BINARY
} dbtool_format;

typedef struct dbtool_bytes_view {
  const unsigned char *data;
  size_t size;
} dbtool_bytes_view;

typedef struct dbtool_cell {
  dbtool_value_kind kind;
  union {
    int64_t int64_value;
    uint64_t uint64_value;
    double double_value;
    int boolean_value;
    dbtool_bytes_view bytes;
  } data;
} dbtool_cell;

/* Cell byte views are borrowed only until the consuming callback returns. */
typedef struct dbtool_record_view {
  const dbtool_cell *cells;
  size_t cell_count;
} dbtool_record_view;

typedef struct dbtool_column_v1 {
  size_t struct_size;
  uint32_t abi_version;
  size_t index;
  uint32_t flags;
  dbtool_scalar_kind scalar_kind;
  const char *name;
  const char *database_name;
} dbtool_column_v1;

typedef struct dbtool_table_v1 {
  size_t struct_size;
  uint32_t abi_version;
  size_t index;
  const char *name;
  const char *database_name;
  const dbtool_column_v1 *columns;
  size_t column_count;
} dbtool_table_v1;

typedef struct dbtool_transfer_limits {
  size_t struct_size;
  uint32_t abi_version;
  size_t chunk_bytes;
  uint64_t max_input_bytes;
  uint64_t max_rows;
  size_t max_columns;
  size_t max_cell_bytes;
  size_t max_record_bytes;
  uint64_t max_output_bytes;
} dbtool_transfer_limits;

#define DBTOOL_TRANSFER_LIMITS_INIT                                           \
  { sizeof(dbtool_transfer_limits), DBTOOL_MODEL_ABI_VERSION, 0u, 0u, 0u,    \
    0u, 0u, 0u, 0u }

typedef dbtool_status (*dbtool_record_emit_fn)(
    void *context, const dbtool_record_view *record, dbtool_error *error);
typedef dbtool_status (*dbtool_write_bytes_fn)(void *context,
                                               const unsigned char *data,
                                               size_t size,
                                               dbtool_error *error);

typedef struct dbtool_decoder_ops {
  size_t struct_size;
  uint32_t abi_version;
  /* data is borrowed only for this call; final is exactly zero or one. */
  dbtool_status (*feed)(void *context, const unsigned char *data, size_t size,
                        int final, dbtool_error *error);
  void (*close)(void *context);
} dbtool_decoder_ops;

typedef struct dbtool_decoder {
  const dbtool_decoder_ops *ops;
  void *context;
} dbtool_decoder;

typedef struct dbtool_encoder_ops {
  size_t struct_size;
  uint32_t abi_version;
  dbtool_status (*write)(void *context, const dbtool_record_view *record,
                         dbtool_error *error);
  dbtool_status (*finish)(void *context, dbtool_error *error);
  void (*close)(void *context);
} dbtool_encoder_ops;

typedef struct dbtool_encoder {
  const dbtool_encoder_ops *ops;
  void *context;
} dbtool_encoder;

typedef struct dbtool_model_v1 {
  size_t struct_size;
  uint32_t abi_version;
  const void *context;
  const dbtool_table_v1 *tables;
  size_t table_count;
  dbtool_status (*open_decoder)(
      const void *model_context, size_t table_index, dbtool_format format,
      const dbtool_transfer_limits *limits, dbtool_record_emit_fn emit,
      void *emit_context, dbtool_decoder *out, dbtool_error *error);
  dbtool_status (*open_encoder)(
      const void *model_context, size_t table_index, dbtool_format format,
      const dbtool_transfer_limits *limits, dbtool_write_bytes_fn writer,
      void *writer_context, dbtool_encoder *out, dbtool_error *error);
} dbtool_model_v1;

#ifdef __cplusplus
}
#endif

#endif

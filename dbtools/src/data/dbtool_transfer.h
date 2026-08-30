#ifndef TURBODB_DBTOOL_TRANSFER_H
#define TURBODB_DBTOOL_TRANSFER_H

#include "data/dbtool_record.h"

#define DBTOOL_TRANSFER_ABI_VERSION 1u

typedef struct dbtool_transfer_result {
  uint64_t rows;
  uint64_t input_bytes;
  uint64_t output_bytes;
} dbtool_transfer_result;

#define DBTOOL_TRANSFER_RESULT_INIT                                           \
  { 0u, 0u, 0u }

typedef struct dbtool_import_request {
  size_t struct_size;
  uint32_t abi_version;
  const dbtool_model_v1 *model;
  size_t table_index;
  dbtool_format format;
  dbtool_transfer_limits limits;
  dbtool_byte_source input;
  const dbtool_record_sink_ops *sink;
  void *sink_factory_context;
} dbtool_import_request;

typedef struct dbtool_export_request {
  size_t struct_size;
  uint32_t abi_version;
  const dbtool_model_v1 *model;
  size_t table_index;
  dbtool_format format;
  dbtool_transfer_limits limits;
  const dbtool_record_source_ops *source;
  void *source_factory_context;
  dbtool_byte_sink output;
} dbtool_export_request;

dbtool_status dbtool_transfer_import(const dbtool_import_request *request,
                                     dbtool_transfer_result *result,
                                     dbtool_error *error);
dbtool_status dbtool_transfer_export(const dbtool_export_request *request,
                                     dbtool_transfer_result *result,
                                     dbtool_error *error);

#endif

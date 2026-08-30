#ifndef TURBODB_DBTOOL_RECORD_H
#define TURBODB_DBTOOL_RECORD_H

#include <turbodb/dbtool_model.h>

#define DBTOOL_RECORD_ABI_VERSION 1u

typedef enum dbtool_record_step {
  DBTOOL_RECORD_ROW = 1,
  DBTOOL_RECORD_DONE,
  DBTOOL_RECORD_ERROR
} dbtool_record_step;

typedef struct dbtool_byte_source_ops {
  size_t struct_size;
  uint32_t abi_version;
  dbtool_status (*read)(void *context, unsigned char *destination,
                        size_t capacity, size_t *out_size,
                        dbtool_error *error);
  void (*close)(void *context);
} dbtool_byte_source_ops;

typedef struct dbtool_byte_source {
  const dbtool_byte_source_ops *ops;
  void *context;
} dbtool_byte_source;

typedef struct dbtool_byte_sink_ops {
  size_t struct_size;
  uint32_t abi_version;
  dbtool_status (*write)(void *context, const unsigned char *data, size_t size,
                         dbtool_error *error);
  void (*close)(void *context);
} dbtool_byte_sink_ops;

typedef struct dbtool_byte_sink {
  const dbtool_byte_sink_ops *ops;
  void *context;
} dbtool_byte_sink;

typedef struct dbtool_record_sink_ops {
  size_t struct_size;
  uint32_t abi_version;
  dbtool_status (*begin)(void *factory_context, void **out_context,
                         const dbtool_model_v1 *model, size_t table_index,
                         dbtool_error *error);
  /*
   * A successful begin returns a non-NULL context in ACTIVE state. Each
   * borrowed record is consumed before write returns. ACTIVE reaches exactly
   * one successful terminal operation: commit or rollback. A failed commit
   * remains ACTIVE and is followed by one rollback attempt. close is then
   * called exactly once and must not commit implicitly.
   */
  dbtool_status (*write)(void *context, const dbtool_record_view *record,
                         dbtool_error *error);
  dbtool_status (*commit)(void *context, dbtool_error *error);
  dbtool_status (*rollback)(void *context, dbtool_error *error);
  void (*close)(void *context);
} dbtool_record_sink_ops;

typedef struct dbtool_record_source_ops {
  size_t struct_size;
  uint32_t abi_version;
  dbtool_status (*open)(void *factory_context, void **out_context,
                        const dbtool_model_v1 *model, size_t table_index,
                        dbtool_error *error);
  /* The returned row and its cells expire on the next next()/close(). */
  dbtool_record_step (*next)(void *context, dbtool_record_view *out,
                             dbtool_error *error);
  void (*close)(void *context);
} dbtool_record_source_ops;

#endif

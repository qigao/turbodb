#ifndef TURBODB_DBTOOL_FILE_H
#define TURBODB_DBTOOL_FILE_H

#include "dbtool_error.h"

#include <stddef.h>

#include "salts_fs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dbtool_file {
  char *data;
  size_t size;
} dbtool_file;

#define DBTOOL_FILE_INIT                                                      \
  { NULL, 0u }

typedef struct dbtool_file_ops {
  int (*stat)(void *context, const char *path, salts_fs_stat_t *out);
  salts_file_t (*open)(void *context, const char *path, int flags, int mode);
  int (*read)(void *context, salts_file_t file, char *data, size_t size);
  int (*close)(void *context, salts_file_t file);
} dbtool_file_ops;

dbtool_status dbtool_file_read(const char *path, size_t max_bytes,
                               dbtool_file *out, dbtool_error *error);
dbtool_status dbtool_file_read_with_ops(const char *path, size_t max_bytes,
                                        dbtool_file *out,
                                        const dbtool_file_ops *ops,
                                        void *ops_context,
                                        dbtool_error *error);
void dbtool_file_release(dbtool_file *file);

#ifdef __cplusplus
}
#endif

#endif

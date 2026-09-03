#include "dbtool_file.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

static int dbtool_native_stat(void *context, const char *path,
                              salts_fs_stat_t *out) {
  (void)context;
  return salts_fs_stat(path, out);
}

static salts_file_t dbtool_native_open(void *context, const char *path,
                                       int flags, int mode) {
  (void)context;
  return salts_fs_open(path, flags, mode);
}

static int dbtool_native_read(void *context, salts_file_t file, char *data,
                              size_t size) {
  (void)context;
  return salts_fs_read(file, data, size);
}

static int dbtool_native_close(void *context, salts_file_t file) {
  (void)context;
  return salts_fs_close(file);
}

static const dbtool_file_ops dbtool_native_file_ops = {
    dbtool_native_stat, dbtool_native_open, dbtool_native_read,
    dbtool_native_close};

static void dbtool_file_fail(dbtool_error *error, dbtool_status status,
                             const char *stage, int native_code,
                             const char *message) {
  dbtool_error_set(error, status, stage, native_code, message);
}

dbtool_status dbtool_file_read_with_ops(const char *path, size_t max_bytes,
                                        dbtool_file *out,
                                        const dbtool_file_ops *ops,
                                        void *ops_context,
                                        dbtool_error *error) {
  salts_fs_stat_t info = {0};
  salts_file_t file = SALTS_INVALID_FILE;
  char *data = NULL;
  size_t expected;
  size_t offset = 0u;
  dbtool_status status = DBTOOL_STATUS_OK;
  int native_code;

  if (out != NULL) {
    out->data = NULL;
    out->size = 0u;
  }
  dbtool_error_init(error);
  if (path == NULL || path[0] == '\0' || max_bytes == 0u || out == NULL ||
      ops == NULL || ops->stat == NULL || ops->open == NULL ||
      ops->read == NULL || ops->close == NULL) {
    dbtool_file_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT, "read-script", 0,
                     "invalid bounded file input");
    return DBTOOL_STATUS_INVALID_ARGUMENT;
  }

  native_code = ops->stat(ops_context, path, &info);
  if (native_code != 0) {
    dbtool_file_fail(error, DBTOOL_STATUS_FILE_ERROR, "stat-script",
                     native_code, "cannot stat SQL script");
    return DBTOOL_STATUS_FILE_ERROR;
  }
  if (!info.is_file) {
    dbtool_file_fail(error, DBTOOL_STATUS_FILE_ERROR, "stat-script", 0,
                     "SQL script path is not a regular file");
    return DBTOOL_STATUS_FILE_ERROR;
  }
  if (info.size == 0u) {
    dbtool_file_fail(error, DBTOOL_STATUS_FILE_ERROR, "read-script", 0,
                     "SQL script is empty");
    return DBTOOL_STATUS_FILE_ERROR;
  }
  if (info.size > (uint64_t)max_bytes || info.size > (uint64_t)SIZE_MAX - 1u) {
    dbtool_file_fail(error, DBTOOL_STATUS_LIMIT_EXCEEDED, "read-script", 0,
                     "SQL script exceeds max_script_bytes");
    return DBTOOL_STATUS_LIMIT_EXCEEDED;
  }
  expected = (size_t)info.size;
  data = (char *)malloc(expected + 1u);
  if (data == NULL) {
    dbtool_file_fail(error, DBTOOL_STATUS_OUT_OF_MEMORY, "read-script", 0,
                     "cannot allocate SQL script buffer");
    return DBTOOL_STATUS_OUT_OF_MEMORY;
  }

  file = ops->open(ops_context, path, SALTS_FS_O_RDONLY, 0);
  if (file == SALTS_INVALID_FILE) {
    status = DBTOOL_STATUS_FILE_ERROR;
    dbtool_file_fail(error, status, "open-script", 0,
                     "cannot open SQL script");
    goto cleanup;
  }
  while (offset < expected) {
    const size_t remaining = expected - offset;
    const size_t request = remaining > (size_t)INT_MAX ? (size_t)INT_MAX
                                                        : remaining;
    native_code = ops->read(ops_context, file, data + offset, request);
    if (native_code < 0) {
      status = DBTOOL_STATUS_FILE_ERROR;
      dbtool_file_fail(error, status, "read-script", native_code,
                       "cannot read SQL script");
      goto cleanup;
    }
    if (native_code == 0) {
      status = DBTOOL_STATUS_FILE_ERROR;
      dbtool_file_fail(error, status, "read-script", 0,
                       "SQL script changed while reading");
      goto cleanup;
    }
    if ((size_t)native_code > request) {
      status = DBTOOL_STATUS_INTERNAL_ERROR;
      dbtool_file_fail(error, status, "read-script", native_code,
                       "file reader returned an invalid byte count");
      goto cleanup;
    }
    offset += (size_t)native_code;
  }
  {
    char extra;
    native_code = ops->read(ops_context, file, &extra, 1u);
    if (native_code < 0) {
      status = DBTOOL_STATUS_FILE_ERROR;
      dbtool_file_fail(error, status, "read-script", native_code,
                       "cannot finish reading SQL script");
      goto cleanup;
    }
    if (native_code != 0) {
      status = DBTOOL_STATUS_LIMIT_EXCEEDED;
      dbtool_file_fail(error, status, "read-script", 0,
                       "SQL script changed while reading");
      goto cleanup;
    }
  }
  data[expected] = '\0';

cleanup:
  if (file != SALTS_INVALID_FILE) {
    native_code = ops->close(ops_context, file);
    if (native_code != 0 && status == DBTOOL_STATUS_OK) {
      status = DBTOOL_STATUS_FILE_ERROR;
      dbtool_file_fail(error, status, "close-script", native_code,
                       "cannot close SQL script");
    }
  }
  if (status != DBTOOL_STATUS_OK) {
    free(data);
    return status;
  }
  out->data = data;
  out->size = expected;
  return DBTOOL_STATUS_OK;
}

dbtool_status dbtool_file_read(const char *path, size_t max_bytes,
                               dbtool_file *out, dbtool_error *error) {
  return dbtool_file_read_with_ops(path, max_bytes, out,
                                   &dbtool_native_file_ops, NULL, error);
}

void dbtool_file_release(dbtool_file *file) {
  if (file == NULL)
    return;
  free(file->data);
  file->data = NULL;
  file->size = 0u;
}

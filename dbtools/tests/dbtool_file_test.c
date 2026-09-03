#include "dbtool_file.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tinytest.h"

static int oversized_stat(void *context, const char *path,
                          salts_fs_stat_t *out) {
  (void)context;
  (void)path;
  memset(out, 0, sizeof(*out));
  out->is_file = true;
  out->size = UINT64_MAX;
  return 0;
}

static salts_file_t unexpected_open(void *context, const char *path,
                                    int flags, int mode) {
  (void)context;
  (void)path;
  (void)flags;
  (void)mode;
  return SALTS_INVALID_FILE;
}

static int unexpected_read(void *context, salts_file_t file, char *data,
                           size_t size) {
  (void)context;
  (void)file;
  (void)data;
  (void)size;
  return -1;
}

static int unexpected_close(void *context, salts_file_t file) {
  (void)context;
  (void)file;
  return -1;
}

spec("standalone database tool bounded file input") {
  it("reads exactly the configured limit and adds a terminator") {
    char *path = tt_make_temp_file("dbtool-exact", ".sql");
    dbtool_file file = DBTOOL_FILE_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;

    check_not_null(path);
    check_equal(tt_write_file(path, "12345678", 8u), 0);
    check_equal(dbtool_file_read(path, 8u, &file, &error), DBTOOL_STATUS_OK);
    check_equal(file.size, (size_t)8u);
    check_equal(file.data, "12345678", 8u);
    check_equal(file.data[8], '\0');

    dbtool_file_release(&file);
    check_null(file.data);
    check_equal(file.size, (size_t)0u);
    check_equal(tt_remove_file(path), 0);
    free(path);
  }

  it("rejects limit plus one before returning a buffer") {
    char *path = tt_make_temp_file("dbtool-over", ".sql");
    dbtool_file file = DBTOOL_FILE_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;

    check_not_null(path);
    check_equal(tt_write_file(path, "123456789", 9u), 0);
    check_equal(dbtool_file_read(path, 8u, &file, &error),
                DBTOOL_STATUS_LIMIT_EXCEEDED);
    check_null(file.data);
    check_equal(error.stage, "read-script");

    check_equal(tt_remove_file(path), 0);
    free(path);
  }

  it("rejects an empty script") {
    char *path = tt_make_temp_file("dbtool-empty", ".sql");
    dbtool_file file = DBTOOL_FILE_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;

    check_not_null(path);
    check_equal(tt_write_file(path, "", 0u), 0);
    check_equal(dbtool_file_read(path, 8u, &file, &error),
                DBTOOL_STATUS_FILE_ERROR);
    check_null(file.data);
    check_contains(error.message, "empty");

    check_equal(tt_remove_file(path), 0);
    free(path);
  }

  it("reports a missing script without producing a buffer") {
    dbtool_file file = DBTOOL_FILE_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;

    check_equal(dbtool_file_read("missing-dbtool-script.sql", 8u, &file,
                                 &error),
                DBTOOL_STATUS_FILE_ERROR);
    check_null(file.data);
    check_equal(error.stage, "stat-script");
    check_less(error.native_code, 0);
  }

  it("rejects an unrepresentable stat size before opening the file") {
    static const dbtool_file_ops ops = {
        oversized_stat, unexpected_open, unexpected_read, unexpected_close};
    dbtool_file file = DBTOOL_FILE_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;

    check_equal(dbtool_file_read_with_ops("oversized.sql", SIZE_MAX, &file,
                                          &ops, NULL, &error),
                DBTOOL_STATUS_LIMIT_EXCEEDED);
    check_null(file.data);
    check_equal(error.stage, "read-script");
  }
}

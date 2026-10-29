#include <turbodb/sqlite_revision.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expression) do { \
  turbodb_status_t status_ = (expression); \
  if (status_ != TURBODB_STATUS_OK) { \
    (void)fprintf(stderr, "Installed C11 consumer failed at line %d: %d (%s)\\n", \
                  __LINE__, (int)status_, err.message); \
    return 1; \
  } \
} while (0)

static turbodb_blob_t blob_view(const void *data, size_t size) {
  turbodb_blob_t value = {data, size};
  return value;
}

int main(int argc, char **argv) {
  turbodb_revision_open_config_t cfg;
  turbodb_revision_db_t *db = NULL;
  turbodb_revision_write_t *write = NULL;
  turbodb_revision_read_t *read = NULL;
  turbodb_revision_value_t *value = NULL;
  turbodb_error_t err;
  uint64_t sequence = 0;
  turbodb_blob_t bytes;
  const char *path;
  if (argc != 2 || argv[1] == NULL) return 2;
  path = argv[1];
  (void)remove(path);
  turbodb_error_init(&err);
  turbodb_revision_open_config_init(&cfg);
  if (cfg.struct_size != sizeof(cfg) || cfg.abi_version != TURBODB_REVISION_ABI_VERSION)
    return 3;
  cfg.path = turbodb_view(path);
  cfg.flags = TURBODB_REVISION_OPEN_CREATE;
  CHECK(turbodb_revision_db_open(&cfg, &db, &err));
  CHECK(turbodb_revision_begin_write(db, blob_view("installed-request", 17),
                                     &write, &err));
  CHECK(turbodb_revision_write_put(write, turbodb_view("sdk"),
                                   blob_view("file", 4),
                                   blob_view("installed-content", 17),
                                   TURBODB_REVISION_EXPECT_ABSENT, &err));
  CHECK(turbodb_revision_write_publish(write, &sequence, &err));
  write = NULL;
  if (sequence != 1) return 4;
  CHECK(turbodb_revision_begin_read(db, sequence, &read, &err));
  CHECK(turbodb_revision_read_get(read, turbodb_view("sdk"),
                                  blob_view("file", 4), &value, &err));
  bytes = turbodb_revision_value_bytes(value);
  if (bytes.size != 17 || memcmp(bytes.data, "installed-content", 17) != 0)
    return 5;
  turbodb_revision_value_destroy(value);
  value = NULL;
  turbodb_revision_read_close(read);
  read = NULL;
  CHECK(turbodb_revision_inspect_request(db, blob_view("installed-request", 17),
                                         &sequence, &err));
  if (sequence != 1) return 6;
  CHECK(turbodb_revision_db_close(db, &err));
  db = NULL;
  (void)remove(path);
  (void)puts("Installed TurboDB::SqliteRevision C11 consumer PASS");
  return 0;
}

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
  if (cfg.struct_size != sizeof(cfg) || cfg.abi_version != TURBODB_REVISION_ABI_VERSION ||
      cfg.max_batch_operations == 0 || cfg.max_batch_bytes == 0)
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

  /* Qualify the new M1a symbols through the installed C11 SDK, not just
   * internal native test linkage. Borrow the binary key only until next(). */
  turbodb_revision_scan_config_t scan_cfg;
  turbodb_revision_scan_t *scan = NULL;
  turbodb_blob_t matched_key = {NULL, 0};
  turbodb_revision_scan_config_init(&scan_cfg);
  if (scan_cfg.abi_version != TURBODB_REVISION_SCAN_ABI_VERSION)
    return 7;
  CHECK(turbodb_revision_read_scan_prefix(read, turbodb_view("sdk"),
                                          blob_view("fi", 2), &scan_cfg, &scan, &err));
  CHECK(turbodb_revision_scan_next(scan, &matched_key, &value, &err));
  bytes = turbodb_revision_value_bytes(value);
  if (matched_key.size != 4 || memcmp(matched_key.data, "file", 4) ||
      bytes.size != 17 || memcmp(bytes.data, "installed-content", 17))
    return 8;
  turbodb_revision_value_destroy(value);
  value = NULL;
  if (turbodb_revision_scan_next(scan, &matched_key, &value, &err) !=
      TURBODB_STATUS_NOT_FOUND)
    return 9;
  turbodb_revision_scan_close(scan);
  scan = NULL;
  turbodb_revision_read_close(read);
  read = NULL;
  CHECK(turbodb_revision_inspect_request(db, blob_view("installed-request", 17),
                                         &sequence, &err));
  if (sequence != 1) return 6;
  CHECK(turbodb_revision_db_close(db, &err));
  db = NULL;

  /* M1b: installed SDK must export the explicit FTS5 projection functions,
   * not merely the M0/M1a revision and prefix symbols. */
  (void)remove(path);
  cfg.flags = TURBODB_REVISION_OPEN_CREATE | TURBODB_REVISION_OPEN_FTS5;
  CHECK(turbodb_revision_db_open(&cfg, &db, &err));
  CHECK(turbodb_revision_begin_write(db, blob_view("fts-sdk-1", 9), &write, &err));
  CHECK(turbodb_revision_write_put(write, turbodb_view("sdk"),
                                   blob_view("file", 4),
                                   blob_view("payload", 7),
                                   TURBODB_REVISION_EXPECT_ABSENT, &err));
  CHECK(turbodb_revision_write_project_text(write, turbodb_view("sdk"),
                                            blob_view("file",4),
                                            turbodb_view("river forest"), &err));
  CHECK(turbodb_revision_write_publish(write, &sequence, &err));
  write = NULL;
  CHECK(turbodb_revision_begin_read(db, sequence, &read, &err));
  turbodb_revision_scan_config_init(&scan_cfg);
  CHECK(turbodb_revision_read_search_text(read, turbodb_view("sdk"),
                                          turbodb_view("river"),
                                          &scan_cfg, &scan, &err));
  CHECK(turbodb_revision_scan_next(scan, &matched_key, &value, &err));
  bytes = turbodb_revision_value_bytes(value);
  if (matched_key.size != 4 || memcmp(matched_key.data,"file",4) != 0 ||
      bytes.size != 7 || memcmp(bytes.data,"payload",7) != 0) return 10;
  turbodb_revision_value_destroy(value);
  value = NULL;
  if (turbodb_revision_scan_next(scan, &matched_key, &value, &err) !=
      TURBODB_STATUS_NOT_FOUND) return 11;
  turbodb_revision_scan_close(scan);
  scan = NULL;
  turbodb_revision_read_close(read);
  read = NULL;
  CHECK(turbodb_revision_db_close(db, &err));
  db = NULL;
  (void)remove(path);
  (void)puts("Installed TurboDB::SqliteRevision C11 consumer PASS");
  return 0;
}

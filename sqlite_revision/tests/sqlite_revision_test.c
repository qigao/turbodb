#include <turbodb/sqlite_revision.h>

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* turbodb_blob() constructs a generic turbodb_value_t, not turbodb_blob_t. */
static turbodb_blob_t revision_blob(const void *data, size_t size) {
  turbodb_blob_t out = {data, size};
  return out;
}

#define CHECK(condition) do { if (!(condition)) { \
  fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
  return 1; } } while (0)
#define CALL(call, expected) do { \
  turbodb_status_t actual_status = (call); \
  if (actual_status != (expected)) { \
    fprintf(stderr, "FAIL %s:%d: status=%d expected=%d detail=%s\n", \
      __FILE__, __LINE__, (int)actual_status, (int)(expected), err.message); \
    return 1; } } while (0)

static int check_value(turbodb_revision_db_t *db, uint64_t at,
                       const char *expected, int deleted, uint64_t expected_seq) {
  turbodb_error_t err;
  turbodb_revision_read_t *read = NULL;
  turbodb_revision_value_t *value = NULL;
  turbodb_blob_t data;
  turbodb_error_init(&err);
  CALL(turbodb_revision_begin_read(db, at, &read, &err), TURBODB_STATUS_OK);
  CALL(turbodb_revision_read_get(read, turbodb_view("docs"),
                                revision_blob("file-1", 6), &value, &err),
       TURBODB_STATUS_OK);
  CHECK(value != NULL);
  CHECK(turbodb_revision_value_sequence(value) == expected_seq);
  CHECK(turbodb_revision_value_is_deleted(value) == deleted);
  data = turbodb_revision_value_bytes(value);
  if (!deleted) {
    CHECK(data.size == strlen(expected));
    CHECK(memcmp(data.data, expected, data.size) == 0);
  } else {
    CHECK(data.size == 0 && data.data == NULL);
  }
  turbodb_revision_value_destroy(value);
  turbodb_revision_read_close(read);
  return 0;
}

static int run(const char *path) {
  turbodb_revision_open_config_t cfg;
  turbodb_revision_db_t *db = NULL;
  turbodb_revision_write_t *write = NULL;
  turbodb_revision_read_t *read = NULL;
  turbodb_revision_value_t *value = NULL;
  turbodb_error_t err;
  uint64_t seq = 0;
  sqlite3 *raw = NULL;
  const char *request1 = "req-1", *request2 = "req-2", *request3 = "req-3";
  turbodb_error_init(&err);
  turbodb_revision_open_config_init(&cfg);
  cfg.path = turbodb_view(path);
  cfg.flags = TURBODB_REVISION_OPEN_CREATE | TURBODB_REVISION_OPEN_WAL;
  (void)remove(path);
  CALL(turbodb_revision_db_open(&cfg, &db, &err), TURBODB_STATUS_OK);
  CHECK(db != NULL);
  CALL(turbodb_revision_begin_read(db, 0, &read, &err), TURBODB_STATUS_OK);
  CHECK(turbodb_revision_read_sequence(read) == 0);
  CALL(turbodb_revision_read_get(read, turbodb_view("docs"),
                                revision_blob("file-1", 6), &value, &err),
       TURBODB_STATUS_NOT_FOUND);
  CHECK(value == NULL);
  turbodb_revision_read_close(read); read = NULL;

  CALL(turbodb_revision_begin_write(db, revision_blob(request1, 5), &write, &err), TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_put(write, turbodb_view("docs"),
       revision_blob("file-1",6), revision_blob("version one",11),
       TURBODB_REVISION_EXPECT_ABSENT, &err), TURBODB_STATUS_OK);
  /* Every additional entry in the batch belongs to the same commit. */
  CALL(turbodb_revision_write_put(write, turbodb_view("docs"),
       revision_blob("file-2",6), revision_blob("shared txn",10),
       TURBODB_REVISION_EXPECT_ABSENT, &err), TURBODB_STATUS_OK);
  CALL(turbodb_revision_db_close(db, &err), TURBODB_STATUS_BUSY);
  CALL(turbodb_revision_write_publish(write, &seq, &err), TURBODB_STATUS_OK);
  write = NULL;
  CHECK(seq == 1);
  CHECK(check_value(db, 1, "version one", 0, 1) == 0);
  CALL(turbodb_revision_inspect_request(db, revision_blob(request1,5), &seq, &err), TURBODB_STATUS_OK);
  CHECK(seq == 1);
  CALL(turbodb_revision_begin_write(db, revision_blob(request1, 5), &write, &err), TURBODB_STATUS_CONSTRAINT);
  CHECK(write == NULL);

  CALL(turbodb_revision_begin_read(db, 0, &read, &err), TURBODB_STATUS_OK);
  CHECK(turbodb_revision_read_sequence(read) == 1);
  CALL(turbodb_revision_begin_write(db, revision_blob(request2, 5), &write, &err), TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_put(write, turbodb_view("docs"),
       revision_blob("file-1", 6), revision_blob("version two", 11), 1, &err), TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_publish(write, &seq, &err), TURBODB_STATUS_OK);
  write = NULL;
  CHECK(seq == 2);
  /* This active reader sees the exact older SQLite transaction snapshot. */
  CALL(turbodb_revision_read_get(read, turbodb_view("docs"),
       revision_blob("file-1",6), &value, &err), TURBODB_STATUS_OK);
  CHECK(turbodb_revision_value_sequence(value) == 1);
  CHECK(memcmp(turbodb_revision_value_bytes(value).data, "version one", 11)==0);
  turbodb_revision_value_destroy(value); value = NULL;
  turbodb_revision_read_close(read); read = NULL;
  CHECK(check_value(db, 1, "version one", 0, 1) == 0);
  CHECK(check_value(db, 2, "version two", 0, 2) == 0);
  CHECK(check_value(db, 0, "version two", 0, 2) == 0);

  /* A transaction with failed CAS is explicitly abandoned; old data remains. */
  CALL(turbodb_revision_begin_write(db, revision_blob("req-bad", 7), &write, &err), TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_put(write, turbodb_view("docs"),
       revision_blob("file-1",6), revision_blob("bad",3), 1, &err), TURBODB_STATUS_CONSTRAINT);
  turbodb_revision_write_abort(write); write = NULL;
  CALL(turbodb_revision_inspect_request(db, revision_blob("req-bad",7), &seq, &err), TURBODB_STATUS_NOT_FOUND);
  CHECK(check_value(db, 0, "version two", 0, 2) == 0);

  CALL(turbodb_revision_begin_write(db, revision_blob(request3,5), &write, &err), TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_delete(write, turbodb_view("docs"),
       revision_blob("file-1",6), 2, &err), TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_publish(write, &seq, &err), TURBODB_STATUS_OK);
  write = NULL;
  CHECK(seq == 3);
  CHECK(check_value(db, 3, "", 1, 3) == 0);
  CHECK(check_value(db, 2, "version two", 0, 2) == 0);
  CALL(turbodb_revision_begin_read(db, 4, &read, &err), TURBODB_STATUS_OUT_OF_RANGE);
  CHECK(read == NULL);
  CALL(turbodb_revision_db_close(db, &err), TURBODB_STATUS_OK); db = NULL;

  cfg.flags = TURBODB_REVISION_OPEN_READ_ONLY;
  CALL(turbodb_revision_db_open(&cfg, &db, &err), TURBODB_STATUS_OK);
  CALL(turbodb_revision_begin_write(db, revision_blob("req-4",5), &write, &err), TURBODB_STATUS_INVALID_STATE);
  CHECK(write == NULL);
  CHECK(check_value(db, 1, "version one", 0, 1) == 0);
  CHECK(check_value(db, 3, "", 1, 3) == 0);
  CALL(turbodb_revision_inspect_request(db, revision_blob(request3,5), &seq, &err), TURBODB_STATUS_OK);
  CHECK(seq == 3);
  CALL(turbodb_revision_db_close(db, &err), TURBODB_STATUS_OK); db = NULL;

  /* Foreign schema must not be silently initialized or migrated. */
  CHECK(sqlite3_open(path, &raw) == SQLITE_OK);
  CHECK(sqlite3_exec(raw, "PRAGMA user_version=42", NULL, NULL, NULL) == SQLITE_OK);
  CHECK(sqlite3_close(raw) == SQLITE_OK); raw = NULL;
  cfg.flags = TURBODB_REVISION_OPEN_CREATE;
  CALL(turbodb_revision_db_open(&cfg, &db, &err), TURBODB_STATUS_UNSUPPORTED);
  CHECK(db == NULL);
  (void)remove(path);
  return 0;
}

static int verify_strict_schema_admission(const char *path) {
  sqlite3 *raw = NULL;
  turbodb_revision_open_config_t cfg;
  turbodb_revision_db_t *db = NULL;
  turbodb_error_t err;
  turbodb_error_init(&err);
  turbodb_revision_open_config_init(&cfg);
  cfg.path = turbodb_view(path);

  /* Correct application_id/version and table NAMES must not admit a fake DDL. */
  (void)remove(path);
  CHECK(sqlite3_open(path, &raw) == SQLITE_OK);
  CHECK(sqlite3_exec(raw,
    "PRAGMA application_id=1413763670; PRAGMA user_version=1;"
    "CREATE TABLE revision_commits(fake INTEGER);"
    "CREATE TABLE revision_values(fake INTEGER);"
    "CREATE TABLE revision_heads(fake INTEGER);",
    NULL, NULL, NULL) == SQLITE_OK);
  CHECK(sqlite3_close(raw) == SQLITE_OK); raw = NULL;
  CALL(turbodb_revision_db_open(&cfg, &db, &err), TURBODB_STATUS_UNSUPPORTED);
  CHECK(db == NULL);

  /* No injected trigger may execute inside a revision commit. */
  (void)remove(path);
  cfg.flags = TURBODB_REVISION_OPEN_CREATE;
  CALL(turbodb_revision_db_open(&cfg, &db, &err), TURBODB_STATUS_OK);
  CALL(turbodb_revision_db_close(db, &err), TURBODB_STATUS_OK); db = NULL;
  CHECK(sqlite3_open(path, &raw) == SQLITE_OK);
  CHECK(sqlite3_exec(raw,
    "CREATE TRIGGER injected_revision AFTER INSERT ON revision_commits "
    "BEGIN SELECT 1; END;",
    NULL, NULL, NULL) == SQLITE_OK);
  CHECK(sqlite3_close(raw) == SQLITE_OK); raw = NULL;
  cfg.flags = 0;
  CALL(turbodb_revision_db_open(&cfg, &db, &err), TURBODB_STATUS_UNSUPPORTED);
  CHECK(db == NULL);

  /* '_' in SQL LIKE is a wildcard: extra 'sqlitex...' objects are not
   * SQLite's reserved internal 'sqlite_' namespace. Reject these too. */
  (void)remove(path);
  cfg.flags = TURBODB_REVISION_OPEN_CREATE;
  CALL(turbodb_revision_db_open(&cfg, &db, &err), TURBODB_STATUS_OK);
  CALL(turbodb_revision_db_close(db, &err), TURBODB_STATUS_OK); db = NULL;
  CHECK(sqlite3_open(path, &raw) == SQLITE_OK);
  CHECK(sqlite3_exec(raw, "CREATE VIEW sqlitexhidden AS SELECT 1;",
                     NULL, NULL, NULL) == SQLITE_OK);
  CHECK(sqlite3_close(raw) == SQLITE_OK); raw = NULL;
  cfg.flags = 0;
  CALL(turbodb_revision_db_open(&cfg, &db, &err), TURBODB_STATUS_UNSUPPORTED);
  CHECK(db == NULL);

  /* Named index membership is part of v1 schema admission. */
  (void)remove(path);
  cfg.flags = TURBODB_REVISION_OPEN_CREATE;
  CALL(turbodb_revision_db_open(&cfg, &db, &err), TURBODB_STATUS_OK);
  CALL(turbodb_revision_db_close(db, &err), TURBODB_STATUS_OK); db = NULL;
  CHECK(sqlite3_open(path, &raw) == SQLITE_OK);
  CHECK(sqlite3_exec(raw, "DROP INDEX revision_changes_by_commit;",
                     NULL, NULL, NULL) == SQLITE_OK);
  CHECK(sqlite3_close(raw) == SQLITE_OK); raw = NULL;
  cfg.flags = 0;
  CALL(turbodb_revision_db_open(&cfg, &db, &err), TURBODB_STATUS_UNSUPPORTED);
  CHECK(db == NULL);
  (void)remove(path);
  return 0;
}

static int verify_bounded_batch(const char *path) {
  turbodb_revision_open_config_t cfg;
  turbodb_revision_db_t *db = NULL;
  turbodb_revision_write_t *write = NULL;
  turbodb_error_t err;
  uint64_t sequence = 0;
  turbodb_error_init(&err);
  turbodb_revision_open_config_init(&cfg);
  cfg.path = turbodb_view(path);
  cfg.flags = TURBODB_REVISION_OPEN_CREATE;
  cfg.max_batch_operations = 0;
  CALL(turbodb_revision_db_open(&cfg, &db, &err),
       TURBODB_STATUS_INVALID_ARGUMENT);
  CHECK(db == NULL);
  cfg.max_batch_operations = 1;
  cfg.max_batch_bytes = 8;
  (void)remove(path);
  CALL(turbodb_revision_db_open(&cfg, &db, &err), TURBODB_STATUS_OK);
  CALL(turbodb_revision_begin_write(db, (turbodb_blob_t){"quota-1", 7},
                                    &write, &err), TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_put(write, turbodb_view("ns"),
       (turbodb_blob_t){"ab", 2}, (turbodb_blob_t){"four", 4},
       TURBODB_REVISION_EXPECT_ABSENT, &err), TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_put(write, turbodb_view("ns"),
       (turbodb_blob_t){"cd", 2}, (turbodb_blob_t){"bad", 3},
       TURBODB_REVISION_EXPECT_ABSENT, &err),
       TURBODB_STATUS_LIMIT_EXCEEDED);
  CALL(turbodb_revision_write_publish(write, &sequence, &err),
       TURBODB_STATUS_INVALID_STATE);
  turbodb_revision_write_abort(write); write = NULL;
  CALL(turbodb_revision_inspect_request(db, (turbodb_blob_t){"quota-1", 7},
       &sequence, &err), TURBODB_STATUS_NOT_FOUND);
  CALL(turbodb_revision_db_close(db, &err), TURBODB_STATUS_OK); db = NULL;

  cfg.flags = 0;
  cfg.max_batch_operations = 2;
  cfg.max_batch_bytes = 7;
  CALL(turbodb_revision_db_open(&cfg, &db, &err), TURBODB_STATUS_OK);
  CALL(turbodb_revision_begin_write(db, (turbodb_blob_t){"quota-2", 7},
                                    &write, &err), TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_put(write, turbodb_view("ns"),
       (turbodb_blob_t){"ab", 2}, (turbodb_blob_t){"four", 4},
       TURBODB_REVISION_EXPECT_ABSENT, &err),
       TURBODB_STATUS_LIMIT_EXCEEDED);
  turbodb_revision_write_abort(write); write = NULL;
  CALL(turbodb_revision_inspect_request(db, (turbodb_blob_t){"quota-2", 7},
       &sequence, &err), TURBODB_STATUS_NOT_FOUND);
  CALL(turbodb_revision_db_close(db, &err), TURBODB_STATUS_OK);
  (void)remove(path);
  return 0;
}

/* A DELETE-journal reader's SHARED lock makes COMMIT return SQLITE_BUSY.
 * The outcome is known (no publication), not COMMIT_UNKNOWN. */
static int verify_busy_commit_abort(const char *path) {
  turbodb_revision_open_config_t cfg;
  turbodb_revision_db_t *db = NULL;
  turbodb_revision_write_t *write = NULL;
  turbodb_revision_read_t *read = NULL;
  turbodb_revision_value_t *value = NULL;
  turbodb_error_t err;
  uint64_t sequence = 0;
  turbodb_error_init(&err);
  turbodb_revision_open_config_init(&cfg);
  cfg.path = turbodb_view(path);
  cfg.flags = TURBODB_REVISION_OPEN_CREATE;
  cfg.busy_timeout_ms = 10;
  (void)remove(path);
  CALL(turbodb_revision_db_open(&cfg, &db, &err), TURBODB_STATUS_OK);
  CALL(turbodb_revision_begin_write(db, (turbodb_blob_t){"lock-1", 6},
                                    &write, &err), TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_put(write, turbodb_view("ns"),
       (turbodb_blob_t){"key", 3}, (turbodb_blob_t){"old", 3},
       TURBODB_REVISION_EXPECT_ABSENT, &err), TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_publish(write, &sequence, &err), TURBODB_STATUS_OK);
  write = NULL;
  CHECK(sequence == 1);

  CALL(turbodb_revision_begin_read(db, 0, &read, &err), TURBODB_STATUS_OK);
  CALL(turbodb_revision_read_get(read, turbodb_view("ns"),
       (turbodb_blob_t){"key", 3}, &value, &err), TURBODB_STATUS_OK);
  CHECK(turbodb_revision_value_bytes(value).size == 3);
  CHECK(memcmp(turbodb_revision_value_bytes(value).data, "old", 3) == 0);
  turbodb_revision_value_destroy(value); value = NULL;

  CALL(turbodb_revision_begin_write(db, (turbodb_blob_t){"lock-2", 6},
                                    &write, &err), TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_put(write, turbodb_view("ns"),
       (turbodb_blob_t){"key", 3}, (turbodb_blob_t){"new", 3},
       1, &err), TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_publish(write, &sequence, &err),
       TURBODB_STATUS_BUSY);
  turbodb_revision_write_abort(write); write = NULL;
  turbodb_revision_read_close(read); read = NULL;

  CALL(turbodb_revision_inspect_request(db, (turbodb_blob_t){"lock-2", 6},
       &sequence, &err), TURBODB_STATUS_NOT_FOUND);
  CALL(turbodb_revision_begin_read(db, 0, &read, &err), TURBODB_STATUS_OK);
  CALL(turbodb_revision_read_get(read, turbodb_view("ns"),
       (turbodb_blob_t){"key", 3}, &value, &err), TURBODB_STATUS_OK);
  CHECK(turbodb_revision_value_bytes(value).size == 3);
  CHECK(memcmp(turbodb_revision_value_bytes(value).data, "old", 3) == 0);
  turbodb_revision_value_destroy(value); value = NULL;
  turbodb_revision_read_close(read); read = NULL;
  CALL(turbodb_revision_db_close(db, &err), TURBODB_STATUS_OK);
  (void)remove(path);
  return 0;
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "sqlite_revision_native_test.db";
  int rc = run(path);
  if (!rc) rc = verify_strict_schema_admission(path);
  if (!rc) rc = verify_bounded_batch(path);
  if (!rc) rc = verify_busy_commit_abort(path);
  if (!rc) puts("SQLite revision native C11 contract PASS");
  return rc;
}

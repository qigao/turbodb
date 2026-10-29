#include <turbodb/sqlite_revision.h>

#include <sqlite3.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REVISION_APPLICATION_ID 0x54445256 /* 'TDRV' */
#define REVISION_SCHEMA_VERSION 1
#define REVISION_DEFAULT_MAX_DB (UINT64_C(256) * 1024 * 1024)
#define REVISION_DEFAULT_MAX_VALUE (UINT64_C(16) * 1024 * 1024)
#define REVISION_DEFAULT_MAX_BATCH_BYTES (UINT64_C(64) * 1024 * 1024)
#define REVISION_DEFAULT_MAX_BATCH_OPERATIONS UINT32_C(1024)
#define REVISION_MAX_BATCH_BYTES (UINT64_C(1024) * 1024 * 1024)
#define REVISION_MAX_BATCH_OPERATIONS UINT32_C(65536)
#define REVISION_MAX_NAMESPACE 1024u
#define REVISION_MAX_KEY 4096u
#define REVISION_MAX_REQUEST_ID 256u

#define REVISION_COMMITS_DDL \
  "CREATE TABLE revision_commits(" \
  "commit_seq INTEGER PRIMARY KEY, request_id BLOB NOT NULL UNIQUE," \
  "CHECK(length(request_id) BETWEEN 1 AND 256))"
#define REVISION_VALUES_DDL \
  "CREATE TABLE revision_values(" \
  "namespace TEXT NOT NULL, item_key BLOB NOT NULL, commit_seq INTEGER NOT NULL," \
  "is_deleted INTEGER NOT NULL CHECK(is_deleted IN (0,1)), payload BLOB," \
  "PRIMARY KEY(namespace,item_key,commit_seq)," \
  "FOREIGN KEY(commit_seq) REFERENCES revision_commits(commit_seq)) WITHOUT ROWID"
#define REVISION_HEADS_DDL \
  "CREATE TABLE revision_heads(" \
  "namespace TEXT NOT NULL, item_key BLOB NOT NULL, commit_seq INTEGER NOT NULL," \
  "PRIMARY KEY(namespace,item_key)," \
  "FOREIGN KEY(commit_seq) REFERENCES revision_commits(commit_seq)) WITHOUT ROWID"
#define REVISION_INDEX_DDL \
  "CREATE INDEX revision_changes_by_commit ON revision_values(commit_seq,namespace,item_key)"

struct turbodb_revision_db_s {
  sqlite3 *sql;
  char *path;
  uint32_t flags;
  uint32_t busy_timeout_ms;
  uint64_t max_value_bytes;
  uint64_t max_batch_bytes;
  uint32_t max_batch_operations;
  size_t active_reads;
  int active_write;
  int quarantined;
};
struct turbodb_revision_read_s {
  turbodb_revision_db_t *owner;
  sqlite3 *sql;
  uint64_t sequence;
  turbodb_revision_scan_t *scan;
};
struct turbodb_revision_write_s {
  turbodb_revision_db_t *owner;
  uint64_t sequence;
  uint64_t used_bytes;
  uint32_t operations;
  int poisoned;
};
struct turbodb_revision_value_s {
  uint64_t sequence;
  size_t size;
  int deleted;
  unsigned char *data;
};

static turbodb_status_t result(turbodb_error_t *error, turbodb_status_t status,
                               const char *message) {
  if (error != NULL) {
    turbodb_error_init(error);
    error->status = status;
    if (message != NULL) {
      (void)snprintf(error->message, sizeof(error->message), "%s", message);
    }
  }
  return status;
}
static turbodb_status_t sql_status(int code) {
  switch (code & 255) {
  case SQLITE_OK: return TURBODB_STATUS_OK;
  case SQLITE_BUSY: case SQLITE_LOCKED: return TURBODB_STATUS_BUSY;
  case SQLITE_NOMEM: return TURBODB_STATUS_OUT_OF_MEMORY;
  case SQLITE_TOOBIG: case SQLITE_FULL: return TURBODB_STATUS_LIMIT_EXCEEDED;
  case SQLITE_CONSTRAINT: return TURBODB_STATUS_CONSTRAINT;
  case SQLITE_READONLY: return TURBODB_STATUS_INVALID_STATE;
  case SQLITE_MISMATCH: return TURBODB_STATUS_TYPE_ERROR;
  default: return TURBODB_STATUS_DATASTORE_ERROR;
  }
}
static turbodb_status_t fail_sql(sqlite3 *sql, int code,
                                 turbodb_error_t *error) {
  return result(error, sql_status(code), sql != NULL ? sqlite3_errmsg(sql) :
                sqlite3_errstr(code));
}
static int is_view(turbodb_string_view_t v, size_t max) {
  return v.data != NULL && v.len > 0 && v.len <= max &&
         memchr(v.data, 0, v.len) == NULL;
}
static int is_blob(turbodb_blob_t b, size_t max) {
  return b.size > 0 && b.size <= max && b.data != NULL;
}
static int valid_key(turbodb_string_view_t ns, turbodb_blob_t key) {
  return is_view(ns, REVISION_MAX_NAMESPACE) && is_blob(key, REVISION_MAX_KEY);
}
static int bind_text(sqlite3_stmt *stmt, int n, turbodb_string_view_t v) {
  return sqlite3_bind_text(stmt, n, v.data, (int)v.len, SQLITE_TRANSIENT);
}
static int bind_blob(sqlite3_stmt *stmt, int n, turbodb_blob_t b) {
  return sqlite3_bind_blob(stmt, n, b.data, (int)b.size, SQLITE_TRANSIENT);
}
static turbodb_status_t prepare(sqlite3 *sql, const char *query,
                                sqlite3_stmt **stmt, turbodb_error_t *error) {
  const int rc = sqlite3_prepare_v2(sql, query, -1, stmt, NULL);
  return rc == SQLITE_OK ? TURBODB_STATUS_OK : fail_sql(sql, rc, error);
}
static turbodb_status_t execute(sqlite3 *sql, const char *query,
                                turbodb_error_t *error) {
  int rc = sqlite3_exec(sql, query, NULL, NULL, NULL);
  return rc == SQLITE_OK ? TURBODB_STATUS_OK : fail_sql(sql, rc, error);
}
static turbodb_status_t pragma_integer(sqlite3 *sql, const char *query,
                                       sqlite3_int64 *value,
                                       turbodb_error_t *error) {
  sqlite3_stmt *stmt = NULL;
  turbodb_status_t status = prepare(sql, query, &stmt, error);
  if (status != TURBODB_STATUS_OK) return status;
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    *value = sqlite3_column_int64(stmt, 0);
    status = TURBODB_STATUS_OK;
  } else {
    status = fail_sql(sql, rc, error);
  }
  rc = sqlite3_finalize(stmt);
  if (status == TURBODB_STATUS_OK && rc != SQLITE_OK)
    status = fail_sql(sql, rc, error);
  return status;
}
/* Reject a spoofed application_id/user_version, missing/altered indexes,
 * trigger injection, and tables with the right names but the wrong schema.
 * v1 has no migration path: its exact DDL is part of admission, not metadata
 * that an untrusted file can assert via PRAGMA user_version alone. */
static turbodb_status_t verify_schema(sqlite3 *sql, turbodb_error_t *error) {
  static const char *const names[] = {
    "revision_changes_by_commit", "revision_commits", "revision_heads",
    "revision_values"
  };
  static const char *const kinds[] = {"index", "table", "table", "table"};
  static const char *const definitions[] = {
    REVISION_INDEX_DDL, REVISION_COMMITS_DDL, REVISION_HEADS_DDL,
    REVISION_VALUES_DDL
  };
  sqlite3_stmt *stmt = NULL;
  turbodb_status_t status = prepare(sql,
    "SELECT name,type,sql FROM sqlite_master "
    "WHERE substr(name,1,7)!='sqlite_' ORDER BY name", &stmt, error);
  if (status != TURBODB_STATUS_OK) return status;
  int rc;
  size_t count = 0;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    const char *kind = (const char *)sqlite3_column_text(stmt, 1);
    const char *definition = (const char *)sqlite3_column_text(stmt, 2);
    if (count >= sizeof(names) / sizeof(names[0]) || !name || !kind ||
        !definition || strcmp(name, names[count]) ||
        strcmp(kind, kinds[count]) || strcmp(definition, definitions[count])) {
      status = result(error, TURBODB_STATUS_UNSUPPORTED,
                      "unsupported revision SQLite schema; no automatic migration");
      break;
    }
    count++;
  }
  if (rc == SQLITE_DONE && count != sizeof(names) / sizeof(names[0])) {
    status = result(error, TURBODB_STATUS_UNSUPPORTED,
                    "missing revision SQLite objects; no automatic migration");
  } else if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
    status = fail_sql(sql, rc, error);
  }
  rc = sqlite3_finalize(stmt);
  if (status == TURBODB_STATUS_OK && rc != SQLITE_OK)
    return fail_sql(sql, rc, error);
  return status;
}
/* Ordered B-tree prefix traversal over immutable revisions.
 * This is a borrowed-key, independently-owned-value cursor. It has no model-
 * supplied SQL or writes, and shares the parent read's pinned SQLite snapshot. */
#define REVISION_SCAN_ABI_VERSION UINT32_C(1)
#define REVISION_SCAN_DEFAULT_MAX_ITEMS UINT32_C(128)
#define REVISION_SCAN_MAX_ITEMS UINT32_C(4096)
#define REVISION_SCAN_DEFAULT_MAX_BYTES (UINT64_C(1) * 1024 * 1024)
#define REVISION_SCAN_MAX_BYTES (UINT64_C(64) * 1024 * 1024)
#define REVISION_SCAN_DEFAULT_MAX_VM_STEPS UINT64_C(1000000)
#define REVISION_SCAN_MAX_VM_STEPS UINT64_C(100000000)

struct turbodb_revision_scan_s {
  turbodb_revision_read_t *read;
  sqlite3_stmt *stmt;
  uint64_t used_bytes;
  uint64_t vm_steps_left;
  uint32_t items;
  uint32_t max_items;
  uint64_t max_bytes;
  int done;
  int expired;
};

void turbodb_revision_scan_config_init(turbodb_revision_scan_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->struct_size = (uint32_t)sizeof(*config);
  config->abi_version = REVISION_SCAN_ABI_VERSION;
  config->max_items = REVISION_SCAN_DEFAULT_MAX_ITEMS;
  config->max_result_bytes = REVISION_SCAN_DEFAULT_MAX_BYTES;
  config->max_vm_steps = REVISION_SCAN_DEFAULT_MAX_VM_STEPS;
}

static int revision_scan_progress(void *context) {
  turbodb_revision_scan_t *scan = (turbodb_revision_scan_t *)context;
  if (scan->vm_steps_left < UINT64_C(100)) {
    scan->expired = 1;
    return 1;
  }
  scan->vm_steps_left -= UINT64_C(100);
  return 0;
}

turbodb_status_t turbodb_revision_read_scan_prefix(
    turbodb_revision_read_t *read, turbodb_string_view_t ns,
    turbodb_blob_t key_prefix, const turbodb_revision_scan_config_t *cfg,
    turbodb_revision_scan_t **out_scan, turbodb_error_t *error) {
  if (out_scan) *out_scan = NULL;
  if (!read || !out_scan || !cfg ||
      cfg->struct_size != sizeof(*cfg) ||
      cfg->abi_version != REVISION_SCAN_ABI_VERSION ||
      !is_view(ns, REVISION_MAX_NAMESPACE) ||
      !is_blob(key_prefix, REVISION_MAX_KEY) ||
      cfg->max_items == 0 || cfg->max_items > REVISION_SCAN_MAX_ITEMS ||
      cfg->max_result_bytes == 0 || cfg->max_result_bytes > REVISION_SCAN_MAX_BYTES ||
      cfg->max_vm_steps < UINT64_C(100) ||
      cfg->max_vm_steps > REVISION_SCAN_MAX_VM_STEPS)
    return result(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid prefix scan config");
  if (read->scan)
    return result(error, TURBODB_STATUS_BUSY, "one prefix scan per snapshot");

  turbodb_revision_scan_t *scan = (turbodb_revision_scan_t *)calloc(1, sizeof(*scan));
  if (!scan) return result(error, TURBODB_STATUS_OUT_OF_MEMORY, "allocate prefix scan");
  scan->read = read;
  scan->max_items = cfg->max_items;
  scan->max_bytes = cfg->max_result_bytes;
  scan->vm_steps_left = cfg->max_vm_steps;

  /* The lexicographic exclusive successor of a binary prefix bounds the B-tree
   * range, including embedded NUL. For an all-FF prefix no finite successor
   * exists; every key >= prefix shares that all-FF prefix. */
  unsigned char successor[REVISION_MAX_KEY];
  memcpy(successor, key_prefix.data, key_prefix.size);
  size_t upper_len = key_prefix.size;
  while (upper_len && successor[upper_len - 1] == UINT8_MAX) upper_len--;
  if (upper_len) successor[upper_len - 1]++;

  turbodb_status_t status = prepare(read->sql,
      "SELECT v.item_key,v.commit_seq,v.is_deleted,v.payload FROM revision_values AS v "
      "WHERE v.namespace=?1 AND v.item_key>=?2 "
      "AND (?3 IS NULL OR v.item_key<?3) "
      "AND v.commit_seq=(SELECT MAX(h.commit_seq) FROM revision_values AS h "
      "WHERE h.namespace=v.namespace AND h.item_key=v.item_key AND "
      "h.commit_seq<=?4) ORDER BY v.item_key", &scan->stmt, error);
  if (status != TURBODB_STATUS_OK) { free(scan); return status; }
  int rc = bind_text(scan->stmt, 1, ns);
  if (rc == SQLITE_OK) rc = bind_blob(scan->stmt, 2, key_prefix);
  if (rc == SQLITE_OK) rc = upper_len ?
      sqlite3_bind_blob(scan->stmt, 3, successor, (int)upper_len, SQLITE_TRANSIENT) :
      sqlite3_bind_null(scan->stmt, 3);
  if (rc == SQLITE_OK)
    rc = sqlite3_bind_int64(scan->stmt, 4, (sqlite3_int64)read->sequence);
  if (rc != SQLITE_OK) {
    status = fail_sql(read->sql, rc, error);
    (void)sqlite3_finalize(scan->stmt);
    free(scan);
    return status;
  }
  read->scan = scan;
  sqlite3_progress_handler(read->sql, 100, revision_scan_progress, scan);
  *out_scan = scan;
  return result(error, TURBODB_STATUS_OK, NULL);
}

turbodb_status_t turbodb_revision_scan_next(
    turbodb_revision_scan_t *scan, turbodb_blob_t *out_key,
    turbodb_revision_value_t **out_value, turbodb_error_t *error) {
  if (out_key) *out_key = (turbodb_blob_t){NULL, 0};
  if (out_value) *out_value = NULL;
  if (!scan || !out_key || !out_value)
    return result(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid scan output");
  if (scan->done)
    return result(error, TURBODB_STATUS_NOT_FOUND, "end of prefix scan");
  if (scan->expired)
    return result(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQLite scan VM budget exhausted");

  const int rc = sqlite3_step(scan->stmt);
  if (rc == SQLITE_DONE) {
    scan->done = 1;
    return result(error, TURBODB_STATUS_NOT_FOUND, "end of prefix scan");
  }
  if (rc == SQLITE_INTERRUPT && scan->expired)
    return result(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQLite scan VM budget exhausted");
  if (rc != SQLITE_ROW) return fail_sql(scan->read->sql, rc, error);
  if (scan->items >= scan->max_items) {
    scan->expired = 1;
    return result(error, TURBODB_STATUS_LIMIT_EXCEEDED, "prefix row budget exceeded");
  }
  if (sqlite3_column_type(scan->stmt, 0) != SQLITE_BLOB ||
      sqlite3_column_type(scan->stmt, 1) != SQLITE_INTEGER ||
      sqlite3_column_type(scan->stmt, 2) != SQLITE_INTEGER) {
    scan->expired = 1;
    return result(error, TURBODB_STATUS_DATASTORE_ERROR, "corrupt prefix scan row");
  }
  int key_len = sqlite3_column_bytes(scan->stmt, 0);
  sqlite3_int64 seq = sqlite3_column_int64(scan->stmt, 1);
  int deleted = sqlite3_column_int(scan->stmt, 2);
  int body_len = sqlite3_column_bytes(scan->stmt, 3);
  if (key_len <= 0 || key_len > (int)REVISION_MAX_KEY ||
      seq <= 0 || seq > (sqlite3_int64)scan->read->sequence ||
      (deleted != 0 && deleted != 1) || body_len < 0 ||
      (uint64_t)body_len > scan->read->owner->max_value_bytes ||
      (deleted && sqlite3_column_type(scan->stmt, 3) != SQLITE_NULL) ||
      (!deleted && sqlite3_column_type(scan->stmt, 3) != SQLITE_BLOB)) {
    scan->expired = 1;
    return result(error, TURBODB_STATUS_DATASTORE_ERROR, "invalid persisted prefix value");
  }
  const uint64_t bytes = (uint64_t)key_len + (uint64_t)body_len;
  if (bytes > scan->max_bytes - scan->used_bytes) {
    scan->expired = 1;
    return result(error, TURBODB_STATUS_LIMIT_EXCEEDED, "prefix byte budget exceeded");
  }
  const void *key_data = sqlite3_column_blob(scan->stmt, 0);
  if (!key_data) {
    scan->expired = 1;
    return result(error, TURBODB_STATUS_DATASTORE_ERROR, "null persisted key");
  }
  turbodb_revision_value_t *value = (turbodb_revision_value_t *)calloc(1, sizeof(*value));
  if (!value) return result(error, TURBODB_STATUS_OUT_OF_MEMORY, "allocate scan result");
  value->sequence = (uint64_t)seq;
  value->deleted = deleted;
  if (!deleted) {
    value->data = (unsigned char *)malloc(body_len ? (size_t)body_len : 1u);
    if (!value->data) {
      free(value);
      return result(error, TURBODB_STATUS_OUT_OF_MEMORY, "allocate scan bytes");
    }
    if (body_len) {
      const void *body = sqlite3_column_blob(scan->stmt, 3);
      if (!body) {
        free(value->data);
        free(value);
        scan->expired = 1;
        return result(error, TURBODB_STATUS_DATASTORE_ERROR, "null persisted scan data");
      }
      memcpy(value->data, body, (size_t)body_len);
    }
    value->size = (size_t)body_len;
  }
  scan->used_bytes += bytes;
  scan->items++;
  *out_key = (turbodb_blob_t){key_data, (size_t)key_len};
  *out_value = value;
  return result(error, TURBODB_STATUS_OK, NULL);
}

void turbodb_revision_scan_close(turbodb_revision_scan_t *scan) {
  if (!scan) return;
  turbodb_revision_read_t *read = scan->read;
  sqlite3_progress_handler(read->sql, 0, NULL, NULL);
  (void)sqlite3_finalize(scan->stmt);
  read->scan = NULL;
  free(scan);
}

void turbodb_revision_open_config_init(turbodb_revision_open_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->struct_size = (uint32_t)sizeof(*config);
  config->abi_version = TURBODB_REVISION_ABI_VERSION;
  config->busy_timeout_ms = 250u;
  config->max_database_bytes = REVISION_DEFAULT_MAX_DB;
  config->max_value_bytes = REVISION_DEFAULT_MAX_VALUE;
  config->max_batch_bytes = REVISION_DEFAULT_MAX_BATCH_BYTES;
  config->max_batch_operations = REVISION_DEFAULT_MAX_BATCH_OPERATIONS;
}

turbodb_status_t turbodb_revision_db_open(const turbodb_revision_open_config_t *cfg,
                                          turbodb_revision_db_t **out_db,
                                          turbodb_error_t *error) {
  if (out_db) *out_db = NULL;
  if (!cfg || !out_db || cfg->struct_size != sizeof(*cfg) ||
      cfg->abi_version != TURBODB_REVISION_ABI_VERSION ||
      !is_view(cfg->path, 4096u) || cfg->max_database_bytes < 65536 ||
      cfg->max_database_bytes > INT64_MAX ||
      cfg->max_value_bytes == 0 || cfg->max_value_bytes > INT_MAX - 4096 ||
      cfg->max_batch_operations == 0 ||
      cfg->max_batch_operations > REVISION_MAX_BATCH_OPERATIONS ||
      cfg->max_batch_bytes == 0 ||
      cfg->max_batch_bytes > REVISION_MAX_BATCH_BYTES ||
      cfg->busy_timeout_ms > INT_MAX ||
      (cfg->flags & ~(TURBODB_REVISION_OPEN_CREATE |
                      TURBODB_REVISION_OPEN_READ_ONLY |
                      TURBODB_REVISION_OPEN_WAL)) != 0 ||
      ((cfg->flags & TURBODB_REVISION_OPEN_CREATE) &&
       (cfg->flags & TURBODB_REVISION_OPEN_READ_ONLY)) ||
      (cfg->path.len == 8 && memcmp(cfg->path.data, ":memory:", 8) == 0) ||
      (cfg->path.len >= 5 && memcmp(cfg->path.data, "file:", 5) == 0))
    return result(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid SQLite revision open");
  turbodb_revision_db_t *db = (turbodb_revision_db_t *)calloc(1, sizeof(*db));
  if (!db) return result(error, TURBODB_STATUS_OUT_OF_MEMORY, "allocate SQLite owner");
  db->path = (char *)malloc(cfg->path.len + 1);
  if (!db->path) { free(db); return result(error, TURBODB_STATUS_OUT_OF_MEMORY, "copy path"); }
  memcpy(db->path, cfg->path.data, cfg->path.len);
  db->path[cfg->path.len] = 0;
  db->flags = cfg->flags;
  db->busy_timeout_ms = cfg->busy_timeout_ms;
  db->max_value_bytes = cfg->max_value_bytes;
  db->max_batch_operations = cfg->max_batch_operations;
  db->max_batch_bytes = cfg->max_batch_bytes;
  const int readonly = (cfg->flags & TURBODB_REVISION_OPEN_READ_ONLY) != 0;
  int flags = SQLITE_OPEN_FULLMUTEX | (readonly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE);
  if (cfg->flags & TURBODB_REVISION_OPEN_CREATE) flags |= SQLITE_OPEN_CREATE;
  int rc = sqlite3_open_v2(db->path, &db->sql, flags, NULL);
  turbodb_status_t status = rc == SQLITE_OK ? TURBODB_STATUS_OK : fail_sql(db->sql, rc, error);
  if (status != TURBODB_STATUS_OK) goto fail;
  rc = sqlite3_extended_result_codes(db->sql, 1);
  if (rc != SQLITE_OK) { status = fail_sql(db->sql, rc, error); goto fail; }
  rc = sqlite3_busy_timeout(db->sql, (int)cfg->busy_timeout_ms);
  if (rc != SQLITE_OK) { status = fail_sql(db->sql, rc, error); goto fail; }
  sqlite3_int64 app_id = 0, version = 0, objects = 0;
  status = pragma_integer(db->sql, "PRAGMA application_id", &app_id, error);
  if (status != TURBODB_STATUS_OK) goto fail;
  status = pragma_integer(db->sql, "PRAGMA user_version", &version, error);
  if (status != TURBODB_STATUS_OK) goto fail;
  status = pragma_integer(db->sql,
     "SELECT count(*) FROM sqlite_master WHERE type IN ('table','view','index','trigger')",
     &objects, error);
  if (status != TURBODB_STATUS_OK) goto fail;
  const int is_new = app_id == 0 && version == 0 && objects == 0 &&
                     (cfg->flags & TURBODB_REVISION_OPEN_CREATE);
  if (!is_new && (app_id != REVISION_APPLICATION_ID || version != REVISION_SCHEMA_VERSION)) {
    status = result(error, TURBODB_STATUS_UNSUPPORTED, "foreign or unsupported SQLite schema");
    goto fail;
  }
  if (readonly && is_new) {
    status = result(error, TURBODB_STATUS_UNSUPPORTED, "read-only cannot initialize schema");
    goto fail;
  }
  if (!readonly) {
    const char *mode_query = (cfg->flags & TURBODB_REVISION_OPEN_WAL) ?
      "PRAGMA journal_mode=WAL" : "PRAGMA journal_mode=DELETE";
    sqlite3_stmt *mode = NULL;
    status = prepare(db->sql, mode_query, &mode, error);
    if (status != TURBODB_STATUS_OK) goto fail;
    rc = sqlite3_step(mode);
    const char *text = rc == SQLITE_ROW ? (const char *)sqlite3_column_text(mode, 0) : NULL;
    const char *expected = (cfg->flags & TURBODB_REVISION_OPEN_WAL) ? "wal" : "delete";
    if (!text || strcmp(text, expected) != 0)
      status = result(error, TURBODB_STATUS_UNSUPPORTED, "SQLite journal mode mismatch");
    rc = sqlite3_finalize(mode);
    if (status == TURBODB_STATUS_OK && rc != SQLITE_OK) status = fail_sql(db->sql, rc, error);
    if (status != TURBODB_STATUS_OK) goto fail;
    status = execute(db->sql, "PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON; PRAGMA trusted_schema=OFF;", error);
    if (status != TURBODB_STATUS_OK) goto fail;
  }
  if (is_new) {
    status = execute(db->sql,
      "BEGIN IMMEDIATE;"
      REVISION_COMMITS_DDL ";"
      REVISION_VALUES_DDL ";"
      REVISION_HEADS_DDL ";"
      REVISION_INDEX_DDL ";"
      "PRAGMA application_id=1413763670;" /* 0x54445256 */
      "PRAGMA user_version=1;"
      "COMMIT;", error);
    if (status != TURBODB_STATUS_OK) {
      (void)sqlite3_exec(db->sql, "ROLLBACK", NULL, NULL, NULL);
      goto fail;
    }
  }
  status = verify_schema(db->sql, error);
  if (status != TURBODB_STATUS_OK) goto fail;
  if (!readonly) {
    sqlite3_int64 page_size = 0, page_count = 0;
    status = pragma_integer(db->sql, "PRAGMA page_size", &page_size, error);
    if (status != TURBODB_STATUS_OK) goto fail;
    status = pragma_integer(db->sql, "PRAGMA page_count", &page_count, error);
    if (status != TURBODB_STATUS_OK) goto fail;
    if (page_size <= 0 || page_count < 0 ||
        (uint64_t)page_count > cfg->max_database_bytes / (uint64_t)page_size) {
      status = result(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQLite database size exceeds configured limit");
      goto fail;
    }
    char query[96];
    (void)snprintf(query, sizeof(query), "PRAGMA max_page_count=%llu",
                   (unsigned long long)(cfg->max_database_bytes / (uint64_t)page_size));
    sqlite3_int64 permitted = 0;
    status = pragma_integer(db->sql, query, &permitted, error);
    if (status != TURBODB_STATUS_OK) goto fail;
    if (permitted > (sqlite3_int64)(cfg->max_database_bytes / (uint64_t)page_size)) {
      status = result(error, TURBODB_STATUS_LIMIT_EXCEEDED, "unable to set SQLite page limit");
      goto fail;
    }
  }
  *out_db = db;
  return result(error, TURBODB_STATUS_OK, NULL);
fail:
  if (db->sql) (void)sqlite3_close_v2(db->sql);
  free(db->path);
  free(db);
  return status;
}

turbodb_status_t turbodb_revision_db_close(turbodb_revision_db_t *db,
                                           turbodb_error_t *error) {
  if (!db) return result(error, TURBODB_STATUS_INVALID_ARGUMENT, "null SQLite owner");
  if (db->active_write || db->active_reads)
    return result(error, TURBODB_STATUS_BUSY, "SQLite revision leases still active");
  const int rc = sqlite3_close(db->sql);
  if (rc != SQLITE_OK) return fail_sql(db->sql, rc, error);
  free(db->path);
  free(db);
  return result(error, TURBODB_STATUS_OK, NULL);
}

static turbodb_status_t query_max_seq(sqlite3 *sql, uint64_t *seq,
                                      turbodb_error_t *error) {
  sqlite3_int64 max = 0;
  turbodb_status_t status = pragma_integer(sql,
     "SELECT COALESCE(MAX(commit_seq),0) FROM revision_commits", &max, error);
  if (status == TURBODB_STATUS_OK) *seq = (uint64_t)max;
  return status;
}

turbodb_status_t turbodb_revision_begin_read(turbodb_revision_db_t *db,
                                             uint64_t as_of_seq,
                                             turbodb_revision_read_t **out_read,
                                             turbodb_error_t *error) {
  if (out_read) *out_read = NULL;
  if (!db || !out_read || db->quarantined || as_of_seq > INT64_MAX)
    return result(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid SQLite read request");
  turbodb_revision_read_t *read = (turbodb_revision_read_t *)calloc(1, sizeof(*read));
  if (!read) return result(error, TURBODB_STATUS_OUT_OF_MEMORY, "allocate read lease");
  read->owner = db;
  int rc = sqlite3_open_v2(db->path, &read->sql, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL);
  turbodb_status_t status = rc == SQLITE_OK ? TURBODB_STATUS_OK : fail_sql(read->sql, rc, error);
  if (status != TURBODB_STATUS_OK) goto fail;
  rc = sqlite3_busy_timeout(read->sql, (int)db->busy_timeout_ms);
  if (rc != SQLITE_OK) { status = fail_sql(read->sql, rc, error); goto fail; }
  status = execute(read->sql, "BEGIN", error);
  if (status != TURBODB_STATUS_OK) goto fail;
  uint64_t last = 0;
  status = query_max_seq(read->sql, &last, error); /* establishes the SQLite snapshot */
  if (status != TURBODB_STATUS_OK) goto fail;
  if (as_of_seq > last) {
    status = result(error, TURBODB_STATUS_OUT_OF_RANGE, "future revision sequence");
    goto fail;
  }
  read->sequence = as_of_seq ? as_of_seq : last;
  db->active_reads++;
  *out_read = read;
  return result(error, TURBODB_STATUS_OK, NULL);
fail:
  if (read->sql) {
    (void)sqlite3_exec(read->sql, "ROLLBACK", NULL, NULL, NULL);
    (void)sqlite3_close_v2(read->sql);
  }
  free(read);
  return status;
}

uint64_t turbodb_revision_read_sequence(const turbodb_revision_read_t *read) {
  return read ? read->sequence : 0;
}
void turbodb_revision_read_close(turbodb_revision_read_t *read) {
  if (!read) return;
  if (read->scan) turbodb_revision_scan_close(read->scan);
  (void)sqlite3_exec(read->sql, "ROLLBACK", NULL, NULL, NULL);
  (void)sqlite3_close_v2(read->sql);
  read->owner->active_reads--;
  free(read);
}

turbodb_status_t turbodb_revision_read_get(turbodb_revision_read_t *read,
                                           turbodb_string_view_t ns,
                                           turbodb_blob_t key,
                                           turbodb_revision_value_t **out_value,
                                           turbodb_error_t *error) {
  if (out_value) *out_value = NULL;
  if (!read || !out_value || !valid_key(ns, key))
    return result(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid read key");
  if (read->scan)
    return result(error, TURBODB_STATUS_BUSY, "close active prefix cursor before get");
  sqlite3_stmt *stmt = NULL;
  turbodb_status_t status = prepare(read->sql,
     "SELECT commit_seq,is_deleted,payload FROM revision_values WHERE namespace=?1 "
     "AND item_key=?2 AND commit_seq<=?3 ORDER BY commit_seq DESC LIMIT 1", &stmt, error);
  if (status != TURBODB_STATUS_OK) return status;
  int rc = bind_text(stmt, 1, ns);
  if (rc == SQLITE_OK) rc = bind_blob(stmt, 2, key);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, (sqlite3_int64)read->sequence);
  if (rc != SQLITE_OK) { status = fail_sql(read->sql, rc, error); goto done; }
  rc = sqlite3_step(stmt);
  if (rc == SQLITE_DONE) { status = result(error, TURBODB_STATUS_NOT_FOUND, "no revision for key"); goto done; }
  if (rc != SQLITE_ROW) { status = fail_sql(read->sql, rc, error); goto done; }
  sqlite3_int64 seq = sqlite3_column_int64(stmt, 0);
  int deleted = sqlite3_column_int(stmt, 1);
  int length = sqlite3_column_bytes(stmt, 2);
  if (seq <= 0 || (deleted != 0 && deleted != 1) || length < 0 ||
      (uint64_t)length > read->owner->max_value_bytes ||
      (deleted && sqlite3_column_type(stmt, 2) != SQLITE_NULL) ||
      (!deleted && sqlite3_column_type(stmt, 2) != SQLITE_BLOB)) {
    status = result(error, TURBODB_STATUS_DATASTORE_ERROR, "invalid persisted revision value");
    goto done;
  }
  turbodb_revision_value_t *value = (turbodb_revision_value_t *)calloc(1, sizeof(*value));
  if (!value) { status = result(error, TURBODB_STATUS_OUT_OF_MEMORY, "allocate result"); goto done; }
  value->sequence = (uint64_t)seq;
  value->deleted = deleted;
  if (!deleted) {
    value->data = (unsigned char *)malloc((size_t)length ? (size_t)length : 1u);
    if (!value->data) {
      free(value);
      status = result(error, TURBODB_STATUS_OUT_OF_MEMORY, "allocate value");
      goto done;
    }
    if (length > 0) {
      const void *bytes = sqlite3_column_blob(stmt, 2);
      if (!bytes) {
        free(value->data); free(value);
        status = result(error, TURBODB_STATUS_DATASTORE_ERROR, "missing persisted bytes");
        goto done;
      }
      memcpy(value->data, bytes, (size_t)length);
    }
    value->size = (size_t)length;
  }
  *out_value = value;
  status = result(error, TURBODB_STATUS_OK, NULL);
done:
  rc = sqlite3_finalize(stmt);
  if (status == TURBODB_STATUS_OK && rc != SQLITE_OK) {
    turbodb_revision_value_destroy(*out_value);
    *out_value = NULL;
    return fail_sql(read->sql, rc, error);
  }
  return status;
}
uint64_t turbodb_revision_value_sequence(const turbodb_revision_value_t *v) { return v ? v->sequence : 0; }
int turbodb_revision_value_is_deleted(const turbodb_revision_value_t *v) { return v ? v->deleted : 0; }
turbodb_blob_t turbodb_revision_value_bytes(const turbodb_revision_value_t *v) {
  return v && !v->deleted ? (turbodb_blob_t){v->data, v->size} : (turbodb_blob_t){NULL, 0};
}
void turbodb_revision_value_destroy(turbodb_revision_value_t *v) {
  if (v) { free(v->data); free(v); }
}

turbodb_status_t turbodb_revision_begin_write(turbodb_revision_db_t *db,
                                              turbodb_blob_t request_id,
                                              turbodb_revision_write_t **out_write,
                                              turbodb_error_t *error) {
  if (out_write) *out_write = NULL;
  if (!db || !out_write || !is_blob(request_id, REVISION_MAX_REQUEST_ID))
    return result(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid revision request ID");
  if ((db->flags & TURBODB_REVISION_OPEN_READ_ONLY) || db->quarantined)
    return result(error, TURBODB_STATUS_INVALID_STATE, "read-only or quarantined SQLite owner");
  if (db->active_write) return result(error, TURBODB_STATUS_BUSY, "one writer already active");
  turbodb_status_t status = execute(db->sql, "BEGIN IMMEDIATE", error);
  if (status != TURBODB_STATUS_OK) return status;
  sqlite3_stmt *stmt = NULL;
  status = prepare(db->sql,
    "SELECT commit_seq FROM revision_commits WHERE request_id=?1", &stmt, error);
  if (status != TURBODB_STATUS_OK) goto rollback;
  int rc = bind_blob(stmt, 1, request_id);
  if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW)
    status = result(error, TURBODB_STATUS_CONSTRAINT, "request ID is already committed");
  else if (rc != SQLITE_DONE)
    status = fail_sql(db->sql, rc, error);
  (void)sqlite3_finalize(stmt);
  if (status != TURBODB_STATUS_OK) goto rollback;
  uint64_t max = 0;
  status = query_max_seq(db->sql, &max, error);
  if (status != TURBODB_STATUS_OK) goto rollback;
  if (max >= INT64_MAX - 1) {
    status = result(error, TURBODB_STATUS_LIMIT_EXCEEDED, "revision sequence exhausted");
    goto rollback;
  }
  turbodb_revision_write_t *write = (turbodb_revision_write_t *)calloc(1, sizeof(*write));
  if (!write) {
    status = result(error, TURBODB_STATUS_OUT_OF_MEMORY, "allocate revision transaction");
    goto rollback;
  }
  write->owner = db;
  write->sequence = max + 1;
  stmt = NULL;
  status = prepare(db->sql,
     "INSERT INTO revision_commits(commit_seq,request_id) VALUES(?1,?2)", &stmt, error);
  if (status == TURBODB_STATUS_OK) {
    rc = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)write->sequence);
    if (rc == SQLITE_OK) rc = bind_blob(stmt, 2, request_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) status = fail_sql(db->sql, rc, error);
  }
  if (stmt) (void)sqlite3_finalize(stmt);
  if (status != TURBODB_STATUS_OK) { free(write); goto rollback; }
  db->active_write = 1;
  *out_write = write;
  return result(error, TURBODB_STATUS_OK, NULL);
rollback:
  (void)sqlite3_exec(db->sql, "ROLLBACK", NULL, NULL, NULL);
  return status;
}

static turbodb_status_t mutate(turbodb_revision_write_t *write,
                               turbodb_string_view_t ns, turbodb_blob_t key,
                               turbodb_blob_t data, int deleted, uint64_t expected,
                               turbodb_error_t *error) {
  if (!write) return result(error, TURBODB_STATUS_INVALID_ARGUMENT, "null write handle");
  if (!valid_key(ns, key) || (!deleted && (data.size && !data.data))) {
    write->poisoned = 1;
    return result(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid revision mutation");
  }
  if (write->poisoned) return result(error, TURBODB_STATUS_INVALID_STATE, "transaction failed; abort required");
  turbodb_revision_db_t *db = write->owner;
  if (data.size > db->max_value_bytes || data.size > INT_MAX) {
    write->poisoned = 1;
    return result(error, TURBODB_STATUS_LIMIT_EXCEEDED, "revision value too large");
  }
  /* Include key and namespace bytes: many small records consume a bounded
   * transaction budget just as much as a single large content blob. */
  const uint64_t needed = (uint64_t)ns.len + (uint64_t)key.size +
                          (uint64_t)data.size;
  if (write->operations >= db->max_batch_operations ||
      needed > db->max_batch_bytes - write->used_bytes) {
    write->poisoned = 1;
    return result(error, TURBODB_STATUS_LIMIT_EXCEEDED,
                  "revision transaction budget exceeded; abort required");
  }
  sqlite3_stmt *stmt = NULL;
  turbodb_status_t status = prepare(db->sql,
     "SELECT commit_seq FROM revision_heads WHERE namespace=?1 AND item_key=?2", &stmt, error);
  if (status != TURBODB_STATUS_OK) goto fail;
  int rc = bind_text(stmt, 1, ns);
  if (rc == SQLITE_OK) rc = bind_blob(stmt, 2, key);
  if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
  uint64_t head = 0;
  if (rc == SQLITE_ROW) {
    sqlite3_int64 v = sqlite3_column_int64(stmt, 0);
    if (v <= 0) { status = result(error, TURBODB_STATUS_DATASTORE_ERROR, "corrupt revision head"); goto fail; }
    head = (uint64_t)v;
  } else if (rc != SQLITE_DONE) { status = fail_sql(db->sql, rc, error); goto fail; }
  (void)sqlite3_finalize(stmt); stmt = NULL;
  if (head == write->sequence ||
      (expected != TURBODB_REVISION_EXPECT_ANY && head != expected)) {
    status = result(error, TURBODB_STATUS_CONSTRAINT, "head CAS conflict or duplicate transaction key");
    goto fail;
  }
  status = prepare(db->sql,
     "INSERT INTO revision_values(namespace,item_key,commit_seq,is_deleted,payload) "
     "VALUES(?1,?2,?3,?4,?5)", &stmt, error);
  if (status != TURBODB_STATUS_OK) goto fail;
  rc = bind_text(stmt, 1, ns);
  if (rc == SQLITE_OK) rc = bind_blob(stmt, 2, key);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, (sqlite3_int64)write->sequence);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 4, deleted);
  if (rc == SQLITE_OK) rc = deleted ? sqlite3_bind_null(stmt, 5) :
       sqlite3_bind_blob(stmt, 5, data.data ? data.data : "", (int)data.size, SQLITE_TRANSIENT);
  if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) { status = fail_sql(db->sql, rc, error); goto fail; }
  (void)sqlite3_finalize(stmt); stmt = NULL;
  status = prepare(db->sql,
     "INSERT INTO revision_heads(namespace,item_key,commit_seq) VALUES(?1,?2,?3) "
     "ON CONFLICT(namespace,item_key) DO UPDATE SET commit_seq=excluded.commit_seq", &stmt, error);
  if (status != TURBODB_STATUS_OK) goto fail;
  rc = bind_text(stmt, 1, ns);
  if (rc == SQLITE_OK) rc = bind_blob(stmt, 2, key);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, (sqlite3_int64)write->sequence);
  if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) { status = fail_sql(db->sql, rc, error); goto fail; }
  (void)sqlite3_finalize(stmt);
  write->used_bytes += needed;
  write->operations++;
  return result(error, TURBODB_STATUS_OK, NULL);
fail:
  if (stmt) (void)sqlite3_finalize(stmt);
  write->poisoned = 1; /* caller must abort; no partial publish */
  return status;
}
turbodb_status_t turbodb_revision_write_put(turbodb_revision_write_t *write,
                                            turbodb_string_view_t ns,
                                            turbodb_blob_t key,
                                            turbodb_blob_t value,
                                            uint64_t expected_head_seq,
                                            turbodb_error_t *error) {
  return mutate(write, ns, key, value, 0, expected_head_seq, error);
}
turbodb_status_t turbodb_revision_write_delete(turbodb_revision_write_t *write,
                                               turbodb_string_view_t ns,
                                               turbodb_blob_t key,
                                               uint64_t expected_head_seq,
                                               turbodb_error_t *error) {
  return mutate(write, ns, key, (turbodb_blob_t){NULL, 0}, 1, expected_head_seq, error);
}
turbodb_status_t turbodb_revision_write_publish(turbodb_revision_write_t *write,
                                                uint64_t *out_commit_seq,
                                                turbodb_error_t *error) {
  if (out_commit_seq) *out_commit_seq = 0;
  if (!write || !out_commit_seq) return result(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid commit request");
  if (write->poisoned) return result(error, TURBODB_STATUS_INVALID_STATE, "abort failed write transaction");
  turbodb_revision_db_t *db = write->owner;
  const int rc = sqlite3_exec(db->sql, "COMMIT", NULL, NULL, NULL);
  if (rc == SQLITE_OK) {
    *out_commit_seq = write->sequence;
    db->active_write = 0;
    free(write);
    return result(error, TURBODB_STATUS_OK, NULL);
  }
  if ((rc & 255) == SQLITE_BUSY || (rc & 255) == SQLITE_LOCKED)
    return fail_sql(db->sql, rc, error); /* still an active txn; caller aborts */
  db->quarantined = 1;
  return result(error, TURBODB_STATUS_COMMIT_UNKNOWN,
                "SQLite commit outcome is uncertain; close and inspect request ID");
}
void turbodb_revision_write_abort(turbodb_revision_write_t *write) {
  if (!write) return;
  turbodb_revision_db_t *db = write->owner;
  (void)sqlite3_exec(db->sql, "ROLLBACK", NULL, NULL, NULL);
  db->active_write = 0;
  free(write);
}

turbodb_status_t turbodb_revision_inspect_request(turbodb_revision_db_t *db,
                                                  turbodb_blob_t request_id,
                                                  uint64_t *out_commit_seq,
                                                  turbodb_error_t *error) {
  if (out_commit_seq) *out_commit_seq = 0;
  if (!db || !out_commit_seq || !is_blob(request_id, REVISION_MAX_REQUEST_ID) || db->quarantined)
    return result(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid inspect request; use clean owner after uncertain commit");
  if (db->active_write)
    return result(error, TURBODB_STATUS_BUSY, "inspect on active writer is not committed-state inspection");
  sqlite3_stmt *stmt = NULL;
  turbodb_status_t status = prepare(db->sql,
    "SELECT commit_seq FROM revision_commits WHERE request_id=?1", &stmt, error);
  if (status != TURBODB_STATUS_OK) return status;
  int rc = bind_blob(stmt, 1, request_id);
  if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    sqlite3_int64 seq = sqlite3_column_int64(stmt, 0);
    if (seq <= 0) status = result(error, TURBODB_STATUS_DATASTORE_ERROR, "invalid persisted sequence");
    else { *out_commit_seq = (uint64_t)seq; status = result(error, TURBODB_STATUS_OK, NULL); }
  } else if (rc == SQLITE_DONE)
    status = result(error, TURBODB_STATUS_NOT_FOUND, "request not committed");
  else status = fail_sql(db->sql, rc, error);
  const int fin = sqlite3_finalize(stmt);
  if (status == TURBODB_STATUS_OK && fin != SQLITE_OK)
    return fail_sql(db->sql, fin, error);
  return status;
}

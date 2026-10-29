#ifndef TURBODB_SQLITE_REVISION_H
#define TURBODB_SQLITE_REVISION_H

#include <turbodb/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBODB_REVISION_ABI_VERSION UINT32_C(1)
#define TURBODB_REVISION_OPEN_CREATE UINT32_C(1)
#define TURBODB_REVISION_OPEN_READ_ONLY UINT32_C(2)
#define TURBODB_REVISION_OPEN_WAL UINT32_C(4)
#define TURBODB_REVISION_EXPECT_ABSENT UINT64_C(0)
#define TURBODB_REVISION_EXPECT_ANY UINT64_MAX

typedef struct turbodb_revision_db_s turbodb_revision_db_t;
typedef struct turbodb_revision_read_s turbodb_revision_read_t;
typedef struct turbodb_revision_write_s turbodb_revision_write_t;
typedef struct turbodb_revision_value_s turbodb_revision_value_t;

typedef struct turbodb_revision_open_config_s {
  uint32_t struct_size;
  uint32_t abi_version;
  turbodb_string_view_t path;  /* borrowed during open; local filesystem only */
  uint32_t flags;              /* CREATE, READ_ONLY, or WAL; no implicit mode switch */
  uint32_t busy_timeout_ms;
  uint64_t max_database_bytes; /* main database, not transient journal/WAL */
  uint64_t max_value_bytes;    /* maximum individual payload */
  uint64_t max_batch_bytes;    /* total namespace + key + value bytes per write */
  uint32_t max_batch_operations; /* finite number of mutations per commit */
} turbodb_revision_open_config_t;

void turbodb_revision_open_config_init(turbodb_revision_open_config_t *config);

turbodb_status_t turbodb_revision_db_open(const turbodb_revision_open_config_t *config,
                                          turbodb_revision_db_t **out_db,
                                          turbodb_error_t *error);
/* Returns BUSY if read/write handles remain; a failed close does not consume db. */
turbodb_status_t turbodb_revision_db_close(turbodb_revision_db_t *db,
                                           turbodb_error_t *error);

/* An owned, stable read transaction. as_of_seq=0 means latest visible at BEGIN.
 * Historical sequence is application-level and remains readable after WAL
 * checkpoint, provided the data has not been explicitly removed by a future GC.
 * Readers close before db_close. */
turbodb_status_t turbodb_revision_begin_read(turbodb_revision_db_t *db,
                                             uint64_t as_of_seq,
                                             turbodb_revision_read_t **out_read,
                                             turbodb_error_t *error);
turbodb_status_t turbodb_revision_read_get(turbodb_revision_read_t *read,
                                           turbodb_string_view_t ns,
                                           turbodb_blob_t key,
                                           turbodb_revision_value_t **out_value,
                                           turbodb_error_t *error);
uint64_t turbodb_revision_read_sequence(const turbodb_revision_read_t *read);
void turbodb_revision_read_close(turbodb_revision_read_t *read);

/* A single writer owns BEGIN IMMEDIATE until publish or abort. request_id must
 * be stable across a retried *inspection*, never replay an uncertain commit.
 * EXPECT_ABSENT=0; EXPECT_ANY=UINT64_MAX; else require exact prior head seq.
 * One mutation per (namespace,key) in a transaction. */
turbodb_status_t turbodb_revision_begin_write(turbodb_revision_db_t *db,
                                              turbodb_blob_t request_id,
                                              turbodb_revision_write_t **out_write,
                                              turbodb_error_t *error);
turbodb_status_t turbodb_revision_write_put(turbodb_revision_write_t *write,
                                            turbodb_string_view_t ns,
                                            turbodb_blob_t key,
                                            turbodb_blob_t value,
                                            uint64_t expected_head_seq,
                                            turbodb_error_t *error);
turbodb_status_t turbodb_revision_write_delete(turbodb_revision_write_t *write,
                                               turbodb_string_view_t ns,
                                               turbodb_blob_t key,
                                               uint64_t expected_head_seq,
                                               turbodb_error_t *error);
/* Successful publish consumes write; on failure write remains owned for abort.
 * COMMIT_UNKNOWN quarantines connection; close it and inspect request in a
 * newly opened read-only connection. There is no automatic retry. */
turbodb_status_t turbodb_revision_write_publish(turbodb_revision_write_t *write,
                                                uint64_t *out_commit_seq,
                                                turbodb_error_t *error);
void turbodb_revision_write_abort(turbodb_revision_write_t *write);

/* Durable request inspection, not automatic replay. Returns NOT_FOUND only if
 * the committed request_id does not exist in the opened database snapshot. */
turbodb_status_t turbodb_revision_inspect_request(turbodb_revision_db_t *db,
                                                  turbodb_blob_t request_id,
                                                  uint64_t *out_commit_seq,
                                                  turbodb_error_t *error);

/* A successful get returns independently owned bytes, even for an empty blob.
 * Tombstones are represented by is_deleted=1, bytes=NULL, length=0. */
uint64_t turbodb_revision_value_sequence(const turbodb_revision_value_t *value);
int turbodb_revision_value_is_deleted(const turbodb_revision_value_t *value);
turbodb_blob_t turbodb_revision_value_bytes(const turbodb_revision_value_t *value);
void turbodb_revision_value_destroy(turbodb_revision_value_t *value);

#ifdef __cplusplus
}
#endif
#endif

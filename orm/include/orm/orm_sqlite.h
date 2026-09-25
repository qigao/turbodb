#ifndef ORM_SQLITE_H
#define ORM_SQLITE_H

#include "orm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ORM_SQLITE_FILE_COPY_ABI_VERSION UINT32_C(1)
#define ORM_SQLITE_DEFAULT_BUSY_TIMEOUT_MS UINT32_C(5000)
#define ORM_SQLITE_DEFAULT_PAGES_PER_STEP UINT32_C(64)
#define ORM_SQLITE_DEFAULT_MAX_BUSY_RETRIES UINT32_C(64)

typedef uint32_t orm_sqlite_publication_state_t;
enum {
  ORM_SQLITE_PUBLICATION_NOT_PUBLISHED = 0u,
  ORM_SQLITE_PUBLICATION_PUBLISHED_DURABLE = 1u,
  ORM_SQLITE_PUBLICATION_DURABILITY_UNKNOWN = 2u
};

typedef struct orm_sqlite_file_copy_config {
  uint32_t struct_size;
  uint32_t abi_version;
  orm_string_view_t source_path;
  orm_string_view_t staging_path;
  orm_string_view_t destination_path;
  uint32_t busy_timeout_ms;
  uint32_t pages_per_step;
  uint32_t max_busy_retries;
  uint32_t reserved;
} orm_sqlite_file_copy_config;

typedef struct orm_sqlite_file_copy_result {
  uint32_t struct_size;
  uint32_t abi_version;
  orm_sqlite_publication_state_t publication_state;
  uint32_t reserved;
  uint64_t pages_copied;
} orm_sqlite_file_copy_result;

ORM_C_API void ORM_C_CALL orm_sqlite_file_copy_config_init(
    orm_sqlite_file_copy_config *config);

ORM_C_API void ORM_C_CALL orm_sqlite_file_copy_result_init(
    orm_sqlite_file_copy_result *result);

/*
 * Create a point-in-time, file-backed SQLite checkpoint.
 *
 * source_path names the source database. staging_path must not already exist.
 * On successful publication, staging_path is consumed and destination_path
 * names the durable checkpoint. On a pre-publication failure the staging file,
 * when created, remains caller-owned for cleanup.
 */
ORM_C_API orm_status_t ORM_C_CALL orm_sqlite_checkpoint_create(
    const orm_sqlite_file_copy_config *config,
    orm_sqlite_file_copy_result *result,
    orm_error_t *error);

/*
 * Restore a checkpoint into an offline SQLite database path.
 *
 * The destination database must not have live SQLite/ORM handles while this
 * call publishes the validated staging database.
 */
ORM_C_API orm_status_t ORM_C_CALL orm_sqlite_restore_publish(
    const orm_sqlite_file_copy_config *config,
    orm_sqlite_file_copy_result *result,
    orm_error_t *error);

#ifdef __cplusplus
}
#endif

#endif

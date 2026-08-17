#ifndef ORM_TIDESDB_BRIDGE_H
#define ORM_TIDESDB_BRIDGE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void orm_tidesdb_database_t;
typedef void orm_tidesdb_column_family_t;
typedef void orm_tidesdb_transaction_t;
typedef void orm_tidesdb_iterator_t;
typedef int orm_tidesdb_isolation_level_t;

typedef struct orm_tidesdb_config { char *db_path; } orm_tidesdb_config_t;
typedef struct orm_tidesdb_column_family_config { uint8_t reserved; }
    orm_tidesdb_column_family_config_t;

enum {
    ORM_TDB_SUCCESS = 0,
    ORM_TDB_ERR_MEMORY = -1,
    ORM_TDB_ERR_INVALID_ARGS = -2,
    ORM_TDB_ERR_NOT_FOUND = -3,
    ORM_TDB_ERR_IO = -4,
    ORM_TDB_ERR_CORRUPTION = -5,
    ORM_TDB_ERR_EXISTS = -6,
    ORM_TDB_ERR_CONFLICT = -7,
    ORM_TDB_ERR_TOO_LARGE = -8,
    ORM_TDB_ERR_MEMORY_LIMIT = -9,
    ORM_TDB_ERR_INVALID_DB = -10,
    ORM_TDB_ERR_UNKNOWN = -11,
    ORM_TDB_ERR_LOCKED = -12,
    ORM_TDB_ERR_READONLY = -13,
    ORM_TDB_ERR_BUSY = -14,
    ORM_TDB_ERR_PRECONDITION = -15,
    ORM_TDB_ISOLATION_READ_UNCOMMITTED = 0,
    ORM_TDB_ISOLATION_READ_COMMITTED = 1,
    ORM_TDB_ISOLATION_REPEATABLE_READ = 2,
    ORM_TDB_ISOLATION_SNAPSHOT = 3,
    ORM_TDB_ISOLATION_SERIALIZABLE = 4
};

orm_tidesdb_config_t orm_tidesdb_default_config(void);
orm_tidesdb_column_family_config_t orm_tidesdb_default_column_family_config(void);
int orm_tidesdb_open(const orm_tidesdb_config_t *, orm_tidesdb_database_t **);
int orm_tidesdb_close(orm_tidesdb_database_t *);
int orm_tidesdb_create_column_family(orm_tidesdb_database_t *, const char *,
                                     const orm_tidesdb_column_family_config_t *);
orm_tidesdb_column_family_t *orm_tidesdb_get_column_family(orm_tidesdb_database_t *,
                                                           const char *);
int orm_tidesdb_txn_begin_with_isolation(orm_tidesdb_database_t *,
                                         orm_tidesdb_isolation_level_t,
                                         orm_tidesdb_transaction_t **);
int orm_tidesdb_txn_put(orm_tidesdb_transaction_t *, orm_tidesdb_column_family_t *,
                        const uint8_t *, size_t, const uint8_t *, size_t, time_t);
int orm_tidesdb_txn_get(orm_tidesdb_transaction_t *, orm_tidesdb_column_family_t *,
                        const uint8_t *, size_t, uint8_t **, size_t *);
int orm_tidesdb_txn_delete(orm_tidesdb_transaction_t *, orm_tidesdb_column_family_t *,
                           const uint8_t *, size_t);
int orm_tidesdb_txn_commit(orm_tidesdb_transaction_t *);
int orm_tidesdb_txn_rollback(orm_tidesdb_transaction_t *);
int orm_tidesdb_txn_savepoint(orm_tidesdb_transaction_t *, const char *);
int orm_tidesdb_txn_rollback_to_savepoint(orm_tidesdb_transaction_t *, const char *);
int orm_tidesdb_txn_release_savepoint(orm_tidesdb_transaction_t *, const char *);
void orm_tidesdb_txn_free(orm_tidesdb_transaction_t *);
int orm_tidesdb_iter_new(orm_tidesdb_transaction_t *, orm_tidesdb_column_family_t *,
                         orm_tidesdb_iterator_t **);
int orm_tidesdb_iter_seek(orm_tidesdb_iterator_t *, const uint8_t *, size_t);
int orm_tidesdb_iter_next(orm_tidesdb_iterator_t *);
int orm_tidesdb_iter_valid(orm_tidesdb_iterator_t *);
int orm_tidesdb_iter_key(orm_tidesdb_iterator_t *, uint8_t **, size_t *);
int orm_tidesdb_iter_value(orm_tidesdb_iterator_t *, uint8_t **, size_t *);
void orm_tidesdb_iter_free(orm_tidesdb_iterator_t *);
void orm_tidesdb_free(void *);

#ifdef __cplusplus
}

#define tidesdb_t orm_tidesdb_database_t
#define tidesdb_column_family_t orm_tidesdb_column_family_t
#define tidesdb_txn_t orm_tidesdb_transaction_t
#define tidesdb_iter_t orm_tidesdb_iterator_t
#define tidesdb_isolation_level_t orm_tidesdb_isolation_level_t
#define tidesdb_config_t orm_tidesdb_config_t
#define tidesdb_column_family_config_t orm_tidesdb_column_family_config_t
#define tidesdb_default_config orm_tidesdb_default_config
#define tidesdb_default_column_family_config orm_tidesdb_default_column_family_config
#define tidesdb_open orm_tidesdb_open
#define tidesdb_close orm_tidesdb_close
#define tidesdb_create_column_family orm_tidesdb_create_column_family
#define tidesdb_get_column_family orm_tidesdb_get_column_family
#define tidesdb_txn_begin_with_isolation orm_tidesdb_txn_begin_with_isolation
#define tidesdb_txn_put orm_tidesdb_txn_put
#define tidesdb_txn_get orm_tidesdb_txn_get
#define tidesdb_txn_delete orm_tidesdb_txn_delete
#define tidesdb_txn_commit orm_tidesdb_txn_commit
#define tidesdb_txn_rollback orm_tidesdb_txn_rollback
#define tidesdb_txn_savepoint orm_tidesdb_txn_savepoint
#define tidesdb_txn_rollback_to_savepoint orm_tidesdb_txn_rollback_to_savepoint
#define tidesdb_txn_release_savepoint orm_tidesdb_txn_release_savepoint
#define tidesdb_txn_free orm_tidesdb_txn_free
#define tidesdb_iter_new orm_tidesdb_iter_new
#define tidesdb_iter_seek orm_tidesdb_iter_seek
#define tidesdb_iter_next orm_tidesdb_iter_next
#define tidesdb_iter_valid orm_tidesdb_iter_valid
#define tidesdb_iter_key orm_tidesdb_iter_key
#define tidesdb_iter_value orm_tidesdb_iter_value
#define tidesdb_iter_free orm_tidesdb_iter_free
#define tidesdb_free orm_tidesdb_free
#define TDB_SUCCESS ORM_TDB_SUCCESS
#define TDB_ERR_MEMORY ORM_TDB_ERR_MEMORY
#define TDB_ERR_INVALID_ARGS ORM_TDB_ERR_INVALID_ARGS
#define TDB_ERR_NOT_FOUND ORM_TDB_ERR_NOT_FOUND
#define TDB_ERR_IO ORM_TDB_ERR_IO
#define TDB_ERR_CORRUPTION ORM_TDB_ERR_CORRUPTION
#define TDB_ERR_EXISTS ORM_TDB_ERR_EXISTS
#define TDB_ERR_CONFLICT ORM_TDB_ERR_CONFLICT
#define TDB_ERR_TOO_LARGE ORM_TDB_ERR_TOO_LARGE
#define TDB_ERR_MEMORY_LIMIT ORM_TDB_ERR_MEMORY_LIMIT
#define TDB_ERR_INVALID_DB ORM_TDB_ERR_INVALID_DB
#define TDB_ERR_UNKNOWN ORM_TDB_ERR_UNKNOWN
#define TDB_ERR_LOCKED ORM_TDB_ERR_LOCKED
#define TDB_ERR_READONLY ORM_TDB_ERR_READONLY
#define TDB_ERR_BUSY ORM_TDB_ERR_BUSY
#define TDB_ERR_PRECONDITION ORM_TDB_ERR_PRECONDITION
#define TDB_ISOLATION_READ_UNCOMMITTED ORM_TDB_ISOLATION_READ_UNCOMMITTED
#define TDB_ISOLATION_READ_COMMITTED ORM_TDB_ISOLATION_READ_COMMITTED
#define TDB_ISOLATION_REPEATABLE_READ ORM_TDB_ISOLATION_REPEATABLE_READ
#define TDB_ISOLATION_SNAPSHOT ORM_TDB_ISOLATION_SNAPSHOT
#define TDB_ISOLATION_SERIALIZABLE ORM_TDB_ISOLATION_SERIALIZABLE
#endif

#endif

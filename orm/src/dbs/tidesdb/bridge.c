#include "bridge.h"

#include <tidesdb.h>

#define ORM_TDB_ASSERT_VALUE(native_name, bridge_name) \
    _Static_assert((int)(native_name) == (int)(bridge_name), #native_name " value changed")

ORM_TDB_ASSERT_VALUE(TDB_SUCCESS, ORM_TDB_SUCCESS);
ORM_TDB_ASSERT_VALUE(TDB_ERR_MEMORY, ORM_TDB_ERR_MEMORY);
ORM_TDB_ASSERT_VALUE(TDB_ERR_INVALID_ARGS, ORM_TDB_ERR_INVALID_ARGS);
ORM_TDB_ASSERT_VALUE(TDB_ERR_NOT_FOUND, ORM_TDB_ERR_NOT_FOUND);
ORM_TDB_ASSERT_VALUE(TDB_ERR_IO, ORM_TDB_ERR_IO);
ORM_TDB_ASSERT_VALUE(TDB_ERR_CORRUPTION, ORM_TDB_ERR_CORRUPTION);
ORM_TDB_ASSERT_VALUE(TDB_ERR_EXISTS, ORM_TDB_ERR_EXISTS);
ORM_TDB_ASSERT_VALUE(TDB_ERR_CONFLICT, ORM_TDB_ERR_CONFLICT);
ORM_TDB_ASSERT_VALUE(TDB_ERR_TOO_LARGE, ORM_TDB_ERR_TOO_LARGE);
ORM_TDB_ASSERT_VALUE(TDB_ERR_MEMORY_LIMIT, ORM_TDB_ERR_MEMORY_LIMIT);
ORM_TDB_ASSERT_VALUE(TDB_ERR_INVALID_DB, ORM_TDB_ERR_INVALID_DB);
ORM_TDB_ASSERT_VALUE(TDB_ERR_UNKNOWN, ORM_TDB_ERR_UNKNOWN);
ORM_TDB_ASSERT_VALUE(TDB_ERR_LOCKED, ORM_TDB_ERR_LOCKED);
ORM_TDB_ASSERT_VALUE(TDB_ERR_READONLY, ORM_TDB_ERR_READONLY);
ORM_TDB_ASSERT_VALUE(TDB_ERR_BUSY, ORM_TDB_ERR_BUSY);
ORM_TDB_ASSERT_VALUE(TDB_ERR_PRECONDITION, ORM_TDB_ERR_PRECONDITION);
_Static_assert((int)TDB_ISOLATION_READ_UNCOMMITTED == 0,
               "TDB_ISOLATION_READ_UNCOMMITTED value changed");
_Static_assert((int)TDB_ISOLATION_READ_COMMITTED == 1,
               "TDB_ISOLATION_READ_COMMITTED value changed");
_Static_assert((int)TDB_ISOLATION_REPEATABLE_READ == 2,
               "TDB_ISOLATION_REPEATABLE_READ value changed");
_Static_assert((int)TDB_ISOLATION_SNAPSHOT == 3,
               "TDB_ISOLATION_SNAPSHOT value changed");
_Static_assert((int)TDB_ISOLATION_SERIALIZABLE == 4,
               "TDB_ISOLATION_SERIALIZABLE value changed");

orm_tidesdb_config_t orm_tidesdb_default_config(void)
{
    const orm_tidesdb_config_t config = {0};
    return config;
}

orm_tidesdb_column_family_config_t orm_tidesdb_default_column_family_config(void)
{
    const orm_tidesdb_column_family_config_t config = {0};
    return config;
}

int orm_tidesdb_open(const orm_tidesdb_config_t *config,
                     orm_tidesdb_database_t **database)
{
    tidesdb_config_t native_config;
    tidesdb_t *native_database = NULL;
    int status;
    if (config == NULL || config->db_path == NULL || database == NULL)
        return TDB_ERR_INVALID_ARGS;
    native_config = tidesdb_default_config();
    native_config.db_path = config->db_path;
    status = tidesdb_open(&native_config, &native_database);
    *database = (orm_tidesdb_database_t *)native_database;
    return status;
}

int orm_tidesdb_close(orm_tidesdb_database_t *database)
{
    return tidesdb_close((tidesdb_t *)database);
}

int orm_tidesdb_create_column_family(
    orm_tidesdb_database_t *database,
    const char *name,
    const orm_tidesdb_column_family_config_t *config)
{
    tidesdb_column_family_config_t native_config;
    if (config == NULL)
        return TDB_ERR_INVALID_ARGS;
    native_config = tidesdb_default_column_family_config();
    return tidesdb_create_column_family((tidesdb_t *)database, name, &native_config);
}

orm_tidesdb_column_family_t *orm_tidesdb_get_column_family(
    orm_tidesdb_database_t *database,
    const char *name)
{
    return (orm_tidesdb_column_family_t *)tidesdb_get_column_family(
        (tidesdb_t *)database, name);
}

int orm_tidesdb_txn_begin_with_isolation(
    orm_tidesdb_database_t *database,
    orm_tidesdb_isolation_level_t isolation,
    orm_tidesdb_transaction_t **transaction)
{
    tidesdb_txn_t *native_transaction = NULL;
    tidesdb_isolation_level_t native_isolation;
    int status;
    if (transaction == NULL)
        return TDB_ERR_INVALID_ARGS;
    if (isolation == ORM_TDB_ISOLATION_READ_UNCOMMITTED)
        native_isolation = TDB_ISOLATION_READ_UNCOMMITTED;
    else if (isolation == ORM_TDB_ISOLATION_READ_COMMITTED)
        native_isolation = TDB_ISOLATION_READ_COMMITTED;
    else if (isolation == ORM_TDB_ISOLATION_REPEATABLE_READ)
        native_isolation = TDB_ISOLATION_REPEATABLE_READ;
    else if (isolation == ORM_TDB_ISOLATION_SNAPSHOT)
        native_isolation = TDB_ISOLATION_SNAPSHOT;
    else if (isolation == ORM_TDB_ISOLATION_SERIALIZABLE)
        native_isolation = TDB_ISOLATION_SERIALIZABLE;
    else
        return TDB_ERR_INVALID_ARGS;
    status = tidesdb_txn_begin_with_isolation(
        (tidesdb_t *)database, native_isolation, &native_transaction);
    *transaction = (orm_tidesdb_transaction_t *)native_transaction;
    return status;
}

int orm_tidesdb_txn_put(orm_tidesdb_transaction_t *transaction,
                        orm_tidesdb_column_family_t *column_family,
                        const uint8_t *key, size_t key_size,
                        const uint8_t *value, size_t value_size, time_t ttl)
{
    return tidesdb_txn_put((tidesdb_txn_t *)transaction,
                           (tidesdb_column_family_t *)column_family,
                           key, key_size, value, value_size, ttl);
}

int orm_tidesdb_txn_get(orm_tidesdb_transaction_t *transaction,
                        orm_tidesdb_column_family_t *column_family,
                        const uint8_t *key, size_t key_size,
                        uint8_t **value, size_t *value_size)
{
    return tidesdb_txn_get((tidesdb_txn_t *)transaction,
                           (tidesdb_column_family_t *)column_family,
                           key, key_size, value, value_size);
}

int orm_tidesdb_txn_delete(orm_tidesdb_transaction_t *transaction,
                           orm_tidesdb_column_family_t *column_family,
                           const uint8_t *key, size_t key_size)
{
    return tidesdb_txn_delete((tidesdb_txn_t *)transaction,
                              (tidesdb_column_family_t *)column_family,
                              key, key_size);
}

int orm_tidesdb_txn_commit(orm_tidesdb_transaction_t *transaction)
{
    return tidesdb_txn_commit((tidesdb_txn_t *)transaction);
}

int orm_tidesdb_txn_rollback(orm_tidesdb_transaction_t *transaction)
{
    return tidesdb_txn_rollback((tidesdb_txn_t *)transaction);
}

int orm_tidesdb_txn_savepoint(orm_tidesdb_transaction_t *transaction,
                              const char *name)
{
    return tidesdb_txn_savepoint((tidesdb_txn_t *)transaction, name);
}

int orm_tidesdb_txn_rollback_to_savepoint(
    orm_tidesdb_transaction_t *transaction,
    const char *name)
{
    return tidesdb_txn_rollback_to_savepoint((tidesdb_txn_t *)transaction, name);
}

int orm_tidesdb_txn_release_savepoint(orm_tidesdb_transaction_t *transaction,
                                      const char *name)
{
    return tidesdb_txn_release_savepoint((tidesdb_txn_t *)transaction, name);
}

void orm_tidesdb_txn_free(orm_tidesdb_transaction_t *transaction)
{
    tidesdb_txn_free((tidesdb_txn_t *)transaction);
}

int orm_tidesdb_iter_new(orm_tidesdb_transaction_t *transaction,
                         orm_tidesdb_column_family_t *column_family,
                         orm_tidesdb_iterator_t **iterator)
{
    tidesdb_iter_t *native_iterator = NULL;
    int status;
    if (iterator == NULL)
        return TDB_ERR_INVALID_ARGS;
    status = tidesdb_iter_new((tidesdb_txn_t *)transaction,
                              (tidesdb_column_family_t *)column_family,
                              &native_iterator);
    *iterator = (orm_tidesdb_iterator_t *)native_iterator;
    return status;
}

int orm_tidesdb_iter_seek(orm_tidesdb_iterator_t *iterator,
                          const uint8_t *key, size_t key_size)
{
    return tidesdb_iter_seek((tidesdb_iter_t *)iterator, key, key_size);
}

int orm_tidesdb_iter_next(orm_tidesdb_iterator_t *iterator)
{
    return tidesdb_iter_next((tidesdb_iter_t *)iterator);
}

int orm_tidesdb_iter_valid(orm_tidesdb_iterator_t *iterator)
{
    return tidesdb_iter_valid((tidesdb_iter_t *)iterator);
}

int orm_tidesdb_iter_key(orm_tidesdb_iterator_t *iterator,
                         uint8_t **key, size_t *key_size)
{
    return tidesdb_iter_key((tidesdb_iter_t *)iterator, key, key_size);
}

int orm_tidesdb_iter_value(orm_tidesdb_iterator_t *iterator,
                           uint8_t **value, size_t *value_size)
{
    return tidesdb_iter_value((tidesdb_iter_t *)iterator, value, value_size);
}

void orm_tidesdb_iter_free(orm_tidesdb_iterator_t *iterator)
{
    tidesdb_iter_free((tidesdb_iter_t *)iterator);
}

void orm_tidesdb_free(void *pointer)
{
    tidesdb_free(pointer);
}

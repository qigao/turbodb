#include "../src/tidesdb.h"
#include "test_utils.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PROVIDER_REPLAY_DB_PATH "./test_tidesdb_provider_replay"
#define PROVIDER_REPLAY_CF_NAME "provider"

static const uint8_t APP_KEY[] = "app/state";
static const uint8_t APPLIED_KEY[] = "__provider/meta/applied";

typedef enum provider_receipt {
    PROVIDER_APPLIED = 1,
    PROVIDER_REPLAYED = 2,
    PROVIDER_GAP = 3,
    PROVIDER_CONFLICT = 4
} provider_receipt;

static void provider_encode_u64(uint64_t value, uint8_t out[8])
{
    for (int i = 7; i >= 0; --i)
    {
        out[i] = (uint8_t)(value & UINT64_C(0xff));
        value >>= 8;
    }
}

static uint64_t provider_decode_u64(const uint8_t data[8])
{
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
        value = (value << 8) | (uint64_t)data[i];
    return value;
}

static tidesdb_t *provider_open(void)
{
    tidesdb_config_t config = tidesdb_default_config();
    tidesdb_t *db = NULL;
    config.db_path = (char *)PROVIDER_REPLAY_DB_PATH;
    config.log_level = TDB_LOG_NONE;
    ASSERT_EQ(tidesdb_open(&config, &db), TDB_SUCCESS);
    ASSERT_NE(db, NULL);
    return db;
}

static tidesdb_column_family_t *provider_create_cf(tidesdb_t *db)
{
    tidesdb_column_family_config_t config =
        tidesdb_default_column_family_config();
    tidesdb_column_family_t *cf;

    config.sync_mode = TDB_SYNC_FULL;
    config.compression_algorithm = TDB_COMPRESS_NONE;
    ASSERT_EQ(tidesdb_create_column_family(db, PROVIDER_REPLAY_CF_NAME, &config),
              TDB_SUCCESS);
    cf = tidesdb_get_column_family(db, PROVIDER_REPLAY_CF_NAME);
    ASSERT_NE(cf, NULL);
    return cf;
}

static tidesdb_column_family_t *provider_get_cf(tidesdb_t *db)
{
    tidesdb_column_family_t *cf =
        tidesdb_get_column_family(db, PROVIDER_REPLAY_CF_NAME);
    ASSERT_NE(cf, NULL);
    return cf;
}

static uint64_t provider_read_applied(tidesdb_txn_t *txn,
                                      tidesdb_column_family_t *cf)
{
    uint8_t *data = NULL;
    size_t size = 0;
    int rc = tidesdb_txn_get(txn, cf, APPLIED_KEY, sizeof(APPLIED_KEY),
                             &data, &size);
    uint64_t applied = 0;

    if (rc == TDB_ERR_NOT_FOUND)
        return 0;
    ASSERT_EQ(rc, TDB_SUCCESS);
    ASSERT_NE(data, NULL);
    ASSERT_EQ(size, (size_t)8);
    if (data != NULL && size == 8u)
        applied = provider_decode_u64(data);
    tidesdb_free(data);
    return applied;
}

static provider_receipt provider_apply(tidesdb_t *db,
                                       tidesdb_column_family_t *cf,
                                       uint64_t position,
                                       const char *identity,
                                       const char *application_value)
{
    tidesdb_txn_t *txn = NULL;
    uint64_t applied;
    char identity_key[96];
    int key_length;

    ASSERT_TRUE(position != 0u);
    ASSERT_NE(identity, NULL);
    ASSERT_NE(application_value, NULL);
    ASSERT_EQ(tidesdb_txn_begin_with_isolation(
                  db, TDB_ISOLATION_SERIALIZABLE, &txn),
              TDB_SUCCESS);
    ASSERT_NE(txn, NULL);

    applied = provider_read_applied(txn, cf);
    key_length = snprintf(identity_key, sizeof(identity_key),
                          "__provider/meta/identity/%020" PRIu64, position);
    ASSERT_TRUE(key_length > 0);
    ASSERT_TRUE((size_t)key_length < sizeof(identity_key));

    if (position <= applied)
    {
        uint8_t *stored = NULL;
        size_t stored_size = 0;
        const size_t identity_size = strlen(identity);
        const int rc = tidesdb_txn_get(
            txn, cf, (const uint8_t *)identity_key, (size_t)key_length,
            &stored, &stored_size);
        provider_receipt receipt = PROVIDER_CONFLICT;

        if (rc == TDB_SUCCESS && stored != NULL &&
            stored_size == identity_size &&
            memcmp(stored, identity, identity_size) == 0)
            receipt = PROVIDER_REPLAYED;
        else
            ASSERT_TRUE(rc == TDB_SUCCESS || rc == TDB_ERR_NOT_FOUND);

        tidesdb_free(stored);
        ASSERT_EQ(tidesdb_txn_rollback(txn), TDB_SUCCESS);
        tidesdb_txn_free(txn);
        return receipt;
    }

    if (position != applied + UINT64_C(1))
    {
        ASSERT_EQ(tidesdb_txn_rollback(txn), TDB_SUCCESS);
        tidesdb_txn_free(txn);
        return PROVIDER_GAP;
    }

    {
        uint8_t encoded_position[8];
        provider_encode_u64(position, encoded_position);
        ASSERT_EQ(tidesdb_txn_put(
                      txn, cf, APP_KEY, sizeof(APP_KEY),
                      (const uint8_t *)application_value,
                      strlen(application_value) + 1u, 0),
                  TDB_SUCCESS);
        ASSERT_EQ(tidesdb_txn_put(
                      txn, cf, (const uint8_t *)identity_key,
                      (size_t)key_length, (const uint8_t *)identity,
                      strlen(identity), 0),
                  TDB_SUCCESS);
        ASSERT_EQ(tidesdb_txn_put(
                      txn, cf, APPLIED_KEY, sizeof(APPLIED_KEY),
                      encoded_position, sizeof(encoded_position), 0),
                  TDB_SUCCESS);
    }

    ASSERT_EQ(tidesdb_txn_commit(txn), TDB_SUCCESS);
    tidesdb_txn_free(txn);
    return PROVIDER_APPLIED;
}

static void provider_expect_state(tidesdb_t *db, tidesdb_column_family_t *cf,
                                  const char *expected_value,
                                  uint64_t expected_applied)
{
    tidesdb_txn_t *txn = NULL;
    uint8_t *value = NULL;
    size_t value_size = 0;

    ASSERT_EQ(tidesdb_txn_begin(db, &txn), TDB_SUCCESS);
    ASSERT_NE(txn, NULL);
    ASSERT_EQ(tidesdb_txn_get(txn, cf, APP_KEY, sizeof(APP_KEY),
                              &value, &value_size),
              TDB_SUCCESS);
    ASSERT_NE(value, NULL);
    ASSERT_EQ(value_size, strlen(expected_value) + 1u);
    ASSERT_EQ(memcmp(value, expected_value, value_size), 0);
    ASSERT_EQ(provider_read_applied(txn, cf), expected_applied);
    tidesdb_free(value);
    ASSERT_EQ(tidesdb_txn_rollback(txn), TDB_SUCCESS);
    tidesdb_txn_free(txn);
}

static void test_ordered_apply_classifies_without_mutating_rejected_paths(void)
{
    tidesdb_t *db;
    tidesdb_column_family_t *cf;

    (void)remove_directory(PROVIDER_REPLAY_DB_PATH);
    db = provider_open();
    cf = provider_create_cf(db);

    ASSERT_EQ(provider_apply(db, cf, UINT64_C(1), "identity-1", "value-1"),
              PROVIDER_APPLIED);
    provider_expect_state(db, cf, "value-1", UINT64_C(1));

    ASSERT_EQ(provider_apply(db, cf, UINT64_C(1), "identity-1", "value-1"),
              PROVIDER_REPLAYED);
    provider_expect_state(db, cf, "value-1", UINT64_C(1));

    ASSERT_EQ(provider_apply(db, cf, UINT64_C(1), "identity-conflict",
                             "value-conflict"),
              PROVIDER_CONFLICT);
    provider_expect_state(db, cf, "value-1", UINT64_C(1));

    ASSERT_EQ(provider_apply(db, cf, UINT64_C(3), "identity-3", "value-3"),
              PROVIDER_GAP);
    provider_expect_state(db, cf, "value-1", UINT64_C(1));

    ASSERT_EQ(provider_apply(db, cf, UINT64_C(2), "identity-2", "value-2"),
              PROVIDER_APPLIED);
    provider_expect_state(db, cf, "value-2", UINT64_C(2));

    ASSERT_EQ(tidesdb_close(db), TDB_SUCCESS);
    ASSERT_EQ(remove_directory(PROVIDER_REPLAY_DB_PATH), 0);
}

static void test_replay_and_conflict_survive_reopen(void)
{
    tidesdb_t *db;
    tidesdb_column_family_t *cf;

    (void)remove_directory(PROVIDER_REPLAY_DB_PATH);
    db = provider_open();
    cf = provider_create_cf(db);
    ASSERT_EQ(provider_apply(db, cf, UINT64_C(1), "identity-1", "value-1"),
              PROVIDER_APPLIED);
    ASSERT_EQ(provider_apply(db, cf, UINT64_C(2), "identity-2", "value-2"),
              PROVIDER_APPLIED);
    ASSERT_EQ(tidesdb_close(db), TDB_SUCCESS);

    db = provider_open();
    cf = provider_get_cf(db);
    ASSERT_EQ(provider_apply(db, cf, UINT64_C(2), "identity-2", "value-2"),
              PROVIDER_REPLAYED);
    ASSERT_EQ(provider_apply(db, cf, UINT64_C(2), "identity-other",
                             "value-other"),
              PROVIDER_CONFLICT);
    ASSERT_EQ(provider_apply(db, cf, UINT64_C(4), "identity-4", "value-4"),
              PROVIDER_GAP);
    provider_expect_state(db, cf, "value-2", UINT64_C(2));

    ASSERT_EQ(tidesdb_close(db), TDB_SUCCESS);
    ASSERT_EQ(remove_directory(PROVIDER_REPLAY_DB_PATH), 0);
}

int main(int argc, char **argv)
{
    int tests_passed = 0;
    int tests_failed = 0;

    INIT_TEST_FILTER(argc, argv);
    RUN_TEST(test_ordered_apply_classifies_without_mutating_rejected_paths,
             tests_passed);
    RUN_TEST(test_replay_and_conflict_survive_reopen, tests_passed);
    PRINT_TEST_RESULTS(tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}

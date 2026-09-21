#include "../src/tidesdb.h"
#include "test_utils.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PROVIDER_DB_PATH "./test_tidesdb_provider_atomicity"
#define PROVIDER_CF_NAME "provider"

static const uint8_t APP_KEY[] = "app/state";
static const uint8_t META_KEY[] = "__provider/meta/applied";

static int failing_hook_calls = 0;

static int failing_commit_hook(const tidesdb_commit_op_t *ops, int num_ops,
                               uint64_t commit_seq, void *ctx)
{
    (void)ops;
    (void)num_ops;
    (void)commit_seq;
    (void)ctx;
    ++failing_hook_calls;
    return -1;
}

static tidesdb_t *provider_open(void)
{
    tidesdb_config_t config = tidesdb_default_config();
    tidesdb_t *db = NULL;
    config.db_path = (char *)PROVIDER_DB_PATH;
    config.log_level = TDB_LOG_NONE;
    ASSERT_EQ(tidesdb_open(&config, &db), TDB_SUCCESS);
    ASSERT_NE(db, NULL);
    return db;
}

static tidesdb_column_family_t *provider_create_cf(
    tidesdb_t *db, tidesdb_commit_hook_fn hook)
{
    tidesdb_column_family_config_t config =
        tidesdb_default_column_family_config();
    tidesdb_column_family_t *cf;
    config.sync_mode = TDB_SYNC_FULL;
    config.compression_algorithm = TDB_COMPRESS_NONE;
    config.commit_hook_fn = hook;
    config.commit_hook_ctx = NULL;
    ASSERT_EQ(tidesdb_create_column_family(db, PROVIDER_CF_NAME, &config),
              TDB_SUCCESS);
    cf = tidesdb_get_column_family(db, PROVIDER_CF_NAME);
    ASSERT_NE(cf, NULL);
    return cf;
}

static tidesdb_column_family_t *provider_get_cf(tidesdb_t *db)
{
    tidesdb_column_family_t *cf =
        tidesdb_get_column_family(db, PROVIDER_CF_NAME);
    ASSERT_NE(cf, NULL);
    return cf;
}

static void provider_write_pair(tidesdb_t *db, tidesdb_column_family_t *cf,
                                const char *app_value,
                                const char *meta_value, int commit)
{
    tidesdb_txn_t *txn = NULL;
    ASSERT_EQ(tidesdb_txn_begin(db, &txn), TDB_SUCCESS);
    ASSERT_NE(txn, NULL);
    ASSERT_EQ(tidesdb_txn_put(txn, cf, APP_KEY, sizeof(APP_KEY),
                              (const uint8_t *)app_value,
                              strlen(app_value) + 1u, 0),
              TDB_SUCCESS);
    ASSERT_EQ(tidesdb_txn_put(txn, cf, META_KEY, sizeof(META_KEY),
                              (const uint8_t *)meta_value,
                              strlen(meta_value) + 1u, 0),
              TDB_SUCCESS);
    if (commit)
        ASSERT_EQ(tidesdb_txn_commit(txn), TDB_SUCCESS);
    else
        ASSERT_EQ(tidesdb_txn_rollback(txn), TDB_SUCCESS);
    tidesdb_txn_free(txn);
}

static void provider_expect_pair(tidesdb_t *db, tidesdb_column_family_t *cf,
                                 const char *app_value,
                                 const char *meta_value)
{
    tidesdb_txn_t *txn = NULL;
    uint8_t *actual_app = NULL;
    uint8_t *actual_meta = NULL;
    size_t actual_app_size = 0u;
    size_t actual_meta_size = 0u;

    ASSERT_EQ(tidesdb_txn_begin(db, &txn), TDB_SUCCESS);
    ASSERT_NE(txn, NULL);
    ASSERT_EQ(tidesdb_txn_get(txn, cf, APP_KEY, sizeof(APP_KEY),
                              &actual_app, &actual_app_size),
              TDB_SUCCESS);
    ASSERT_EQ(tidesdb_txn_get(txn, cf, META_KEY, sizeof(META_KEY),
                              &actual_meta, &actual_meta_size),
              TDB_SUCCESS);
    ASSERT_EQ(actual_app_size, strlen(app_value) + 1u);
    ASSERT_EQ(actual_meta_size, strlen(meta_value) + 1u);
    ASSERT_EQ(memcmp(actual_app, app_value, actual_app_size), 0);
    ASSERT_EQ(memcmp(actual_meta, meta_value, actual_meta_size), 0);

    tidesdb_free(actual_app);
    tidesdb_free(actual_meta);
    tidesdb_txn_free(txn);
}

static void test_provider_transaction_commit_and_reopen(void)
{
    tidesdb_t *db;
    tidesdb_column_family_t *cf;

    (void)remove_directory(PROVIDER_DB_PATH);
    db = provider_open();
    cf = provider_create_cf(db, NULL);
    provider_write_pair(db, cf, "application-v2", "101", 1);
    ASSERT_EQ(tidesdb_close(db), TDB_SUCCESS);

    db = provider_open();
    cf = provider_get_cf(db);
    provider_expect_pair(db, cf, "application-v2", "101");
    ASSERT_EQ(tidesdb_close(db), TDB_SUCCESS);
    ASSERT_EQ(remove_directory(PROVIDER_DB_PATH), 0);
}

static void test_provider_transaction_rollback_and_reopen(void)
{
    tidesdb_t *db;
    tidesdb_column_family_t *cf;

    (void)remove_directory(PROVIDER_DB_PATH);
    db = provider_open();
    cf = provider_create_cf(db, NULL);
    provider_write_pair(db, cf, "application-v1", "100", 1);
    provider_write_pair(db, cf, "application-v2", "101", 0);
    ASSERT_EQ(tidesdb_close(db), TDB_SUCCESS);

    db = provider_open();
    cf = provider_get_cf(db);
    provider_expect_pair(db, cf, "application-v1", "100");
    ASSERT_EQ(tidesdb_close(db), TDB_SUCCESS);
    ASSERT_EQ(remove_directory(PROVIDER_DB_PATH), 0);
}

static void test_failing_commit_hook_does_not_own_durability(void)
{
    tidesdb_t *db;
    tidesdb_column_family_t *cf;

    (void)remove_directory(PROVIDER_DB_PATH);
    failing_hook_calls = 0;

    db = provider_open();
    cf = provider_create_cf(db, failing_commit_hook);
    provider_write_pair(db, cf, "application-hook", "202", 1);
    ASSERT_TRUE(failing_hook_calls > 0);
    ASSERT_EQ(tidesdb_close(db), TDB_SUCCESS);

    db = provider_open();
    cf = provider_get_cf(db);
    provider_expect_pair(db, cf, "application-hook", "202");
    ASSERT_EQ(tidesdb_close(db), TDB_SUCCESS);
    ASSERT_EQ(remove_directory(PROVIDER_DB_PATH), 0);
}

int main(int argc, char **argv)
{
    int tests_passed = 0;
    int tests_failed = 0;

    INIT_TEST_FILTER(argc, argv);
    RUN_TEST(test_provider_transaction_commit_and_reopen, tests_passed);
    RUN_TEST(test_provider_transaction_rollback_and_reopen, tests_passed);
    RUN_TEST(test_failing_commit_hook_does_not_own_durability, tests_passed);
    PRINT_TEST_RESULTS(tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}

#include "../src/tidesdb.h"
#include "test_utils.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PROVIDER_DB_PATH "./test_tidesdb_provider_checkpoint_source"
#define PROVIDER_CHECKPOINT_PATH "./test_tidesdb_provider_checkpoint_image"
#define PROVIDER_CF_NAME "provider"

static const uint8_t APP_KEY[] = "app/state";
static const uint8_t META_KEY[] = "__provider/meta/applied";

static tidesdb_t *provider_checkpoint_open(const char *path)
{
    tidesdb_config_t config = tidesdb_default_config();
    tidesdb_t *db = NULL;
    config.db_path = (char *)path;
    config.log_level = TDB_LOG_NONE;
    ASSERT_EQ(tidesdb_open(&config, &db), TDB_SUCCESS);
    ASSERT_NE(db, NULL);
    return db;
}

static tidesdb_column_family_t *provider_checkpoint_create_cf(tidesdb_t *db)
{
    tidesdb_column_family_config_t config =
        tidesdb_default_column_family_config();
    tidesdb_column_family_t *cf;

    config.sync_mode = TDB_SYNC_FULL;
    config.compression_algorithm = TDB_COMPRESS_NONE;

    ASSERT_EQ(tidesdb_create_column_family(db, PROVIDER_CF_NAME, &config),
              TDB_SUCCESS);
    cf = tidesdb_get_column_family(db, PROVIDER_CF_NAME);
    ASSERT_NE(cf, NULL);
    return cf;
}

static tidesdb_column_family_t *provider_checkpoint_get_cf(tidesdb_t *db)
{
    tidesdb_column_family_t *cf =
        tidesdb_get_column_family(db, PROVIDER_CF_NAME);
    ASSERT_NE(cf, NULL);
    return cf;
}

static void provider_checkpoint_write_pair(
    tidesdb_t *db, tidesdb_column_family_t *cf,
    const char *app_value, const char *meta_value)
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
    ASSERT_EQ(tidesdb_txn_commit(txn), TDB_SUCCESS);
    tidesdb_txn_free(txn);
}

static void provider_checkpoint_expect_pair(
    tidesdb_t *db, tidesdb_column_family_t *cf,
    const char *app_value, const char *meta_value)
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

static void cleanup_provider_checkpoint_paths(void)
{
    (void)remove_directory(PROVIDER_CHECKPOINT_PATH);
    (void)remove_directory(PROVIDER_DB_PATH);
}

static void test_checkpoint_reopens_at_completed_boundary(void)
{
    tidesdb_t *source;
    tidesdb_t *checkpoint;
    tidesdb_column_family_t *cf;

    cleanup_provider_checkpoint_paths();

    source = provider_checkpoint_open(PROVIDER_DB_PATH);
    cf = provider_checkpoint_create_cf(source);
    provider_checkpoint_write_pair(source, cf, "application-v1", "100");

    ASSERT_EQ(tidesdb_checkpoint(source, PROVIDER_CHECKPOINT_PATH),
              TDB_SUCCESS);

    /* Mutate the source only after the completed checkpoint. The reopened
     * checkpoint must remain at the completed v1/100 boundary. */
    provider_checkpoint_write_pair(source, cf, "application-v2", "101");
    provider_checkpoint_expect_pair(source, cf, "application-v2", "101");
    ASSERT_EQ(tidesdb_close(source), TDB_SUCCESS);

    checkpoint = provider_checkpoint_open(PROVIDER_CHECKPOINT_PATH);
    cf = provider_checkpoint_get_cf(checkpoint);
    provider_checkpoint_expect_pair(checkpoint, cf, "application-v1", "100");
    ASSERT_EQ(tidesdb_close(checkpoint), TDB_SUCCESS);

    cleanup_provider_checkpoint_paths();
}

static void test_checkpoint_rejects_nonempty_destination(void)
{
    tidesdb_t *source;
    tidesdb_column_family_t *cf;

    cleanup_provider_checkpoint_paths();

    source = provider_checkpoint_open(PROVIDER_DB_PATH);
    cf = provider_checkpoint_create_cf(source);
    provider_checkpoint_write_pair(source, cf, "application-v1", "100");

    ASSERT_EQ(tidesdb_checkpoint(source, PROVIDER_CHECKPOINT_PATH),
              TDB_SUCCESS);
    ASSERT_EQ(tidesdb_checkpoint(source, PROVIDER_CHECKPOINT_PATH),
              TDB_ERR_EXISTS);

    ASSERT_EQ(tidesdb_close(source), TDB_SUCCESS);
    cleanup_provider_checkpoint_paths();
}

int main(int argc, char **argv)
{
    int tests_passed = 0;
    int tests_failed = 0;

    INIT_TEST_FILTER(argc, argv);
    RUN_TEST(test_checkpoint_reopens_at_completed_boundary, tests_passed);
    RUN_TEST(test_checkpoint_rejects_nonempty_destination, tests_passed);
    PRINT_TEST_RESULTS(tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}

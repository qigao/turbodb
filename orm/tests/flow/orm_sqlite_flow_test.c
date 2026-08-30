#include "orm_cbind_publisher.h"
#include "orm_sqlite_cursor.h"

#include <cmeta/struct.h>
#include <sqlite3.h>
#include <turbo_cmeta_data.h>
#include <turbo_str.h>
#include "tinytest.h"

#include <stddef.h>
#include <stdlib.h>

#define ORM_SQLITE_TEST_DATA_PREFIX_SIZE \
  (offsetof(cmeta_data_desc, shape) + sizeof(((cmeta_data_desc *)0)->shape))

static const orm_sqlite_cursor_config orm_sqlite_test_cursor_config =
    ORM_SQLITE_CURSOR_CONFIG_INIT(UINT64_MAX, UINT64_MAX);

Struct(orm_sqlite_test_row,
    (int, id),
    (long, score)
);

static const cmeta_type_identity orm_sqlite_test_row_identity =
    CMETA_TYPE_ID_ATOM_INIT("orm.test.SqliteRow");
static const cmeta_type_traits orm_sqlite_test_row_traits = {
    .flags = CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY
};
static const cmeta_type_desc orm_sqlite_test_row_type = {
    .name = "orm_sqlite_test_row",
    .size = sizeof(orm_sqlite_test_row),
    .align = _Alignof(orm_sqlite_test_row),
    .kind = CMETA_T_OBJECT,
    .traits = &orm_sqlite_test_row_traits,
    .identity = &orm_sqlite_test_row_identity
};
static const cmeta_data_field_desc orm_sqlite_test_row_fields[] = {
    {"orm.test.SqliteRow.id", "id", offsetof(orm_sqlite_test_row, id),
     &cmeta_data_int},
    {"orm.test.SqliteRow.score", "score", offsetof(orm_sqlite_test_row, score),
     &cmeta_data_long}
};
static const cmeta_data_struct_shape orm_sqlite_test_row_shape = {
    .layout = StructMeta(orm_sqlite_test_row),
    .fields = orm_sqlite_test_row_fields,
    .field_count = sizeof(orm_sqlite_test_row_fields) /
                   sizeof(orm_sqlite_test_row_fields[0])
};
static const cmeta_data_desc orm_sqlite_test_row_data = {
    .struct_size = ORM_SQLITE_TEST_DATA_PREFIX_SIZE,
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orm.test.SqliteRow.data",
    .display_name = "SqliteRow",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &orm_sqlite_test_row_type,
    .shape = &orm_sqlite_test_row_shape
};

Struct(orm_sqlite_test_text_row,
    (int, id),
    (tstr, name)
);

static bool orm_sqlite_test_text_row_copy(void *destination_,
                                          const void *source_) {
  orm_sqlite_test_text_row *destination =
      (orm_sqlite_test_text_row *)destination_;
  const orm_sqlite_test_text_row *source =
      (const orm_sqlite_test_text_row *)source_;
  destination->id = source->id;
  destination->name = source->name == NULL ? NULL : tstr_dup(source->name);
  return source->name == NULL || destination->name != NULL;
}

static void orm_sqlite_test_text_row_move(void *destination_, void *source_) {
  orm_sqlite_test_text_row *destination =
      (orm_sqlite_test_text_row *)destination_;
  orm_sqlite_test_text_row *source = (orm_sqlite_test_text_row *)source_;
  *destination = *source;
  source->id = 0;
  source->name = NULL;
}

static void orm_sqlite_test_text_row_destroy(void *value_) {
  orm_sqlite_test_text_row *value = (orm_sqlite_test_text_row *)value_;
  tstr_freep(&value->name);
  value->id = 0;
}

static const cmeta_type_identity orm_sqlite_test_text_row_identity =
    CMETA_TYPE_ID_ATOM_INIT("orm.test.SqliteTextRow");
static const cmeta_type_traits orm_sqlite_test_text_row_traits = {
    .flags = CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY,
    .copy_construct = orm_sqlite_test_text_row_copy,
    .move_construct = orm_sqlite_test_text_row_move,
    .destroy = orm_sqlite_test_text_row_destroy
};
static const cmeta_type_desc orm_sqlite_test_text_row_type = {
    .name = "orm_sqlite_test_text_row",
    .size = sizeof(orm_sqlite_test_text_row),
    .align = _Alignof(orm_sqlite_test_text_row),
    .kind = CMETA_T_OBJECT,
    .traits = &orm_sqlite_test_text_row_traits,
    .identity = &orm_sqlite_test_text_row_identity
};
static const cmeta_data_buffer_shape orm_sqlite_test_owned_string_shape = {
    .ownership = CMETA_DATA_BUFFER_OWNED
};
static const cmeta_data_desc orm_sqlite_test_owned_string_data = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orm.test.owned-string",
    .display_name = "owned string",
    .kind = CMETA_DATA_STRING,
    .storage_type = &turbo_tstr_cmeta_type,
    .shape = &orm_sqlite_test_owned_string_shape,
    .buffer_ops = &turbo_tstr_cmeta_buffer_ops
};
static const cmeta_data_desc orm_sqlite_test_owned_bytes_data = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orm.test.owned-bytes",
    .display_name = "owned bytes",
    .kind = CMETA_DATA_BYTES,
    .storage_type = &turbo_tstr_cmeta_type,
    .shape = &orm_sqlite_test_owned_string_shape,
    .buffer_ops = &turbo_tstr_cmeta_buffer_ops
};
static const cmeta_data_field_desc orm_sqlite_test_text_row_fields[] = {
    {"orm.test.SqliteTextRow.id", "id", offsetof(orm_sqlite_test_text_row, id),
     &cmeta_data_int},
    {"orm.test.SqliteTextRow.name", "name",
     offsetof(orm_sqlite_test_text_row, name),
     &orm_sqlite_test_owned_string_data}
};
static const cmeta_data_struct_shape orm_sqlite_test_text_row_shape = {
    .layout = StructMeta(orm_sqlite_test_text_row),
    .fields = orm_sqlite_test_text_row_fields,
    .field_count = sizeof(orm_sqlite_test_text_row_fields) /
                   sizeof(orm_sqlite_test_text_row_fields[0])
};
static const cmeta_data_desc orm_sqlite_test_text_row_data = {
    .struct_size = ORM_SQLITE_TEST_DATA_PREFIX_SIZE,
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orm.test.SqliteTextRow.data",
    .display_name = "SqliteTextRow",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &orm_sqlite_test_text_row_type,
    .shape = &orm_sqlite_test_text_row_shape
};
static const cmeta_data_field_desc orm_sqlite_test_blob_row_fields[] = {
    {"orm.test.SqliteBlobRow.id", "id", offsetof(orm_sqlite_test_text_row, id),
     &cmeta_data_int},
    {"orm.test.SqliteBlobRow.name", "name",
     offsetof(orm_sqlite_test_text_row, name),
     &orm_sqlite_test_owned_bytes_data}
};
static const cmeta_data_struct_shape orm_sqlite_test_blob_row_shape = {
    .layout = StructMeta(orm_sqlite_test_text_row),
    .fields = orm_sqlite_test_blob_row_fields,
    .field_count = sizeof(orm_sqlite_test_blob_row_fields) /
                   sizeof(orm_sqlite_test_blob_row_fields[0])
};
static const cmeta_data_desc orm_sqlite_test_blob_row_data = {
    .struct_size = ORM_SQLITE_TEST_DATA_PREFIX_SIZE,
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orm.test.SqliteBlobRow.data",
    .display_name = "SqliteBlobRow",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &orm_sqlite_test_text_row_type,
    .shape = &orm_sqlite_test_blob_row_shape
};

typedef struct orm_sqlite_test_sink_state {
  orm_sqlite_test_row last;
  size_t values;
  size_t dones;
  const char *error;
} orm_sqlite_test_sink_state;

typedef struct orm_sqlite_parity_sink_state {
  orm_sqlite_test_row rows[2];
  size_t values;
  size_t dones;
  const char *error;
} orm_sqlite_parity_sink_state;

static bool orm_sqlite_parity_sink_value(void *context,
                                          const cmeta_type_desc *type,
                                          const void *value) {
  orm_sqlite_parity_sink_state *state =
      (orm_sqlite_parity_sink_state *)context;
  if (!cmeta_type_equal(type, &orm_sqlite_test_row_type) || value == NULL ||
      state->values == 2u)
    return false;
  state->rows[state->values++] = *(const orm_sqlite_test_row *)value;
  return true;
}

static void orm_sqlite_parity_sink_error(void *context,
                                          const char *message) {
  orm_sqlite_parity_sink_state *state =
      (orm_sqlite_parity_sink_state *)context;
  state->error = message;
}

static void orm_sqlite_parity_sink_done(void *context) {
  orm_sqlite_parity_sink_state *state =
      (orm_sqlite_parity_sink_state *)context;
  ++state->dones;
}

static bool orm_sqlite_test_sink_value(void *context,
                                       const cmeta_type_desc *type,
                                       const void *value) {
  orm_sqlite_test_sink_state *state =
      (orm_sqlite_test_sink_state *)context;
  if (!cmeta_type_equal(type, &orm_sqlite_test_row_type) || value == NULL)
    return false;
  state->last = *(const orm_sqlite_test_row *)value;
  ++state->values;
  return true;
}

static void orm_sqlite_test_sink_error(void *context, const char *message) {
  orm_sqlite_test_sink_state *state =
      (orm_sqlite_test_sink_state *)context;
  state->error = message;
}

static void orm_sqlite_test_sink_done(void *context) {
  orm_sqlite_test_sink_state *state =
      (orm_sqlite_test_sink_state *)context;
  ++state->dones;
}

spec("ORM SQLite CFlow cursor") {
  it("completes an empty result without emitting a row") {
    sqlite3 *database = NULL;
    sqlite3_stmt *statement = NULL;
    orm_row_cursor cursor = {0};
    orm_cbind_publisher_config config = ORM_CBIND_PUBLISHER_CONFIG_INIT(
        &orm_sqlite_test_row_data, 1u, 1u, 2u, 1u);
    orm_error_t error;
    cflow_publisher source = {0};
    cflow_graph surface = {0};
    cflow_graph normalized = {0};
    cflow_scheduler scheduler = {0};
    cflow_subscription run = {0};
    orm_sqlite_test_sink_state sink_state = {0};
    cflow_subscriber_callbacks callbacks = {
        orm_sqlite_test_sink_value, orm_sqlite_test_sink_error,
        orm_sqlite_test_sink_done, &sink_state};
    cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);

    normalized.root = CMETA_INVALID_ID;
    orm_error_init(&error);
    check_equal(sqlite3_open(":memory:", &database), SQLITE_OK);
    check_equal(sqlite3_prepare_v2(
                    database, "select 1 as id, 2 as score where 0", -1,
                    &statement, NULL),
                SQLITE_OK);
    check_equal(orm_sqlite_cursor_from_statement(
                    &cursor, &statement, &orm_sqlite_test_cursor_config, &error),
                ORM_STATUS_OK);
    check_equal(orm_cbind_publisher_init(&source, &cursor, &config, &error),
                ORM_STATUS_OK);
    cflow_graph_init(&surface, &orm_sqlite_test_row_type);
    check_true(cflow_graph_normalize(&normalized, &surface));
    check_true(cflow_scheduler_test_init(&scheduler));
    check_true(cflow_subscribe(&run, &normalized, &source, &scheduler, &sink));
    check_true(cflow_subscription_request(&run, 1u));
    (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
    check_equal(sink_state.values, (size_t)0u);
    check_equal(sink_state.dones, (size_t)1u);
    check_null(sink_state.error);

    cflow_subscription_close(&run);
    cflow_scheduler_destroy(&scheduler);
    cflow_graph_destroy(&normalized);
    cflow_graph_destroy(&surface);
    check_null(sqlite3_next_stmt(database, NULL));
    check_equal(sqlite3_close(database), SQLITE_OK);
  }

  it("preserves row values and ordering through typed demand") {
    static const char create_sql[] =
        "create table parity(id integer, score integer);"
        "insert into parity values(7, 19);"
        "insert into parity values(11, 29);";
    static const char select_sql[] =
        "select id, score from parity order by id";
    char *path = tt_make_temp_file("orm-sqlite-cflow-parity", ".db");
    sqlite3 *database = NULL;
    sqlite3_stmt *statement = NULL;
    orm_row_cursor cursor = {0};
    orm_cbind_publisher_config publisher_config = ORM_CBIND_PUBLISHER_CONFIG_INIT(
        &orm_sqlite_test_row_data, 1u, 1u, 2u, 1u);
    orm_error_t error;
    cflow_publisher source = {0};
    cflow_graph surface = {0};
    cflow_graph normalized = {0};
    cflow_scheduler scheduler = {0};
    cflow_subscription run = {0};
    orm_sqlite_parity_sink_state sink_state = {0};
    cflow_subscriber_callbacks callbacks = {
        orm_sqlite_parity_sink_value, orm_sqlite_parity_sink_error,
        orm_sqlite_parity_sink_done, &sink_state};
    cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);

    check_not_null(path);
    normalized.root = CMETA_INVALID_ID;
    orm_error_init(&error);
    check_equal(sqlite3_open(path, &database), SQLITE_OK);
    check_equal(sqlite3_exec(database, create_sql, NULL, NULL, NULL), SQLITE_OK);
    check_equal(sqlite3_prepare_v2(database, select_sql, -1, &statement, NULL),
                SQLITE_OK);
    check_equal(orm_sqlite_cursor_from_statement(
                    &cursor, &statement, &orm_sqlite_test_cursor_config, &error),
                ORM_STATUS_OK);
    check_equal(orm_cbind_publisher_init(&source, &cursor, &publisher_config, &error),
                ORM_STATUS_OK);
    cflow_graph_init(&surface, &orm_sqlite_test_row_type);
    check_true(cflow_graph_normalize(&normalized, &surface));
    check_true(cflow_scheduler_test_init(&scheduler));
    check_true(cflow_subscribe(&run, &normalized, &source, &scheduler, &sink));
    check_true(cflow_subscription_request(&run, 3u));
    (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
    check_equal(sink_state.values, (size_t)2u);
    check_equal(sink_state.dones, (size_t)1u);
    check_null(sink_state.error);

    check_equal(sink_state.rows[0].id, 7);
    check_equal(sink_state.rows[0].score, 19L);
    check_equal(sink_state.rows[1].id, 11);
    check_equal(sink_state.rows[1].score, 29L);

    cflow_subscription_close(&run);
    cflow_scheduler_destroy(&scheduler);
    cflow_graph_destroy(&normalized);
    cflow_graph_destroy(&surface);
    check_equal(sqlite3_close(database), SQLITE_OK);
    check_equal(tt_remove_file(path), 0);
    free(path);
  }

  it("steps one SQLite row for each unit of downstream demand") {
    static const char sql[] =
        "select 7 as id, 19 as score union all select 11, 29 order by id";
    sqlite3 *database = NULL;
    sqlite3_stmt *statement = NULL;
    orm_row_cursor cursor = {0};
    orm_cbind_publisher_config config = ORM_CBIND_PUBLISHER_CONFIG_INIT(
        &orm_sqlite_test_row_data, 1u, 1u, 2u, 1u);
    orm_error_t error;
    cflow_publisher source = {0};
    cflow_graph surface = {0};
    cflow_graph normalized = {0};
    cflow_scheduler scheduler = {0};
    cflow_subscription run = {0};
    orm_sqlite_test_sink_state sink_state = {0};
    cflow_subscriber_callbacks callbacks = {
        orm_sqlite_test_sink_value, orm_sqlite_test_sink_error,
        orm_sqlite_test_sink_done, &sink_state};
    cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);

    normalized.root = CMETA_INVALID_ID;
    orm_error_init(&error);
    check_equal(sqlite3_open(":memory:", &database), SQLITE_OK);
    check_equal(sqlite3_prepare_v2(database, sql, -1, &statement, NULL),
                SQLITE_OK);
    check_equal(orm_sqlite_cursor_from_statement(
                    &cursor, &statement, &orm_sqlite_test_cursor_config, &error),
                ORM_STATUS_OK);
    check_null(statement);
    check_equal(orm_cbind_publisher_init(&source, &cursor, &config, &error),
                ORM_STATUS_OK);
    cflow_graph_init(&surface, &orm_sqlite_test_row_type);
    check_true(cflow_graph_normalize(&normalized, &surface));
    check_true(cflow_scheduler_test_init(&scheduler));
    check_true(cflow_subscribe(&run, &normalized, &source, &scheduler, &sink));
    check_equal(sink_state.values, (size_t)0u);

    check_true(cflow_subscription_request(&run, 1u));
    (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
    check_equal(sink_state.values, (size_t)1u);
    check_equal(sink_state.last.id, 7);
    check_equal(sink_state.last.score, 19L);
    check_equal(sink_state.dones, (size_t)0u);

    check_true(cflow_subscription_request(&run, 1u));
    (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
    check_equal(sink_state.values, (size_t)2u);
    check_equal(sink_state.last.id, 11);
    check_equal(sink_state.last.score, 29L);
    check_equal(sink_state.dones, (size_t)0u);

    check_true(cflow_subscription_request(&run, 1u));
    (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
    check_equal(sink_state.dones, (size_t)1u);
    check_null(sink_state.error);

    cflow_subscription_close(&run);
    check_null(sqlite3_next_stmt(database, NULL));
    cflow_scheduler_destroy(&scheduler);
    cflow_graph_destroy(&normalized);
    cflow_graph_destroy(&surface);
    check_equal(sqlite3_close(database), SQLITE_OK);
  }

  it("copies transient SQLite text before advancing the statement") {
    static const char sql[] =
        "select 1 as id, 'Alice' as name union all select 2, 'Bob'";
    sqlite3 *database = NULL;
    sqlite3_stmt *statement = NULL;
    orm_row_cursor cursor = {0};
    orm_cbind_publisher_config config = ORM_CBIND_PUBLISHER_CONFIG_INIT(
        &orm_sqlite_test_text_row_data, 1u, 1u, 2u, 16u);
    orm_error_t error;
    cflow_publisher source = {0};
    orm_sqlite_test_text_row first = {0};
    orm_sqlite_test_text_row second = {0};
    cflow_step step;

    orm_error_init(&error);
    check_equal(sqlite3_open(":memory:", &database), SQLITE_OK);
    check_equal(sqlite3_prepare_v2(database, sql, -1, &statement, NULL),
                SQLITE_OK);
    check_equal(orm_sqlite_cursor_from_statement(
                    &cursor, &statement, &orm_sqlite_test_cursor_config, &error),
                ORM_STATUS_OK);
    check_equal(orm_cbind_publisher_init(&source, &cursor, &config, &error),
                ORM_STATUS_OK);

    step = cflow_publisher_resume(&source, NULL, &first);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(first.id, 1);
    check_equal(first.name, "Alice");
    step = cflow_publisher_resume(&source, NULL, &second);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(second.id, 2);
    check_equal(second.name, "Bob");
    check_equal(first.name, "Alice");

    orm_sqlite_test_text_row_destroy(&second);
    orm_sqlite_test_text_row_destroy(&first);
    cflow_publisher_destroy(&source);
    check_null(sqlite3_next_stmt(database, NULL));
    check_equal(sqlite3_close(database), SQLITE_OK);
  }

  it("fails instead of truncating text beyond the configured byte bound") {
    sqlite3 *database = NULL;
    sqlite3_stmt *statement = NULL;
    orm_row_cursor cursor = {0};
    orm_cbind_publisher_config config = ORM_CBIND_PUBLISHER_CONFIG_INIT(
        &orm_sqlite_test_text_row_data, 1u, 1u, 2u, 4u);
    orm_error_t error;
    cflow_publisher source = {0};
    orm_sqlite_test_text_row row = {0};
    cflow_step step;

    orm_error_init(&error);
    check_equal(sqlite3_open(":memory:", &database), SQLITE_OK);
    check_equal(sqlite3_prepare_v2(
                    database, "select 1 as id, 'Alice' as name", -1,
                    &statement, NULL),
                SQLITE_OK);
    check_equal(orm_sqlite_cursor_from_statement(
                    &cursor, &statement, &orm_sqlite_test_cursor_config, &error),
                ORM_STATUS_OK);
    check_equal(orm_cbind_publisher_init(&source, &cursor, &config, &error),
                ORM_STATUS_OK);

    step = cflow_publisher_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_ERROR);
    check_not_null(step.error);
    check_equal(row.id, 0);
    check_null(row.name);

    cflow_publisher_destroy(&source);
    check_null(sqlite3_next_stmt(database, NULL));
    check_equal(sqlite3_close(database), SQLITE_OK);
  }

  it("copies transient SQLite blobs before advancing the statement") {
    static const unsigned char expected[] = {0u, 'A', 'B'};
    static const char sql[] =
        "select 1 as id, x'004142' as name union all select 2, x'4344'";
    sqlite3 *database = NULL;
    sqlite3_stmt *statement = NULL;
    orm_row_cursor cursor = {0};
    orm_cbind_publisher_config config = ORM_CBIND_PUBLISHER_CONFIG_INIT(
        &orm_sqlite_test_blob_row_data, 1u, 1u, 2u, 16u);
    orm_error_t error;
    cflow_publisher source = {0};
    orm_sqlite_test_text_row first = {0};
    orm_sqlite_test_text_row second = {0};
    cflow_step step;

    orm_error_init(&error);
    check_equal(sqlite3_open(":memory:", &database), SQLITE_OK);
    check_equal(sqlite3_prepare_v2(database, sql, -1, &statement, NULL),
                SQLITE_OK);
    check_equal(orm_sqlite_cursor_from_statement(
                    &cursor, &statement, &orm_sqlite_test_cursor_config, &error),
                ORM_STATUS_OK);
    check_equal(orm_cbind_publisher_init(&source, &cursor, &config, &error),
                ORM_STATUS_OK);

    step = cflow_publisher_resume(&source, NULL, &first);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(tstr_len(first.name), sizeof(expected));
    check_equal(first.name, expected, sizeof(expected));
    step = cflow_publisher_resume(&source, NULL, &second);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(tstr_len(second.name), (size_t)2u);
    check_equal(second.name, "CD", (size_t)2u);
    check_equal(first.name, expected, sizeof(expected));

    orm_sqlite_test_text_row_destroy(&second);
    orm_sqlite_test_text_row_destroy(&first);
    cflow_publisher_destroy(&source);
    check_null(sqlite3_next_stmt(database, NULL));
    check_equal(sqlite3_close(database), SQLITE_OK);
  }

  it("resets and finalizes the SQLite statement when the source is cancelled") {
    sqlite3 *database = NULL;
    sqlite3_stmt *statement = NULL;
    orm_row_cursor cursor = {0};
    orm_cbind_publisher_config config = ORM_CBIND_PUBLISHER_CONFIG_INIT(
        &orm_sqlite_test_row_data, 1u, 1u, 2u, 1u);
    orm_error_t error;
    cflow_publisher source = {0};
    const char *terminal_error = "not cleared";

    orm_error_init(&error);
    check_equal(sqlite3_open(":memory:", &database), SQLITE_OK);
    check_equal(sqlite3_prepare_v2(
                    database, "select 7 as id, 19 as score", -1,
                    &statement, NULL),
                SQLITE_OK);
    check_equal(orm_sqlite_cursor_from_statement(
                    &cursor, &statement, &orm_sqlite_test_cursor_config, &error),
                ORM_STATUS_OK);
    check_equal(orm_cbind_publisher_init(&source, &cursor, &config, &error),
                ORM_STATUS_OK);

    cflow_publisher_cancel(&source);
    check_equal(cflow_publisher_poll_terminal(&source, &terminal_error),
                CFLOW_PUBLISHER_DONE);
    check_null(terminal_error);
    cflow_publisher_destroy(&source);
    check_null(sqlite3_next_stmt(database, NULL));
    check_equal(sqlite3_close(database), SQLITE_OK);
  }
}

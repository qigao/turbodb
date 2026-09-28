#include "cursor.h"

#include <tinytest.h>

#include <stdint.h>
#include <string.h>

typedef struct fake_source_t {
  const mysql_cursor_source_step_t *steps;
  size_t step_count;
  size_t next_index;
  unsigned cancel_count;
  unsigned destroy_count;
} fake_source_t;

static mysql_cursor_source_step_t fake_next(void *context) {
  fake_source_t *source = (fake_source_t *)context;
  if (source == NULL || source->next_index >= source->step_count)
    return (mysql_cursor_source_step_t)MYSQL_CURSOR_SOURCE_STEP_INIT;
  return source->steps[source->next_index++];
}

static void fake_cancel(void *context) {
  fake_source_t *source = (fake_source_t *)context;
  if (source != NULL)
    ++source->cancel_count;
}

static void fake_destroy(void *context) {
  fake_source_t *source = (fake_source_t *)context;
  if (source != NULL)
    ++source->destroy_count;
}

static const mysql_cursor_source_ops_t fake_ops = {
    sizeof(mysql_cursor_source_ops_t),
    MYSQL_CURSOR_SOURCE_OPS_ABI_VERSION,
    fake_next,
    fake_cancel,
    fake_destroy};

static mysql_column_definition_t one_column(void) {
  static const uint8_t name[] = {'v'};
  mysql_column_definition_t column;
  memset(&column, 0, sizeof(column));
  column.name.data = name;
  column.name.length = sizeof(name);
  column.type = MYSQL_FIELD_TYPE_TINY;
  column.character_set = 63u;
  return column;
}

static mysql_cursor_config_t default_config(void) {
  return (mysql_cursor_config_t)
      MYSQL_CURSOR_CONFIG_INIT(4u, 64u, 4u, 64u, 32u);
}

static void expect_single_sint_row(cserde_reader *reader, int64_t expected) {
  cserde_token token;

  check_equal(cserde_reader_next(reader, &token), CSERDE_OK);
  check_equal(token.kind, CSERDE_MAP_BEGIN);

  check_equal(cserde_reader_next(reader, &token), CSERDE_OK);
  check_equal(token.kind, CSERDE_STRING);
  check_equal(token.value.slice.size, (size_t)1u);
  check_equal(token.value.slice.data[0], (unsigned char)'v');

  check_equal(cserde_reader_next(reader, &token), CSERDE_OK);
  check_equal(token.kind, CSERDE_SINT);
  check_equal(token.value.sint, expected);

  check_equal(cserde_reader_next(reader, &token), CSERDE_OK);
  check_equal(token.kind, CSERDE_MAP_END);
  check_equal(cserde_reader_next(reader, &token), CSERDE_DONE);
}

spec("mysql incremental Driver cursor") {
  (void)ttest_config__;

  it("moves source ownership and emits rows one at a time until DONE") {
    static const uint8_t row1[] = {0x00,0x00,0x01};
    static const uint8_t row2[] = {0x00,0x00,0x02};
    const mysql_cursor_source_step_t steps[] = {
      {MYSQL_CURSOR_SOURCE_ROW, ORM_STATUS_OK, NULL, row1, sizeof(row1)},
      {MYSQL_CURSOR_SOURCE_ROW, ORM_STATUS_OK, NULL, row2, sizeof(row2)},
      MYSQL_CURSOR_SOURCE_STEP_INIT
    };
    fake_source_t fake = {steps, 3u, 0u, 0u, 0u};
    mysql_cursor_source_t source = {&fake_ops, &fake};
    mysql_column_definition_t column = one_column();
    mysql_cursor_config_t config = default_config();
    orm_row_cursor cursor = {0};
    orm_error_t error;
    cserde_reader reader = {0};
    orm_row_cursor_step step;
    uint64_t columns = 0u;

    memset(&error, 0, sizeof(error));
    error.struct_size = (uint32_t)sizeof(error);
    check_equal(mysql_cursor_start(
                    &cursor, &source, &column, 1u, &config, &error),
                ORM_STATUS_OK);
    check_null(source.ops);
    check_null(source.context);
    check_not_null(cursor.ops);
    check_not_null(cursor.context);

    check_equal(cursor.ops->column_count(
                    cursor.context, &columns),
                ORM_STATUS_OK);
    check_equal(columns, UINT64_C(1));

    step = cursor.ops->next(cursor.context, &reader);
    check_equal(step.kind, ORM_ROW_CURSOR_ROW);
    expect_single_sint_row(&reader, INT64_C(1));

    memset(&reader, 0, sizeof(reader));
    step = cursor.ops->next(cursor.context, &reader);
    check_equal(step.kind, ORM_ROW_CURSOR_ROW);
    expect_single_sint_row(&reader, INT64_C(2));

    memset(&reader, 0, sizeof(reader));
    step = cursor.ops->next(cursor.context, &reader);
    check_equal(step.kind, ORM_ROW_CURSOR_DONE);

    cursor.ops->destroy(cursor.context);
    check_equal(fake.destroy_count, 1u);
    check_equal(fake.cancel_count, 0u);
  }

  it("requests cancellation exactly once and destroy still settles the source") {
    const mysql_cursor_source_step_t steps[] = {
      MYSQL_CURSOR_SOURCE_STEP_INIT
    };
    fake_source_t fake = {steps, 1u, 0u, 0u, 0u};
    mysql_cursor_source_t source = {&fake_ops, &fake};
    mysql_column_definition_t column = one_column();
    mysql_cursor_config_t config = default_config();
    orm_row_cursor cursor = {0};
    orm_error_t error;

    memset(&error, 0, sizeof(error));
    error.struct_size = (uint32_t)sizeof(error);
    check_equal(mysql_cursor_start(
                    &cursor, &source, &column, 1u, &config, &error),
                ORM_STATUS_OK);

    cursor.ops->cancel(cursor.context);
    cursor.ops->cancel(cursor.context);
    check_equal(fake.cancel_count, 1u);

    cursor.ops->destroy(cursor.context);
    check_equal(fake.destroy_count, 1u);
  }

  it("propagates source errors and becomes terminal") {
    const mysql_cursor_source_step_t steps[] = {
      {MYSQL_CURSOR_SOURCE_ERROR, ORM_STATUS_CONNECTION_ERROR,
       "connection lost", NULL, 0u}
    };
    fake_source_t fake = {steps, 1u, 0u, 0u, 0u};
    mysql_cursor_source_t source = {&fake_ops, &fake};
    mysql_column_definition_t column = one_column();
    mysql_cursor_config_t config = default_config();
    orm_row_cursor cursor = {0};
    orm_error_t error;
    cserde_reader reader = {0};
    orm_row_cursor_step step;

    memset(&error, 0, sizeof(error));
    error.struct_size = (uint32_t)sizeof(error);
    check_equal(mysql_cursor_start(
                    &cursor, &source, &column, 1u, &config, &error),
                ORM_STATUS_OK);

    step = cursor.ops->next(cursor.context, &reader);
    check_equal(step.kind, ORM_ROW_CURSOR_ERROR);
    check_equal(step.status, ORM_STATUS_CONNECTION_ERROR);
    check_equal(strcmp(step.message, "connection lost"), 0);

    step = cursor.ops->next(cursor.context, &reader);
    check_equal(step.kind, ORM_ROW_CURSOR_DONE);

    cursor.ops->destroy(cursor.context);
    check_equal(fake.destroy_count, 1u);
  }

  it("enforces aggregate row-count and byte budgets") {
    static const uint8_t row1[] = {0x00,0x00,0x01};
    static const uint8_t row2[] = {0x00,0x00,0x02};
    const mysql_cursor_source_step_t steps[] = {
      {MYSQL_CURSOR_SOURCE_ROW, ORM_STATUS_OK, NULL, row1, sizeof(row1)},
      {MYSQL_CURSOR_SOURCE_ROW, ORM_STATUS_OK, NULL, row2, sizeof(row2)}
    };
    fake_source_t fake = {steps, 2u, 0u, 0u, 0u};
    mysql_cursor_source_t source = {&fake_ops, &fake};
    mysql_column_definition_t column = one_column();
    mysql_cursor_config_t config =
        MYSQL_CURSOR_CONFIG_INIT(1u, 64u, 1u, 8u, 8u);
    orm_row_cursor cursor = {0};
    orm_error_t error;
    cserde_reader reader = {0};
    orm_row_cursor_step step;

    memset(&error, 0, sizeof(error));
    error.struct_size = (uint32_t)sizeof(error);
    check_equal(mysql_cursor_start(
                    &cursor, &source, &column, 1u, &config, &error),
                ORM_STATUS_OK);

    step = cursor.ops->next(cursor.context, &reader);
    check_equal(step.kind, ORM_ROW_CURSOR_ROW);

    step = cursor.ops->next(cursor.context, &reader);
    check_equal(step.kind, ORM_ROW_CURSOR_ERROR);
    check_equal(step.status, ORM_STATUS_LIMIT_EXCEEDED);

    cursor.ops->destroy(cursor.context);
  }

  it("enforces per-row storage bounds before exposing a reader") {
    static const uint8_t row[] = {0x00,0x00,0x01};
    const mysql_cursor_source_step_t steps[] = {
      {MYSQL_CURSOR_SOURCE_ROW, ORM_STATUS_OK, NULL, row, sizeof(row)}
    };
    fake_source_t fake = {steps, 1u, 0u, 0u, 0u};
    mysql_cursor_source_t source = {&fake_ops, &fake};
    mysql_column_definition_t column = one_column();
    mysql_cursor_config_t config =
        MYSQL_CURSOR_CONFIG_INIT(1u, 64u, 1u, 8u, 2u);
    orm_row_cursor cursor = {0};
    orm_error_t error;
    cserde_reader reader = {0};
    orm_row_cursor_step step;

    memset(&error, 0, sizeof(error));
    error.struct_size = (uint32_t)sizeof(error);
    check_equal(mysql_cursor_start(
                    &cursor, &source, &column, 1u, &config, &error),
                ORM_STATUS_OK);

    step = cursor.ops->next(cursor.context, &reader);
    check_equal(step.kind, ORM_ROW_CURSOR_ERROR);
    check_equal(step.status, ORM_STATUS_LIMIT_EXCEEDED);

    cursor.ops->destroy(cursor.context);
  }

  it("rejects invalid source/configuration without moving ownership") {
    const mysql_cursor_source_step_t steps[] = {
      MYSQL_CURSOR_SOURCE_STEP_INIT
    };
    fake_source_t fake = {steps, 1u, 0u, 0u, 0u};
    mysql_cursor_source_t source = {&fake_ops, &fake};
    mysql_column_definition_t column = one_column();
    mysql_cursor_config_t config = default_config();
    orm_row_cursor cursor = {0};
    orm_error_t error;

    memset(&error, 0, sizeof(error));
    error.struct_size = (uint32_t)sizeof(error);
    config.max_result_rows = 0u;
    check_equal(mysql_cursor_start(
                    &cursor, &source, &column, 1u, &config, &error),
                ORM_STATUS_INVALID_ARGUMENT);
    check_true(source.ops == &fake_ops);
    check_true(source.context == &fake);
    check_null(cursor.ops);
    check_null(cursor.context);
    check_equal(fake.destroy_count, 0u);
  }
}

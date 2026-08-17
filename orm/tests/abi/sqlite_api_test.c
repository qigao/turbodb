#include "orm.h"

#include <sqlite3.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *database_filename = "orm_sqlite_c_api_test.db";

static orm_string_view_t string_view(const char *value) {
  return orm_view(value);
}

static orm_value_t int64_value(int64_t value) {
  orm_value_t result;
  memset(&result, 0, sizeof(result));
  result.kind = ORM_VALUE_INT64;
  result.data.int64_value = value;
  return result;
}

static orm_value_t uint64_value(uint64_t value) {
  orm_value_t result;
  memset(&result, 0, sizeof(result));
  result.kind = ORM_VALUE_UINT64;
  result.data.uint64_value = value;
  return result;
}

static orm_value_t text_value(const char *value) {
  orm_value_t result;
  memset(&result, 0, sizeof(result));
  result.kind = ORM_VALUE_TEXT;
  result.data.text_value = string_view(value);
  return result;
}

static orm_value_t null_value(void) {
  orm_value_t result;
  memset(&result, 0, sizeof(result));
  result.kind = ORM_VALUE_NULL;
  return result;
}

static orm_value_t blob_value(const uint8_t *data, size_t size) {
  return orm_blob(data, size);
}

static void require_true(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "SQLite C ABI test failed: %s\n", message);
    (void)remove(database_filename);
    exit(EXIT_FAILURE);
  }
}

static void require_status(orm_status_t actual, orm_status_t expected, const orm_error_t *error,
                           const char *operation) {
  if (actual != expected) {
    fprintf(stderr, "SQLite C ABI test failed: %s returned %s (%s), expected %s\n", operation,
            orm_status_message(actual), error != NULL ? error->message : "no error object",
            orm_status_message(expected));
    (void)remove(database_filename);
    exit(EXIT_FAILURE);
  }
}

static void create_fixture(void) {
  sqlite3 *database = NULL;
  char *message = NULL;
  const char *sql = "create table department(id integer primary key, name text not null);"
                    "create table person("
                    "id integer primary key, name text not null, active integer not null, "
                    "score real not null, note text, department_id integer not null);"
                    "create table blobs(id integer primary key, data blob, note text);"
                    "insert into department values(1, 'Engineering');"
                    "insert into department values(2, 'Sales');"
                    "insert into person values(1, 'Alice', 1, 1.5, null, 1);"
                    "insert into person values(2, 'Bob', 0, 2.25, 'memo', 2);";

  (void)remove(database_filename);
  require_true(sqlite3_open(database_filename, &database) == SQLITE_OK,
               "could not create the SQLite fixture");
  if (sqlite3_exec(database, sql, NULL, NULL, &message) != SQLITE_OK) {
    fprintf(stderr, "SQLite fixture failed: %s\n", message != NULL ? message : "unknown");
    sqlite3_free(message);
    (void)sqlite3_close(database);
    (void)remove(database_filename);
    exit(EXIT_FAILURE);
  }
  require_true(sqlite3_close(database) == SQLITE_OK, "could not close the SQLite fixture");
}

static orm_connection_t *create_sqlite_connection(uint64_t max_result_bytes, orm_error_t *error) {
  orm_config_t config;
  orm_option_t options[2];
  orm_connection_t *connection = NULL;
  orm_config(&config);
  options[0].keyword = string_view("filename");
  options[0].value = string_view(database_filename);
  options[1].keyword = string_view("open_mode");
  options[1].value = string_view("read_write");
  config.driver = string_view("sqlite");
  config.options = options;
  config.option_count = (uint32_t)(sizeof(options) / sizeof(options[0]));
  if (max_result_bytes != 0) config.max_result_bytes = max_result_bytes;
  require_status(orm_connect(&config, &connection, error), ORM_STATUS_OK, error,
                 "create SQLite connection");
  require_true(connection != NULL, "SQLite connection is null");
  return connection;
}

static orm_query_t *create_raw(orm_connection_t *connection, const char *sql,
                               orm_error_t *error) {
  orm_query_t *query = NULL;
  require_status(orm_raw(connection, string_view(sql), &query, error), ORM_STATUS_OK, error,
                 "create SQLite raw transaction query");
  return query;
}

static void test_explicit_transactions(void) {
  orm_error_t error;
  orm_connection_t *connection;
  orm_transaction_t *transaction = NULL;
  orm_transaction_t *nested = NULL;
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  orm_string_view_t name;
  int64_t identifier = 0;
  uint64_t affected = 0;
  const orm_isolation_t levels[] = {
      ORM_ISOLATION_READ_UNCOMMITTED, ORM_ISOLATION_READ_COMMITTED,
      ORM_ISOLATION_REPEATABLE_READ, ORM_ISOLATION_SNAPSHOT,
      ORM_ISOLATION_SERIALIZABLE};
  size_t index;

  orm_error_init(&error);
  connection = create_sqlite_connection(0, &error);
  require_status(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                       &transaction, &error),
                 ORM_STATUS_OK, &error, "begin SQLite transaction");
  require_status(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                       &nested, &error),
                 ORM_STATUS_INVALID_STATE, &error, "reject nested SQLite transaction");
  require_true(nested == NULL, "failed nested SQLite transaction returned a handle");

  query = create_raw(connection,
                     "insert into person values(?1, ?2, ?3, ?4, ?5, ?6)", &error);
  require_status(orm_query_bind(query, int64_value(3), &error), ORM_STATUS_OK, &error,
                 "bind SQLite transaction id");
  require_status(orm_query_bind(query, text_value("Cara"), &error), ORM_STATUS_OK, &error,
                 "bind SQLite transaction name");
  require_status(orm_query_bind(query, int64_value(1), &error), ORM_STATUS_OK, &error,
                 "bind SQLite transaction active");
  require_status(orm_query_bind(query, int64_value(3), &error), ORM_STATUS_OK, &error,
                 "bind SQLite transaction score");
  require_status(orm_query_bind(query, null_value(), &error), ORM_STATUS_OK, &error,
                 "bind SQLite transaction note");
  require_status(orm_query_bind(query, int64_value(2), &error), ORM_STATUS_OK, &error,
                 "bind SQLite transaction department");
  require_status(orm_query_execute(query, &result, &error), ORM_STATUS_INVALID_STATE, &error,
                 "reject SQLite transaction handle bypass");
  require_status(orm_query_execute_in_transaction(query, transaction, &result, &error),
                 ORM_STATUS_OK, &error, "insert inside SQLite transaction");
  orm_result_destroy(result);
  orm_query_destroy(query);
  result = NULL;

  require_status(orm_transaction_savepoint(transaction, string_view("before_name"), &error),
                 ORM_STATUS_OK, &error, "create SQLite savepoint");
  query = create_raw(connection, "update person set name = ?1 where id = ?2", &error);
  require_status(orm_query_bind(query, text_value("Changed"), &error), ORM_STATUS_OK, &error,
                 "bind SQLite savepoint name");
  require_status(orm_query_bind(query, int64_value(3), &error), ORM_STATUS_OK, &error,
                 "bind SQLite savepoint id");
  require_status(orm_query_execute_in_transaction(query, transaction, &result, &error),
                 ORM_STATUS_OK, &error, "update inside SQLite savepoint");
  orm_result_destroy(result);
  orm_query_destroy(query);
  result = NULL;
  require_status(orm_transaction_rollback_to_savepoint(
                     transaction, string_view("before_name"), &error),
                 ORM_STATUS_OK, &error, "roll back SQLite savepoint");
  require_status(orm_transaction_release_savepoint(
                     transaction, string_view("before_name"), &error),
                 ORM_STATUS_OK, &error, "release SQLite savepoint");

  query = create_raw(connection, "select id, name from person where id = ?1", &error);
  require_status(orm_query_bind(query, int64_value(3), &error), ORM_STATUS_OK, &error,
                 "bind SQLite read-own-write id");
  require_status(orm_query_execute_in_transaction(query, transaction, &result, &error),
                 ORM_STATUS_OK, &error, "read own SQLite transaction write");
  require_status(orm_result_get_int64(result, 0, 0, &identifier, &error), ORM_STATUS_OK, &error,
                 "read SQLite transaction id");
  require_status(orm_result_get_text(result, 0, 1, &name, &error), ORM_STATUS_OK, &error,
                 "read SQLite transaction name");
  require_true(identifier == 3 && name.len == 4 && memcmp(name.data, "Cara", 4) == 0,
               "SQLite savepoint rollback or read-own-write behavior is incorrect");
  orm_result_destroy(result);
  orm_query_destroy(query);
  result = NULL;
  require_status(orm_transaction_commit(transaction, &error), ORM_STATUS_OK, &error,
                 "commit SQLite transaction");
  orm_transaction_destroy(transaction);
  transaction = NULL;

  query = create_raw(connection, "delete from person where id = ?1", &error);
  require_status(orm_query_bind(query, int64_value(3), &error), ORM_STATUS_OK, &error,
                 "bind SQLite rollback id");
  require_status(orm_transaction_begin(connection, ORM_ISOLATION_SNAPSHOT,
                                       &transaction, &error),
                 ORM_STATUS_OK, &error, "begin SQLite snapshot transaction");
  require_status(orm_query_execute_in_transaction(query, transaction, &result, &error),
                 ORM_STATUS_OK, &error, "delete inside SQLite rollback transaction");
  orm_result_destroy(result);
  result = NULL;
  require_status(orm_transaction_rollback(transaction, &error), ORM_STATUS_OK, &error,
                 "roll back SQLite transaction");
  orm_transaction_destroy(transaction);
  orm_query_destroy(query);
  transaction = NULL;

  query = create_raw(connection, "delete from person where id = ?1", &error);
  require_status(orm_query_bind(query, int64_value(3), &error), ORM_STATUS_OK, &error,
                 "bind SQLite cleanup id");
  require_status(orm_query_execute(query, &result, &error), ORM_STATUS_OK, &error,
                 "verify rollback and clean SQLite transaction row");
  require_status(orm_result_affected_rows(result, &affected, &error), ORM_STATUS_OK, &error,
                 "read SQLite transaction cleanup count");
  require_true(affected == 1, "SQLite rollback did not preserve the committed row");
  orm_result_destroy(result);
  orm_query_destroy(query);
  result = NULL;

  for (index = 0; index < sizeof(levels) / sizeof(levels[0]); ++index) {
    require_status(orm_transaction_begin(connection, levels[index], &transaction, &error),
                   ORM_STATUS_OK, &error, "begin supported SQLite isolation level");
    orm_transaction_destroy(transaction);
    transaction = NULL;
  }
  orm_disconnect(connection);
}

static void test_query_and_lifetimes(void) {
  orm_error_t error;
  orm_connection_t *connection;
  orm_query_t *query = NULL;
  orm_query_t *offset_query = NULL;
  orm_result_t *result = NULL;
  orm_result_t *offset_result = NULL;
  uint64_t count = 0;
  int64_t identifier = 0;
  double score = 0.0;
  uint8_t active = 0;
  uint8_t is_null = 0;
  orm_string_view_t name;

  orm_error_init(&error);
  connection = create_sqlite_connection(0, &error);
  require_status(orm_query_create(connection, string_view("person"), &query, &error), ORM_STATUS_OK,
                 &error, "create structured query");
  require_status(orm_query_add_column(query, string_view("id"), &error), ORM_STATUS_OK, &error,
                 "select id");
  require_status(orm_query_add_column(query, string_view("name"), &error), ORM_STATUS_OK, &error,
                 "select name");
  require_status(orm_query_add_column(query, string_view("active"), &error), ORM_STATUS_OK, &error,
                 "select active");
  require_status(orm_query_add_column(query, string_view("score"), &error), ORM_STATUS_OK, &error,
                 "select score");
  require_status(orm_query_add_column(query, string_view("note"), &error), ORM_STATUS_OK, &error,
                 "select note");
  require_status(
      orm_query_where(query, string_view("id"), ORM_COMPARE_GREATER_EQUAL, int64_value(1), &error),
      ORM_STATUS_OK, &error, "bind integer predicate");
  require_status(
      orm_query_where(query, string_view("name"), ORM_COMPARE_LIKE, text_value("A%"), &error),
      ORM_STATUS_OK, &error, "bind text predicate");
  require_status(orm_query_order_by(query, string_view("id"), ORM_ORDER_ASCENDING, &error),
                 ORM_STATUS_OK, &error, "order structured query");
  require_status(orm_query_set_limit(query, 1, &error), ORM_STATUS_OK, &error,
                 "limit structured query");

  require_status(orm_query_create(connection, string_view("person"), &offset_query, &error),
                 ORM_STATUS_OK, &error, "create offset query");
  require_status(orm_query_add_column(offset_query, string_view("id"), &error), ORM_STATUS_OK,
                 &error, "select offset id");
  require_status(orm_query_order_by(offset_query, string_view("id"), ORM_ORDER_ASCENDING, &error),
                 ORM_STATUS_OK, &error, "order offset query");
  require_status(orm_query_set_offset(offset_query, 1, &error), ORM_STATUS_OK, &error,
                 "set offset without limit");

  orm_disconnect(connection);
  require_status(orm_query_execute(query, &result, &error), ORM_STATUS_OK, &error,
                 "execute retained SQLite query");
  require_status(orm_result_row_count(result, &count, &error), ORM_STATUS_OK, &error,
                 "read row count");
  require_true(count == 1, "unexpected SQLite row count");
  require_status(orm_result_column_count(result, &count, &error), ORM_STATUS_OK, &error,
                 "read column count");
  require_true(count == 5, "unexpected SQLite column count");
  require_status(orm_result_get_int64(result, 0, 0, &identifier, &error), ORM_STATUS_OK, &error,
                 "read id");
  require_true(identifier == 1, "SQLite integer conversion failed");
  require_status(orm_result_get_text(result, 0, 1, &name, &error), ORM_STATUS_OK, &error,
                 "read name");
  require_true(name.len == 5 && memcmp(name.data, "Alice", 5) == 0,
               "SQLite borrowed text is incorrect");
  require_status(orm_result_get_boolean(result, 0, 2, &active, &error), ORM_STATUS_OK, &error,
                 "read boolean");
  require_true(active == 1, "SQLite boolean conversion failed");
  require_status(orm_result_get_double(result, 0, 3, &score, &error), ORM_STATUS_OK, &error,
                 "read double");
  require_true(score == 1.5, "SQLite double conversion failed");
  require_status(orm_result_is_null(result, 0, 4, &is_null, &error), ORM_STATUS_OK, &error,
                 "read null state");
  require_true(is_null == 1, "SQLite null state is incorrect");

  orm_query_destroy(query);
  require_true(name.len == 5 && memcmp(name.data, "Alice", 5) == 0,
               "SQLite result view did not outlive its query");
  require_status(orm_query_execute(offset_query, &offset_result, &error), ORM_STATUS_OK, &error,
                 "execute offset without limit");
  require_status(orm_result_get_int64(offset_result, 0, 0, &identifier, &error), ORM_STATUS_OK,
                 &error, "read offset row");
  require_true(identifier == 2, "SQLite offset-only pagination is incorrect");

  orm_query_destroy(offset_query);
  orm_result_destroy(offset_result);
  orm_result_destroy(result);
}

static void test_limits_and_errors(void) {
  orm_error_t error;
  orm_connection_t *connection;
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  orm_config_t config;

  orm_error_init(&error);
  connection = create_sqlite_connection(1, &error);
  require_status(orm_query_create(connection, string_view("person"), &query, &error), ORM_STATUS_OK,
                 &error, "create bounded query");
  require_status(orm_query_add_column(query, string_view("id"), &error), ORM_STATUS_OK, &error,
                 "select bounded id");
  require_status(orm_query_execute(query, &result, &error), ORM_STATUS_LIMIT_EXCEEDED, &error,
                 "enforce result memory limit");
  require_true(result == NULL, "failed SQLite execution returned a result");
  orm_query_destroy(query);
  orm_disconnect(connection);

  connection = create_sqlite_connection(0, &error);
  query = NULL;
  require_status(orm_query_create(connection, string_view("person"), &query, &error), ORM_STATUS_OK,
                 &error, "create unsigned query");
  require_status(orm_query_select_all(query, &error), ORM_STATUS_OK, &error,
                 "select all for unsigned query");
  require_status(orm_query_where(query, string_view("id"), ORM_COMPARE_EQUAL,
                                 uint64_value(UINT64_MAX), &error),
                 ORM_STATUS_OK, &error, "store unsigned predicate");
  require_status(orm_query_execute(query, &result, &error), ORM_STATUS_OUT_OF_RANGE, &error,
                 "reject oversized SQLite integer");
  orm_query_destroy(query);
  orm_disconnect(connection);

  orm_config(&config);
  config.driver = string_view("sqlite");
  config.abi_version += 1;
  connection = NULL;
  require_status(orm_connect(&config, &connection, &error), ORM_STATUS_ABI_MISMATCH, &error,
                 "reject SQLite ABI mismatch");
  require_true(connection == NULL, "invalid SQLite config returned a connection");

  orm_config(&config);
  config.driver = string_view("sqlite");
  require_status(orm_connect(&config, &connection, &error), ORM_STATUS_INVALID_ARGUMENT, &error,
                 "reject missing driver options");
  require_true(connection == NULL, "missing options returned a connection");
}

static void require_affected(orm_result_t *result, uint64_t expected, orm_error_t *error,
                             const char *operation) {
  uint64_t actual = 0;
  require_status(orm_result_affected_rows(result, &actual, error), ORM_STATUS_OK, error, operation);
  require_true(actual == expected, "unexpected affected-row count");
}

static void test_query_builder_features(void) {
  orm_error_t error;
  orm_connection_t *connection;
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  orm_chain_t chain;
  const orm_string_view_t empty = {NULL, 0};
  uint64_t rows = 0;
  orm_string_view_t text;

  orm_error_init(&error);
  connection = create_sqlite_connection(0, &error);

  require_status(orm_insert(connection, string_view("person"), &query, &error), ORM_STATUS_OK,
                 &error, "create SQLite insert");
  require_status(orm_query_set(query, string_view("id"), int64_value(3), &error), ORM_STATUS_OK,
                 &error, "set inserted id");
  require_status(orm_query_set(query, string_view("name"), text_value("Cara"), &error),
                 ORM_STATUS_OK, &error, "set inserted name");
  require_status(orm_query_set(query, string_view("active"), int64_value(1), &error), ORM_STATUS_OK,
                 &error, "set inserted active");
  require_status(orm_query_set(query, string_view("score"), int64_value(3), &error), ORM_STATUS_OK,
                 &error, "set inserted score");
  require_status(orm_query_set(query, string_view("note"), null_value(), &error), ORM_STATUS_OK,
                 &error, "set inserted null");
  require_status(orm_query_set(query, string_view("department_id"), int64_value(2), &error),
                 ORM_STATUS_OK, &error, "set inserted department");
  require_status(orm_query_execute(query, &result, &error), ORM_STATUS_OK, &error,
                 "execute SQLite insert");
  require_affected(result, 1, &error, "read SQLite insert affected rows");
  orm_result_destroy(result);
  orm_query_destroy(query);

  query = NULL;
  result = NULL;
  require_status(orm_update(connection, string_view("person"), &query, &error), ORM_STATUS_OK,
                 &error, "create SQLite update");
  chain = orm_chain(query, &error);
  require_status(chain.set(&chain, orm_view("name"), orm_text("Caroline"))
                     ->where_expr(&chain, ORM_OR(ORM_EQ("id", orm_i64(3)),
                                                 ORM_EQ("name", orm_text("missing"))))
                     ->execute(&chain, &result),
                 ORM_STATUS_OK, &error, "execute SQLite update");
  require_affected(result, 1, &error, "read SQLite update affected rows");
  orm_result_destroy(result);
  orm_query_destroy(query);

  query = NULL;
  result = NULL;
  require_status(orm_query_create(connection, string_view("person"), &query, &error), ORM_STATUS_OK,
                 &error, "create SQLite aggregate query");
  require_status(orm_query_add_column(query, string_view("department.name"), &error), ORM_STATUS_OK,
                 &error, "select SQLite group column");
  require_status(
      orm_query_add_aggregate(query, ORM_AGGREGATE_COUNT_ALL, empty, string_view("total"), &error),
      ORM_STATUS_OK, &error, "select SQLite aggregate");
  require_status(orm_query_join(query, ORM_JOIN_LEFT, string_view("department"),
                                string_view("person.department_id"), ORM_COMPARE_EQUAL,
                                string_view("department.id"), &error),
                 ORM_STATUS_OK, &error, "add SQLite join");
  chain = orm_chain(query, &error);
  require_status(chain
                     .where_expr(&chain, ORM_OR(ORM_EQ("person.active", orm_bool(1)),
                                                ORM_LIKE("person.name", orm_text("B%"))))
                     ->select.group_by(&chain, orm_view("department.name"))
                     ->having_expr(&chain, ORM_AGG_GT(ORM_AGGREGATE_COUNT_ALL, "", orm_i64(0)))
                     ->order_by(&chain, orm_view("department.name"), ORM_ORDER_ASCENDING)
                     ->execute(&chain, &result),
                 ORM_STATUS_OK, &error, "execute SQLite aggregate query");
  require_status(orm_result_row_count(result, &rows, &error), ORM_STATUS_OK, &error,
                 "read SQLite aggregate rows");
  require_true(rows == 2, "SQLite aggregate/JOIN result is incorrect");
  orm_result_destroy(result);
  orm_query_destroy(query);

  query = NULL;
  result = NULL;
  require_status(
      orm_raw(connection, string_view("select name from person where id = ?1"), &query, &error),
      ORM_STATUS_OK, &error, "create SQLite raw query");
  require_status(orm_query_bind(query, int64_value(3), &error), ORM_STATUS_OK, &error,
                 "bind SQLite raw query");
  require_status(orm_query_execute(query, &result, &error), ORM_STATUS_OK, &error,
                 "execute SQLite raw query");
  require_status(orm_result_get_text(result, 0, 0, &text, &error), ORM_STATUS_OK, &error,
                 "read SQLite raw result");
  require_true(text.len == 8 && memcmp(text.data, "Caroline", 8) == 0,
               "SQLite raw result is incorrect");
  orm_result_destroy(result);
  orm_query_destroy(query);

  query = NULL;
  result = NULL;
  require_status(
      orm_raw(connection, string_view("update person set note = ?1 where id = ?2"), &query, &error),
      ORM_STATUS_OK, &error, "create SQLite raw update");
  require_status(orm_query_bind(query, null_value(), &error), ORM_STATUS_OK, &error,
                 "bind SQLite raw null");
  require_status(orm_query_bind(query, int64_value(3), &error), ORM_STATUS_OK, &error,
                 "bind SQLite raw update id");
  require_status(orm_query_execute(query, &result, &error), ORM_STATUS_OK, &error,
                 "execute SQLite raw update");
  require_affected(result, 1, &error, "read SQLite raw affected rows");
  orm_result_destroy(result);
  orm_query_destroy(query);

  query = NULL;
  result = NULL;
  require_status(orm_delete(connection, string_view("person"), &query, &error), ORM_STATUS_OK,
                 &error, "create SQLite delete");
  require_status(
      orm_query_where(query, string_view("id"), ORM_COMPARE_EQUAL, int64_value(3), &error),
      ORM_STATUS_OK, &error, "add SQLite delete predicate");
  require_status(orm_query_execute(query, &result, &error), ORM_STATUS_OK, &error,
                 "execute SQLite delete");
  require_affected(result, 1, &error, "read SQLite delete affected rows");
  orm_result_destroy(result);
  orm_query_destroy(query);
  orm_disconnect(connection);
}

static void test_result_row_boundary(void) {
  orm_error_t error;
  orm_config_t config;
  orm_option_t options[2];
  orm_connection_t *connection = NULL;
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  sqlite3 *database = NULL;
  char *message = NULL;
  uint64_t rows = 0;
  const char *create_sql =
      "create table big(id integer);"
      "with recursive c(x) as (values(1) union all select x + 1 from c where x < 10001) "
      "insert into big select x from c;";

  require_true(sqlite3_open(database_filename, &database) == SQLITE_OK,
               "could not open the SQLite boundary fixture");
  if (sqlite3_exec(database, create_sql, NULL, NULL, &message) != SQLITE_OK) {
    fprintf(stderr, "SQLite boundary fixture failed: %s\n",
            message != NULL ? message : "unknown");
    sqlite3_free(message);
    (void)sqlite3_close(database);
    exit(EXIT_FAILURE);
  }
  require_true(sqlite3_close(database) == SQLITE_OK,
               "could not close the SQLite boundary fixture");

  orm_error_init(&error);
  orm_config(&config);
  options[0].keyword = string_view("filename");
  options[0].value = string_view(database_filename);
  options[1].keyword = string_view("open_mode");
  options[1].value = string_view("read_write");
  config.driver = string_view("sqlite");
  config.options = options;
  config.option_count = (uint32_t)(sizeof(options) / sizeof(options[0]));
  require_status(orm_connect(&config, &connection, &error), ORM_STATUS_OK, &error,
                 "create bounded-row SQLite connection");
  query = create_raw(connection, "select id from big", &error);
  require_status(orm_query_execute(query, &result, &error), ORM_STATUS_LIMIT_EXCEEDED, &error,
                 "enforce max_result_rows at 10000");
  require_true(result == NULL, "failed row-boundary execution returned a result");
  orm_query_destroy(query);
  orm_disconnect(connection);

  config.max_result_rows = 10001;
  connection = NULL;
  query = NULL;
  result = NULL;
  require_status(orm_connect(&config, &connection, &error), ORM_STATUS_OK, &error,
                 "create 10001-row SQLite connection");
  query = create_raw(connection, "select id from big", &error);
  require_status(orm_query_execute(query, &result, &error), ORM_STATUS_OK, &error,
                 "execute 10001-row SQLite query");
  require_status(orm_result_row_count(result, &rows, &error), ORM_STATUS_OK, &error,
                 "read 10001-row count");
  require_true(rows == 10001, "SQLite result row boundary is incorrect");
  orm_result_destroy(result);
  orm_query_destroy(query);
  orm_disconnect(connection);
}

static void test_blob_values(void) {
  orm_error_t error;
  orm_connection_t *connection;
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  const uint8_t payload[] = {0x00, 0x01, 0x00, 0xFF, 0x00, 0x80};
  orm_blob_t read_back;
  uint64_t affected = 0;

  orm_error_init(&error);
  connection = create_sqlite_connection(0, &error);

  require_status(orm_insert(connection, string_view("blobs"), &query, &error), ORM_STATUS_OK,
                 &error, "create SQLite blob insert");
  require_status(orm_query_set(query, string_view("id"), int64_value(1), &error), ORM_STATUS_OK,
                 &error, "set SQLite blob id");
  require_status(orm_query_set(query, string_view("data"), blob_value(payload, sizeof(payload)),
                               &error),
                 ORM_STATUS_OK, &error, "set SQLite blob payload");
  require_status(orm_query_execute(query, &result, &error), ORM_STATUS_OK, &error,
                 "execute SQLite blob insert");
  require_status(orm_result_affected_rows(result, &affected, &error), ORM_STATUS_OK, &error,
                 "read SQLite blob affected rows");
  require_true(affected == 1, "SQLite blob insert affected rows is incorrect");
  orm_result_destroy(result);
  orm_query_destroy(query);
  result = NULL;
  query = NULL;

  require_status(orm_query_create(connection, string_view("blobs"), &query, &error), ORM_STATUS_OK,
                 &error, "create SQLite blob select");
  require_status(orm_query_add_column(query, string_view("data"), &error), ORM_STATUS_OK, &error,
                 "select SQLite blob column");
  require_status(orm_query_execute(query, &result, &error), ORM_STATUS_OK, &error,
                 "execute SQLite blob select");
  read_back.data = NULL;
  read_back.size = 0;
  require_status(orm_result_get_blob(result, 0, 0, &read_back, &error), ORM_STATUS_OK, &error,
                 "read SQLite blob result");
  require_true(read_back.size == sizeof(payload) &&
                   memcmp(read_back.data, payload, sizeof(payload)) == 0,
               "SQLite blob round-trip is incorrect");
  orm_result_destroy(result);
  orm_query_destroy(query);
  result = NULL;
  query = NULL;

  require_status(orm_query_create(connection, string_view("blobs"), &query, &error), ORM_STATUS_OK,
                 &error, "create SQLite blob predicate query");
  require_status(
      orm_query_where(query, string_view("data"), ORM_COMPARE_EQUAL,
                      blob_value(payload, sizeof(payload)), &error),
      ORM_STATUS_UNSUPPORTED, &error, "reject SQLite blob predicate");
  orm_query_destroy(query);
  orm_disconnect(connection);
}

int main(void) {
  create_fixture();
  test_explicit_transactions();
  test_query_and_lifetimes();
  test_limits_and_errors();
  test_query_builder_features();
  test_result_row_boundary();
  test_blob_values();
  require_true(remove(database_filename) == 0, "could not remove SQLite fixture");
  puts("ORM SQLite C11 DSL/ABI tests passed");
  return EXIT_SUCCESS;
}

#include "c_api_test_support.h"
#include "orm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    fprintf(stderr, "C ABI test failed: %s\n", message);
    exit(EXIT_FAILURE);
  }
}

static void require_status(orm_status_t actual, orm_status_t expected, const orm_error_t *error,
                           const char *operation) {
  if (actual != expected) {
    fprintf(stderr, "C ABI test failed: %s returned %s (%s), expected %s\n", operation,
            orm_status_message(actual), error != NULL ? error->message : "no error object",
            orm_status_message(expected));
    exit(EXIT_FAILURE);
  }
}

static void test_turboutils_string_interop(void) {
  tstr owned = tstr_format("{}-{}", "user", 42);
  const orm_string_view_t view = orm_view_tstr(owned);
  const orm_value_t value = orm_text_v(view);
  require_true(owned != NULL && view.len == 7 && memcmp(view.data, "user-42", 7) == 0,
               "tstr view conversion failed");
  require_true(value.kind == ORM_VALUE_TEXT && value.data.text_value.data == owned,
               "vstr text value conversion failed");
  tstr_free(owned);
}

static orm_connection_t *create_postgres_connection(orm_config_t *config, uint64_t max_result_bytes,
                                                    orm_error_t *error) {
  static const orm_option_t options[] = {{{"host", 4}, {"localhost", 9}},
                                         {{"dbname", 6}, {"orm_test", 8}}};
  orm_connection_t *connection = NULL;
  orm_config(config);
  config->driver = string_view("postgresql");
  config->options = options;
  config->option_count = (uint32_t)(sizeof(options) / sizeof(options[0]));
  if (max_result_bytes != 0) config->max_result_bytes = max_result_bytes;
  require_status(orm_connect(config, &connection, error), ORM_STATUS_OK, error, "orm_connect");
  require_true(connection != NULL, "connection create returned a null handle");
  return connection;
}

static void test_explicit_transactions(void) {
  orm_error_t error;
  orm_config_t config;
  orm_connection_t *connection;
  orm_transaction_t *transaction = NULL;
  orm_transaction_t *nested = NULL;
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;

  fake_pg_reset();
  orm_error_init(&error);
  connection = create_postgres_connection(&config, 0, &error);

  require_status(orm_transaction_begin(connection, ORM_ISOLATION_SNAPSHOT, &transaction, &error),
                 ORM_STATUS_OK, &error, "begin PostgreSQL snapshot transaction");
  require_true(strcmp(fake_pg_last_sql(), "begin isolation level repeatable read") == 0,
               "PostgreSQL snapshot isolation did not map to repeatable read");
  require_status(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE, &nested, &error),
                 ORM_STATUS_INVALID_STATE, &error, "reject nested PostgreSQL transaction");
  require_true(nested == NULL, "failed nested PostgreSQL transaction returned a handle");

  require_status(orm_raw(connection, string_view("select 1"), &query, &error), ORM_STATUS_OK,
                 &error, "create PostgreSQL transaction query");
  require_status(orm_query_execute(query, &result, &error), ORM_STATUS_INVALID_STATE, &error,
                 "reject PostgreSQL transaction handle bypass");
  require_true(result == NULL, "bypassed PostgreSQL transaction returned a result");
  require_status(orm_query_execute_in_transaction(query, transaction, &result, &error),
                 ORM_STATUS_OK, &error, "execute PostgreSQL transaction query");
  orm_result_destroy(result);
  result = NULL;

  require_status(orm_transaction_savepoint(transaction, string_view("before_change"), &error),
                 ORM_STATUS_OK, &error, "create PostgreSQL savepoint");
  require_true(strcmp(fake_pg_last_sql(), "savepoint \"before_change\"") == 0,
               "PostgreSQL savepoint SQL is incorrect");
  require_status(orm_transaction_rollback_to_savepoint(
                     transaction, string_view("before_change"), &error),
                 ORM_STATUS_OK, &error, "roll back PostgreSQL savepoint");
  require_true(strcmp(fake_pg_last_sql(), "rollback to savepoint \"before_change\"") == 0,
               "PostgreSQL rollback-to-savepoint SQL is incorrect");
  require_status(orm_transaction_release_savepoint(
                     transaction, string_view("before_change"), &error),
                 ORM_STATUS_OK, &error, "release PostgreSQL savepoint");
  require_status(orm_transaction_commit(transaction, &error), ORM_STATUS_OK, &error,
                 "commit PostgreSQL transaction");
  require_true(strcmp(fake_pg_last_sql(), "commit") == 0,
               "PostgreSQL transaction did not commit");
  require_status(orm_transaction_commit(transaction, &error), ORM_STATUS_INVALID_STATE, &error,
                 "reject repeated PostgreSQL commit");
  orm_transaction_destroy(transaction);
  transaction = NULL;

  require_status(orm_transaction_begin(connection, ORM_ISOLATION_READ_UNCOMMITTED,
                                       &transaction, &error),
                 ORM_STATUS_OK, &error, "begin PostgreSQL read-uncommitted transaction");
  require_true(strcmp(fake_pg_last_sql(), "begin isolation level read committed") == 0,
               "PostgreSQL read-uncommitted behavior was not made explicit");
  orm_transaction_destroy(transaction);
  require_true(strcmp(fake_pg_last_sql(), "rollback") == 0,
               "destroying PostgreSQL transaction did not roll back");

  orm_query_destroy(query);
  orm_disconnect(connection);
}

static void test_query_and_result_lifetimes(void) {
  orm_error_t error;
  orm_config_t config;
  orm_connection_t *connection;
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  const char *values[] = {"7", "Alice", "8", NULL, "9", "t"};
  const uint8_t nulls[] = {0, 0, 0, 1, 0, 0};
  uint64_t count = 0;
  uint64_t unsigned_identifier = 0;
  int64_t identifier = 0;
  double floating_identifier = 0.0;
  uint8_t boolean_value = 0;
  uint8_t is_null = 0;
  orm_string_view_t name;

  fake_pg_reset();
  orm_error_init(&error);
  connection = create_postgres_connection(&config, 0, &error);
  require_status(orm_query_create(connection, string_view("public.person"), &query, &error),
                 ORM_STATUS_OK, &error, "orm_query_create");
  require_status(orm_query_add_column(query, string_view("id"), &error), ORM_STATUS_OK, &error,
                 "add id column");
  require_status(orm_query_add_column(query, string_view("name"), &error), ORM_STATUS_OK, &error,
                 "add name column");
  require_status(
      orm_query_where(query, string_view("id"), ORM_COMPARE_GREATER_EQUAL, int64_value(7), &error),
      ORM_STATUS_OK, &error, "add id predicate");
  require_status(
      orm_query_where(query, string_view("deleted_at"), ORM_COMPARE_EQUAL, null_value(), &error),
      ORM_STATUS_OK, &error, "add null predicate");
  require_status(
      orm_query_where(query, string_view("name"), ORM_COMPARE_LIKE, text_value("A%"), &error),
      ORM_STATUS_OK, &error, "add name predicate");
  require_status(orm_query_order_by(query, string_view("id"), ORM_ORDER_DESCENDING, &error),
                 ORM_STATUS_OK, &error, "set ordering");
  require_status(orm_query_set_limit(query, 10, &error), ORM_STATUS_OK, &error, "set limit");
  require_status(orm_query_set_offset(query, 2, &error), ORM_STATUS_OK, &error, "set offset");

  orm_disconnect(connection);
  require_true(fake_pg_finished_connections() == 0,
               "query did not retain its PostgreSQL connection");

  fake_pg_set_result(3, 2, values, nulls);
  require_status(orm_query_execute(query, &result, &error), ORM_STATUS_OK, &error, "execute query");
  require_true(strcmp(fake_pg_last_sql(),
                      "select id, name from public.person where id >= $1 and "
                      "deleted_at is null and name like $2 order by id desc limit 10 offset 2") ==
                   0,
               "generated SQL did not match the expected parameterized query");
  require_true(fake_pg_parameter_count() == 2, "unexpected PostgreSQL parameter count");
  require_true(strcmp(fake_pg_parameter_at(0), "7") == 0 &&
                   strcmp(fake_pg_parameter_at(1), "A%") == 0,
               "PostgreSQL parameters were not copied in placeholder order");

  require_status(orm_result_row_count(result, &count, &error), ORM_STATUS_OK, &error,
                 "read row count");
  require_true(count == 3, "unexpected row count");
  require_status(orm_result_column_count(result, &count, &error), ORM_STATUS_OK, &error,
                 "read column count");
  require_true(count == 2, "unexpected column count");
  require_status(orm_result_get_int64(result, 0, 0, &identifier, &error), ORM_STATUS_OK, &error,
                 "read integer cell");
  require_true(identifier == 7, "integer result conversion failed");
  require_status(orm_result_get_uint64(result, 0, 0, &unsigned_identifier, &error), ORM_STATUS_OK,
                 &error, "read unsigned integer cell");
  require_true(unsigned_identifier == 7, "unsigned result conversion failed");
  require_status(orm_result_get_double(result, 0, 0, &floating_identifier, &error), ORM_STATUS_OK,
                 &error, "read floating-point cell");
  require_true(floating_identifier == 7.0, "floating-point result conversion failed");
  require_status(orm_result_get_text(result, 0, 1, &name, &error), ORM_STATUS_OK, &error,
                 "read text cell");
  require_true(name.len == 5 && memcmp(name.data, "Alice", 5) == 0,
               "borrowed text result was incorrect");
  require_status(orm_result_is_null(result, 1, 1, &is_null, &error), ORM_STATUS_OK, &error,
                 "inspect null cell");
  require_true(is_null == 1, "SQL NULL was not reported");
  require_status(orm_result_get_text(result, 1, 1, &name, &error), ORM_STATUS_NULL_VALUE, &error,
                 "read null text cell");
  require_true(name.data == NULL && name.len == 0, "failed text output was not cleared");
  require_status(orm_result_get_boolean(result, 2, 1, &boolean_value, &error), ORM_STATUS_OK,
                 &error, "read boolean cell");
  require_true(boolean_value == 1, "boolean result conversion failed");
  require_status(orm_result_get_int64(result, 3, 0, &identifier, &error), ORM_STATUS_OUT_OF_RANGE,
                 &error, "read out-of-range cell");

  orm_query_destroy(query);
  require_true(fake_pg_finished_connections() == 1,
               "retained PostgreSQL connection was not released exactly once");
  orm_result_destroy(result);
  require_true(fake_pg_cleared_results() == 1, "PostgreSQL result was not released exactly once");
}

static void test_limits_and_errors(void) {
  orm_error_t error;
  orm_config_t config;
  orm_connection_t *connection;
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  const char *values[] = {"12345"};
  const uint8_t nulls[] = {0};

  fake_pg_reset();
  orm_error_init(&error);
  connection = create_postgres_connection(&config, 0, &error);
  config.abi_version = ORM_C_ABI_VERSION + 1;
  {
    orm_connection_t *rejected = NULL;
    require_status(orm_connect(&config, &rejected, &error), ORM_STATUS_ABI_MISMATCH, &error,
                   "reject ABI mismatch");
    require_true(rejected == NULL, "ABI mismatch returned a connection handle");
  }
  config.abi_version = ORM_C_ABI_VERSION;
  config.driver = string_view("unsupported");
  {
    orm_connection_t *rejected = NULL;
    require_status(orm_connect(&config, &rejected, &error), ORM_STATUS_INVALID_ARGUMENT, &error,
                   "reject unknown driver");
    require_true(rejected == NULL, "unknown driver returned a connection handle");
  }

  require_status(orm_query_create(connection, string_view("person;drop"), &query, &error),
                 ORM_STATUS_INVALID_ARGUMENT, &error, "reject unsafe identifier");
  require_true(query == NULL, "unsafe identifier returned a query handle");

  require_status(orm_query_create(connection, string_view("person"), &query, &error), ORM_STATUS_OK,
                 &error, "create bounded query");
  require_status(orm_query_execute(query, &result, &error), ORM_STATUS_INVALID_STATE, &error,
                 "reject missing selection");
  require_true(result == NULL, "invalid query returned a result handle");
  {
    orm_chain_t chain = orm_chain(query, &error);
    require_status(chain.select.all(&chain)
                       ->condition.begin_where(&chain, ORM_LOGIC_OR)
                       ->where(&chain, string_view("id"), ORM_COMPARE_EQUAL, int64_value(1))
                       ->execute(&chain, &result),
                   ORM_STATUS_INVALID_STATE, &error, "reject fluent unclosed group");
    require_true(chain.status == ORM_STATUS_INVALID_STATE,
                 "fluent execution did not retain its first failure");
    require_true(chain.where(&chain, string_view("id"), ORM_COMPARE_EQUAL, int64_value(2)) ==
                         &chain &&
                     chain.status == ORM_STATUS_INVALID_STATE,
                 "fluent failure was not sticky");
  }
  require_true(result == NULL, "unclosed group returned a result handle");
  require_status(orm_query_end_where_group(query, &error), ORM_STATUS_OK, &error,
                 "close recovered group");

  orm_disconnect(connection);
  orm_query_destroy(query);

  connection = create_postgres_connection(&config, 4, &error);
  require_status(orm_query_create(connection, string_view("person"), &query, &error), ORM_STATUS_OK,
                 &error, "create result-limited query");
  require_status(orm_query_select_all(query, &error), ORM_STATUS_OK, &error,
                 "select all for result limit");
  fake_pg_set_result(1, 1, values, nulls);
  require_status(orm_query_execute(query, &result, &error), ORM_STATUS_LIMIT_EXCEEDED, &error,
                 "enforce result byte limit");
  require_true(result == NULL, "oversized result returned a handle");
  require_true(fake_pg_cleared_results() == 1, "oversized PostgreSQL result was not released");

  fake_pg_fail_next_query();
  require_status(orm_query_execute(query, &result, &error), ORM_STATUS_SQL_ERROR, &error,
                 "map PostgreSQL failure");
  require_true(result == NULL, "failed SQL execution returned a result handle");
  require_true(strstr(error.message, "forced SQL failure") != NULL,
               "SQL error context was not propagated");

  orm_query_destroy(query);
  orm_disconnect(connection);
}

static void require_affected(orm_result_t *result, uint64_t expected, orm_error_t *error,
                             const char *operation) {
  uint64_t actual = 0;
  require_status(orm_result_affected_rows(result, &actual, error), ORM_STATUS_OK, error, operation);
  require_true(actual == expected, "unexpected affected-row count");
}

static void test_query_builder_features(void) {
  orm_error_t error;
  orm_config_t config;
  orm_connection_t *connection;
  orm_query_t *query = NULL;
  orm_query_t *subquery = NULL;
  orm_result_t *result = NULL;
  orm_chain_t chain;
  orm_expression_t overflow_expression;
  uint32_t expression_index;
  const orm_string_view_t empty = {NULL, 0};

  fake_pg_reset();
  orm_error_init(&error);
  connection = create_postgres_connection(&config, 0, &error);

  require_status(orm_query_create(connection, string_view("person"), &query, &error), ORM_STATUS_OK,
                 &error, "create aggregate query");
  chain = orm_chain(query, &error);
  fake_pg_set_result(0, 2, NULL, NULL);
  require_status(
      chain.select.column(&chain, string_view("department.name"))
          ->select.aggregate(&chain, ORM_AGGREGATE_COUNT_ALL, empty, string_view("total"))
          ->select
          .join(&chain, ORM_JOIN_INNER, string_view("department"),
                string_view("person.department_id"), ORM_COMPARE_EQUAL,
                string_view("department.id"))
          ->where_expr(&chain, ORM_OR(ORM_GE("person.age", orm_i64(18)),
                                      ORM_AND(ORM_LIKE("person.name", orm_text("A%")),
                                              ORM_NE("person.id", orm_i64(99)))))
          ->select.group_by(&chain, string_view("department.name"))
          ->having_expr(&chain, ORM_OR(ORM_AGG_GT(ORM_AGGREGATE_COUNT_ALL, "", orm_i64(2)),
                                       ORM_NE("department.name", orm_text("Archived"))))
          ->order_by(&chain, string_view("department.name"), ORM_ORDER_ASCENDING)
          ->limit(&chain, 100)
          ->offset(&chain, 0)
          ->execute(&chain, &result),
      ORM_STATUS_OK, &error, "execute fluent aggregate query");
  require_true(strcmp(fake_pg_last_sql(),
                      "select department.name, count(*) as total from person "
                      "inner join department on person.department_id = department.id "
                      "where (person.age >= $1 or (person.name like $2 and person.id != $3)) "
                      "group by department.name having (count(*) > $4 or "
                      "department.name != $5) order by department.name asc "
                      "limit 100 offset 0") == 0,
               "aggregate/JOIN/group SQL is incorrect");
  require_true(fake_pg_parameter_count() == 5 && strcmp(fake_pg_parameter_at(0), "18") == 0 &&
                   strcmp(fake_pg_parameter_at(1), "A%") == 0 &&
                   strcmp(fake_pg_parameter_at(2), "99") == 0 &&
                   strcmp(fake_pg_parameter_at(3), "2") == 0 &&
                   strcmp(fake_pg_parameter_at(4), "Archived") == 0,
               "aggregate query parameter order is incorrect");
  orm_result_destroy(result);
  orm_query_destroy(query);

  query = NULL;
  result = NULL;
  {
    orm_scalar_token_t malformed_tokens[] = {
        {ORM_SCALAR_COLUMN, string_view("score"), orm_null()},
        {ORM_SCALAR_ADD, string_view(""), orm_null()}};
    const orm_scalar_expression_t malformed = {malformed_tokens, 2};
    orm_scalar_token_t tokens[] = {
        {ORM_SCALAR_COLUMN, string_view("score"), orm_null()},
        {ORM_SCALAR_VALUE, string_view(""), orm_f64(2.0)},
        {ORM_SCALAR_MULTIPLY, string_view(""), orm_null()},
        {ORM_SCALAR_VALUE, string_view(""), orm_f64(5.0)},
        {ORM_SCALAR_ADD, string_view(""), orm_null()}};
    const orm_scalar_expression_t expression = {tokens, 5};
    require_status(orm_query_create(connection, string_view("person"), &query, &error),
                   ORM_STATUS_OK, &error, "create scalar expression query");
    require_status(orm_query_add_expression(query, malformed, string_view("invalid"), &error),
                   ORM_STATUS_INVALID_ARGUMENT, &error,
                   "reject malformed scalar postfix expression");
    require_status(orm_query_add_expression(query, expression, string_view("adjusted"), &error),
                   ORM_STATUS_OK, &error, "select scalar expression");
    fake_pg_set_result(0, 0, NULL, NULL);
    require_status(orm_query_execute(query, &result, &error), ORM_STATUS_OK, &error,
                   "execute scalar expression query");
    require_true(strcmp(fake_pg_last_sql(),
                        "select ((score * $1) + $2) as adjusted from person") == 0,
                 "scalar expression SQL is incorrect");
    require_true(fake_pg_parameter_count() == 2 &&
                     strcmp(fake_pg_parameter_at(0), "2") == 0 &&
                     strcmp(fake_pg_parameter_at(1), "5") == 0,
                 "scalar expression parameters are not in postfix operand order");
  }
  orm_result_destroy(result);
  orm_query_destroy(query);

  query = NULL;
  result = NULL;
  {
    orm_scalar_token_t order_tokens[] = {
        {ORM_SCALAR_COLUMN, string_view("score"), orm_null()},
        {ORM_SCALAR_VALUE, string_view(""), orm_f64(5.0)},
        {ORM_SCALAR_ADD, string_view(""), orm_null()}};
    const orm_scalar_expression_t order_expression = {order_tokens, 3};
    require_status(orm_query_create(connection, string_view("person"), &query, &error),
                   ORM_STATUS_OK, &error, "create order-by scalar query");
    require_status(orm_query_add_column(query, string_view("id"), &error), ORM_STATUS_OK, &error,
                   "select order-by scalar column");
    require_status(orm_query_order_by_expression(query, order_expression, ORM_ORDER_DESCENDING, &error),
                   ORM_STATUS_OK, &error, "append ORDER BY scalar expression");
    fake_pg_set_result(0, 0, NULL, NULL);
    require_status(orm_query_execute(query, &result, &error), ORM_STATUS_OK, &error,
                   "execute ORDER BY scalar expression query");
    require_true(strcmp(fake_pg_last_sql(),
                       "select id from person order by (score + $1) desc") == 0,
                 "scalar ORDER BY SQL is incorrect");
    require_true(fake_pg_parameter_count() == 1 && strcmp(fake_pg_parameter_at(0), "5") == 0,
                 "ORDER BY scalar expression parameter order is incorrect");
  }
  orm_result_destroy(result);
  orm_query_destroy(query);

  query = NULL;
  subquery = NULL;
  result = NULL;
  require_status(orm_query_create(connection, string_view("person"), &subquery, &error),
                 ORM_STATUS_OK, &error, "create scalar subquery");
  require_status(orm_query_add_aggregate(subquery, ORM_AGGREGATE_AVERAGE,
                                         string_view("score"), string_view(""), &error),
                 ORM_STATUS_OK, &error, "select scalar aggregate");
  require_status(orm_query_create(connection, string_view("person"), &query, &error),
                 ORM_STATUS_OK, &error, "create outer scalar query");
  require_status(orm_query_add_column(query, string_view("id"), &error), ORM_STATUS_OK,
                 &error, "select outer scalar column");
  require_status(orm_query_where_scalar_subquery(query, string_view("score"),
                                                 ORM_COMPARE_GREATER, subquery, &error),
                 ORM_STATUS_OK, &error, "attach scalar subquery snapshot");
  orm_query_destroy(subquery);
  subquery = NULL;
  fake_pg_set_result(0, 0, NULL, NULL);
  require_status(orm_query_execute(query, &result, &error), ORM_STATUS_OK, &error,
                 "execute scalar subquery");
  require_true(strcmp(fake_pg_last_sql(),
                      "select id from person where score > (select avg(score) from person)") ==
                   0,
               "scalar subquery SQL is incorrect");
  orm_result_destroy(result);
  orm_query_destroy(query);

  query = NULL;
  subquery = NULL;
  result = NULL;
  require_status(orm_query_create(connection, string_view("person"), &subquery, &error),
                 ORM_STATUS_OK, &error, "create quantified subquery");
  require_status(orm_query_add_column(subquery, string_view("score"), &error), ORM_STATUS_OK,
                 &error, "select quantified subquery column");
  require_status(orm_query_where(subquery, string_view("active"), ORM_COMPARE_EQUAL,
                                 orm_bool(1), &error),
                 ORM_STATUS_OK, &error, "filter quantified subquery");
  require_status(orm_query_create(connection, string_view("person"), &query, &error),
                 ORM_STATUS_OK, &error, "create outer quantified query");
  require_status(orm_query_add_column(query, string_view("id"), &error), ORM_STATUS_OK,
                 &error, "select outer quantified column");
  require_status(orm_query_where(query, string_view("name"), ORM_COMPARE_NOT_EQUAL,
                                 text_value("Nobody"), &error),
                 ORM_STATUS_OK, &error, "filter outer quantified query");
  require_status(orm_query_where_quantified_subquery(
                     query, string_view("score"), ORM_COMPARE_GREATER,
                     (orm_subquery_quantifier_t)99, subquery, &error),
                 ORM_STATUS_INVALID_ARGUMENT, &error, "reject unknown subquery quantifier");
  require_status(orm_query_where_quantified_subquery(
                     query, string_view("score"), ORM_COMPARE_GREATER,
                     ORM_SUBQUERY_ANY, subquery, &error),
                 ORM_STATUS_OK, &error, "attach ANY subquery snapshot");
  orm_query_destroy(subquery);
  subquery = NULL;
  fake_pg_set_result(0, 0, NULL, NULL);
  require_status(orm_query_execute(query, &result, &error), ORM_STATUS_OK, &error,
                 "execute ANY subquery");
  require_true(strcmp(fake_pg_last_sql(),
                      "select id from person where name != $1 and score > any (select score "
                      "from person where active = $2)") == 0,
               "ANY subquery SQL is incorrect");
  orm_result_destroy(result);
  orm_query_destroy(query);

  query = NULL;
  subquery = NULL;
  result = NULL;
  require_status(orm_query_create(connection, string_view("person"), &subquery, &error),
                 ORM_STATUS_OK, &error, "create ALL subquery");
  require_status(orm_query_add_column(subquery, string_view("score"), &error), ORM_STATUS_OK,
                 &error, "select ALL subquery column");
  require_status(orm_query_create(connection, string_view("person"), &query, &error),
                 ORM_STATUS_OK, &error, "create outer ALL query");
  require_status(orm_query_add_column(query, string_view("id"), &error), ORM_STATUS_OK,
                 &error, "select outer ALL column");
  require_status(orm_query_where_quantified_subquery(
                     query, string_view("score"), ORM_COMPARE_LESS_EQUAL,
                     ORM_SUBQUERY_ALL, subquery, &error),
                 ORM_STATUS_OK, &error, "attach ALL subquery snapshot");
  orm_query_destroy(subquery);
  subquery = NULL;
  fake_pg_set_result(0, 0, NULL, NULL);
  require_status(orm_query_execute(query, &result, &error), ORM_STATUS_OK, &error,
                 "execute ALL subquery");
  require_true(strcmp(fake_pg_last_sql(),
                      "select id from person where score <= all (select score from person)") ==
                   0,
               "ALL subquery SQL is incorrect");
  orm_result_destroy(result);
  orm_query_destroy(query);

  query = NULL;
  result = NULL;
  require_status(orm_query_create(connection, string_view("department"), &subquery, &error),
                 ORM_STATUS_OK, &error, "create IN subquery");
  require_status(orm_query_add_column(subquery, string_view("id"), &error), ORM_STATUS_OK,
                 &error, "select IN subquery column");
  require_status(orm_query_where(subquery, string_view("name"), ORM_COMPARE_EQUAL,
                                 text_value("Sales"), &error),
                 ORM_STATUS_OK, &error, "filter IN subquery");
  require_status(orm_query_create(connection, string_view("person"), &query, &error),
                 ORM_STATUS_OK, &error, "create outer IN query");
  require_status(orm_query_add_column(query, string_view("id"), &error), ORM_STATUS_OK,
                 &error, "select outer IN column");
  require_status(orm_query_where_in_subquery(query, string_view("department_id"),
                                             subquery, 0, &error),
                 ORM_STATUS_OK, &error, "attach IN subquery snapshot");
  orm_query_destroy(subquery);
  subquery = NULL;
  fake_pg_set_result(0, 0, NULL, NULL);
  require_status(orm_query_execute(query, &result, &error), ORM_STATUS_OK, &error,
                 "execute IN subquery");
  require_true(strcmp(fake_pg_last_sql(),
                      "select id from person where department_id in (select id from "
                      "department where name = $1)") == 0,
               "IN subquery SQL is incorrect");
  orm_result_destroy(result);
  orm_query_destroy(query);

  query = NULL;
  result = NULL;
  require_status(orm_query_create(connection, string_view("department"), &subquery, &error),
                 ORM_STATUS_OK, &error, "create EXISTS subquery");
  require_status(orm_query_add_column(subquery, string_view("id"), &error), ORM_STATUS_OK,
                 &error, "select EXISTS subquery column");
  require_status(orm_query_where(subquery, string_view("name"), ORM_COMPARE_EQUAL,
                                 text_value("Sales"), &error),
                 ORM_STATUS_OK, &error, "filter EXISTS subquery");
  require_status(orm_query_where_columns(subquery, string_view("department.id"),
                                         ORM_COMPARE_EQUAL,
                                         string_view("person.department_id"), &error),
                 ORM_STATUS_OK, &error, "correlate EXISTS subquery");
  require_status(orm_query_create(connection, string_view("person"), &query, &error),
                 ORM_STATUS_OK, &error, "create outer EXISTS query");
  require_status(orm_query_select_all(query, &error), ORM_STATUS_OK, &error,
                 "select outer EXISTS rows");
  require_status(orm_query_where(query, string_view("active"), ORM_COMPARE_EQUAL,
                                 orm_bool(1), &error),
                 ORM_STATUS_OK, &error, "filter outer EXISTS query");
  require_status(orm_query_where_exists(query, subquery, 0, &error), ORM_STATUS_OK,
                 &error, "attach EXISTS subquery snapshot");
  orm_query_destroy(subquery);
  subquery = NULL;
  fake_pg_set_result(0, 0, NULL, NULL);
  require_status(orm_query_execute(query, &result, &error), ORM_STATUS_OK, &error,
                 "execute EXISTS query");
  require_true(strcmp(fake_pg_last_sql(),
                      "select * from person where active = $1 and exists (select id from "
                      "department where name = $2 and department.id = person.department_id)") == 0,
               "EXISTS SQL or parameter numbering is incorrect");
  orm_result_destroy(result);
  orm_query_destroy(query);

  query = NULL;
  result = NULL;
  require_status(orm_query_create(connection, string_view("person"), &query, &error),
                 ORM_STATUS_OK, &error, "create range and membership query");
  chain = orm_chain(query, &error);
  {
    const orm_value_t ids[] = {orm_i64(1), orm_i64(2), orm_i64(3)};
    fake_pg_set_result(0, 0, NULL, NULL);
    require_status(
        chain.select.all(&chain)
            ->select.distinct(&chain, 1)
            ->where_expr(&chain,
                         ORM_AND(ORM_BETWEEN("age", orm_i64(18), orm_i64(65)),
                                 ORM_AND(orm_expression_in(orm_view("id"), ids, 3),
                                         ORM_IS_NULL("deleted_at"))))
            ->execute(&chain, &result),
        ORM_STATUS_OK, &error, "execute range and membership query");
  }
  require_true(strcmp(fake_pg_last_sql(),
                      "select distinct * from person where (age >= $1 and age <= $2 and "
                      "(id = $3 or id = $4 or id = $5) and deleted_at is null)") == 0,
               "range/membership/null SQL is incorrect");
  orm_result_destroy(result);
  orm_query_destroy(query);

  query = NULL;
  result = NULL;
  require_status(orm_insert(connection, string_view("person"), &query, &error), ORM_STATUS_OK,
                 &error, "create insert");
  chain = orm_chain(query, &error);
  require_status(chain.set(&chain, string_view("name"), text_value("Cara"))
                     ->set(&chain, string_view("note"), null_value())
                     ->execute(&chain, &result),
                 ORM_STATUS_OK, &error, "execute fluent insert");
  require_true(strcmp(fake_pg_last_sql(), "insert into person (name, note) values ($1, null)") == 0,
               "insert SQL is incorrect");
  require_affected(result, 1, &error, "read insert affected rows");
  orm_result_destroy(result);
  orm_query_destroy(query);

  query = NULL;
  result = NULL;
  require_status(orm_update(connection, string_view("person"), &query, &error), ORM_STATUS_OK,
                 &error, "create update");
  chain = orm_chain(query, &error);
  require_status(
      chain.set(&chain, string_view("name"), text_value("Renamed"))
          ->where_expr(&chain, ORM_OR(ORM_EQ("id", orm_i64(2)), ORM_LIKE("name", orm_text("B%"))))
          ->execute(&chain, &result),
      ORM_STATUS_OK, &error, "execute fluent update");
  require_true(strcmp(fake_pg_last_sql(),
                      "update person set name = $1 where (id = $2 or name like $3)") == 0,
               "update SQL or placeholder ordering is incorrect");
  require_affected(result, 1, &error, "read update affected rows");
  orm_result_destroy(result);
  orm_query_destroy(query);

  query = NULL;
  result = NULL;
  require_status(orm_delete(connection, string_view("person"), &query, &error), ORM_STATUS_OK,
                 &error, "create delete");
  chain = orm_chain(query, &error);
  require_status(chain.where_expr(&chain, ORM_EQ("id", orm_i64(3)))->execute(&chain, &result),
                 ORM_STATUS_OK, &error, "execute fluent delete");
  require_true(strcmp(fake_pg_last_sql(), "delete from person where id = $1") == 0,
               "delete SQL is incorrect");
  require_affected(result, 1, &error, "read delete affected rows");
  orm_result_destroy(result);
  orm_query_destroy(query);

  query = NULL;
  result = NULL;
  require_status(orm_query_create(connection, string_view("person"), &query, &error), ORM_STATUS_OK,
                 &error, "create expression validation query");
  require_status(orm_query_select_all(query, &error), ORM_STATUS_OK, &error,
                 "select all for expression validation");
  chain = orm_chain(query, &error);
  require_true(chain.where_expr(&chain, ORM_AGG_GT(ORM_AGGREGATE_COUNT_ALL, "", orm_i64(0))) ==
                   &chain,
               "invalid expression did not preserve fluent self");
  require_status(chain.status, ORM_STATUS_INVALID_ARGUMENT, &error,
                 "reject aggregate WHERE expression");
  require_true(strstr(error.message, "condition expression") != NULL,
               "expression validation omitted diagnostic context");
  require_true(chain.execute(&chain, &result) == ORM_STATUS_INVALID_ARGUMENT && result == NULL,
               "expression validation failure was not sticky");

  overflow_expression = ORM_EQ("id", orm_i64(1));
  for (expression_index = 1; expression_index < ORM_C_EXPRESSION_CAPACITY; ++expression_index)
    overflow_expression = ORM_AND(overflow_expression, ORM_EQ("id", orm_i64(expression_index)));
  chain = orm_chain(query, &error);
  require_true(chain.where_expr(&chain, overflow_expression) == &chain,
               "oversized expression did not preserve fluent self");
  require_status(chain.status, ORM_STATUS_LIMIT_EXCEEDED, &error,
                 "reject oversized condition expression");
  require_true(strstr(error.message, "capacity") != NULL,
               "expression capacity failure omitted diagnostic context");

  chain = orm_chain(query, &error);
  fake_pg_set_result(0, 1, NULL, NULL);
  require_status(chain.where_expr(&chain, ORM_EQ("id", orm_i64(4)))->execute(&chain, &result),
                 ORM_STATUS_OK, &error, "execute query after rejected expression");
  require_true(strcmp(fake_pg_last_sql(), "select * from person where id = $1") == 0,
               "rejected expression partially mutated the query");
  orm_result_destroy(result);
  orm_query_destroy(query);

  query = NULL;
  result = NULL;
  require_status(orm_raw(connection, string_view("select $1"), &query, &error), ORM_STATUS_OK,
                 &error, "create raw query");
  chain = orm_chain(query, &error);
  fake_pg_set_result(0, 1, NULL, NULL);
  require_status(chain.bind(&chain, null_value())->execute(&chain, &result), ORM_STATUS_OK, &error,
                 "execute fluent raw query");
  require_true(strcmp(fake_pg_last_sql(), "select $1") == 0 && fake_pg_parameter_count() == 1 &&
                   fake_pg_parameter_at(0) == NULL,
               "raw SQL or NULL binding is incorrect");
  orm_result_destroy(result);
  orm_query_destroy(query);
  orm_disconnect(connection);
}

static void test_blob_bytea(void) {
  orm_error_t error;
  orm_config_t config;
  orm_connection_t *connection;
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  const uint8_t payload[] = {0x00, 0x01, 0x00, 0xFF, 0x10, 0x80};
  const char *hex_payload = "\\x000100ff1080";
  const char *values[1];
  const uint8_t null_flags[1] = {0};
  const uint32_t types[1] = {17};
  orm_blob_t read_back;

  fake_pg_reset();
  orm_error_init(&error);
  connection = create_postgres_connection(&config, 0, &error);

  require_status(orm_insert(connection, string_view("files"), &query, &error), ORM_STATUS_OK,
                 &error, "create PostgreSQL blob insert");
  require_status(orm_query_set(query, string_view("id"), int64_value(1), &error), ORM_STATUS_OK,
                 &error, "set PostgreSQL blob id");
  require_status(orm_query_set(query, string_view("data"), blob_value(payload, sizeof(payload)),
                               &error),
                 ORM_STATUS_OK, &error, "set PostgreSQL blob payload");
  require_status(orm_query_execute(query, &result, &error), ORM_STATUS_OK, &error,
                 "execute PostgreSQL blob insert");
  require_true(strcmp(fake_pg_last_sql(),
                     "insert into files (id, data) values ($1, $2)") == 0 &&
                   fake_pg_parameter_count() == 2 &&
                   strcmp(fake_pg_parameter_at(0), "1") == 0 &&
                   strcmp(fake_pg_parameter_at(1), hex_payload) == 0,
               "PostgreSQL blob parameter was not encoded as bytea hex");
  orm_result_destroy(result);
  orm_query_destroy(query);
  result = NULL;
  query = NULL;

  values[0] = hex_payload;
  fake_pg_set_result_types(1, types);
  fake_pg_set_result(1, 1, values, null_flags);
  require_status(orm_raw(connection, string_view("select data from files"), &query, &error),
                 ORM_STATUS_OK, &error, "create PostgreSQL blob select");
  require_status(orm_query_execute(query, &result, &error), ORM_STATUS_OK, &error,
                 "execute PostgreSQL blob select");
  read_back.data = NULL;
  read_back.size = 0;
  require_status(orm_result_get_blob(result, 0, 0, &read_back, &error), ORM_STATUS_OK, &error,
                 "read PostgreSQL bytea result");
  require_true(read_back.size == sizeof(payload) &&
                   memcmp(read_back.data, payload, sizeof(payload)) == 0,
               "PostgreSQL bytea round-trip is incorrect");
  orm_result_destroy(result);
  orm_query_destroy(query);
  orm_disconnect(connection);
}

int main(void) {
  require_true(orm_c_abi_version() == ORM_C_ABI_VERSION,
               "runtime ABI version does not match the header");
  test_turboutils_string_interop();
  test_explicit_transactions();
  test_query_and_result_lifetimes();
  test_limits_and_errors();
  test_query_builder_features();
  test_blob_bytea();
  puts("ORM C11 DSL/ABI tests passed");
  return EXIT_SUCCESS;
}

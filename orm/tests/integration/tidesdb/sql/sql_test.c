#include "../row_fixture.h"
#include <orm_runtime.h>
#include <tinytest.h>
#include <stdlib.h>

static orm_runtime_t *runtime;
static orm_connection_t *connection;
static orm_query_t *query;
static orm_transaction_t *transaction;
static cflow_publisher publisher;
static orm_error_t error;
static char *directory;

static void raw_query(const char *sql) {
  orm_query_destroy(query);
  query = NULL;
  check_equal(orm_raw(connection, orm_view(sql), &query, &error), ORM_STATUS_OK);
}

static void parameter(orm_value_t input) {
  check_equal(orm_query_bind(query, input, &error), ORM_STATUS_OK);
}

static orm_status_t open_command(void) {
  return transaction != NULL
      ? orm_query_open_command_flow_in_transaction(query, transaction, &publisher, &error)
      : orm_query_open_command_flow(query, &publisher, &error);
}

static void command(uint64_t expected) {
  orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
  check_equal(open_command(), ORM_STATUS_OK);
  check_equal(cflow_publisher_resume(&publisher, NULL, &result).kind, CFLOW_STEP_VALUE_AND_DONE);
  check_equal(result.affected_rows, expected);
  cflow_publisher_destroy(&publisher);
  publisher = (cflow_publisher){0};
}

static void open_rows(void) {
  orm_flow_config_t flow;
  orm_flow_config(&flow, &orm_tides_public_row_data);
  orm_status_t status = transaction != NULL
      ? orm_query_open_flow_in_transaction(query, transaction, &flow, &publisher, &error)
      : orm_query_open_flow(query, &flow, &publisher, &error);
  check_equal(status, ORM_STATUS_OK);
}

static void row(long id, long score) {
  orm_tides_public_row value = {0};
  check_equal(cflow_publisher_resume(&publisher, NULL, &value).kind, CFLOW_STEP_VALUE);
  check_equal(value.id, id);
  check_equal(value.score, score);
}

static void end_rows(void) {
  orm_tides_public_row value = {0};
  check_equal(cflow_publisher_resume(&publisher, NULL, &value).kind, CFLOW_STEP_DONE);
  cflow_publisher_destroy(&publisher);
  publisher = (cflow_publisher){0};
}

spec("TidesDB SQL through the installed driver contract") {
  before_each() {
    runtime = NULL; connection = NULL; query = NULL; transaction = NULL;
    publisher = (cflow_publisher){0};
    directory = tt_make_temp_dir("orm-tidesdb-sql");
    check_not_null(directory);
    orm_runtime_config_t runtime_config;
    orm_runtime_config_init(&runtime_config);
    orm_error_init(&error);
    check_equal(orm_runtime_create(&runtime_config, &runtime, &error), ORM_STATUS_OK);
    const char *module = getenv("ORM_TIDESDB_PLUGIN");
    check_not_null(module);
    orm_driver_load_config_t load = {0};
    load.struct_size = sizeof(load);
    load.abi_version = ORM_RUNTIME_ABI_VERSION;
    load.module_path = orm_view(module);
    load.expected_driver_id = orm_view("tidesdb");
    check_equal(orm_runtime_load_driver(runtime, &load, &error), ORM_STATUS_OK);
    orm_config_t config;
    orm_config(&config);
    orm_option_t option = {orm_view("path"), orm_view(directory)};
    config.driver = orm_view("tidesdb");
    config.options = &option; config.option_count = 1;
    check_equal(orm_runtime_connect(runtime, &config, &connection, &error), ORM_STATUS_OK);
  }
  after_each() {
    if (cflow_publisher_valid(&publisher)) cflow_publisher_destroy(&publisher);
    orm_query_destroy(query);
    orm_transaction_destroy(transaction);
    orm_disconnect(connection);
    if (runtime != NULL) {
      check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
      orm_runtime_release(runtime);
    }
    if (directory != NULL) {
      check_equal(tt_remove_tree(directory), 0);
      free(directory);
    }
  }
  it("executes parameterized CRUD and exposes the same rows to structured queries") {
    raw_query("INSERT INTO people (id, score, note) VALUES (?, ?, ?)");
    parameter(orm_i64(7)); parameter(orm_i64(19));
    parameter(orm_text("'); DELETE FROM people; --"));
    command(1);
    orm_query_destroy(query); query = NULL;
    check_equal(orm_query_create(connection, orm_view("people"), &query, &error), ORM_STATUS_OK);
    check_equal(orm_query_add_column(query, orm_view("id"), &error), ORM_STATUS_OK);
    check_equal(orm_query_add_column(query, orm_view("score"), &error), ORM_STATUS_OK);
    open_rows(); row(7, 19); end_rows();
    raw_query("UPDATE people SET score = ? WHERE id = ?");
    parameter(orm_i64(23)); parameter(orm_i64(7)); command(1);
    raw_query("SELECT id, score FROM people WHERE note = ? AND score >= 20 LIMIT 1 OFFSET 0;");
    parameter(orm_text("'); DELETE FROM people; --"));
    open_rows(); row(7, 23); end_rows();
    raw_query("DELETE FROM people WHERE id = 7"); command(1);
    raw_query("SELECT id, score FROM people"); open_rows(); end_rows();
  }
  it("preserves transaction rollback savepoints and commit boundaries") {
    check_equal(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE, &transaction, &error), ORM_STATUS_OK);
    raw_query("INSERT INTO people (id, score) VALUES (1, 10)"); command(1);
    check_equal(orm_transaction_savepoint(transaction, orm_view("before_update"), &error), ORM_STATUS_OK);
    raw_query("UPDATE people SET score=20 WHERE id=1"); command(1);
    check_equal(orm_transaction_rollback_to_savepoint(transaction, orm_view("before_update"), &error), ORM_STATUS_OK);
    raw_query("SELECT id, score FROM people WHERE id=1"); open_rows();
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_BUSY);
    row(1, 10); end_rows();
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    orm_transaction_destroy(transaction); transaction = NULL;
    check_equal(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE, &transaction, &error), ORM_STATUS_OK);
    raw_query("DELETE FROM people WHERE id=1"); command(1);
    check_equal(orm_transaction_rollback(transaction, &error), ORM_STATUS_OK);
    orm_transaction_destroy(transaction); transaction = NULL;
    raw_query("SELECT id, score FROM people"); open_rows(); row(1, 10); end_rows();
  }
  it("keeps the parsed SELECT alive after query and connection handles are released") {
    raw_query("INSERT INTO people (id, score) VALUES (2, 30)"); command(1);
    raw_query("SELECT id, score FROM people WHERE id=?"); parameter(orm_i64(2));
    open_rows();
    orm_query_destroy(query); query = NULL;
    orm_disconnect(connection); connection = NULL;
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_BUSY);
    row(2, 30); end_rows();
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
  }
  it("rejects trailing SQL and missing bindings without executing a partial mutation") {
    const char *invalid[] = {
      "INSERT INTO people (id, score) VALUES (1,10); DELETE FROM people",
      "INSERT INTO people (id, score) VALUES (2,?)",
      "INSERT INTO people (id, score) VALUES (3,10),(4,20)",
      "INSERT LOW_PRIORITY INTO people (id, score) VALUES (5,10)",
      "INSERT INTO people (id, score, note) VALUES (6,10,'C:\\'); DELETE FROM people WHERE id=6",
      "INSERT INTO people (id, score) VALUES (7,DEFAULT)"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      raw_query(invalid[i]);
      check_equal(open_command(), ORM_STATUS_OK);
      orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
      const cflow_step step = cflow_publisher_resume(&publisher, NULL, &result);
      check_equal(step.kind, CFLOW_STEP_ERROR);
      check_contains(step.error, "TidesDB SQL");
      cflow_publisher_destroy(&publisher);
      publisher = (cflow_publisher){0};
    }
    raw_query("SELECT id, score FROM people"); open_rows(); end_rows();
    raw_query("INSERT INTO people (id, score) VALUES (5,50)"); command(1);
  }
  it("executes MySQL syntax while retaining literal bytes and binding order") {
    raw_query("INSERT INTO `people` (`id`, score, note) VALUES (1, 10, 'C:\\')"); command(1);
    raw_query("SELECT `id`, score FROM `people` /* AST */ WHERE (note=? AND (id=?)) LIMIT ?, ?");
    parameter(orm_text("C:\\")); parameter(orm_i64(1)); parameter(orm_i64(0)); parameter(orm_i64(1));
    open_rows(); row(1, 10); end_rows();
    raw_query("UPDATE people SET score=?, note=? WHERE id=?");
    parameter(orm_i64(20)); parameter(orm_text("updated")); parameter(orm_i64(1)); command(1);
    raw_query("SELECT id, score FROM people WHERE note='updated' -- ordinary comment\n");
    open_rows(); row(1, 20); end_rows();
  }
  it("rejects unsupported write semantics without changing an existing row") {
    raw_query("INSERT INTO people (id, score) VALUES(1, 10)"); command(1);
    const char *unsupported[] = {
      "UPDATE people SET score=99 WHERE id=1 LIMIT 0",
      "UPDATE people SET score=99 WHERE id=1 OR id=2",
      "UPDATE people SET score=99 WHERE score=10",
      "UPDATE people SET score=99 WHERE id=1 AND score>0",
      "DELETE FROM people WHERE id=1 ORDER BY id",
      "REPLACE INTO people (id, score) VALUES(1, 99)"
    };
    for (size_t i=0; i<sizeof(unsupported)/sizeof(unsupported[0]); ++i) {
      raw_query(unsupported[i]);
      check_equal(open_command(), ORM_STATUS_OK);
      orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
      check_equal(cflow_publisher_resume(&publisher, NULL, &result).kind, CFLOW_STEP_ERROR);
      cflow_publisher_destroy(&publisher);
      publisher = (cflow_publisher){0};
    }
    raw_query("SELECT id, score FROM people WHERE id=1"); open_rows(); row(1, 10); end_rows();
  }
  it("applies IS NULL and zero LIMIT without changing structured NULL semantics") {
    raw_query("INSERT INTO people (id, score, note) VALUES (1,10,NULL)"); command(1);
    raw_query("INSERT INTO people (id, score, note) VALUES (2,20,'x')"); command(1);
    raw_query("SELECT id, score FROM people WHERE note IS NULL"); open_rows(); row(1, 10); end_rows();
    raw_query("SELECT id, score FROM people WHERE note IS NOT NULL"); open_rows(); row(2, 20); end_rows();
    raw_query("SELECT id, score FROM people LIMIT 0"); open_rows(); end_rows();
  }
  it("reports UPDATE and DELETE predicate type errors instead of successful no-ops") {
    raw_query("INSERT INTO people (id, score) VALUES (1,10)"); command(1);
    const char *invalid[] = {
      "UPDATE people SET score=99 WHERE id=1 AND score='text'",
      "DELETE FROM people WHERE id=1 AND score='text'"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      raw_query(invalid[i]);
      check_equal(open_command(), ORM_STATUS_OK);
      orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
      const cflow_step step = cflow_publisher_resume(&publisher, NULL, &result);
      check_equal(step.kind, CFLOW_STEP_ERROR);
      check_not_null(step.error);
      check_contains(step.error, "compatible value types");
      check_equal(result.affected_rows, UINT64_C(0));
      cflow_publisher_destroy(&publisher); publisher = (cflow_publisher){0};
    }
    raw_query("SELECT id, score FROM people WHERE id=1"); open_rows(); row(1, 10); end_rows();
  }
  it("cancels a SQL cursor without retaining its parsed plan or runtime lease") {
    raw_query("INSERT INTO people (id, score) VALUES (1,10)"); command(1);
    raw_query("SELECT id, score FROM people"); open_rows();
    orm_query_destroy(query); query = NULL;
    orm_disconnect(connection); connection = NULL;
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_BUSY);
    cflow_publisher_cancel(&publisher);
    cflow_publisher_destroy(&publisher); publisher = (cflow_publisher){0};
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
  }
}

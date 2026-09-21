/* Quarantine is deliberately process-lifetime. Exercise it in owned child
 * processes, never by disabling the normal tests' leak checks or repairing a
 * failed native owner from the test. Salts supplies spawn/capture/deadlines. */
#define CSTL_NO_LEGACY_STACK_T /* signal.h owns stack_t on POSIX. */
#define TINYTEST_NO_MAIN
#include "orm_internal.h"
#include <tinytest.h>
#include <salts_process.h>
#include <salts_error.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CHILD_TIMEOUT_MS = 10000, CHILD_OUTPUT_BYTES = 8192, CHILD_CHECK_FAILED = 70 };
static const char *program_path;
static salts_process_t *child_process;
static orm_owner control_probe;
static const char rollback_message[] = "injected final rollback failure";
static unsigned transaction_destroys;
static unsigned rollback_calls;
static unsigned notifications;
static orm_error_t notified_error;
static unsigned connection_destroys;
static orm_backend_ops observed_connection_ops;
static void (*native_connection_destroy)(void *);
static void (*native_destroy)(void *);
static orm_status_t (*native_rollback)(void *, orm_error_t *);
static orm_transaction_backend_ops observed_ops;

static void child_require(bool condition, const char *description) {
  if (!condition) {
    (void)fprintf(stderr, "child assertion failed: %s\n", description);
    (void)fflush(stderr);
    _Exit(CHILD_CHECK_FAILED);
  }
}

static void observe_destroy(void *context) {
  ++transaction_destroys;
  native_destroy(context);
}

static orm_status_t fail_rollback(void *context, orm_error_t *error) {
  (void)context;
  ++rollback_calls;
  orm_error_set(error, ORM_STATUS_SQL_ERROR, rollback_message);
  return ORM_STATUS_SQL_ERROR;
}

static void observe_connection_destroy(void *context) {
  ++connection_destroys;
  native_connection_destroy(context);
}

static void record_cleanup(void *context, const orm_error_t *error) {
  child_require(context == &notifications, "callback context preserved");
  ++notifications;
  notified_error = *error;
}

static orm_status_t lost_rollback_ack(void *context, orm_error_t *error) {
  ++rollback_calls;
  const orm_status_t status = native_rollback(context, error);
  if (status != ORM_STATUS_OK) return status;
  orm_error_set(error, ORM_STATUS_CONNECTION_ERROR, rollback_message);
  return ORM_STATUS_CONNECTION_ERROR;
}

static orm_status_t silent_rollback_failure(void *context, orm_error_t *error) {
  (void)context; (void)error;
  ++rollback_calls;
  return ORM_STATUS_OUT_OF_MEMORY;
}

static void drop_publisher(cflow_publisher *publisher) {
  if (!cflow_publisher_valid(publisher)) return;
  cflow_publisher released = *publisher;
  memset(publisher, 0, sizeof(*publisher));
  cflow_publisher_destroy(&released);
}

static orm_connection_t *open_connection(void) {
  orm_config_t config;
  orm_option_t filename;
  orm_connection_t *connection = NULL;
  orm_error_t error;
  orm_config(&config);
  orm_error_init(&error);
  filename.keyword = orm_view("filename");
  filename.value = orm_view(":memory:");
  config.driver = orm_view("sqlite");
  config.options = &filename;
  config.option_count = 1u;
  child_require(orm_connect(&config, &connection, &error) == ORM_STATUS_OK,
                "real SQLite connection");
  observed_connection_ops = *connection->backend.ops;
  native_connection_destroy = observed_connection_ops.destroy;
  observed_connection_ops.destroy = observe_connection_destroy;
  connection->backend.ops = &observed_connection_ops;
  return connection;
}

static orm_transaction_t *open_transaction(orm_connection_t *connection) {
  orm_error_t error;
  orm_transaction_t *transaction = NULL;
  orm_error_init(&error);
  child_require(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                      &transaction, &error) == ORM_STATUS_OK,
                "real SQLite transaction");
  observed_ops = *transaction->backend.ops;
  native_destroy = observed_ops.destroy;
  native_rollback = observed_ops.rollback;
  observed_ops.destroy = observe_destroy;
  observed_ops.rollback = fail_rollback;
  transaction->backend.ops = &observed_ops;
  return transaction;
}

static int run_child(const char *mode) {
  orm_connection_t *connection = open_connection();
  const bool default_policy = strcmp(mode, "default") == 0;
  const bool normal = strcmp(mode, "normal") == 0;
  const bool explicit_error = strcmp(mode, "explicit") == 0;
  const bool delayed = strcmp(mode, "delayed") == 0;
  const bool released_parent = strcmp(mode, "released-parent") == 0;
  const bool lost_ack = strcmp(mode, "lost-ack") == 0;
  const bool silent = strcmp(mode, "silent") == 0;
  child_require(default_policy || normal || explicit_error || delayed || released_parent ||
                  lost_ack || silent || strcmp(mode, "immediate") == 0,
                "recognized child mode");
  /* Exclusive unpublished-to-children configuration; this is not a substitute
   * lease, and no test code supplies cleanup after failure. */
  if (!default_policy) {
    connection->cleanup_policy.notify = record_cleanup;
    connection->cleanup_policy.context = &notifications;
  }
  orm_transaction_t *transaction = open_transaction(connection);
  orm_error_t error;
  orm_error_init(&error);
  if (normal || explicit_error) {
    if (explicit_error) {
      child_require(orm_transaction_rollback(transaction, &error) == ORM_STATUS_SQL_ERROR,
                    "explicit rollback reports its error synchronously");
      child_require(notifications == 0u && transaction_destroys == 0u,
                    "explicit error is not hidden final cleanup");
      child_require(transaction->state == ORM_TRANSACTION_ACTIVE,
                    "rejected explicit rollback remains active");
    }
    observed_ops.rollback = native_rollback;
    if (explicit_error)
      child_require(orm_transaction_rollback(transaction, &error) == ORM_STATUS_OK,
                    "caller may explicitly finish after a recoverable rejection");
    orm_transaction_release(transaction);
    child_require(transaction_destroys == 1u && notifications == 0u,
                  "normal cleanup destroys exactly once without notification");
    orm_connection_release(connection);
    child_require(connection_destroys == 1u, "normal cleanup releases parent");
    (void)fputs("normal-cleanup-ok\n", stderr);
    return EXIT_SUCCESS; /* Normal controls retain normal leak checking. */
  }
  if (lost_ack) observed_ops.rollback = lost_rollback_ack;
  if (silent) observed_ops.rollback = silent_rollback_failure;
  if (default_policy) {
    (void)fputs("cleanup:before\n", stderr);
    (void)fflush(stderr);
    orm_transaction_release(transaction);
    (void)fputs("cleanup:after\n", stderr);
    orm_connection_release(connection);
    return EXIT_SUCCESS;
  }

  orm_query_t *query = NULL;
  cflow_publisher pending = {0}, in_transaction = {0};
  child_require(orm_raw(connection, orm_view("create table cleanup_data(id integer)"),
                        &query, &error) == ORM_STATUS_OK, "existing command query");
  child_require(orm_query_open_command_flow(query, &pending, &error) == ORM_STATUS_OK,
                "admitted lazy command before connection failure");
  if (delayed || released_parent) {
    child_require(orm_query_open_command_flow_in_transaction(query, transaction,
                    &in_transaction, &error) == ORM_STATUS_OK, "transaction Publisher");
  }
  if (released_parent) {
    orm_query_release(query); query = NULL;
    orm_connection_release(connection); connection = NULL;
  }
  orm_transaction_release(transaction); transaction = NULL;
  if (delayed || released_parent) {
    child_require(notifications == 0u && rollback_calls == 0u,
                  "Publisher keeps cleanup deferred");
    cflow_publisher_cancel(&in_transaction);
    child_require(notifications == 0u, "cancel is not native completion");
    drop_publisher(&in_transaction);
  }
  /* These observations guard every subsequent native-object access in RED. */
  child_require(notifications == 1u, "final cleanup error reaches handler once");
  child_require(rollback_calls == 1u && transaction_destroys == 0u,
                "failed cleanup neither retries nor destroys native transaction");
  child_require(connection_destroys == 0u, "quarantined transaction pins its parent");
  const orm_status_t cause = lost_ack ? ORM_STATUS_CONNECTION_ERROR :
                            silent ? ORM_STATUS_OUT_OF_MEMORY : ORM_STATUS_SQL_ERROR;
  child_require(notified_error.status == cause && notified_error.message[0] != '\0',
                "bounded native cause and message preserved");
  if (!silent)
    child_require(strcmp(notified_error.message, rollback_message) == 0,
                  "original native message preserved");
  if (connection != NULL) {
    child_require(connection->owner.phase == ORM_OWNER_CLOSE_FAILED,
                  "parent exposes terminal CLOSE_FAILED state");
    child_require(orm_connection_close(connection, &error) == ORM_OWNER_STATUS_CLEANUP_FAILED,
                  "failed close is not BUSY or success");
    child_require(error.status == ORM_OWNER_STATUS_CLEANUP_FAILED &&
                  strcmp(error.message, notified_error.message) == 0,
                  "close returns stored failure diagnostic");
    child_require(orm_connection_close(connection, NULL) == ORM_OWNER_STATUS_CLEANUP_FAILED,
                  "repeated close without output does not retry cleanup");
    child_require(orm_owner_try_retain(&connection->owner) == ORM_STATUS_INVALID_STATE,
                  "failure cannot be revived with another reference");
    orm_query_t *next_query = NULL;
    orm_transaction_t *next_transaction = NULL;
    child_require(orm_raw(connection, orm_view("select 1"), &next_query, &error) ==
                    ORM_STATUS_INVALID_STATE && next_query == NULL, "new query rejected");
    child_require(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                    &next_transaction, &error) == ORM_STATUS_INVALID_STATE &&
                    next_transaction == NULL, "new transaction rejected");
    child_require(orm_query_open_command_flow(query, &in_transaction, &error) ==
                    ORM_STATUS_INVALID_STATE && !cflow_publisher_valid(&in_transaction),
                  "existing query cannot bypass terminal failure");
  }
  orm_command_result_t command = ORM_COMMAND_RESULT_INIT;
  const cflow_step step = cflow_publisher_resume(&pending, NULL, &command);
  child_require(step.kind == CFLOW_STEP_ERROR, "pending command cannot execute after failure");
  child_require(step.error != NULL && strstr(step.error, "cleanup") != NULL,
                "pending command identifies cleanup failure");
  drop_publisher(&pending);
  if (query != NULL) orm_query_release(query);
  if (connection != NULL) orm_connection_release(connection);
  child_require(notifications == 1u && rollback_calls == 1u &&
                transaction_destroys == 0u && connection_destroys == 0u,
                "final releases neither repeat notification nor reclaim quarantine");
  (void)fputs("quarantine-ok\n", stderr);
  (void)fflush(stderr);
  /* The approved failure policy intentionally pins native resources until
   * process exit. Do not invent a cleanup/recovery backdoor just for LSan. */
  _Exit(EXIT_SUCCESS);
}

static void launch_child(const char *mode, salts_process_result_t *result,
                          char output[CHILD_OUTPUT_BYTES]) {
  salts_process_options_t options;
  const char *args[] = {"--cleanup-child", mode, NULL};
  salts_process_options_init(&options);
  options.program = program_path;
  options.args = args;
  options.timeout_ms = CHILD_TIMEOUT_MS;
  options.max_output_bytes = CHILD_OUTPUT_BYTES;
  check_equal(salts_process_spawn(&options, &child_process), SALTS_OK);
  check_equal(salts_process_wait(child_process, result), SALTS_OK);
  size_t total = 0u;
  while (total < CHILD_OUTPUT_BYTES - 1u) {
    size_t count = 0u;
    const int status = salts_process_read_stderr(child_process, output + total,
        CHILD_OUTPUT_BYTES - 1u - total, &count);
    total += count;
    if (status == SALTS_EOF || count == 0u) break;
    check_equal(status, SALTS_OK);
  }
  output[total] = '\0';
  (void)fputs(output, stderr);
}

static void require_successful_child(const char *mode, const char *marker) {
  salts_process_result_t result = {0};
  char output[CHILD_OUTPUT_BYTES] = {0};
  launch_child(mode, &result, output);
  check_equal(result.state, SALTS_PROCESS_EXITED);
  check_equal(result.exit_code, EXIT_SUCCESS);
  check_not_null(strstr(output, marker));
  check_null(strstr(output, "AddressSanitizer"));
  check_null(strstr(output, "runtime error:"));
}

spec("native cleanup failure policy") {
  (void)ttest_config__;
  before_each() { child_process = NULL; memset(&control_probe, 0, sizeof(control_probe)); }
  after_each() {
    salts_process_destroy(child_process); child_process = NULL;
    /* This stack-state probe owns no native payload; reclaim only its mutex. */
    if (control_probe.mutex != NULL) salts_mutex_destroy(&control_probe.mutex);
  }
  it("fails fast by default rather than silently destroying after a failed rollback") {
    salts_process_result_t result = {0};
    char output[CHILD_OUTPUT_BYTES] = {0};
    launch_child("default", &result, output);
#ifndef _WIN32
    check_equal(result.state, SALTS_PROCESS_SIGNALED);
    check_equal(result.term_signal, SIGABRT);
#else
    check_equal(result.state, SALTS_PROCESS_EXITED);
    check_true(result.exit_code != EXIT_SUCCESS);
#endif
    check_not_null(strstr(output, "ORM cleanup failed"));
    check_not_null(strstr(output, rollback_message));
    check_null(strstr(output, "cleanup:after"));
  }
  it("reports and quarantines immediate final rollback failure") {
    require_successful_child("immediate", "quarantine-ok");
  }
  it("defers failure reporting through Publisher cancellation and destruction") {
    require_successful_child("delayed", "quarantine-ok");
  }
  it("keeps the failed parent after all application references were released") {
    require_successful_child("released-parent", "quarantine-ok");
  }
  it("does not assume cleanup succeeded when a real rollback acknowledgement is lost") {
    require_successful_child("lost-ack", "quarantine-ok");
  }
  it("normalizes an unfilled native error without losing the returned status") {
    require_successful_child("silent", "quarantine-ok");
  }
  it("keeps successful automatic rollback on the ordinary cleanup path") {
    require_successful_child("normal", "normal-cleanup-ok");
  }
  it("does not quarantine an explicit rollback rejection before final release") {
    require_successful_child("explicit", "normal-cleanup-ok");
  }

  it("returns terminal cleanup failure rather than BUSY for a failed control state") {
    check_equal(orm_status_message(ORM_OWNER_STATUS_CLEANUP_FAILED), "cleanup failed");
    orm_owner_action action = ORM_OWNER_FREE_MEMORY;
    check_equal(orm_owner_init(&control_probe, 2u, 2u), ORM_STATUS_OK);
    control_probe.phase = ORM_OWNER_CLOSE_FAILED;
    check_equal(orm_owner_begin_close(&control_probe, &action), ORM_OWNER_STATUS_CLEANUP_FAILED);
    check_equal(action, ORM_OWNER_KEEP);
    check_equal(control_probe.phase, ORM_OWNER_CLOSE_FAILED);
  }
  it("does not retain or admit work to a published failed owner") {
    check_equal(orm_owner_init(&control_probe, 2u, 2u), ORM_STATUS_OK);
    control_probe.phase = ORM_OWNER_CLOSE_FAILED;
    check_equal(orm_owner_try_retain(&control_probe), ORM_STATUS_INVALID_STATE);
    check_equal(orm_owner_admit(&control_probe), ORM_STATUS_INVALID_STATE);
    check_equal(orm_owner_begin_write(&control_probe), ORM_STATUS_INVALID_STATE);
    check_equal(control_probe.references, 1u);
  }
  it("keeps terminal failure after the final preexisting dependent is released") {
    check_equal(orm_owner_init(&control_probe, 2u, 2u), ORM_STATUS_OK);
    check_equal(orm_owner_admit(&control_probe), ORM_STATUS_OK);
    control_probe.phase = ORM_OWNER_CLOSE_FAILED;
    check_equal(orm_owner_release_reference(&control_probe), ORM_OWNER_KEEP);
    check_equal(orm_owner_release_dependent(&control_probe), ORM_OWNER_KEEP);
    check_equal(control_probe.phase, ORM_OWNER_CLOSE_FAILED);
    check_equal(control_probe.references, 0u);
    check_equal(control_probe.dependents, 0u);
  }

}

int main(int argc, char **argv) {
  program_path = argv[0];
  if (argc == 3 && strcmp(argv[1], "--cleanup-child") == 0) {
#ifdef _WIN32
    /* Expected abort must not open an interactive CRT report dialog in CI. */
    (void)_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    return run_child(argv[2]);
  }
  return ttest_main__(argc, argv, TTEST_INVOKE_SPEC_ADAPTER__, TT_USE_COLOR != 0,
      TT_USE_TAP != 0, TTEST_IS_ATTY__() != 0, TTEST_PRINT_TRACE_DEFAULT__);
}

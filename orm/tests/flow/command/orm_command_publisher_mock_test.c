#include "orm_command_publisher.h"

#include <tinytest.h>
#include <string.h>

typedef struct command_fake_state {
  orm_command_driver_result *result;
  void *execute_context;
  void *destroy_context;
  size_t execute_calls;
  size_t destroy_calls;
} command_fake_state;

static command_fake_state command_fake;

static orm_command_driver_result *command_execute(void *context) {
  ++command_fake.execute_calls;
  command_fake.execute_context = context;
  return command_fake.result;
}

static void command_destroy(void *context) {
  ++command_fake.destroy_calls;
  command_fake.destroy_context = context;
}

static orm_command_driver_result execute_result(void *context) {
  return *command_execute(context);
}

static const orm_command_driver_ops command_ops = {
    sizeof(orm_command_driver_ops), ORM_COMMAND_DRIVER_OPS_ABI_VERSION,
    execute_result, command_destroy};

spec("ORM command Publisher") {
  (void)ttest_config__;
  static cflow_publisher publisher;
  static orm_command_driver driver;
  static orm_command_driver_result native_result;
  static orm_command_result_t output;
  static orm_error_t error;
  static char diagnostic[ORM_C_ERROR_MESSAGE_CAPACITY];

  before_each() {
    publisher = (cflow_publisher){0};
    native_result = (orm_command_driver_result)ORM_COMMAND_DRIVER_RESULT_INIT;
    output = (orm_command_result_t)ORM_COMMAND_RESULT_INIT;
    orm_error_init(&error);
    memset(diagnostic, 0, sizeof(diagnostic));
    driver = (orm_command_driver){&command_ops, &native_result};
    command_fake = (command_fake_state){.result = &native_result};
    check_equal(orm_command_publisher_init(&publisher, &driver, &error), ORM_STATUS_OK);
  }

  after_each() {
    if (cflow_publisher_valid(&publisher)) {
      cflow_publisher_destroy(&publisher);
      publisher = (cflow_publisher){0};
    }
    check_equal(command_fake.destroy_calls, (size_t)1u);
    check_true(command_fake.destroy_context == &native_result);
  }

  it("moves driver ownership without executing an unconsumed command") {
    const char *terminal_error = "stale";
    check_null(driver.ops);
    check_null(driver.context);
    check_equal(cflow_publisher_poll_terminal(&publisher, &terminal_error), CFLOW_PUBLISHER_OPEN);
    check_null(terminal_error);
    cflow_publisher_destroy(&publisher);
    publisher = (cflow_publisher){0};
    check_equal(command_fake.execute_calls, (size_t)0u);
  }

  it("publishes the full affected-row count once and never executes again") {
    native_result.affected_rows = UINT64_MAX;
    check_equal(cflow_publisher_resume(&publisher, NULL, &output).kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(output.affected_rows, UINT64_MAX);
    check_equal(output.struct_size, sizeof(output));
    check_equal(cflow_publisher_poll_terminal(&publisher, NULL), CFLOW_PUBLISHER_DONE);
    check_equal(cflow_publisher_resume(&publisher, NULL, &output).kind, CFLOW_STEP_DONE);
    cflow_publisher_cancel(&publisher);
    check_equal(cflow_publisher_resume(&publisher, NULL, &output).kind, CFLOW_STEP_DONE);
    check_equal(command_fake.execute_calls, (size_t)1u);
    check_true(command_fake.execute_context == &native_result);
  }

  it("makes repeated cancellation terminal before any native execution") {
    cflow_publisher_cancel(&publisher);
    cflow_publisher_cancel(&publisher);
    check_equal(cflow_publisher_poll_terminal(&publisher, NULL), CFLOW_PUBLISHER_DONE);
    check_equal(cflow_publisher_resume(&publisher, NULL, &output).kind, CFLOW_STEP_DONE);
    check_equal(command_fake.execute_calls, (size_t)0u);
  }

  it("rejects missing output storage without dispatch and retains the error") {
    const char *terminal_error = NULL;
    check_equal(cflow_publisher_resume(&publisher, NULL, NULL).kind, CFLOW_STEP_ERROR);
    check_equal(cflow_publisher_poll_terminal(&publisher, &terminal_error), CFLOW_PUBLISHER_ERROR);
    check_not_null(terminal_error);
    check_not_null(strstr(terminal_error, "null command result storage"));
    check_equal(cflow_publisher_resume(&publisher, NULL, &output).kind, CFLOW_STEP_ERROR);
    check_equal(command_fake.execute_calls, (size_t)0u);
  }

  it("owns the native error text across buffer reuse cancellation and repeated resume") {
    const char *terminal_error = NULL;
    const char *expected = "native command rejected";
    memcpy(diagnostic, expected, strlen(expected) + 1u);
    native_result.status = ORM_STATUS_SQL_ERROR;
    native_result.message = diagnostic;
    output.affected_rows = UINT64_MAX;
    cflow_step step = cflow_publisher_resume(&publisher, NULL, &output);
    check_equal(step.kind, CFLOW_STEP_ERROR);
    check_equal(step.error, expected);
    memset(diagnostic, 'x', sizeof(diagnostic) - 1u);
    check_equal(step.error, expected);
    cflow_publisher_cancel(&publisher);
    check_equal(cflow_publisher_poll_terminal(&publisher, &terminal_error), CFLOW_PUBLISHER_ERROR);
    check_equal(terminal_error, expected);
    step = cflow_publisher_resume(&publisher, NULL, &output);
    check_equal(step.kind, CFLOW_STEP_ERROR);
    check_equal(step.error, expected);
    check_equal(output.affected_rows, UINT64_MAX);
    check_equal(command_fake.execute_calls, (size_t)1u);
    check_true(command_fake.execute_context == &native_result);
  }

  it("supplies a status diagnostic when the native driver has no message") {
    native_result.status = ORM_STATUS_OUT_OF_MEMORY;
    const cflow_step step = cflow_publisher_resume(&publisher, NULL, &output);
    check_equal(step.kind, CFLOW_STEP_ERROR);
    check_equal(step.error, orm_status_message(ORM_STATUS_OUT_OF_MEMORY));
    check_equal(command_fake.execute_calls, (size_t)1u);
    check_true(command_fake.execute_context == &native_result);
  }

  it("rejects replacing a live Publisher without consuming the second driver") {
    orm_command_driver_result second_result = ORM_COMMAND_DRIVER_RESULT_INIT;
    orm_command_driver second = {&command_ops, &second_result};
    check_equal(orm_command_publisher_init(&publisher, &second, &error), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(error.status, ORM_STATUS_INVALID_ARGUMENT);
    check_true(second.ops == &command_ops);
    check_true(second.context == &second_result);
    check_equal(cflow_publisher_poll_terminal(&publisher, NULL), CFLOW_PUBLISHER_OPEN);
    check_equal(command_fake.execute_calls, (size_t)0u);
  }

  it("rejects incompatible driver ABI without transferring ownership") {
    cflow_publisher rejected = {0};
    orm_command_driver_ops invalid_ops = command_ops;
    invalid_ops.abi_version = ORM_COMMAND_DRIVER_OPS_ABI_VERSION + 1u;
    orm_command_driver candidate = {&invalid_ops, &native_result};
    check_equal(orm_command_publisher_init(&rejected, &candidate, &error), ORM_STATUS_INVALID_ARGUMENT);
    check_false(cflow_publisher_valid(&rejected));
    check_true(candidate.ops == &invalid_ops);
    check_true(candidate.context == &native_result);
    check_equal(command_fake.execute_calls, (size_t)0u);
    check_equal(command_fake.destroy_calls, (size_t)0u);
  }
}

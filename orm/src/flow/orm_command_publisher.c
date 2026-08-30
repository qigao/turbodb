#include "orm_command_publisher.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct orm_command_publisher_state {
  orm_command_driver driver;
  cflow_publisher_terminal terminal;
  char error_message[ORM_C_ERROR_MESSAGE_CAPACITY];
} orm_command_publisher_state;

static const cmeta_type_identity orm_command_result_identity =
    CMETA_TYPE_ID_ATOM_INIT("orm.command_result");
static const cmeta_type_traits orm_command_result_traits = {
    .flags = CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY};
static const cmeta_type_desc orm_command_result_type = {
    .name = "orm_command_result_t",
    .size = sizeof(orm_command_result_t),
    .align = _Alignof(orm_command_result_t),
    .kind = CMETA_T_OBJECT,
    .pointee = NULL,
    .traits = &orm_command_result_traits,
    .identity = &orm_command_result_identity};

static void orm_command_set_error(orm_error_t *error, orm_status_t status,
                                  const char *message) {
  if (error == NULL || error->struct_size < sizeof(*error))
    return;
  error->status = status;
  (void)snprintf(error->message, sizeof(error->message), "%s",
                 status == ORM_STATUS_OK
                     ? ""
                     : (message != NULL ? message
                                        : orm_status_message(status)));
}

static int orm_command_driver_valid(const orm_command_driver *driver) {
  return driver != NULL && driver->ops != NULL && driver->context != NULL &&
         driver->ops->struct_size >= sizeof(*driver->ops) &&
         driver->ops->abi_version == ORM_COMMAND_DRIVER_OPS_ABI_VERSION &&
         driver->ops->execute != NULL && driver->ops->destroy != NULL;
}

static const char *orm_command_publisher_name(void *state) {
  (void)state;
  return "orm-command";
}

static const cmeta_type_desc *orm_command_publisher_output_type(void *state) {
  (void)state;
  return &orm_command_result_type;
}

static cflow_step orm_command_publisher_resume(void *state_,
                                            cflow_publish_context *context,
                                            void *out_value) {
  orm_command_publisher_state *state = (orm_command_publisher_state *)state_;
  orm_command_result_t *output = (orm_command_result_t *)out_value;
  orm_command_driver_result result;
  (void)context;
  if (state->terminal == CFLOW_PUBLISHER_DONE)
    return (cflow_step){CFLOW_STEP_DONE, {0}, NULL};
  if (state->terminal == CFLOW_PUBLISHER_ERROR)
    return (cflow_step){CFLOW_STEP_ERROR, {0}, state->error_message};
  if (output == NULL) {
    state->terminal = CFLOW_PUBLISHER_ERROR;
    (void)snprintf(state->error_message, sizeof(state->error_message), "%s",
                   "CFlow supplied null command result storage");
    return (cflow_step){CFLOW_STEP_ERROR, {0}, state->error_message};
  }
  result = state->driver.ops->execute(state->driver.context);
  if (result.status != ORM_STATUS_OK) {
    state->terminal = CFLOW_PUBLISHER_ERROR;
    (void)snprintf(state->error_message, sizeof(state->error_message), "%s",
                   result.message != NULL
                       ? result.message
                       : orm_status_message(result.status));
    return (cflow_step){CFLOW_STEP_ERROR, {0}, state->error_message};
  }
  *output = (orm_command_result_t)ORM_COMMAND_RESULT_INIT;
  output->affected_rows = result.affected_rows;
  state->terminal = CFLOW_PUBLISHER_DONE;
  return (cflow_step){CFLOW_STEP_VALUE_AND_DONE, {0}, NULL};
}

static void orm_command_publisher_cancel(void *state_) {
  orm_command_publisher_state *state = (orm_command_publisher_state *)state_;
  if (state->terminal == CFLOW_PUBLISHER_OPEN)
    state->terminal = CFLOW_PUBLISHER_DONE;
}

static void orm_command_publisher_destroy(void *state_) {
  orm_command_publisher_state *state = (orm_command_publisher_state *)state_;
  if (state == NULL)
    return;
  state->driver.ops->destroy(state->driver.context);
  free(state);
}

static void orm_command_publisher_bind_terminal_waker(void *state,
                                                   cflow_waker waker) {
  (void)state;
  (void)waker;
}

static cflow_publisher_terminal orm_command_publisher_poll_terminal(
    void *state_, const char **error) {
  const orm_command_publisher_state *state =
      (const orm_command_publisher_state *)state_;
  if (error != NULL)
    *error = state->terminal == CFLOW_PUBLISHER_ERROR
                 ? state->error_message
                 : NULL;
  return state->terminal;
}

CMETA_IMPLEMENTS(cflow_publisher, orm_command_publisher,
                 CFLOW_PUBLISHER_CAP_CONSTRUCTS_VALUES,
    .name = orm_command_publisher_name,
    .output_type = orm_command_publisher_output_type,
    .resume = orm_command_publisher_resume,
    .cancel = orm_command_publisher_cancel,
    .destroy = orm_command_publisher_destroy,
    .bind_terminal_waker = orm_command_publisher_bind_terminal_waker,
    .poll_terminal = orm_command_publisher_poll_terminal
);

orm_status_t orm_command_publisher_init(cflow_publisher *out_publisher,
                                     orm_command_driver *driver,
                                     orm_error_t *error) {
  orm_command_publisher_state *state;
  if (out_publisher == NULL || cflow_publisher_valid(out_publisher) ||
      !orm_command_driver_valid(driver)) {
    orm_command_set_error(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid command Publisher configuration");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  state = (orm_command_publisher_state *)calloc(1u, sizeof(*state));
  if (state == NULL) {
    orm_command_set_error(error, ORM_STATUS_OUT_OF_MEMORY, NULL);
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  state->driver = *driver;
  state->terminal = CFLOW_PUBLISHER_OPEN;
  *out_publisher = orm_command_publisher_as_cflow_publisher(state);
  driver->ops = NULL;
  driver->context = NULL;
  orm_command_set_error(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

#include "orm_cbind_source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct orm_cbind_source_state {
  orm_row_cursor cursor;
  const cmeta_data_desc *row_shape;
  cbind_context bind_context;
  void *scratch;
  cflow_source_terminal terminal;
  int cancelled;
  char error_message[ORM_C_ERROR_MESSAGE_CAPACITY];
} orm_cbind_source_state;

static void orm_cbind_set_error(orm_error_t *error, orm_status_t status,
                                const char *message) {
  if (error == NULL || error->struct_size < sizeof(*error))
    return;
  error->status = status;
  if (status == ORM_STATUS_OK) {
    error->message[0] = '\0';
    return;
  }
  (void)snprintf(error->message, sizeof(error->message), "%s",
                 message != NULL ? message : orm_status_message(status));
}

int orm_row_cursor_valid(const orm_row_cursor *cursor) {
  return cursor != NULL && cursor->ops != NULL && cursor->context != NULL &&
         cursor->ops->struct_size >= sizeof(orm_row_cursor_ops) &&
         cursor->ops->abi_version == ORM_ROW_CURSOR_OPS_ABI_VERSION &&
         cursor->ops->name != NULL && cursor->ops->next != NULL &&
         cursor->ops->cancel != NULL && cursor->ops->destroy != NULL;
}

void orm_row_cursor_dispose(orm_row_cursor *cursor) {
  if (cursor == NULL)
    return;
  if (cursor->ops != NULL && cursor->context != NULL &&
      cursor->ops->struct_size >= sizeof(orm_row_cursor_ops) &&
      cursor->ops->abi_version == ORM_ROW_CURSOR_OPS_ABI_VERSION &&
      cursor->ops->destroy != NULL)
    cursor->ops->destroy(cursor->context);
  cursor->ops = NULL;
  cursor->context = NULL;
}

static int orm_cbind_source_config_valid(
    const orm_cbind_source_config *config) {
  return config != NULL && config->struct_size >= sizeof(*config) &&
         config->abi_version == ORM_CBIND_SOURCE_CONFIG_ABI_VERSION &&
         config->row_shape != NULL &&
         cmeta_data_desc_valid(config->row_shape) &&
         config->row_shape->storage_type != NULL && config->max_depth != 0u;
}

static const char *orm_cbind_source_name(void *state_) {
  const orm_cbind_source_state *state =
      (const orm_cbind_source_state *)state_;
  return state->cursor.ops->name;
}

static const cmeta_type_desc *orm_cbind_source_output_type(void *state_) {
  const orm_cbind_source_state *state =
      (const orm_cbind_source_state *)state_;
  return state->row_shape->storage_type;
}

static void orm_cbind_source_cancel_cursor(orm_cbind_source_state *state) {
  if (state->cancelled)
    return;
  state->cancelled = 1;
  state->cursor.ops->cancel(state->cursor.context);
}

static cflow_step orm_cbind_source_fail(orm_cbind_source_state *state,
                                        orm_status_t status,
                                        const char *message) {
  state->terminal = CFLOW_SOURCE_ERROR;
  orm_cbind_source_cancel_cursor(state);
  (void)snprintf(state->error_message, sizeof(state->error_message),
                 "%s", message != NULL ? message : orm_status_message(status));
  return (cflow_step){CFLOW_STEP_ERROR, {0}, state->error_message};
}

static orm_status_t orm_cbind_status_to_orm(cbind_status status) {
  switch (status) {
    case CBIND_LIMIT_EXCEEDED:
      return ORM_STATUS_LIMIT_EXCEEDED;
    case CBIND_UNSUPPORTED:
      return ORM_STATUS_UNSUPPORTED;
    case CBIND_SOURCE_ERROR:
      return ORM_STATUS_DATASTORE_ERROR;
    case CBIND_INVALID_ARGUMENT:
    case CBIND_INVALID_CONTEXT:
    case CBIND_INVALID_SHAPE:
    case CBIND_DESTINATION_NOT_EMPTY:
    case CBIND_TARGET_ERROR:
      return ORM_STATUS_INTERNAL_ERROR;
    case CBIND_TOKEN_MISMATCH:
    case CBIND_VALUE_OUT_OF_RANGE:
    case CBIND_UNKNOWN_FIELD:
    case CBIND_DUPLICATE_FIELD:
    case CBIND_MISSING_FIELD:
    case CBIND_UNEXPECTED_END:
      return ORM_STATUS_TYPE_ERROR;
    case CBIND_OK:
    default:
      return ORM_STATUS_INTERNAL_ERROR;
  }
}

static cflow_step orm_cbind_source_resume(void *state_, cflow_resume_ctx *ctx,
                                          void *out_value) {
  orm_cbind_source_state *state = (orm_cbind_source_state *)state_;
  cserde_reader reader = {0};
  orm_row_cursor_step cursor_step;
  cbind_error bind_error = CBIND_ERROR_INIT;
  cbind_status bind_status;

  (void)ctx;
  if (state->terminal == CFLOW_SOURCE_DONE)
    return (cflow_step){CFLOW_STEP_DONE, {0}, NULL};
  if (state->terminal == CFLOW_SOURCE_ERROR)
    return (cflow_step){CFLOW_STEP_ERROR, {0}, state->error_message};
  if (out_value == NULL)
    return orm_cbind_source_fail(state, ORM_STATUS_INVALID_ARGUMENT,
                                 "CFlow supplied null row storage");

  cursor_step = state->cursor.ops->next(state->cursor.context, &reader);
  switch (cursor_step.kind) {
    case ORM_ROW_CURSOR_WAIT:
      if (!cflow_waitable_valid(&cursor_step.waitable))
        return orm_cbind_source_fail(
            state, ORM_STATUS_INTERNAL_ERROR,
            "driver returned WAIT without a valid waitable");
      return (cflow_step){CFLOW_STEP_WAIT, cursor_step.waitable, NULL};
    case ORM_ROW_CURSOR_DONE:
      state->terminal = CFLOW_SOURCE_DONE;
      return (cflow_step){CFLOW_STEP_DONE, {0}, NULL};
    case ORM_ROW_CURSOR_ERROR:
      return orm_cbind_source_fail(
          state,
          cursor_step.status == ORM_STATUS_OK ? ORM_STATUS_DATASTORE_ERROR
                                              : cursor_step.status,
          cursor_step.message);
    case ORM_ROW_CURSOR_ROW:
    case ORM_ROW_CURSOR_ROW_AND_DONE:
      break;
    default:
      return orm_cbind_source_fail(state, ORM_STATUS_INTERNAL_ERROR,
                                   "driver returned an invalid cursor step");
  }

  memset(out_value, 0, state->row_shape->storage_type->size);
  bind_status = cbind_decode(&state->bind_context, state->row_shape, &reader,
                             out_value, &bind_error);
  if (bind_status != CBIND_OK) {
    char message[ORM_C_ERROR_MESSAGE_CAPACITY];
    (void)snprintf(message, sizeof(message),
                   "row binding failed: cbind=%d cserde=%d depth=%zu",
                   (int)bind_status, (int)bind_error.source_status,
                   bind_error.depth);
    return orm_cbind_source_fail(
        state, orm_cbind_status_to_orm(bind_status), message);
  }

  if (cursor_step.kind == ORM_ROW_CURSOR_ROW_AND_DONE) {
    state->terminal = CFLOW_SOURCE_DONE;
    return (cflow_step){CFLOW_STEP_VALUE_AND_DONE, {0}, NULL};
  }
  return (cflow_step){CFLOW_STEP_VALUE, {0}, NULL};
}

static void orm_cbind_source_cancel(void *state_) {
  orm_cbind_source_state *state = (orm_cbind_source_state *)state_;
  if (state->terminal == CFLOW_SOURCE_OPEN) {
    orm_cbind_source_cancel_cursor(state);
    state->terminal = CFLOW_SOURCE_DONE;
  }
}

static void orm_cbind_source_destroy(void *state_) {
  orm_cbind_source_state *state = (orm_cbind_source_state *)state_;
  if (state == NULL)
    return;
  if (state->terminal == CFLOW_SOURCE_OPEN)
    orm_cbind_source_cancel_cursor(state);
  state->cursor.ops->destroy(state->cursor.context);
  free(state->scratch);
  free(state);
}

static void orm_cbind_source_bind_terminal_waker(void *state_,
                                                 cflow_waker waker) {
  (void)state_;
  (void)waker;
}

static cflow_source_terminal orm_cbind_source_poll_terminal(void *state_,
                                                            const char **error) {
  const orm_cbind_source_state *state =
      (const orm_cbind_source_state *)state_;
  if (error != NULL)
    *error = state->terminal == CFLOW_SOURCE_ERROR ? state->error_message : NULL;
  return state->terminal;
}

CMETA_IMPLEMENTS(cflow_source, orm_cbind_source,
                 CFLOW_SOURCE_CAP_CONSTRUCTS_VALUES,
    .name = orm_cbind_source_name,
    .output_type = orm_cbind_source_output_type,
    .resume = orm_cbind_source_resume,
    .cancel = orm_cbind_source_cancel,
    .destroy = orm_cbind_source_destroy,
    .bind_terminal_waker = orm_cbind_source_bind_terminal_waker,
    .poll_terminal = orm_cbind_source_poll_terminal
);

orm_status_t orm_cbind_source_init(cflow_source *out_source,
                                   orm_row_cursor *cursor,
                                   const orm_cbind_source_config *config,
                                   orm_error_t *error) {
  orm_cbind_source_state *state;
  cflow_source source;

  if (out_source == NULL || out_source->self != NULL ||
      !orm_row_cursor_valid(cursor) ||
      !orm_cbind_source_config_valid(config)) {
    orm_cbind_set_error(error, ORM_STATUS_INVALID_ARGUMENT,
                        "invalid CBind source configuration");
    return ORM_STATUS_INVALID_ARGUMENT;
  }

  state = (orm_cbind_source_state *)calloc(1u, sizeof(*state));
  if (state == NULL) {
    orm_cbind_set_error(error, ORM_STATUS_OUT_OF_MEMORY, NULL);
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  if (config->scratch_bytes != 0u) {
    state->scratch = malloc(config->scratch_bytes);
    if (state->scratch == NULL) {
      free(state);
      orm_cbind_set_error(error, ORM_STATUS_OUT_OF_MEMORY, NULL);
      return ORM_STATUS_OUT_OF_MEMORY;
    }
  }

  state->cursor = *cursor;
  state->row_shape = config->row_shape;
  state->terminal = CFLOW_SOURCE_OPEN;
  state->bind_context = (cbind_context)CBIND_CONTEXT_WITH_BUFFERS_INIT(
      state->scratch, config->scratch_bytes, config->max_depth,
      config->max_container_items, config->max_buffer_bytes);
  source = orm_cbind_source_as_cflow_source(state);
  *out_source = source;
  cursor->ops = NULL;
  cursor->context = NULL;
  orm_cbind_set_error(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

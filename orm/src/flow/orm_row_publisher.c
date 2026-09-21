#include "orm_row_publisher.h"

#if defined(ORM_NATIVE_OWNER_CANDIDATE)
#include "orm_owner.h"
#endif

#include <data_bind_native.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct orm_row_publisher_state {
  orm_row_cursor cursor;
  const cmeta_data_desc *row_shape;
  DataBindNativeOptions bind_options;
  void *scratch;
  size_t max_buffer_bytes;
  cflow_publisher_terminal terminal;
  int cancelled;
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  orm_native_cleanup native_cleanup;
#endif
  char error_message[ORM_C_ERROR_MESSAGE_CAPACITY];
} orm_row_publisher_state;

static void orm_row_set_error(orm_error_t *error, orm_status_t status,
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
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
         ((cursor->begin_execution == NULL) == (cursor->end_execution == NULL)) &&
#endif
         cursor->ops->struct_size >= sizeof(orm_row_cursor_ops) &&
         cursor->ops->abi_version == ORM_ROW_CURSOR_OPS_ABI_VERSION &&
         cursor->ops->name != NULL && cursor->ops->next != NULL &&
         cursor->ops->cancel != NULL && cursor->ops->destroy != NULL;
}

/* Checked cancellation observes errors while the native context and all
 * parent holds still exist. Ordinary backends retain their void cancel path. */
static void orm_row_cursor_cancel_native(orm_row_cursor *cursor) {
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  if (cursor->cancel_checked != NULL) {
    orm_error_t error;
    orm_error_init(&error);
    const orm_status_t status = cursor->cancel_checked(cursor->context, &error);
    cursor->cancel_checked = NULL; /* Cancellation is consumed once by the host. */
    if (cursor->report_owner_error != NULL)
      cursor->report_owner_error(cursor->owner, status);
    return;
  }
#endif
  cursor->ops->cancel(cursor->context);
}

void orm_row_cursor_dispose(orm_row_cursor *cursor) {
  if (cursor == NULL)
    return;
  if (cursor->ops != NULL && cursor->context != NULL &&
      cursor->ops->struct_size >= sizeof(orm_row_cursor_ops) &&
      cursor->ops->abi_version == ORM_ROW_CURSOR_OPS_ABI_VERSION &&
      cursor->ops->destroy != NULL) {
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
    /* Also cover failed Publisher construction/direct cursor disposal. This
     * optional callback is idempotent after an earlier Publisher cancel. */
    if (cursor->cancel_checked != NULL)
      orm_row_cursor_cancel_native(cursor);
#endif
    cursor->ops->destroy(cursor->context);
  }
  cursor->ops = NULL;
  cursor->context = NULL;
  cursor->wait_timeout_ns = 0u;
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  void *owner = cursor->owner;
  void (*release_owner)(void *) = cursor->release_owner;
  void *transaction_owner = cursor->transaction_owner;
  void (*release_transaction_owner)(void *) = cursor->release_transaction_owner;
  cursor->cancel_checked = NULL;
  cursor->owner = NULL;
  cursor->release_owner = NULL;
  cursor->owner_status = NULL;
  cursor->report_owner_error = NULL;
  cursor->begin_execution = NULL;
  cursor->end_execution = NULL;
  cursor->request_cleanup = NULL;
  cursor->transaction_owner = NULL;
  cursor->release_transaction_owner = NULL;
  if (release_transaction_owner != NULL)
    release_transaction_owner(transaction_owner);
  if (release_owner != NULL) release_owner(owner);
#endif
}

static int orm_row_publisher_config_valid(
    const orm_row_publisher_config *config) {
  return config != NULL && config->struct_size >= sizeof(*config) &&
         config->abi_version == ORM_ROW_PUBLISHER_CONFIG_ABI_VERSION &&
         config->row_shape != NULL &&
         cmeta_data_desc_valid(config->row_shape) &&
         config->row_shape->storage_type != NULL && config->max_depth != 0u;
}

static const char *orm_row_publisher_name(void *state_) {
  const orm_row_publisher_state *state =
      (const orm_row_publisher_state *)state_;
  return state->cursor.ops->name;
}

static const cmeta_type_desc *orm_row_publisher_output_type(void *state_) {
  const orm_row_publisher_state *state =
      (const orm_row_publisher_state *)state_;
  return state->row_shape->storage_type;
}

static void orm_row_publisher_cancel_cursor(orm_row_publisher_state *state) {
  if (state->cancelled)
    return;
  state->cancelled = 1;
  orm_row_cursor_cancel_native(&state->cursor);
}

static cflow_step orm_row_publisher_fail(orm_row_publisher_state *state,
                                        orm_status_t status,
                                        const char *message) {
  state->terminal = CFLOW_PUBLISHER_ERROR;
  /* A backend's message is borrowed only until cancel: own it first. */
  (void)snprintf(state->error_message, sizeof(state->error_message),
                 "%s", message != NULL ? message : orm_status_message(status));
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  if (state->cursor.report_owner_error != NULL)
    state->cursor.report_owner_error(state->cursor.owner, status);
#endif
  orm_row_publisher_cancel_cursor(state);
  return (cflow_step){CFLOW_STEP_ERROR, {0}, state->error_message};
}

static orm_status_t orm_row_status_to_orm(DataBindStatus status) {
  switch (status) {
    case DATA_BIND_ERR_LIMIT:
      return ORM_STATUS_LIMIT_EXCEEDED;
    case DATA_BIND_ERR_IO:
      return ORM_STATUS_DATASTORE_ERROR;
    case DATA_BIND_ERR_PARSE:
    case DATA_BIND_ERR_TYPE_NOT_FOUND:
    case DATA_BIND_ERR_TYPE_MISMATCH:
      return ORM_STATUS_TYPE_ERROR;
    case DATA_BIND_ERR_OOM:
      return ORM_STATUS_OUT_OF_MEMORY;
    case DATA_BIND_ERR_SCHEMA:
      return ORM_STATUS_UNSUPPORTED;
    case DATA_BIND_ERR_INVALID_ARG:
    case DATA_BIND_ERR_RUNTIME:
    case DATA_BIND_ERR_BUFFER_TOO_SMALL:
    case DATA_BIND_ERR_CANCELED:
    case DATA_BIND_OK:
    default:
      return ORM_STATUS_INTERNAL_ERROR;
  }
}

static cflow_step orm_row_publisher_resume_admitted(
    void *state_, cflow_publish_context *ctx, void *out_value) {
  orm_row_publisher_state *state = (orm_row_publisher_state *)state_;
  cserde_reader reader = {0};
  orm_row_cursor_step cursor_step;
  DataBindNativeDiagnostic bind_error = DATA_BIND_NATIVE_DIAGNOSTIC_INIT;
  DataBindStatus bind_status;

  (void)ctx;
  if (state->terminal == CFLOW_PUBLISHER_DONE)
    return (cflow_step){CFLOW_STEP_DONE, {0}, NULL};
  if (state->terminal == CFLOW_PUBLISHER_ERROR)
    return (cflow_step){CFLOW_STEP_ERROR, {0}, state->error_message};
  if (out_value == NULL)
    return orm_row_publisher_fail(state, ORM_STATUS_INVALID_ARGUMENT,
                                 "CFlow supplied null row storage");

#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  if (state->cursor.owner_status != NULL) {
    orm_error_t owner_error;
    orm_error_init(&owner_error);
    const orm_status_t owner_status = state->cursor.owner_status(
        state->cursor.owner, &owner_error);
    if (owner_status != ORM_STATUS_OK)
      return orm_row_publisher_fail(state, owner_status, owner_error.message);
  }
#endif
  cursor_step = state->cursor.ops->next(state->cursor.context, &reader);
  switch (cursor_step.kind) {
    case ORM_ROW_CURSOR_WAIT:
      if (!cflow_waitable_valid(&cursor_step.waitable))
        return orm_row_publisher_fail(
            state, ORM_STATUS_INTERNAL_ERROR,
            "driver returned WAIT without a valid waitable");
      return (cflow_step){CFLOW_STEP_WAIT, cursor_step.waitable, NULL};
    case ORM_ROW_CURSOR_DONE:
      state->terminal = CFLOW_PUBLISHER_DONE;
      return (cflow_step){CFLOW_STEP_DONE, {0}, NULL};
    case ORM_ROW_CURSOR_ERROR:
      return orm_row_publisher_fail(
          state,
          cursor_step.status == ORM_STATUS_OK ? ORM_STATUS_DATASTORE_ERROR
                                              : cursor_step.status,
          cursor_step.message);
    case ORM_ROW_CURSOR_ROW:
    case ORM_ROW_CURSOR_ROW_AND_DONE:
      break;
    default:
      return orm_row_publisher_fail(state, ORM_STATUS_INTERNAL_ERROR,
                                   "driver returned an invalid cursor step");
  }

  bind_status = data_bind_native_init(
      &state->bind_options, state->row_shape, out_value,
      state->row_shape->storage_type->size, &bind_error);
  if (bind_status == DATA_BIND_OK)
    bind_status = data_bind_native_decode_bounded(
        &state->bind_options, state->row_shape, &reader, out_value,
        state->row_shape->storage_type->size, state->max_buffer_bytes, &bind_error);
  if (bind_status != DATA_BIND_OK) {
    char message[ORM_C_ERROR_MESSAGE_CAPACITY];
    (void)snprintf(message, sizeof(message),
                   "row binding failed: databind=%d cserde=%d path=%s",
                   (int)bind_status, (int)bind_error.source_status,
                   bind_error.error.path);
    return orm_row_publisher_fail(
        state, orm_row_status_to_orm(bind_status), message);
  }

  if (cursor_step.kind == ORM_ROW_CURSOR_ROW_AND_DONE) {
    state->terminal = CFLOW_PUBLISHER_DONE;
    return (cflow_step){CFLOW_STEP_VALUE_AND_DONE, {0}, NULL};
  }
  return (cflow_step){CFLOW_STEP_VALUE, {0}, NULL};
}

static cflow_step orm_row_publisher_resume(void *state_, cflow_publish_context *ctx,
                                          void *out_value) {
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  orm_row_publisher_state *state = (orm_row_publisher_state *)state_;
  if (state->terminal == CFLOW_PUBLISHER_OPEN &&
      state->cursor.begin_execution != NULL) {
    orm_error_t error;
    orm_error_init(&error);
    const orm_status_t status = state->cursor.begin_execution(
        state->cursor.owner, &error);
    if (status != ORM_STATUS_OK) {
      /* Admission refusal is not a native failure. In particular, BUSY and
       * exhausted completion capacity must not reenter native cancellation.
       * Keep the cursor and all parent holds for its later explicit disposal. */
      state->terminal = CFLOW_PUBLISHER_ERROR;
      (void)snprintf(state->error_message, sizeof(state->error_message), "%s",
                     error.message[0] != '\0' ? error.message
                                              : orm_status_message(status));
      return (cflow_step){CFLOW_STEP_ERROR, {0}, state->error_message};
    }
    /* Every admitted return, including WAIT/DONE/bind errors, ends here after
     * the borrowed reader and any error message have been consumed. */
    const cflow_step result = orm_row_publisher_resume_admitted(state_, ctx, out_value);
    state->cursor.end_execution(state->cursor.owner);
    return result;
  }
#endif
  return orm_row_publisher_resume_admitted(state_, ctx, out_value);
}

#if defined(ORM_NATIVE_OWNER_CANDIDATE)
static void orm_row_publisher_run_cleanup(void *context, unsigned request) {
  orm_row_publisher_state *state = context;
  if ((request & ORM_NATIVE_CLEANUP_CANCEL) != 0u)
    orm_row_publisher_cancel_cursor(state);
  if ((request & ORM_NATIVE_CLEANUP_DESTROY) != 0u)
    orm_row_cursor_dispose(&state->cursor);
}

static void orm_row_publisher_finish_cleanup(void *context, unsigned completed) {
  orm_row_publisher_state *state = context;
  if ((completed & ORM_NATIVE_CLEANUP_DESTROY) != 0u) {
    free(state->scratch);
    free(state);
  }
}
#endif

static void orm_row_publisher_cancel(void *state_) {
  orm_row_publisher_state *state = (orm_row_publisher_state *)state_;
  if (state->terminal == CFLOW_PUBLISHER_OPEN) {
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
    if (state->cursor.request_cleanup != NULL) {
      state->terminal = CFLOW_PUBLISHER_DONE;
      state->cursor.request_cleanup(state->cursor.owner, &state->native_cleanup,
                                      ORM_NATIVE_CLEANUP_CANCEL);
      return;
    }
#endif
    orm_row_publisher_cancel_cursor(state);
    state->terminal = CFLOW_PUBLISHER_DONE;
  }
}

static void orm_row_publisher_destroy(void *state_) {
  orm_row_publisher_state *state = (orm_row_publisher_state *)state_;
  if (state == NULL)
    return;
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  if (state->cursor.request_cleanup != NULL) {
    const unsigned request = ORM_NATIVE_CLEANUP_DESTROY |
        (state->terminal == CFLOW_PUBLISHER_OPEN ? ORM_NATIVE_CLEANUP_CANCEL : 0u);
    /* Destruction consumes the public Publisher, not its pending-cleanup hold.
     * The embedded node, cursor, scratch and parents survive through finish. */
    state->terminal = CFLOW_PUBLISHER_DONE;
    state->cursor.request_cleanup(state->cursor.owner, &state->native_cleanup,
                                    request);
    return; /* It may have completed synchronously and freed state. */
  }
#endif
  if (state->terminal == CFLOW_PUBLISHER_OPEN)
    orm_row_publisher_cancel_cursor(state);
  orm_row_cursor_dispose(&state->cursor);
  free(state->scratch);
  free(state);
}

static void orm_row_publisher_bind_terminal_waker(void *state_,
                                                 cflow_waker waker) {
  (void)state_;
  (void)waker;
}

static cflow_publisher_terminal orm_row_publisher_poll_terminal(void *state_,
                                                            const char **error) {
  const orm_row_publisher_state *state =
      (const orm_row_publisher_state *)state_;
  if (error != NULL)
    *error = state->terminal == CFLOW_PUBLISHER_ERROR ? state->error_message : NULL;
  return state->terminal;
}

CMETA_IMPLEMENTS(cflow_publisher, orm_row_publisher,
                 CFLOW_PUBLISHER_CAP_CONSTRUCTS_VALUES,
    .name = orm_row_publisher_name,
    .output_type = orm_row_publisher_output_type,
    .resume = orm_row_publisher_resume,
    .cancel = orm_row_publisher_cancel,
    .destroy = orm_row_publisher_destroy,
    .bind_terminal_waker = orm_row_publisher_bind_terminal_waker,
    .poll_terminal = orm_row_publisher_poll_terminal
);

void orm_row_publisher_prepared_destroy(orm_row_publisher_prepared *prepared) {
  if (prepared != NULL) {
    free(prepared->scratch);
    free(prepared);
  }
}

orm_status_t orm_row_publisher_prepare(
    const orm_row_publisher_config *config,
    orm_row_publisher_prepared **out_prepared, orm_error_t *error) {
  DataBindNativeOptions options = DATA_BIND_NATIVE_OPTIONS_INIT;
  DataBindNativeRequirements required = DATA_BIND_NATIVE_REQUIREMENTS_INIT;
  DataBindNativeDiagnostic diagnostic = DATA_BIND_NATIVE_DIAGNOSTIC_INIT;
  DataBindStatus status;
  orm_row_publisher_state *state;
  size_t probe_bytes, allocation_bytes, padding;
  void *probe;
  if (out_prepared == NULL || *out_prepared != NULL ||
      !orm_row_publisher_config_valid(config)) {
    orm_row_set_error(error, ORM_STATUS_INVALID_ARGUMENT,
                      "invalid DataBind source configuration");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  /* Native depth counts scalar leaves; ORM depth counts containers. The
   * measured container depth separately rejects an extra empty Struct. */
  if (config->max_depth == SIZE_MAX) {
    orm_row_set_error(error, ORM_STATUS_LIMIT_EXCEEDED, "row depth overflow");
    return ORM_STATUS_LIMIT_EXCEEDED;
  }
  options.max_depth = config->max_depth + 1u;
  /* Static Struct fields were never collection items. The supported native
   * graph has no dynamic collections: unsupported kinds still fail preflight.
   * Tighten the whole-graph traversal count to the measured canonical graph. */
  options.max_items = SIZE_MAX;
  options.max_owned_bytes = SIZE_MAX; /* Per-value bound is enforced at decode. */
  status = data_bind_native_probe_workspace_size(options.max_depth, &probe_bytes);
  if (status != DATA_BIND_OK) {
    orm_row_set_error(error, orm_row_status_to_orm(status), "row traversal size overflow");
    return orm_row_status_to_orm(status);
  }
  probe = malloc(probe_bytes);
  if (probe == NULL) {
    orm_row_set_error(error, ORM_STATUS_OUT_OF_MEMORY, NULL);
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  options.workspace = probe;
  options.workspace_bytes = probe_bytes;
  status = data_bind_native_measure(&options, config->row_shape, &required, &diagnostic);
  free(probe);
  if (status != DATA_BIND_OK) {
    orm_row_set_error(error, orm_row_status_to_orm(status), diagnostic.error.message);
    return orm_row_status_to_orm(status);
  }
  if (required.container_depth > config->max_depth ||
      required.field_tracking_bytes > config->scratch_bytes) {
    orm_row_set_error(error, ORM_STATUS_LIMIT_EXCEEDED,
                      "row container depth or field bitmap exceeds caller budget");
    return ORM_STATUS_LIMIT_EXCEEDED;
  }
  if (required.workspace_alignment == 0u ||
      required.decode_bytes > SIZE_MAX - (required.workspace_alignment - 1u)) {
    orm_row_set_error(error, ORM_STATUS_LIMIT_EXCEEDED, "row workspace size overflow");
    return ORM_STATUS_LIMIT_EXCEEDED;
  }
  allocation_bytes = required.decode_bytes + required.workspace_alignment - 1u;
  state = (orm_row_publisher_state *)calloc(1u, sizeof(*state));
  if (state == NULL) {
    orm_row_set_error(error, ORM_STATUS_OUT_OF_MEMORY, NULL);
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  state->scratch = malloc(allocation_bytes);
  if (state->scratch == NULL) {
    free(state);
    orm_row_set_error(error, ORM_STATUS_OUT_OF_MEMORY, NULL);
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  padding = (uintptr_t)state->scratch % required.workspace_alignment;
  if (padding != 0u) padding = required.workspace_alignment - padding;
  options.workspace = (unsigned char *)state->scratch + padding;
  options.workspace_bytes = required.decode_bytes;
  options.max_items = required.descriptor_nodes;
  state->bind_options = options;
  state->max_buffer_bytes = config->max_buffer_bytes;
  state->row_shape = config->row_shape;
  state->terminal = CFLOW_PUBLISHER_OPEN;
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  state->native_cleanup.run = orm_row_publisher_run_cleanup;
  state->native_cleanup.finish = orm_row_publisher_finish_cleanup;
  state->native_cleanup.context = state;
#endif
  *out_prepared = state;
  orm_row_set_error(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

orm_status_t orm_row_publisher_publish(
    cflow_publisher *out_publisher, orm_row_cursor *cursor,
    orm_row_publisher_prepared *prepared, orm_error_t *error) {
  orm_row_publisher_state *state = prepared;
  cflow_publisher source;
  if (state == NULL || out_publisher == NULL || out_publisher->self != NULL ||
      !orm_row_cursor_valid(cursor)) {
    orm_row_set_error(error, ORM_STATUS_INVALID_ARGUMENT,
                      "invalid prepared row Publisher");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  if (cursor->ops->configure_shape != NULL) {
    const orm_status_t status = cursor->ops->configure_shape(
        cursor->context, state->row_shape, error);
    if (status != ORM_STATUS_OK) return status;
  }
  state->cursor = *cursor;
  source = orm_row_publisher_as_cflow_publisher(state);
  *out_publisher = source;
  cursor->ops = NULL;
  cursor->context = NULL;
  cursor->wait_timeout_ns = 0u;
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  cursor->cancel_checked = NULL;
  cursor->owner = NULL;
  cursor->release_owner = NULL;
  cursor->owner_status = NULL;
  cursor->report_owner_error = NULL;
  cursor->begin_execution = NULL;
  cursor->end_execution = NULL;
  cursor->request_cleanup = NULL;
  cursor->transaction_owner = NULL;
  cursor->release_transaction_owner = NULL;
#endif
  orm_row_set_error(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}


orm_status_t orm_row_publisher_init(cflow_publisher *out_publisher,
                                   orm_row_cursor *cursor,
                                   const orm_row_publisher_config *config,
                                   orm_error_t *error) {
  orm_row_publisher_prepared *prepared = NULL;
  orm_status_t status;
  if (out_publisher == NULL || out_publisher->self != NULL ||
      !orm_row_cursor_valid(cursor)) {
    orm_row_set_error(error, ORM_STATUS_INVALID_ARGUMENT,
                      "invalid DataBind source configuration");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  status = orm_row_publisher_prepare(config, &prepared, error);
  if (status != ORM_STATUS_OK) return status;
  status = orm_row_publisher_publish(out_publisher, cursor, prepared, error);
  if (status != ORM_STATUS_OK) orm_row_publisher_prepared_destroy(prepared);
  return status;
}

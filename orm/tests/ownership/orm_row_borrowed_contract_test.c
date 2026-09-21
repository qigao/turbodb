/* #59: source lifetime is not result ownership. Use the real row Publisher,
 * CSerde reader and canonical Salts providers; no substitute binder or lease. */
#define TINYTEST_NO_MAIN
#include "orm_row_publisher.h"
#include <salts_cmeta_data.h>
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

typedef struct BorrowedSource {
  unsigned char *payload;
  cserde_token_kind token_kind;
  char message[ORM_C_ERROR_MESSAGE_CAPACITY + 32u];
  size_t configure_calls, next_calls, read_calls, cancel_calls, destroy_calls;
  bool emitted, fail_next;
} BorrowedSource;
static BorrowedSource borrowed_source;
static orm_row_cursor borrowed_cursor;
static cflow_publisher borrowed_publisher;
static tstr owned_output;
static const unsigned char original_payload[] = {'A', 0u, 'B'};

static cserde_status borrowed_reader_next(void *context, cserde_token *out) {
  BorrowedSource *state = context;
  ++state->read_calls;
  if (state->emitted) return CSERDE_DONE;
  *out = (cserde_token){.kind = state->token_kind};
  out->value.slice = (cserde_slice){state->payload, sizeof(original_payload),
                                   CSERDE_VIEW_STABLE};
  state->emitted = true;
  return CSERDE_OK;
}
static const cserde_reader_ops borrowed_reader_ops = {
    sizeof(cserde_reader_ops), CSERDE_READER_OPS_ABI_VERSION, borrowed_reader_next};
static orm_row_cursor_step borrowed_next(void *context, cserde_reader *reader) {
  BorrowedSource *state = context;
  orm_row_cursor_step result = ORM_ROW_CURSOR_STEP_INIT;
  ++state->next_calls;
  if (state->fail_next) {
    result.kind = ORM_ROW_CURSOR_ERROR;
    result.status = ORM_STATUS_DATASTORE_ERROR;
    result.message = state->message;
    return result;
  }
  state->emitted = false;
  if (cserde_reader_init(reader, &borrowed_reader_ops, state) != CSERDE_OK) {
    result.kind = ORM_ROW_CURSOR_ERROR;
    result.status = ORM_STATUS_INTERNAL_ERROR;
    return result;
  }
  result.kind = ORM_ROW_CURSOR_ROW;
  return result;
}
static orm_status_t borrowed_configure(void *context,
    const cmeta_data_desc *shape, orm_error_t *error) {
  (void)shape; (void)error;
  ++((BorrowedSource *)context)->configure_calls;
  return ORM_STATUS_OK;
}
static void borrowed_cancel(void *context) {
  BorrowedSource *state = context;
  ++state->cancel_calls;
  /* Both borrowed spans expire at cancellation, not before reader consumption. */
  if (state->payload != NULL) memset(state->payload, 0xcc, sizeof(original_payload));
  memset(state->message, 'X', sizeof(state->message) - 1u);
  state->message[sizeof(state->message) - 1u] = '\0';
}
static void borrowed_destroy(void *context) {
  BorrowedSource *state = context;
  ++state->destroy_calls;
  free(state->payload);
  state->payload = NULL;
}
static const orm_row_cursor_ops borrowed_cursor_ops = {
    sizeof(orm_row_cursor_ops), ORM_ROW_CURSOR_OPS_ABI_VERSION, "borrowed-contract",
    borrowed_next, borrowed_cancel, borrowed_destroy, borrowed_configure, NULL};

static void require_stable_owned(cmeta_data_kind kind) {
  cmeta_data_desc shape = salts_tstr_cmeta_data;
  shape.kind = kind;
  borrowed_source.token_kind = kind == CMETA_DATA_STRING ? CSERDE_STRING : CSERDE_BYTES;
  borrowed_source.payload = malloc(sizeof(original_payload));
  check_not_null(borrowed_source.payload);
  memcpy(borrowed_source.payload, original_payload, sizeof(original_payload));
  const orm_row_publisher_config config = ORM_ROW_PUBLISHER_CONFIG_INIT(
      &shape, 0u, 1u, 0u, sizeof(original_payload));
  orm_error_t error;
  orm_error_init(&error);
  check_equal(orm_row_publisher_init(&borrowed_publisher, &borrowed_cursor,
                                    &config, &error), ORM_STATUS_OK);
  check_null(borrowed_cursor.context);
  check_equal(cflow_publisher_resume(&borrowed_publisher, NULL, &owned_output).kind,
              CFLOW_STEP_VALUE);
  check_equal(borrowed_source.read_calls, 1u); /* No extra EOF probe. */
  check_true((const void *)owned_output != (const void *)borrowed_source.payload);
  check_equal(tstr_len(owned_output), sizeof(original_payload));
  check_equal(memcmp(owned_output, original_payload, sizeof(original_payload)), 0);
  cflow_publisher_cancel(&borrowed_publisher);
  cflow_publisher_cancel(&borrowed_publisher);
  check_equal(borrowed_source.cancel_calls, 1u);
  check_equal(borrowed_source.payload[0], 0xcc);
  check_equal(memcmp(owned_output, original_payload, sizeof(original_payload)), 0);
  cflow_publisher_destroy(&borrowed_publisher);
  borrowed_publisher = (cflow_publisher){0};
  check_equal(borrowed_source.destroy_calls, 1u);
  check_null(borrowed_source.payload);
  check_equal(tstr_len(owned_output), sizeof(original_payload));
  check_equal(memcmp(owned_output, original_payload, sizeof(original_payload)), 0);
}

static void require_borrowed_rejection(cmeta_data_kind kind, bool nested) {
  cmeta_data_desc value = salts_tstr_cmeta_data;
  const cmeta_data_buffer_shape borrowed_shape = {CMETA_DATA_BUFFER_BORROWED};
  value.kind = kind;
  value.stable_id = kind == CMETA_DATA_STRING ? "orm.test.borrowed.text" : "orm.test.borrowed.bytes";
  value.display_name = "BorrowedValue";
  value.shape = &borrowed_shape;
  value.storage_type = &salts_vstr_cmeta_type;
  value.buffer_ops = &salts_vstr_cmeta_buffer_ops;
  check_true(cmeta_data_desc_valid(&value));
  check_true(cmeta_data_buffer_ops_of(&value) == &salts_vstr_cmeta_buffer_ops);
  check_equal(value.buffer_ops->ownership, CMETA_DATA_BUFFER_BORROWED);
  typedef struct BorrowedRow { vstr value; } BorrowedRow;
  const cmeta_type_identity identity = CMETA_TYPE_ID_ATOM_INIT("orm.test.BorrowedRow");
  const cmeta_type_desc type = {
      .name = "BorrowedRow", .size = sizeof(BorrowedRow), .align = _Alignof(BorrowedRow),
      .kind = CMETA_T_OBJECT, .identity = &identity};
  const cmeta_field_desc field = {
      .name = "value", .type_name = "vstr", .offset = offsetof(BorrowedRow, value),
      .size = sizeof(vstr), .align = _Alignof(vstr), .type = &salts_vstr_cmeta_type};
  const cmeta_struct_desc layout = {"BorrowedRow", sizeof(BorrowedRow),
                                   _Alignof(BorrowedRow), &field, 1u};
  const cmeta_data_field_desc data_field = {"row.value", "value",
                                           offsetof(BorrowedRow, value), &value};
  const cmeta_data_struct_shape fields = {&layout, &data_field, 1u};
  const cmeta_data_desc row = {
      .struct_size = sizeof(cmeta_data_desc), .abi_version = CMETA_DATA_DESC_ABI_VERSION,
      .stable_id = "orm.test.BorrowedRow.data", .display_name = "BorrowedRow",
      .kind = CMETA_DATA_STRUCT, .storage_type = &type, .shape = &fields};
  check_true(cmeta_data_desc_valid(&row));
  const orm_row_publisher_config config = ORM_ROW_PUBLISHER_CONFIG_INIT(
      nested ? &row : &value, 1u, 1u, 0u, sizeof(original_payload));
  orm_error_t error;
  orm_error_init(&error);
  check_equal(orm_row_publisher_init(&borrowed_publisher, &borrowed_cursor,
                                    &config, &error), ORM_STATUS_UNSUPPORTED);
  check_equal(error.status, ORM_STATUS_UNSUPPORTED);
  check_not_null(strstr(error.message, "owned v2 buffer provider"));
  check_null(borrowed_publisher.self);
  check_true(borrowed_cursor.context == &borrowed_source);
  check_equal(borrowed_source.configure_calls, 0u);
  check_equal(borrowed_source.next_calls, 0u);
  check_equal(borrowed_source.read_calls, 0u);
  check_equal(borrowed_source.cancel_calls, 0u);
  check_equal(borrowed_source.destroy_calls, 0u);
  orm_row_cursor_dispose(&borrowed_cursor); /* Still explicitly caller-owned. */
  check_equal(borrowed_source.destroy_calls, 1u);
}

static void require_borrowed_diagnostic(bool long_message) {
  char expected[ORM_C_ERROR_MESSAGE_CAPACITY];
  const char *terminal_error = NULL;
  int output = 91;
  borrowed_source.fail_next = true;
  if (long_message) {
    memset(borrowed_source.message, 'D', sizeof(borrowed_source.message) - 1u);
    borrowed_source.message[sizeof(borrowed_source.message) - 1u] = '\0';
    memset(expected, 'D', sizeof(expected) - 1u);
    expected[sizeof(expected) - 1u] = '\0';
  } else {
    static const char message[] = "native error before cancellation";
    memcpy(borrowed_source.message, message, sizeof(message));
    memcpy(expected, message, sizeof(message));
  }
  const orm_row_publisher_config config = ORM_ROW_PUBLISHER_CONFIG_INIT(
      &cmeta_data_int, 0u, 1u, 0u, 0u);
  orm_error_t error;
  orm_error_init(&error);
  check_equal(orm_row_publisher_init(&borrowed_publisher, &borrowed_cursor,
                                    &config, &error), ORM_STATUS_OK);
  const cflow_step step = cflow_publisher_resume(&borrowed_publisher, NULL, &output);
  check_equal(step.kind, CFLOW_STEP_ERROR);
  check_not_null(step.error);
  check_true(step.error != borrowed_source.message);
  check_equal(strcmp(step.error, expected), 0);
  check_equal(borrowed_source.message[0], 'X'); /* Cancellation already invalidated it. */
  check_equal(output, 91); /* No row was offered; no raw output was constructed. */
  check_equal(borrowed_source.cancel_calls, 1u);
  check_equal(borrowed_source.next_calls, 1u);
  check_equal(borrowed_source.read_calls, 0u);
  check_equal(cflow_publisher_poll_terminal(&borrowed_publisher, &terminal_error),
              CFLOW_PUBLISHER_ERROR);
  check_equal(strcmp(terminal_error, expected), 0);
  check_equal(cflow_publisher_resume(&borrowed_publisher, NULL, &output).kind,
              CFLOW_STEP_ERROR);
  cflow_publisher_cancel(&borrowed_publisher);
  check_equal(borrowed_source.next_calls, 1u);
  check_equal(borrowed_source.cancel_calls, 1u);
  cflow_publisher_destroy(&borrowed_publisher);
  borrowed_publisher = (cflow_publisher){0};
  check_equal(borrowed_source.destroy_calls, 1u);
  /* Publisher-owned error pointers are not used after Publisher destruction. */
}

spec("ORM source lifetime and borrowed output admission") {
  before_each() {
    borrowed_source = (BorrowedSource){0};
    borrowed_cursor = (orm_row_cursor){.ops = &borrowed_cursor_ops, .context = &borrowed_source};
    borrowed_publisher = (cflow_publisher){0};
    owned_output = NULL;
  }
  after_each() {
    if (cflow_publisher_valid(&borrowed_publisher)) cflow_publisher_destroy(&borrowed_publisher);
    orm_row_cursor_dispose(&borrowed_cursor);
    free(borrowed_source.payload);
    borrowed_source.payload = NULL;
    salts_tstr_cmeta_buffer_ops.restore_zero(&owned_output);
  }
  it("copies STABLE text into ownership surviving cancellation and source destruction") {
    require_stable_owned(CMETA_DATA_STRING);
  }
  it("copies STABLE bytes into ownership surviving cancellation and source destruction") {
    require_stable_owned(CMETA_DATA_BYTES);
  }
  it("rejects canonical borrowed text output before native callbacks") {
    require_borrowed_rejection(CMETA_DATA_STRING, false);
  }
  it("rejects canonical borrowed bytes output before native callbacks") {
    require_borrowed_rejection(CMETA_DATA_BYTES, false);
  }
  it("rejects a nested borrowed text field before native callbacks") {
    require_borrowed_rejection(CMETA_DATA_STRING, true);
  }
  it("rejects a nested borrowed bytes field before native callbacks") {
    require_borrowed_rejection(CMETA_DATA_BYTES, true);
  }
  it("owns a native diagnostic before cancellation overwrites its borrowed message") {
    require_borrowed_diagnostic(false);
  }
  it("bounds and terminates a long native diagnostic before source invalidation") {
    require_borrowed_diagnostic(true);
  }
}

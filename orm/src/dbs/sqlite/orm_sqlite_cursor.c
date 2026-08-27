#include "orm_sqlite_cursor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum orm_sqlite_reader_phase {
  ORM_SQLITE_READER_MAP_BEGIN = 0,
  ORM_SQLITE_READER_KEY,
  ORM_SQLITE_READER_VALUE,
  ORM_SQLITE_READER_MAP_END,
  ORM_SQLITE_READER_DONE
} orm_sqlite_reader_phase;

typedef struct orm_sqlite_reader_state {
  sqlite3_stmt *statement;
  int column_count;
  int column;
  int text_projection;
  orm_sqlite_reader_phase phase;
} orm_sqlite_reader_state;

typedef struct orm_sqlite_cursor_state {
  sqlite3_stmt *statement;
  orm_sqlite_reader_state reader;
  int terminal;
  char error_message[ORM_C_ERROR_MESSAGE_CAPACITY];
} orm_sqlite_cursor_state;

static orm_status_t orm_sqlite_cursor_status(int sqlite_status,
                                             orm_status_t fallback) {
  switch (sqlite_status & 0xff) {
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
      return ORM_STATUS_BUSY;
    case SQLITE_NOMEM:
      return ORM_STATUS_OUT_OF_MEMORY;
    case SQLITE_TOOBIG:
      return ORM_STATUS_LIMIT_EXCEEDED;
    case SQLITE_RANGE:
      return ORM_STATUS_OUT_OF_RANGE;
    case SQLITE_READONLY:
      return ORM_STATUS_INVALID_STATE;
    default:
      return fallback;
  }
}

static void orm_sqlite_cursor_set_error(orm_error_t *error,
                                        orm_status_t status,
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

static cserde_status orm_sqlite_reader_emit_value(
    orm_sqlite_reader_state *state, cserde_token *out) {
  const int column = state->column;
  const int sqlite_type = sqlite3_column_type(state->statement, column);

  if (state->text_projection && sqlite_type != SQLITE_NULL) {
    const void *data = sqlite_type == SQLITE_BLOB
                           ? sqlite3_column_blob(state->statement, column)
                           : (const void *)sqlite3_column_text(
                                 state->statement, column);
    const int bytes = sqlite3_column_bytes(state->statement, column);
    if (bytes < 0 || data == NULL)
      return CSERDE_SOURCE_ERROR;
    out->kind = sqlite_type == SQLITE_BLOB ? CSERDE_BYTES : CSERDE_STRING;
    out->value.slice.data = (const unsigned char *)data;
    out->value.slice.size = (size_t)bytes;
    out->value.slice.lifetime = CSERDE_VIEW_TRANSIENT;
    return CSERDE_OK;
  }

  switch (sqlite_type) {
    case SQLITE_NULL:
      out->kind = CSERDE_NULL;
      return CSERDE_OK;
    case SQLITE_INTEGER:
      out->kind = CSERDE_SINT;
      out->value.sint = (int64_t)sqlite3_column_int64(state->statement, column);
      return CSERDE_OK;
    case SQLITE_FLOAT:
      out->kind = CSERDE_FLOAT;
      out->value.floating = sqlite3_column_double(state->statement, column);
      return CSERDE_OK;
    case SQLITE_TEXT:
    case SQLITE_BLOB: {
      const void *data = sqlite_type == SQLITE_TEXT
                             ? (const void *)sqlite3_column_text(
                                   state->statement, column)
                             : sqlite3_column_blob(state->statement, column);
      const int bytes = sqlite3_column_bytes(state->statement, column);
      if (bytes < 0 || (data == NULL && bytes != 0))
        return CSERDE_SOURCE_ERROR;
      out->kind = sqlite_type == SQLITE_TEXT ? CSERDE_STRING : CSERDE_BYTES;
      out->value.slice.data = (const unsigned char *)data;
      out->value.slice.size = (size_t)bytes;
      out->value.slice.lifetime = CSERDE_VIEW_TRANSIENT;
      return CSERDE_OK;
    }
    default:
      return CSERDE_UNSUPPORTED;
  }
}

static cserde_status orm_sqlite_reader_next(void *context,
                                             cserde_token *out) {
  orm_sqlite_reader_state *state = (orm_sqlite_reader_state *)context;

  if (state == NULL || out == NULL)
    return CSERDE_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  switch (state->phase) {
    case ORM_SQLITE_READER_MAP_BEGIN:
      out->kind = CSERDE_MAP_BEGIN;
      state->phase = state->column_count == 0 ? ORM_SQLITE_READER_MAP_END
                                              : ORM_SQLITE_READER_KEY;
      return CSERDE_OK;
    case ORM_SQLITE_READER_KEY: {
      const char *name = sqlite3_column_name(state->statement, state->column);
      if (name == NULL)
        return CSERDE_SOURCE_ERROR;
      out->kind = CSERDE_STRING;
      out->value.slice.data = (const unsigned char *)name;
      out->value.slice.size = strlen(name);
      out->value.slice.lifetime = CSERDE_VIEW_STABLE;
      state->phase = ORM_SQLITE_READER_VALUE;
      return CSERDE_OK;
    }
    case ORM_SQLITE_READER_VALUE: {
      const cserde_status status = orm_sqlite_reader_emit_value(state, out);
      if (status != CSERDE_OK)
        return status;
      ++state->column;
      state->phase = state->column == state->column_count
                         ? ORM_SQLITE_READER_MAP_END
                         : ORM_SQLITE_READER_KEY;
      return CSERDE_OK;
    }
    case ORM_SQLITE_READER_MAP_END:
      out->kind = CSERDE_MAP_END;
      state->phase = ORM_SQLITE_READER_DONE;
      return CSERDE_OK;
    case ORM_SQLITE_READER_DONE:
      return CSERDE_DONE;
    default:
      return CSERDE_INVALID_STATE;
  }
}

static const cserde_reader_ops orm_sqlite_reader_ops = {
    sizeof(cserde_reader_ops), CSERDE_READER_OPS_ABI_VERSION,
    orm_sqlite_reader_next};

static orm_row_cursor_step orm_sqlite_cursor_next(void *context,
                                                   cserde_reader *out_row) {
  orm_sqlite_cursor_state *state = (orm_sqlite_cursor_state *)context;
  orm_row_cursor_step result = ORM_ROW_CURSOR_STEP_INIT;
  int sqlite_status;

  if (state->terminal) {
    result.kind = ORM_ROW_CURSOR_DONE;
    return result;
  }
  sqlite_status = sqlite3_step(state->statement);
  if (sqlite_status == SQLITE_DONE) {
    state->terminal = 1;
    result.kind = ORM_ROW_CURSOR_DONE;
    return result;
  }
  if (sqlite_status != SQLITE_ROW) {
    sqlite3 *database = sqlite3_db_handle(state->statement);
    result.kind = ORM_ROW_CURSOR_ERROR;
    result.status =
        orm_sqlite_cursor_status(sqlite_status, ORM_STATUS_SQL_ERROR);
    (void)snprintf(state->error_message, sizeof(state->error_message),
                   "step SQLite cursor: %s",
                   database != NULL ? sqlite3_errmsg(database)
                                    : "unknown SQLite error");
    result.message = state->error_message;
    state->terminal = 1;
    return result;
  }

  state->reader.statement = state->statement;
  state->reader.column_count = sqlite3_column_count(state->statement);
  state->reader.column = 0;
  state->reader.phase = ORM_SQLITE_READER_MAP_BEGIN;
  if (cserde_reader_init(out_row, &orm_sqlite_reader_ops, &state->reader) !=
      CSERDE_OK) {
    result.kind = ORM_ROW_CURSOR_ERROR;
    result.status = ORM_STATUS_INTERNAL_ERROR;
    result.message = "initialize SQLite row reader";
    state->terminal = 1;
    return result;
  }
  result.kind = ORM_ROW_CURSOR_ROW;
  return result;
}

static void orm_sqlite_cursor_cancel(void *context) {
  orm_sqlite_cursor_state *state = (orm_sqlite_cursor_state *)context;
  if (state->terminal)
    return;
  state->terminal = 1;
  (void)sqlite3_reset(state->statement);
}

static void orm_sqlite_cursor_destroy(void *context) {
  orm_sqlite_cursor_state *state = (orm_sqlite_cursor_state *)context;
  if (state == NULL)
    return;
  if (state->statement != NULL)
    (void)sqlite3_finalize(state->statement);
  free(state);
}

static const orm_row_cursor_ops orm_sqlite_cursor_ops = {
    sizeof(orm_row_cursor_ops), ORM_ROW_CURSOR_OPS_ABI_VERSION,
    "sqlite", orm_sqlite_cursor_next, orm_sqlite_cursor_cancel,
    orm_sqlite_cursor_destroy};

static orm_status_t orm_sqlite_cursor_from_statement_impl(
    orm_row_cursor *out_cursor, sqlite3_stmt **statement, int text_projection,
    orm_error_t *error) {
  orm_sqlite_cursor_state *state;

  if (out_cursor == NULL || out_cursor->ops != NULL ||
      out_cursor->context != NULL || statement == NULL || *statement == NULL ||
      sqlite3_stmt_busy(*statement)) {
    orm_sqlite_cursor_set_error(error, ORM_STATUS_INVALID_ARGUMENT,
                                "invalid or active SQLite statement");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  state = (orm_sqlite_cursor_state *)calloc(1u, sizeof(*state));
  if (state == NULL) {
    orm_sqlite_cursor_set_error(error, ORM_STATUS_OUT_OF_MEMORY, NULL);
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  state->statement = *statement;
  state->reader.text_projection = text_projection;
  out_cursor->ops = &orm_sqlite_cursor_ops;
  out_cursor->context = state;
  *statement = NULL;
  orm_sqlite_cursor_set_error(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

orm_status_t orm_sqlite_cursor_from_statement(orm_row_cursor *out_cursor,
                                              sqlite3_stmt **statement,
                                              orm_error_t *error) {
  return orm_sqlite_cursor_from_statement_impl(out_cursor, statement, 0,
                                               error);
}

orm_status_t orm_sqlite_text_cursor_from_statement(
    orm_row_cursor *out_cursor, sqlite3_stmt **statement, orm_error_t *error) {
  return orm_sqlite_cursor_from_statement_impl(out_cursor, statement, 1,
                                               error);
}

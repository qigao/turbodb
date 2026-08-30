#include "fake_libpq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FAKE_LIBPQ_MAX_RESULTS = 8, FAKE_LIBPQ_MAX_COLUMNS = 16 };

struct pg_conn {
  ConnStatusType status;
};

struct pg_result {
  ExecStatusType status;
  const char *message;
  int cleared;
  size_t column_count;
  const char *values[FAKE_LIBPQ_MAX_COLUMNS];
  int lengths[FAKE_LIBPQ_MAX_COLUMNS];
  int nulls[FAKE_LIBPQ_MAX_COLUMNS];
};

typedef struct fake_libpq_state {
  struct pg_conn connection;
  struct pg_result results[FAKE_LIBPQ_MAX_RESULTS];
  size_t result_count;
  size_t result_index;
  struct pg_result sync_result;
  int send_result;
  int single_row_result;
  const char *connection_error;
  fake_libpq_metrics metrics;
} fake_libpq_state;

static fake_libpq_state fake_state;

static void fake_copy(char *destination, size_t capacity, const char *source) {
  size_t length;
  if (destination == NULL || capacity == 0u)
    return;
  if (source == NULL)
    source = "";
  length = strlen(source);
  if (length >= capacity)
    length = capacity - 1u;
  if (length != 0u)
    memcpy(destination, source, length);
  destination[length] = '\0';
}

void fake_libpq_reset(void) {
  memset(&fake_state, 0, sizeof(fake_state));
  fake_state.connection.status = CONNECTION_OK;
  fake_state.send_result = 1;
  fake_state.single_row_result = 1;
  fake_state.sync_result.status = PGRES_COMMAND_OK;
  fake_state.sync_result.message = "";
  fake_state.connection_error = "";
}

void fake_libpq_set_connect_failure(const char *message) {
  fake_state.connection.status = CONNECTION_BAD;
  fake_state.connection_error = message;
}

void fake_libpq_set_send_failure(const char *message) {
  fake_state.send_result = 0;
  fake_state.connection_error = message;
}

void fake_libpq_set_connection_error(const char *message) {
  fake_state.connection_error = message;
}

void fake_libpq_set_results(const ExecStatusType *statuses,
                            const char *const *messages, size_t count) {
  size_t index;
  if (count > FAKE_LIBPQ_MAX_RESULTS)
    count = FAKE_LIBPQ_MAX_RESULTS;
  fake_state.result_count = count;
  fake_state.result_index = 0u;
  for (index = 0u; index < count; ++index) {
    fake_state.results[index].status = statuses[index];
    fake_state.results[index].message = messages != NULL ? messages[index] : "";
    fake_state.results[index].cleared = 0;
  }
}

void fake_libpq_set_result_row(size_t result_index,
                               const char *const *values,
                               const int *lengths, const int *nulls,
                               size_t column_count) {
  size_t index;
  if (result_index >= fake_state.result_count) return;
  if (column_count > FAKE_LIBPQ_MAX_COLUMNS)
    column_count = FAKE_LIBPQ_MAX_COLUMNS;
  fake_state.results[result_index].column_count = column_count;
  for (index = 0u; index < column_count; ++index) {
    fake_state.results[result_index].values[index] = values[index];
    fake_state.results[result_index].lengths[index] = lengths[index];
    fake_state.results[result_index].nulls[index] = nulls[index];
  }
}

void fake_libpq_set_sync_result(ExecStatusType status, const char *message) {
  fake_state.sync_result.status = status;
  fake_state.sync_result.message = message != NULL ? message : "";
}

void fake_libpq_set_single_row_result(int result) {
  fake_state.single_row_result = result;
}

const fake_libpq_metrics *fake_libpq_get_metrics(void) {
  return &fake_state.metrics;
}

int fake_libpq_all_results_cleared(void) {
  size_t index;
  for (index = 0u; index < fake_state.result_count; ++index) {
    if (!fake_state.results[index].cleared)
      return 0;
  }
  return 1;
}

PGconn *PQconnectdb(const char *conninfo) {
  ++fake_state.metrics.connect_calls;
  fake_copy(fake_state.metrics.conninfo, sizeof(fake_state.metrics.conninfo),
            conninfo);
  return &fake_state.connection;
}

void PQfinish(PGconn *connection) {
  if (connection == &fake_state.connection)
    ++fake_state.metrics.finish_calls;
}

ConnStatusType PQstatus(const PGconn *connection) {
  return connection != NULL ? connection->status : CONNECTION_BAD;
}

char *PQerrorMessage(const PGconn *connection) {
  (void)connection;
  return (char *)fake_state.connection_error;
}

int PQsendQuery(PGconn *connection, const char *query) {
  (void)connection;
  ++fake_state.metrics.send_calls;
  fake_copy(fake_state.metrics.sql, sizeof(fake_state.metrics.sql), query);
  return fake_state.send_result;
}

static PGresult *fake_sync_result(void) {
  fake_state.sync_result.cleared = 0;
  return &fake_state.sync_result;
}

PGresult *PQexec(PGconn *connection, const char *query) {
  (void)connection;
  ++fake_state.metrics.exec_calls;
  fake_copy(fake_state.metrics.sql, sizeof(fake_state.metrics.sql), query);
  return fake_sync_result();
}

PGresult *PQprepare(PGconn *connection, const char *statement_name,
                    const char *query, int parameter_count,
                    const Oid *parameter_types) {
  int index;
  (void)connection;
  (void)statement_name;
  ++fake_state.metrics.prepare_calls;
  fake_state.metrics.parameter_count = parameter_count;
  fake_copy(fake_state.metrics.sql, sizeof(fake_state.metrics.sql), query);
  for (index = 0; index < parameter_count && index < 16; ++index)
    fake_state.metrics.parameter_types[index] = parameter_types[index];
  return fake_sync_result();
}

PGresult *PQexecPrepared(PGconn *connection, const char *statement_name,
                         int parameter_count, const char *const *values,
                         const int *lengths, const int *formats,
                         int result_format) {
  int index;
  (void)connection;
  (void)statement_name;
  (void)result_format;
  ++fake_state.metrics.exec_prepared_calls;
  fake_state.metrics.parameter_count = parameter_count;
  for (index = 0; index < parameter_count && index < 16; ++index) {
    size_t size = values[index] == NULL
                      ? 0u
                      : (formats[index] != 0 ? (size_t)lengths[index]
                                             : strlen(values[index]));
    if (size >= sizeof(fake_state.metrics.parameter_values[index]))
      size = sizeof(fake_state.metrics.parameter_values[index]) - 1u;
    fake_state.metrics.parameter_lengths[index] = lengths[index];
    fake_state.metrics.parameter_formats[index] = formats[index];
    fake_state.metrics.parameter_nulls[index] = values[index] == NULL;
    if (size != 0u)
      memcpy(fake_state.metrics.parameter_values[index], values[index], size);
    fake_state.metrics.parameter_values[index][size] = '\0';
  }
  return fake_sync_result();
}

int PQsendQueryParams(PGconn *connection, const char *query,
                      int parameter_count, const Oid *parameter_types,
                      const char *const *parameter_values,
                      const int *parameter_lengths,
                      const int *parameter_formats, int result_format) {
  (void)connection;
  (void)parameter_count;
  (void)parameter_types;
  (void)parameter_values;
  (void)parameter_lengths;
  (void)parameter_formats;
  (void)result_format;
  ++fake_state.metrics.send_params_calls;
  fake_copy(fake_state.metrics.sql, sizeof(fake_state.metrics.sql), query);
  return fake_state.send_result;
}

int PQsetSingleRowMode(PGconn *connection) {
  (void)connection;
  ++fake_state.metrics.single_row_calls;
  return fake_state.single_row_result;
}

PGresult *PQgetResult(PGconn *connection) {
  (void)connection;
  ++fake_state.metrics.get_result_calls;
  if (fake_state.result_index >= fake_state.result_count)
    return NULL;
  return &fake_state.results[fake_state.result_index++];
}

ExecStatusType PQresultStatus(const PGresult *result) {
  return result->status;
}

char *PQresultErrorMessage(const PGresult *result) {
  return (char *)(result->message != NULL ? result->message : "");
}

int PQntuples(const PGresult *result) {
  return result->status == PGRES_SINGLE_TUPLE ? 1 : 0;
}

int PQnfields(const PGresult *result) { return (int)result->column_count; }

int PQgetisnull(const PGresult *result, int row, int column) {
  (void)row;
  return result->nulls[column];
}

char *PQgetvalue(const PGresult *result, int row, int column) {
  (void)row;
  return (char *)result->values[column];
}

int PQgetlength(const PGresult *result, int row, int column) {
  (void)row;
  return result->lengths[column];
}

unsigned char *PQunescapeBytea(const unsigned char *text, size_t *retbuflen) {
  unsigned char *output;
  size_t input_size = strlen((const char *)text);
  size_t output_size;
  size_t index;
  if (input_size < 2u || text[0] != '\\' || text[1] != 'x' ||
      ((input_size - 2u) & 1u) != 0u)
    return NULL;
  output_size = (input_size - 2u) / 2u;
  output = (unsigned char *)malloc(output_size == 0u ? 1u : output_size);
  if (output == NULL) return NULL;
  for (index = 0u; index < output_size; ++index) {
    unsigned int value;
    if (sscanf((const char *)text + 2u + index * 2u, "%2x", &value) != 1) {
      free(output);
      return NULL;
    }
    output[index] = (unsigned char)value;
  }
  *retbuflen = output_size;
  return output;
}

void PQfreemem(void *pointer) { free(pointer); }

void PQclear(PGresult *result) {
  if (result != NULL && !result->cleared) {
    result->cleared = 1;
    ++fake_state.metrics.clear_calls;
  }
}

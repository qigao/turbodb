#include "fake_libpq.h"

#include <string.h>

enum { FAKE_LIBPQ_MAX_RESULTS = 8 };

struct pg_conn {
  ConnStatusType status;
};

struct pg_result {
  ExecStatusType status;
  const char *message;
  int cleared;
};

typedef struct fake_libpq_state {
  struct pg_conn connection;
  struct pg_result results[FAKE_LIBPQ_MAX_RESULTS];
  size_t result_count;
  size_t result_index;
  int send_result;
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

void PQclear(PGresult *result) {
  if (result != NULL && !result->cleared) {
    result->cleared = 1;
    ++fake_state.metrics.clear_calls;
  }
}

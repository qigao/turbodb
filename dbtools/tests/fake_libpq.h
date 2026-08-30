#ifndef TURBODB_DBTOOLS_TESTS_FAKE_LIBPQ_H
#define TURBODB_DBTOOLS_TESTS_FAKE_LIBPQ_H

#include <stddef.h>

#include <libpq-fe.h>

typedef struct fake_libpq_metrics {
  int connect_calls;
  int finish_calls;
  int send_calls;
  int get_result_calls;
  int clear_calls;
  char conninfo[256];
  char sql[512];
} fake_libpq_metrics;

void fake_libpq_reset(void);
void fake_libpq_set_connect_failure(const char *message);
void fake_libpq_set_send_failure(const char *message);
void fake_libpq_set_connection_error(const char *message);
void fake_libpq_set_results(const ExecStatusType *statuses,
                            const char *const *messages, size_t count);
const fake_libpq_metrics *fake_libpq_get_metrics(void);
int fake_libpq_all_results_cleared(void);

#endif

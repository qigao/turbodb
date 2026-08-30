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
  int exec_calls;
  int prepare_calls;
  int exec_prepared_calls;
  int send_params_calls;
  int single_row_calls;
  int parameter_count;
  char conninfo[256];
  char sql[512];
  Oid parameter_types[16];
  int parameter_lengths[16];
  int parameter_formats[16];
  int parameter_nulls[16];
  unsigned char parameter_values[16][128];
} fake_libpq_metrics;

void fake_libpq_reset(void);
void fake_libpq_set_connect_failure(const char *message);
void fake_libpq_set_send_failure(const char *message);
void fake_libpq_set_connection_error(const char *message);
void fake_libpq_set_results(const ExecStatusType *statuses,
                            const char *const *messages, size_t count);
void fake_libpq_set_result_row(size_t result_index,
                               const char *const *values,
                               const int *lengths, const int *nulls,
                               size_t column_count);
void fake_libpq_set_sync_result(ExecStatusType status, const char *message);
void fake_libpq_set_single_row_result(int result);
const fake_libpq_metrics *fake_libpq_get_metrics(void);
int fake_libpq_all_results_cleared(void);

#endif

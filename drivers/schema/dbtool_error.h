#ifndef TURBODB_DBTOOL_ERROR_H
#define TURBODB_DBTOOL_ERROR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  DBTOOL_ERROR_STAGE_CAPACITY = 32u,
  DBTOOL_ERROR_MESSAGE_CAPACITY = 384u
};

typedef enum dbtool_status {
  DBTOOL_STATUS_OK = 0,
  DBTOOL_STATUS_HELP,
  DBTOOL_STATUS_INVALID_ARGUMENT,
  DBTOOL_STATUS_LIMIT_EXCEEDED,
  DBTOOL_STATUS_FILE_ERROR,
  DBTOOL_STATUS_CONNECTION_ERROR,
  DBTOOL_STATUS_SQL_ERROR,
  DBTOOL_STATUS_UNSUPPORTED,
  DBTOOL_STATUS_OUT_OF_MEMORY,
  DBTOOL_STATUS_INTERNAL_ERROR
} dbtool_status;

typedef struct dbtool_error {
  dbtool_status status;
  int native_code;
  char stage[DBTOOL_ERROR_STAGE_CAPACITY];
  char message[DBTOOL_ERROR_MESSAGE_CAPACITY];
} dbtool_error;

#define DBTOOL_ERROR_INIT                                                     \
  { DBTOOL_STATUS_OK, 0, {0}, {0} }

void dbtool_error_init(dbtool_error *error);
void dbtool_error_set(dbtool_error *error, dbtool_status status,
                      const char *stage, int native_code,
                      const char *message);
const char *dbtool_status_name(dbtool_status status);
int dbtool_status_exit_code(dbtool_status status);

#ifdef __cplusplus
}
#endif

#endif

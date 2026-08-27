#ifndef REDIS_REPLY_H
#define REDIS_REPLY_H

#include "redis_export.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct redis_reply_s redis_reply_t;

typedef enum redis_reply_type {
  REDIS_REPLY_STRING,
  REDIS_REPLY_ERROR,
  REDIS_REPLY_INTEGER,
  REDIS_REPLY_BULK_STRING,
  REDIS_REPLY_ARRAY,
  REDIS_REPLY_NULL
} redis_reply_type_t;

struct redis_reply_s {
  redis_reply_type_t type;
  int64_t integer;
  char *str;
  size_t len;
  redis_reply_t **elements;
  size_t element_count;
};

typedef enum redis_command_outcome {
  REDIS_COMMAND_NOT_SENT,
  REDIS_COMMAND_SEND_UNCERTAIN,
  REDIS_COMMAND_REPLY_UNKNOWN,
  REDIS_COMMAND_REPLIED
} redis_command_outcome_t;

typedef enum redis_server_error {
  REDIS_SERVER_ERROR_NONE,
  REDIS_SERVER_ERROR_ERR,
  REDIS_SERVER_ERROR_BUSY_GROUP,
  REDIS_SERVER_ERROR_NO_GROUP,
  REDIS_SERVER_ERROR_WRONG_TYPE,
  REDIS_SERVER_ERROR_NO_AUTH,
  REDIS_SERVER_ERROR_MOVED,
  REDIS_SERVER_ERROR_ASK,
  REDIS_SERVER_ERROR_TRY_AGAIN,
  REDIS_SERVER_ERROR_CLUSTER_DOWN,
  REDIS_SERVER_ERROR_READ_ONLY,
  REDIS_SERVER_ERROR_NO_SCRIPT,
  REDIS_SERVER_ERROR_LOADING,
  REDIS_SERVER_ERROR_OOM,
  REDIS_SERVER_ERROR_EXEC_ABORT,
  REDIS_SERVER_ERROR_MASTER_DOWN,
  REDIS_SERVER_ERROR_MISCONF,
  REDIS_SERVER_ERROR_UNKNOWN
} redis_server_error_t;

REDIS_API void redis_reply_free(redis_reply_t *reply);
REDIS_API redis_server_error_t redis_server_error_classify(
    const redis_reply_t *reply);

#ifdef __cplusplus
}
#endif

#endif

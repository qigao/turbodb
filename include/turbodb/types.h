#ifndef TURBODB_TYPES_H
#define TURBODB_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <tstr.h>

#define TURBODB_TYPES_ABI_VERSION UINT32_C(1)

/* Shared immutable DTOs. Historical ORM struct tags intentionally remain for
 * source compatibility; this header has no ORM runtime or flow dependency.
 * TEXT/BLOB borrow their bytes, reserved is zero, and the kind determines the
 * active union field. Lifetimes are defined by the consuming API. */
#define TURBODB_ERROR_MESSAGE_CAPACITY UINT32_C(512)
#define TURBODB_DEFAULT_MAX_PARAMETERS UINT32_C(256)
#define TURBODB_DEFAULT_MAX_COLUMNS UINT32_C(256)
#define TURBODB_DEFAULT_MAX_QUERY_BYTES UINT64_C(65536)
#define TURBODB_DEFAULT_MAX_PARAMETER_BYTES UINT64_C(1048576)
#define TURBODB_DEFAULT_MAX_RESULT_ROWS UINT64_C(10000)
#define TURBODB_DEFAULT_MAX_RESULT_BYTES UINT64_C(16777216)

typedef vstr turbodb_string_view_t;

typedef int32_t turbodb_status_t;
enum {
  TURBODB_STATUS_OK = 0,
  TURBODB_STATUS_INVALID_ARGUMENT = 1,
  TURBODB_STATUS_ABI_MISMATCH = 2,
  TURBODB_STATUS_OUT_OF_MEMORY = 3,
  TURBODB_STATUS_CONNECTION_ERROR = 4,
  TURBODB_STATUS_SQL_ERROR = 5,
  TURBODB_STATUS_TYPE_ERROR = 6,
  TURBODB_STATUS_OUT_OF_RANGE = 7,
  TURBODB_STATUS_LIMIT_EXCEEDED = 8,
  TURBODB_STATUS_INVALID_STATE = 9,
  TURBODB_STATUS_NULL_VALUE = 10,
  TURBODB_STATUS_INTERNAL_ERROR = 11,
  TURBODB_STATUS_BUSY = 12,
  TURBODB_STATUS_UNSUPPORTED = 13,
  TURBODB_STATUS_DATASTORE_ERROR = 14,
  TURBODB_STATUS_CONSTRAINT = 15,
  /* A dispatched commit may have reached the datastore, but acknowledgement
   * was lost. The transaction/connection is quarantined and must not replay. */
  TURBODB_STATUS_COMMIT_UNKNOWN = 16,
  /* Final native cleanup failed. The owner is quarantined; this is not a
   * retryable close result and resources may remain pinned until process exit. */
  TURBODB_STATUS_CLEANUP_FAILED = 17,
};

typedef int32_t turbodb_value_kind_t;
enum {
  TURBODB_VALUE_NULL = 0,
  TURBODB_VALUE_INT64 = 1,
  TURBODB_VALUE_UINT64 = 2,
  TURBODB_VALUE_DOUBLE = 3,
  TURBODB_VALUE_BOOLEAN = 4,
  TURBODB_VALUE_TEXT = 5,
  TURBODB_VALUE_BLOB = 6
};

typedef struct orm_blob {
  const void *data;
  size_t size;
} turbodb_blob_t;

typedef struct orm_option {
  turbodb_string_view_t keyword;
  turbodb_string_view_t value;
} turbodb_option_t;

typedef struct orm_error {
  uint32_t struct_size;
  turbodb_status_t status;
  char message[TURBODB_ERROR_MESSAGE_CAPACITY];
} turbodb_error_t;

typedef union orm_value_data {
  int64_t int64_value;
  uint64_t uint64_value;
  double double_value;
  uint8_t boolean_value;
  turbodb_string_view_t text_value;
  turbodb_blob_t blob_value;
} turbodb_value_data_t;

typedef struct orm_value {
  turbodb_value_kind_t kind;
  uint32_t reserved;
  turbodb_value_data_t data;
} turbodb_value_t;

static inline turbodb_string_view_t turbodb_view(const char *text) {
  return vstr_from_cstr(text);
}
static inline turbodb_string_view_t turbodb_view_tstr(tstr text) {
  return tstr_to_v(text);
}
static inline turbodb_value_t turbodb_null(void) {
  turbodb_value_t value = {TURBODB_VALUE_NULL, 0u, {0}};
  return value;
}
static inline turbodb_value_t turbodb_i64(int64_t input) {
  turbodb_value_t value = {TURBODB_VALUE_INT64, 0u, {0}};
  value.data.int64_value = input;
  return value;
}
static inline turbodb_value_t turbodb_u64(uint64_t input) {
  turbodb_value_t value = {TURBODB_VALUE_UINT64, 0u, {0}};
  value.data.uint64_value = input;
  return value;
}
static inline turbodb_value_t turbodb_f64(double input) {
  turbodb_value_t value = {TURBODB_VALUE_DOUBLE, 0u, {0}};
  value.data.double_value = input;
  return value;
}
static inline turbodb_value_t turbodb_bool(int input) {
  turbodb_value_t value = {TURBODB_VALUE_BOOLEAN, 0u, {0}};
  value.data.boolean_value = input != 0;
  return value;
}
static inline turbodb_value_t turbodb_text_v(vstr input) {
  turbodb_value_t value = {TURBODB_VALUE_TEXT, 0u, {0}};
  value.data.text_value = input;
  return value;
}
static inline turbodb_value_t turbodb_text(const char *input) {
  return turbodb_text_v(turbodb_view(input));
}
static inline turbodb_value_t turbodb_text_tstr(tstr input) {
  return turbodb_text_v(tstr_to_v(input));
}
static inline turbodb_value_t turbodb_blob_v(turbodb_blob_t input) {
  turbodb_value_t value = {TURBODB_VALUE_BLOB, 0u, {0}};
  value.data.blob_value = input;
  return value;
}
static inline turbodb_value_t turbodb_blob(const void *data, size_t size) {
  const turbodb_blob_t blob = {data, size};
  return turbodb_blob_v(blob);
}
static inline void turbodb_error_init(turbodb_error_t *error) {
  if (!error) return;
  memset(error, 0, sizeof(*error));
  error->struct_size = sizeof(*error);
}

#endif

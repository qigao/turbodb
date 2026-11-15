#ifndef TURBODB_MYSQL_PARAMETERS_H
#define TURBODB_MYSQL_PARAMETERS_H

#include "wire/codec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mysql_parameter_style_t {
  MYSQL_PARAMETER_STYLE_NONE = 0,
  MYSQL_PARAMETER_STYLE_NATIVE = 1,
  MYSQL_PARAMETER_STYLE_PORTABLE = 2
} mysql_parameter_style_t;

typedef struct mysql_parameter_lower_options_t {
  bool ansi_quotes;
  bool no_backslash_escapes;
  uint32_t max_parameters;
} mysql_parameter_lower_options_t;

typedef struct mysql_parameter_lower_result_t {
  mysql_parameter_style_t style;
  size_t bind_count;
} mysql_parameter_lower_result_t;

mysql_wire_status_t mysql_parameter_lower(
    const uint8_t *sql, size_t sql_size,
    const mysql_parameter_lower_options_t *options,
    uint8_t *out_sql, size_t out_capacity, size_t *out_size,
    uint32_t *bind_order, size_t bind_order_capacity,
    mysql_parameter_lower_result_t *result);

bool mysql_sql_reject_managed_transaction(
    const uint8_t *sql, size_t sql_size);

#ifdef __cplusplus
}
#endif

#endif

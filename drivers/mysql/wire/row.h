#ifndef TURBODB_MYSQL_WIRE_ROW_H
#define TURBODB_MYSQL_WIRE_ROW_H

#include "codec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYSQL_COLUMN_FLAG_UNSIGNED UINT16_C(0x0020)

#define MYSQL_FIELD_TYPE_DECIMAL UINT8_C(0x00)
#define MYSQL_FIELD_TYPE_TINY UINT8_C(0x01)
#define MYSQL_FIELD_TYPE_SHORT UINT8_C(0x02)
#define MYSQL_FIELD_TYPE_LONG UINT8_C(0x03)
#define MYSQL_FIELD_TYPE_FLOAT UINT8_C(0x04)
#define MYSQL_FIELD_TYPE_DOUBLE UINT8_C(0x05)
#define MYSQL_FIELD_TYPE_NULL UINT8_C(0x06)
#define MYSQL_FIELD_TYPE_TIMESTAMP UINT8_C(0x07)
#define MYSQL_FIELD_TYPE_LONGLONG UINT8_C(0x08)
#define MYSQL_FIELD_TYPE_INT24 UINT8_C(0x09)
#define MYSQL_FIELD_TYPE_DATE UINT8_C(0x0a)
#define MYSQL_FIELD_TYPE_TIME UINT8_C(0x0b)
#define MYSQL_FIELD_TYPE_DATETIME UINT8_C(0x0c)
#define MYSQL_FIELD_TYPE_YEAR UINT8_C(0x0d)
#define MYSQL_FIELD_TYPE_VARCHAR UINT8_C(0x0f)
#define MYSQL_FIELD_TYPE_BIT UINT8_C(0x10)
#define MYSQL_FIELD_TYPE_JSON UINT8_C(0xf5)
#define MYSQL_FIELD_TYPE_NEWDECIMAL UINT8_C(0xf6)
#define MYSQL_FIELD_TYPE_ENUM UINT8_C(0xf7)
#define MYSQL_FIELD_TYPE_SET UINT8_C(0xf8)
#define MYSQL_FIELD_TYPE_TINY_BLOB UINT8_C(0xf9)
#define MYSQL_FIELD_TYPE_MEDIUM_BLOB UINT8_C(0xfa)
#define MYSQL_FIELD_TYPE_LONG_BLOB UINT8_C(0xfb)
#define MYSQL_FIELD_TYPE_BLOB UINT8_C(0xfc)
#define MYSQL_FIELD_TYPE_VAR_STRING UINT8_C(0xfd)
#define MYSQL_FIELD_TYPE_STRING UINT8_C(0xfe)
#define MYSQL_FIELD_TYPE_GEOMETRY UINT8_C(0xff)

typedef struct mysql_column_definition_t {
  mysql_wire_bytes_t catalog;
  mysql_wire_bytes_t schema;
  mysql_wire_bytes_t table;
  mysql_wire_bytes_t org_table;
  mysql_wire_bytes_t name;
  mysql_wire_bytes_t org_name;
  uint16_t character_set;
  uint32_t column_length;
  uint8_t type;
  uint16_t flags;
  uint8_t decimals;
} mysql_column_definition_t;

typedef struct mysql_binary_datetime_t {
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  uint32_t microsecond;
  uint8_t wire_length;
} mysql_binary_datetime_t;

typedef struct mysql_binary_time_t {
  bool negative;
  uint32_t days;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  uint32_t microsecond;
  uint8_t wire_length;
} mysql_binary_time_t;

typedef enum mysql_binary_value_kind_t {
  MYSQL_BINARY_VALUE_NULL = 0,
  MYSQL_BINARY_VALUE_SINT64,
  MYSQL_BINARY_VALUE_UINT64,
  MYSQL_BINARY_VALUE_DOUBLE,
  MYSQL_BINARY_VALUE_BYTES,
  MYSQL_BINARY_VALUE_DATETIME,
  MYSQL_BINARY_VALUE_TIME
} mysql_binary_value_kind_t;

typedef struct mysql_binary_value_t {
  mysql_binary_value_kind_t kind;
  uint8_t mysql_type;
  union {
    int64_t sint64_value;
    uint64_t uint64_value;
    double double_value;
    mysql_wire_bytes_t bytes;
    mysql_binary_datetime_t datetime_value;
    mysql_binary_time_t time_value;
  } data;
} mysql_binary_value_t;

mysql_wire_status_t mysql_wire_decode_column_definition41(
    const uint8_t *payload, size_t payload_size,
    mysql_column_definition_t *out);

mysql_wire_status_t mysql_wire_decode_binary_row(
    const uint8_t *payload, size_t payload_size,
    const mysql_column_definition_t *columns, size_t column_count,
    mysql_binary_value_t *values, size_t value_capacity);

#ifdef __cplusplus
}
#endif

#endif

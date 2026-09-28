#ifndef TURBODB_ORM_MYSQL_WIRE_STATEMENT_H
#define TURBODB_ORM_MYSQL_WIRE_STATEMENT_H

#include "codec.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYSQL_COM_STMT_PREPARE UINT8_C(0x16)
#define MYSQL_COM_STMT_EXECUTE UINT8_C(0x17)
#define MYSQL_COM_STMT_CLOSE UINT8_C(0x19)
#define MYSQL_COM_STMT_RESET UINT8_C(0x1a)

#define MYSQL_TYPE_TINY UINT8_C(0x01)
#define MYSQL_TYPE_DOUBLE UINT8_C(0x05)
#define MYSQL_TYPE_NULL UINT8_C(0x06)
#define MYSQL_TYPE_LONGLONG UINT8_C(0x08)
#define MYSQL_TYPE_BLOB UINT8_C(0xfc)
#define MYSQL_TYPE_VAR_STRING UINT8_C(0xfd)
#define MYSQL_TYPE_UNSIGNED_FLAG UINT8_C(0x80)

typedef enum mysql_stmt_value_kind_t {
  MYSQL_STMT_VALUE_NULL = 0,
  MYSQL_STMT_VALUE_SINT64,
  MYSQL_STMT_VALUE_UINT64,
  MYSQL_STMT_VALUE_DOUBLE,
  MYSQL_STMT_VALUE_BOOL,
  MYSQL_STMT_VALUE_TEXT,
  MYSQL_STMT_VALUE_BLOB
} mysql_stmt_value_kind_t;

typedef struct mysql_stmt_bytes_t {
  const uint8_t *data;
  size_t size;
} mysql_stmt_bytes_t;

typedef struct mysql_stmt_value_t {
  mysql_stmt_value_kind_t kind;
  union {
    int64_t sint64_value;
    uint64_t uint64_value;
    double double_value;
    uint8_t bool_value;
    mysql_stmt_bytes_t bytes;
  } data;
} mysql_stmt_value_t;

typedef struct mysql_stmt_prepare_ok_t {
  uint32_t statement_id;
  uint16_t column_count;
  uint16_t parameter_count;
  uint16_t warning_count;
} mysql_stmt_prepare_ok_t;

mysql_wire_status_t mysql_wire_build_stmt_prepare(
    const uint8_t *sql, size_t sql_size,
    uint8_t *out, size_t out_capacity, size_t *out_size);

mysql_wire_status_t mysql_wire_decode_stmt_prepare_ok(
    const uint8_t *payload, size_t payload_size,
    mysql_stmt_prepare_ok_t *out);

mysql_wire_status_t mysql_wire_build_stmt_close(
    uint32_t statement_id,
    uint8_t *out, size_t out_capacity, size_t *out_size);

mysql_wire_status_t mysql_wire_build_stmt_reset(
    uint32_t statement_id,
    uint8_t *out, size_t out_capacity, size_t *out_size);

mysql_wire_status_t mysql_wire_build_stmt_execute(
    uint32_t statement_id,
    const mysql_stmt_value_t *values, size_t value_count,
    uint8_t *out, size_t out_capacity, size_t *out_size);

#ifdef __cplusplus
}
#endif

#endif

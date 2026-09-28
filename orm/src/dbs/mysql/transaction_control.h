#ifndef TURBODB_ORM_MYSQL_TRANSACTION_CONTROL_H
#define TURBODB_ORM_MYSQL_TRANSACTION_CONTROL_H

#include "wire/codec.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mysql_savepoint_control_t {
  MYSQL_SAVEPOINT_CREATE = 0,
  MYSQL_SAVEPOINT_ROLLBACK_TO,
  MYSQL_SAVEPOINT_RELEASE
} mysql_savepoint_control_t;

mysql_wire_status_t mysql_transaction_build_savepoint_control(
    mysql_savepoint_control_t control,
    const uint8_t *name, size_t name_size,
    uint8_t *out, size_t out_capacity, size_t *out_size);

#ifdef __cplusplus
}
#endif

#endif

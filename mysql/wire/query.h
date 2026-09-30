#ifndef TURBODB_MYSQL_WIRE_QUERY_H
#define TURBODB_MYSQL_WIRE_QUERY_H

#include "codec.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYSQL_COM_QUIT UINT8_C(0x01)
#define MYSQL_COM_QUERY UINT8_C(0x03)

mysql_wire_status_t mysql_wire_build_quit(
    uint8_t *out, size_t out_capacity, size_t *out_size);

mysql_wire_status_t mysql_wire_build_query(
    const uint8_t *sql, size_t sql_size,
    uint8_t *out, size_t out_capacity, size_t *out_size);

#ifdef __cplusplus
}
#endif

#endif

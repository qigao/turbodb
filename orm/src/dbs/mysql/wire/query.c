#include "query.h"

#include <string.h>

mysql_wire_status_t mysql_wire_build_query(
    const uint8_t *sql, size_t sql_size,
    uint8_t *out, size_t out_capacity, size_t *out_size) {
  if (out_size != NULL)
    *out_size = 0u;
  if (sql == NULL || sql_size == 0u ||
      out == NULL || out_size == NULL)
    return MYSQL_WIRE_STATUS_INVALID;
  if (sql_size > SIZE_MAX - 1u ||
      out_capacity < sql_size + 1u)
    return MYSQL_WIRE_STATUS_LIMIT;

  out[0] = MYSQL_COM_QUERY;
  memcpy(out + 1u, sql, sql_size);
  *out_size = sql_size + 1u;
  return MYSQL_WIRE_STATUS_OK;
}

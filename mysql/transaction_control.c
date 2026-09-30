#include "transaction_control.h"

#include <string.h>

enum { MYSQL_SAVEPOINT_NAME_MAX = 64u };

static int mysql_savepoint_identifier_start(uint8_t value) {
  return (value >= (uint8_t)'a' && value <= (uint8_t)'z') ||
         (value >= (uint8_t)'A' && value <= (uint8_t)'Z') ||
         value == (uint8_t)'_';
}

static int mysql_savepoint_identifier_continue(uint8_t value) {
  return mysql_savepoint_identifier_start(value) ||
         (value >= (uint8_t)'0' && value <= (uint8_t)'9');
}

static mysql_wire_status_t mysql_savepoint_prefix(
    mysql_savepoint_control_t control,
    const char **out_prefix, size_t *out_size) {
  static const char create_prefix[] = "SAVEPOINT \x60";
  static const char rollback_prefix[] = "ROLLBACK TO SAVEPOINT \x60";
  static const char release_prefix[] = "RELEASE SAVEPOINT \x60";

  if (out_prefix == NULL || out_size == NULL)
    return MYSQL_WIRE_STATUS_INVALID;

  switch (control) {
    case MYSQL_SAVEPOINT_CREATE:
      *out_prefix = create_prefix;
      *out_size = sizeof(create_prefix) - 1u;
      return MYSQL_WIRE_STATUS_OK;
    case MYSQL_SAVEPOINT_ROLLBACK_TO:
      *out_prefix = rollback_prefix;
      *out_size = sizeof(rollback_prefix) - 1u;
      return MYSQL_WIRE_STATUS_OK;
    case MYSQL_SAVEPOINT_RELEASE:
      *out_prefix = release_prefix;
      *out_size = sizeof(release_prefix) - 1u;
      return MYSQL_WIRE_STATUS_OK;
    default:
      *out_prefix = NULL;
      *out_size = 0u;
      return MYSQL_WIRE_STATUS_INVALID;
  }
}

mysql_wire_status_t mysql_transaction_build_savepoint_control(
    mysql_savepoint_control_t control,
    const uint8_t *name, size_t name_size,
    uint8_t *out, size_t out_capacity, size_t *out_size) {
  const char *prefix = NULL;
  size_t prefix_size = 0u;
  size_t required;
  size_t i;
  mysql_wire_status_t status;

  if (out_size != NULL)
    *out_size = 0u;
  if (name == NULL || name_size == 0u ||
      name_size > MYSQL_SAVEPOINT_NAME_MAX ||
      out == NULL || out_size == NULL)
    return MYSQL_WIRE_STATUS_INVALID;
  if (!mysql_savepoint_identifier_start(name[0]))
    return MYSQL_WIRE_STATUS_INVALID;
  for (i = 1u; i < name_size; ++i) {
    if (!mysql_savepoint_identifier_continue(name[i]))
      return MYSQL_WIRE_STATUS_INVALID;
  }

  status = mysql_savepoint_prefix(control, &prefix, &prefix_size);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;

  if (prefix_size > SIZE_MAX - name_size - 1u)
    return MYSQL_WIRE_STATUS_LIMIT;
  required = prefix_size + name_size + 1u;
  if (required > out_capacity)
    return MYSQL_WIRE_STATUS_LIMIT;

  memcpy(out, prefix, prefix_size);
  memcpy(out + prefix_size, name, name_size);
  out[prefix_size + name_size] = UINT8_C(0x60);
  *out_size = required;
  return MYSQL_WIRE_STATUS_OK;
}

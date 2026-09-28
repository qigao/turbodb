#include "orm_driver_contract.h"
#include <orm_driver_ops.h>

#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(orm_driver_header_v1) == ORM_DRIVER_HEADER_BYTES,
               "driver header must remain an eight-byte prefix");
_Static_assert(offsetof(orm_driver_header_v1, struct_size) == 0u,
               "driver size must be the first field");
_Static_assert(offsetof(orm_driver_header_v1, abi_version) == 4u,
               "driver version must be the second field");

orm_status_t ORM_DRIVER_CALL orm_driver_check_prefix(
    const void *buffer, uint32_t buffer_bytes, uint32_t expected_version,
    uint32_t required_bytes, uint32_t *out_struct_bytes) {
  orm_driver_header_v1 header;
  if (out_struct_bytes == NULL)
    return ORM_STATUS_INVALID_ARGUMENT;
  *out_struct_bytes = 0u;
  if (buffer == NULL || required_bytes < ORM_DRIVER_HEADER_BYTES)
    return ORM_STATUS_INVALID_ARGUMENT;
  if (buffer_bytes < ORM_DRIVER_HEADER_BYTES)
    return ORM_STATUS_ABI_MISMATCH;

  /* The supplied span, not the untrusted size field, admits this read. */
  memcpy(&header, buffer, sizeof(header));
  if (header.abi_version != expected_version ||
      header.struct_size < required_bytes || header.struct_size > buffer_bytes)
    return ORM_STATUS_ABI_MISMATCH;
  if (header.struct_size > ORM_DRIVER_DESCRIPTOR_MAX_BYTES)
    return ORM_STATUS_LIMIT_EXCEEDED;

  *out_struct_bytes = header.struct_size;
  return ORM_STATUS_OK;
}

orm_status_t ORM_DRIVER_CALL orm_driver_check_bytes(orm_driver_bytes_v1 value,
                                                   uint64_t max_bytes) {
  if (value.data == NULL && value.size != 0u)
    return ORM_STATUS_INVALID_ARGUMENT;
  if (value.size > max_bytes || value.size > (uint64_t)SIZE_MAX)
    return ORM_STATUS_LIMIT_EXCEEDED;
  return ORM_STATUS_OK;
}

#define DRIVER_FIELD_END(T, field) \
  ((uint32_t)(offsetof(T, field) + sizeof(((T *)0)->field)))
#define DRIVER_SAVEPOINT_CALLBACK_COUNT 3u
#define DRIVER_ROW_CAPS (ORM_DRIVER_CAP_SELECT | ORM_DRIVER_CAP_RAW_SQL)
#define DRIVER_COMMAND_CAPS (ORM_DRIVER_CAP_INSERT | ORM_DRIVER_CAP_UPDATE | \
                             ORM_DRIVER_CAP_DELETE | ORM_DRIVER_CAP_RAW_SQL)

/* Copy only the admitted prefix, never an optional or partially present field. */
static orm_status_t driver_copy_prefix(const void *source, uint32_t bytes,
    uint32_t required, void *out, uint32_t *declared) {
  orm_status_t status = orm_driver_check_prefix(source, bytes,
      ORM_DRIVER_ABI_VERSION, required, declared);
  if (status != ORM_STATUS_OK)
    return status;
  memcpy(out, source, required);
  return ORM_STATUS_OK;
}

static orm_status_t driver_copy_table(orm_driver_table_v1 table,
    uint32_t required, void *out, uint32_t *declared) {
  if (table.reserved != 0u)
    return ORM_STATUS_INVALID_ARGUMENT;
  return driver_copy_prefix(table.data, table.bytes, required, out, declared);
}

static orm_status_t driver_check_capabilities(uint64_t caps) {
  if ((caps & ~ORM_DRIVER_CAP_KNOWN_MASK) != 0u)
    return ORM_STATUS_UNSUPPORTED;
  if ((caps & (ORM_DRIVER_CAP_SAVEPOINT | ORM_DRIVER_CAP_ISOLATION_MASK)) != 0u &&
      (caps & ORM_DRIVER_CAP_TRANSACTION) == 0u)
    return ORM_STATUS_ABI_MISMATCH;
  if ((caps & ORM_DRIVER_CAP_INCREMENTAL_ROWS) != 0u &&
      (caps & DRIVER_ROW_CAPS) == 0u)
    return ORM_STATUS_ABI_MISMATCH;
  return ORM_STATUS_OK;
}

static orm_status_t driver_check_connection_ops(orm_driver_table_v1 table,
                                                uint64_t caps) {
  orm_driver_connection_ops_v1 ops = {0};
  uint32_t declared = 0u;
  uint32_t required = DRIVER_FIELD_END(orm_driver_connection_ops_v1, destroy);
  orm_status_t status;
  if ((caps & DRIVER_ROW_CAPS) != 0u)
    required = DRIVER_FIELD_END(orm_driver_connection_ops_v1, open_cursor);
  if ((caps & DRIVER_COMMAND_CAPS) != 0u)
    required = DRIVER_FIELD_END(orm_driver_connection_ops_v1, execute_command);
  if ((caps & ORM_DRIVER_CAP_TRANSACTION) != 0u)
    required = DRIVER_FIELD_END(orm_driver_connection_ops_v1, begin_transaction);
  status = driver_copy_table(table, required, &ops, &declared);
  if (status != ORM_STATUS_OK)
    return status;
  if (ops.destroy == NULL ||
      ((caps & DRIVER_ROW_CAPS) != 0u && ops.open_cursor == NULL) ||
      ((caps & DRIVER_COMMAND_CAPS) != 0u && ops.execute_command == NULL) ||
      ((caps & ORM_DRIVER_CAP_TRANSACTION) != 0u && ops.begin_transaction == NULL))
    return ORM_STATUS_ABI_MISMATCH;
  return ORM_STATUS_OK;
}

/* Optional callback bytes are copied only when that whole field is present. */
#define DRIVER_OPTIONAL_TX(field) do { \
  if (declared >= DRIVER_FIELD_END(orm_driver_transaction_ops_v1, field)) \
    memcpy(&ops.field, (const unsigned char *)table.data + \
        offsetof(orm_driver_transaction_ops_v1, field), sizeof(ops.field)); \
} while (0)

static orm_status_t driver_check_transaction_ops(orm_driver_table_v1 table,
                                                 uint64_t caps) {
  orm_driver_transaction_ops_v1 ops = {0};
  uint32_t declared = 0u;
  orm_status_t status = driver_copy_table(table,
      DRIVER_FIELD_END(orm_driver_transaction_ops_v1, rollback), &ops, &declared);
  unsigned int savepoint_count;
  if (status != ORM_STATUS_OK)
    return status;
  if (ops.destroy == NULL || ops.commit == NULL || ops.rollback == NULL ||
      ((caps & DRIVER_ROW_CAPS) != 0u && ops.open_cursor == NULL) ||
      ((caps & DRIVER_COMMAND_CAPS) != 0u && ops.execute_command == NULL))
    return ORM_STATUS_ABI_MISMATCH;
  DRIVER_OPTIONAL_TX(savepoint);
  DRIVER_OPTIONAL_TX(rollback_to_savepoint);
  DRIVER_OPTIONAL_TX(release_savepoint);
  savepoint_count = (unsigned int)(ops.savepoint != NULL) +
      (unsigned int)(ops.rollback_to_savepoint != NULL) +
      (unsigned int)(ops.release_savepoint != NULL);
  if ((savepoint_count != 0u && savepoint_count != DRIVER_SAVEPOINT_CALLBACK_COUNT) ||
      ((caps & ORM_DRIVER_CAP_SAVEPOINT) != 0u && savepoint_count != DRIVER_SAVEPOINT_CALLBACK_COUNT))
    return ORM_STATUS_ABI_MISMATCH;
  return ORM_STATUS_OK;
}
#undef DRIVER_OPTIONAL_TX

static orm_status_t driver_check_cursor_ops(orm_driver_table_v1 table) {
  orm_driver_cursor_ops_v1 ops = {0};
  uint32_t declared = 0u;
  orm_status_t status = driver_copy_table(table,
      DRIVER_FIELD_END(orm_driver_cursor_ops_v1, destroy), &ops, &declared);
  if (status != ORM_STATUS_OK)
    return status;
  if (ops.next == NULL || ops.cancel == NULL || ops.destroy == NULL)
    return ORM_STATUS_ABI_MISMATCH;
  return ORM_STATUS_OK;
}

static orm_status_t driver_result(orm_error_t *error, orm_status_t status) {
  const char *message = "";
  if (error == NULL)
    return status;
  switch (status) {
  case ORM_STATUS_OK: break;
  case ORM_STATUS_ABI_MISMATCH: message = "driver ABI or callback contract mismatch"; break;
  case ORM_STATUS_LIMIT_EXCEEDED: message = "driver descriptor budget exceeded"; break;
  case ORM_STATUS_UNSUPPORTED: message = "unknown driver capability or execution model"; break;
  default: message = "invalid driver descriptor argument"; break;
  }
  /* Error storage is caller-owned and disjoint from immutable input buffers. */
  memset(error, 0, sizeof(*error));
  error->struct_size = (uint32_t)sizeof(*error);
  error->status = status;
  memcpy(error->message, message, strlen(message));
  return status;
}

orm_status_t ORM_DRIVER_CALL orm_driver_validate_connection_v1(
    const void *object, uint32_t bytes, uint64_t capabilities, orm_error_t *error) {
  orm_driver_connection_v1 connection = {0};
  uint32_t declared = 0u;
  orm_status_t status = driver_copy_prefix(object, bytes,
      DRIVER_FIELD_END(orm_driver_connection_v1, ops), &connection, &declared);
  if (status == ORM_STATUS_OK)
    status = driver_check_capabilities(capabilities);
  if (status == ORM_STATUS_OK)
    status = driver_check_connection_ops(connection.ops, capabilities);
  return driver_result(error, status);
}

orm_status_t ORM_DRIVER_CALL orm_driver_validate_transaction_v1(
    const void *object, uint32_t bytes, uint64_t capabilities, orm_error_t *error) {
  orm_driver_transaction_v1 transaction = {0};
  uint32_t declared = 0u;
  orm_status_t status = driver_copy_prefix(object, bytes,
      DRIVER_FIELD_END(orm_driver_transaction_v1, ops), &transaction, &declared);
  if (status == ORM_STATUS_OK)
    status = driver_check_capabilities(capabilities);
  if (status == ORM_STATUS_OK && (capabilities & ORM_DRIVER_CAP_TRANSACTION) == 0u)
    status = ORM_STATUS_ABI_MISMATCH;
  if (status == ORM_STATUS_OK)
    status = driver_check_transaction_ops(transaction.ops, capabilities);
  return driver_result(error, status);
}

orm_status_t ORM_DRIVER_CALL orm_driver_validate_cursor_v1(
    const void *object, uint32_t bytes, uint64_t capabilities, orm_error_t *error) {
  orm_driver_cursor_v1 cursor = {0};
  uint32_t declared = 0u;
  orm_status_t status = driver_copy_prefix(object, bytes,
      DRIVER_FIELD_END(orm_driver_cursor_v1, ops), &cursor, &declared);
  if (status == ORM_STATUS_OK)
    status = driver_check_capabilities(capabilities);
  if (status == ORM_STATUS_OK && (capabilities & DRIVER_ROW_CAPS) == 0u)
    status = ORM_STATUS_ABI_MISMATCH;
  if (status == ORM_STATUS_OK)
    status = driver_check_cursor_ops(cursor.ops);
  return driver_result(error, status);
}

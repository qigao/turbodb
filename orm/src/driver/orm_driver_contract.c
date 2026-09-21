#include "orm_driver_contract.h"
#include "orm_driver_abi.h"

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

static orm_status_t driver_check_id(orm_driver_bytes_v1 id) {
  const unsigned char *text;
  orm_status_t status = orm_driver_check_bytes(id, ORM_DRIVER_ID_MAX_BYTES);
  if (status != ORM_STATUS_OK)
    return status;
  if (id.size == 0u)
    return ORM_STATUS_INVALID_ARGUMENT;
  text = (const unsigned char *)id.data;
  if (text[0] < 'a' || text[0] > 'z')
    return ORM_STATUS_INVALID_ARGUMENT;
  for (uint64_t i = 1u; i < id.size; ++i) {
    unsigned char c = text[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
      return ORM_STATUS_INVALID_ARGUMENT;
  }
  return ORM_STATUS_OK;
}

static int driver_same_id(orm_driver_bytes_v1 a, orm_driver_bytes_v1 b) {
  return a.size == b.size && memcmp(a.data, b.data, (size_t)a.size) == 0;
}

static orm_driver_bytes_v1 driver_alias_at(const orm_driver_api_v1 *api, uint32_t i) {
  orm_driver_bytes_v1 alias;
  memcpy(&alias, (const unsigned char *)api->aliases + (size_t)i * sizeof(alias),
         sizeof(alias));
  return alias;
}

static orm_status_t driver_check_aliases(const orm_driver_api_v1 *api,
                                         uint32_t max_aliases) {
  uint64_t used = sizeof(*api) + api->canonical_id.size +
      (uint64_t)api->alias_count * sizeof(orm_driver_bytes_v1);
  if (api->alias_count > max_aliases || used > ORM_DRIVER_DESCRIPTOR_MAX_BYTES)
    return ORM_STATUS_LIMIT_EXCEEDED;
  if (api->alias_count != 0u && api->aliases == NULL)
    return ORM_STATUS_INVALID_ARGUMENT;
  for (uint32_t i = 0u; i < api->alias_count; ++i) {
    orm_driver_bytes_v1 alias = driver_alias_at(api, i);
    orm_status_t status = driver_check_id(alias);
    if (status != ORM_STATUS_OK)
      return status;
    if (alias.size > ORM_DRIVER_DESCRIPTOR_MAX_BYTES - used)
      return ORM_STATUS_LIMIT_EXCEEDED;
    used += alias.size;
    if (driver_same_id(alias, api->canonical_id))
      return ORM_STATUS_INVALID_ARGUMENT;
    for (uint32_t j = 0u; j < i; ++j) {
      if (driver_same_id(alias, driver_alias_at(api, j)))
        return ORM_STATUS_INVALID_ARGUMENT;
    }
  }
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

static orm_status_t driver_check_host(const void *buffer, uint32_t bytes,
    const uint8_t expected_bundle[ORM_DRIVER_BUNDLE_ID_BYTES]) {
  orm_driver_host_v1 host = {0};
  orm_driver_plan_metadata_ops_v1 metadata = {0};
  orm_driver_plan_value_ops_v1 values = {0};
  orm_driver_lifetime_ops_v1 lifetime = {0};
  orm_driver_execution_ops_v1 execution = {0};
  uint32_t declared = 0u;
  orm_status_t status;
  if (expected_bundle == NULL)
    return ORM_STATUS_INVALID_ARGUMENT;
  status = driver_copy_prefix(buffer, bytes,
      DRIVER_FIELD_END(orm_driver_host_v1, execution), &host, &declared);
  if (status != ORM_STATUS_OK)
    return status;
  if (memcmp(host.bundle_id, expected_bundle, sizeof(host.bundle_id)) != 0)
    return ORM_STATUS_ABI_MISMATCH;
  status = driver_copy_table(host.plan_metadata,
      DRIVER_FIELD_END(orm_driver_plan_metadata_ops_v1, ordering), &metadata, &declared);
  if (status != ORM_STATUS_OK)
    return status;
  if (metadata.describe == NULL || metadata.column_at == NULL || metadata.ordering == NULL)
    return ORM_STATUS_ABI_MISMATCH;
  status = driver_copy_table(host.plan_values,
      DRIVER_FIELD_END(orm_driver_plan_value_ops_v1, raw_parameter_at), &values, &declared);
  if (status != ORM_STATUS_OK)
    return status;
  if (values.assignment_at == NULL || values.predicate_at == NULL || values.raw_parameter_at == NULL)
    return ORM_STATUS_ABI_MISMATCH;
  status = driver_copy_table(host.lifetime,
      DRIVER_FIELD_END(orm_driver_lifetime_ops_v1, release), &lifetime, &declared);
  if (status != ORM_STATUS_OK)
    return status;
  if (lifetime.acquire == NULL || lifetime.release == NULL)
    return ORM_STATUS_ABI_MISMATCH;
  if (host.execution.reserved != 0u)
    return ORM_STATUS_INVALID_ARGUMENT;
  if (host.execution.data == NULL && host.execution.bytes == 0u)
    return ORM_STATUS_OK;
  status = driver_copy_table(host.execution,
      DRIVER_FIELD_END(orm_driver_execution_ops_v1, release_task), &execution, &declared);
  if (status != ORM_STATUS_OK)
    return status;
  if (execution.submit == NULL || execution.request_cancel == NULL || execution.release_task == NULL)
    return ORM_STATUS_ABI_MISMATCH;
  return ORM_STATUS_OK;
}

static orm_status_t driver_check_api(const void *buffer, uint32_t bytes,
    const uint8_t expected_bundle[ORM_DRIVER_BUNDLE_ID_BYTES],
    orm_driver_bytes_v1 expected_id, uint32_t max_aliases) {
  orm_driver_api_v1 api = {0};
  orm_driver_module_ops_v1 module = {0};
  uint32_t declared = 0u;
  orm_status_t status;
  if (expected_bundle == NULL)
    return ORM_STATUS_INVALID_ARGUMENT;
  status = driver_copy_prefix(buffer, bytes,
      DRIVER_FIELD_END(orm_driver_api_v1, connection_ops), &api, &declared);
  if (status != ORM_STATUS_OK)
    return status;
  if (memcmp(api.bundle_id, expected_bundle, sizeof(api.bundle_id)) != 0)
    return ORM_STATUS_ABI_MISMATCH;
  if (api.reserved != 0u)
    return ORM_STATUS_INVALID_ARGUMENT;
  status = driver_check_capabilities(api.capabilities);
  if (status != ORM_STATUS_OK)
    return status;
  if ((api.execution_models & ~ORM_DRIVER_EXEC_KNOWN_MASK) != 0u)
    return ORM_STATUS_UNSUPPORTED;
  if (api.execution_models == 0u)
    return ORM_STATUS_ABI_MISMATCH;
  status = driver_check_id(api.canonical_id);
  if (status != ORM_STATUS_OK)
    return status;
  status = driver_check_id(expected_id);
  if (status != ORM_STATUS_OK)
    return status;
  if (!driver_same_id(api.canonical_id, expected_id))
    return ORM_STATUS_INVALID_ARGUMENT;
  status = driver_check_aliases(&api, max_aliases);
  if (status != ORM_STATUS_OK)
    return status;
  status = driver_copy_table(api.module_ops,
      DRIVER_FIELD_END(orm_driver_module_ops_v1, finalize), &module, &declared);
  if (status != ORM_STATUS_OK)
    return status;
  if (module.initialize == NULL || module.finalize == NULL || api.create_connection == NULL)
    return ORM_STATUS_ABI_MISMATCH;
  return driver_check_connection_ops(api.connection_ops, api.capabilities);
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

orm_status_t ORM_DRIVER_CALL orm_driver_validate_host_v1(
    const void *host, uint32_t bytes,
    const uint8_t expected_bundle[ORM_DRIVER_BUNDLE_ID_BYTES], orm_error_t *error) {
  return driver_result(error, driver_check_host(host, bytes, expected_bundle));
}

orm_status_t ORM_DRIVER_CALL orm_driver_validate_api_v1(
    const void *api, uint32_t bytes,
    const uint8_t expected_bundle[ORM_DRIVER_BUNDLE_ID_BYTES],
    orm_driver_bytes_v1 expected_id, uint32_t max_aliases, orm_error_t *error) {
  return driver_result(error, driver_check_api(api, bytes, expected_bundle,
                                              expected_id, max_aliases));
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

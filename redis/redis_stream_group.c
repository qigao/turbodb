#include "redis_stream_group.h"

#include "salts_error.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  REDIS_STREAM_GROUP_TEXT_MAX_BYTES = 4096u,
  REDIS_STREAM_GROUP_ID_MAX_BYTES = 63u
};

typedef struct redis_stream_group_impl redis_stream_group_impl;

typedef struct redis_stream_group_owned_field {
  char *name;
  size_t name_length;
  char *value;
  size_t value_length;
} redis_stream_group_owned_field;

struct redis_stream_group_receipt {
  redis_stream_group_impl *owner;
  redis_stream_group_receipt_kind kind;
  char id[REDIS_STREAM_GROUP_ID_MAX_BYTES + 1u];
  size_t id_length;
  uint64_t delivery_count;
  size_t payload_bytes;
  redis_stream_group_owned_field *fields;
  size_t field_count;
  int acknowledged;
  int needs_delivery_count;
  struct redis_stream_group_receipt *next;
};

typedef enum redis_stream_group_command_kind {
  REDIS_STREAM_GROUP_COMMAND_NONE = 0,
  REDIS_STREAM_GROUP_COMMAND_DIRECT,
  REDIS_STREAM_GROUP_COMMAND_POOL
} redis_stream_group_command_kind;

typedef struct redis_stream_group_command {
  redis_stream_group_command_kind kind;
  union {
    redis_cflow_stream direct;
    redis_pool_stream pooled;
  } stream;
} redis_stream_group_command;

typedef enum redis_stream_group_operation {
  REDIS_STREAM_GROUP_OPERATION_IDLE = 0,
  REDIS_STREAM_GROUP_OPERATION_FETCH,
  REDIS_STREAM_GROUP_OPERATION_CLAIM,
  REDIS_STREAM_GROUP_OPERATION_ACK
} redis_stream_group_operation;

struct redis_stream_group_impl {
  redis_stream_group_source source;
  char *stream_key;
  size_t stream_key_length;
  char *group;
  size_t group_length;
  char *consumer;
  size_t consumer_length;
  uint64_t min_idle_ms;
  uint32_t max_delivery_attempts;
  redis_stream_group_retention_policy retention_policy;
  size_t max_records_per_fetch;
  size_t max_claim_batch;
  size_t max_reply_bytes;
  size_t max_payload_bytes;
  size_t max_fields_per_record;

  redis_stream_group_operation operation;
  redis_stream_group_budget budget;
  size_t remaining_payload_bytes;
  int zero_budget;
  int main_item_seen;
  int main_complete;
  int inspection_active;
  int ack_item_seen;
  int ack_result_seen;

  redis_stream_group_command command;
  redis_stream_group_receipt *queue_head;
  redis_stream_group_receipt *queue_tail;
  redis_stream_group_receipt *ack_receipt;
  size_t active_receipts;

  char claim_cursor[REDIS_STREAM_GROUP_ID_MAX_BYTES + 1u];
  size_t claim_cursor_length;
};

static redis_stream_group_impl *redis_stream_group_get(
    redis_stream_group *owner) {
  return owner != NULL ? (redis_stream_group_impl *)owner->impl : NULL;
}

static int redis_stream_group_source_valid(redis_stream_group_source source) {
  if (source.handle == NULL) return 0;
  switch (source.kind) {
  case REDIS_STREAM_GROUP_SOURCE_CONNECTION:
  case REDIS_STREAM_GROUP_SOURCE_POOL:
  case REDIS_STREAM_GROUP_SOURCE_CLUSTER:
  case REDIS_STREAM_GROUP_SOURCE_SENTINEL:
    return 1;
  default:
    return 0;
  }
}

static char *redis_stream_group_copy_bytes(
    const char *value, size_t length) {
  char *copy;
  if (value == NULL || length == 0u ||
      length > REDIS_STREAM_GROUP_TEXT_MAX_BYTES)
    return NULL;
  copy = (char *)malloc(length + 1u);
  if (copy == NULL) return NULL;
  memcpy(copy, value, length);
  copy[length] = '\0';
  return copy;
}

static void redis_stream_group_command_reset(
    redis_stream_group_command *command) {
  if (command == NULL) return;
  memset(command, 0, sizeof(*command));
}

static int redis_stream_group_command_open(
    redis_stream_group_impl *impl, int argc, const char **argv,
    const size_t *argvlen, size_t max_reply_bytes) {
  int status;
  if (impl == NULL || argc <= 0 || argv == NULL ||
      max_reply_bytes == 0u ||
      impl->command.kind != REDIS_STREAM_GROUP_COMMAND_NONE)
    return SALTS_EINVAL;

  switch (impl->source.kind) {
  case REDIS_STREAM_GROUP_SOURCE_CONNECTION:
    status = redis_cflow_command_open(
        (redis_cflow_connection *)impl->source.handle, argc, argv, argvlen,
        max_reply_bytes, &impl->command.stream.direct);
    if (status == SALTS_OK)
      impl->command.kind = REDIS_STREAM_GROUP_COMMAND_DIRECT;
    return status;
  case REDIS_STREAM_GROUP_SOURCE_POOL:
    status = redis_pool_command_open(
        (redis_pool *)impl->source.handle, argc, argv, argvlen,
        max_reply_bytes, &impl->command.stream.pooled);
    if (status == SALTS_OK)
      impl->command.kind = REDIS_STREAM_GROUP_COMMAND_POOL;
    return status;
  case REDIS_STREAM_GROUP_SOURCE_CLUSTER:
    status = redis_cluster_command_open(
        (redis_cluster *)impl->source.handle,
        impl->stream_key, impl->stream_key_length,
        argc, argv, argvlen, max_reply_bytes,
        &impl->command.stream.pooled);
    if (status == SALTS_OK)
      impl->command.kind = REDIS_STREAM_GROUP_COMMAND_POOL;
    return status;
  case REDIS_STREAM_GROUP_SOURCE_SENTINEL:
    status = redis_sentinel_command_open(
        (redis_sentinel *)impl->source.handle, argc, argv, argvlen,
        max_reply_bytes, &impl->command.stream.pooled);
    if (status == SALTS_OK)
      impl->command.kind = REDIS_STREAM_GROUP_COMMAND_POOL;
    return status;
  default:
    return SALTS_EINVAL;
  }
}

static redis_cflow_stream_step redis_stream_group_command_next(
    redis_stream_group_command *command) {
  redis_cflow_stream_step step = REDIS_CFLOW_STREAM_STEP_INIT;
  if (command == NULL) {
    step.kind = REDIS_CFLOW_STREAM_ERROR;
    step.status = SALTS_EINVAL;
    return step;
  }
  if (command->kind == REDIS_STREAM_GROUP_COMMAND_DIRECT)
    return redis_cflow_stream_next(&command->stream.direct);
  if (command->kind == REDIS_STREAM_GROUP_COMMAND_POOL)
    return redis_pool_stream_next(&command->stream.pooled);
  step.kind = REDIS_CFLOW_STREAM_ERROR;
  step.status = SALTS_EINVAL;
  return step;
}

static int redis_stream_group_command_cancel(
    redis_stream_group_command *command) {
  if (command == NULL) return SALTS_EINVAL;
  if (command->kind == REDIS_STREAM_GROUP_COMMAND_NONE) return SALTS_OK;
  if (command->kind == REDIS_STREAM_GROUP_COMMAND_DIRECT)
    return redis_cflow_stream_cancel(&command->stream.direct);
  if (command->kind == REDIS_STREAM_GROUP_COMMAND_POOL)
    return redis_pool_stream_cancel(&command->stream.pooled);
  return SALTS_EINVAL;
}

static int redis_stream_group_command_destroy(
    redis_stream_group_command *command) {
  int status;
  if (command == NULL) return SALTS_EINVAL;
  if (command->kind == REDIS_STREAM_GROUP_COMMAND_NONE) return SALTS_OK;
  if (command->kind == REDIS_STREAM_GROUP_COMMAND_DIRECT)
    status = redis_cflow_stream_destroy(&command->stream.direct);
  else if (command->kind == REDIS_STREAM_GROUP_COMMAND_POOL)
    status = redis_pool_stream_destroy(&command->stream.pooled);
  else
    return SALTS_EINVAL;
  if (status == SALTS_OK) redis_stream_group_command_reset(command);
  return status;
}

static void redis_stream_group_receipt_free_local(
    redis_stream_group_receipt *receipt) {
  size_t index;
  if (receipt == NULL) return;
  for (index = 0u; index < receipt->field_count; ++index) {
    free(receipt->fields[index].value);
    free(receipt->fields[index].name);
  }
  free(receipt->fields);
  free(receipt);
}

static void redis_stream_group_queue_clear(redis_stream_group_impl *impl) {
  redis_stream_group_receipt *current;
  if (impl == NULL) return;
  current = impl->queue_head;
  while (current != NULL) {
    redis_stream_group_receipt *next = current->next;
    redis_stream_group_receipt_free_local(current);
    current = next;
  }
  impl->queue_head = NULL;
  impl->queue_tail = NULL;
}

static void redis_stream_group_queue_push(
    redis_stream_group_impl *impl, redis_stream_group_receipt *receipt) {
  receipt->next = NULL;
  if (impl->queue_tail != NULL)
    impl->queue_tail->next = receipt;
  else
    impl->queue_head = receipt;
  impl->queue_tail = receipt;
}

static redis_stream_group_receipt *redis_stream_group_queue_pop(
    redis_stream_group_impl *impl) {
  redis_stream_group_receipt *receipt = impl->queue_head;
  if (receipt == NULL) return NULL;
  impl->queue_head = receipt->next;
  if (impl->queue_head == NULL) impl->queue_tail = NULL;
  receipt->next = NULL;
  return receipt;
}

static int redis_stream_group_reply_text(
    const redis_reply_t *reply, const char **text, size_t *length) {
  if (reply == NULL || text == NULL || length == NULL ||
      (reply->type != REDIS_REPLY_STRING &&
       reply->type != REDIS_REPLY_BULK_STRING) ||
      reply->str == NULL)
    return SALTS_EPROTO;
  *text = reply->str;
  *length = reply->len;
  return SALTS_OK;
}

static int redis_stream_group_copy_id(
    const redis_reply_t *reply,
    char out[REDIS_STREAM_GROUP_ID_MAX_BYTES + 1u],
    size_t *out_length) {
  const char *text;
  size_t length;
  if (redis_stream_group_reply_text(reply, &text, &length) != SALTS_OK ||
      length == 0u || length > REDIS_STREAM_GROUP_ID_MAX_BYTES)
    return SALTS_EPROTO;
  memcpy(out, text, length);
  out[length] = '\0';
  *out_length = length;
  return SALTS_OK;
}

static redis_stream_group_receipt *redis_stream_group_receipt_new_id(
    redis_stream_group_impl *impl, redis_stream_group_receipt_kind kind,
    const redis_reply_t *id_reply) {
  redis_stream_group_receipt *receipt =
      (redis_stream_group_receipt *)calloc(1u, sizeof(*receipt));
  if (receipt == NULL) return NULL;
  receipt->owner = impl;
  receipt->kind = kind;
  if (redis_stream_group_copy_id(
          id_reply, receipt->id, &receipt->id_length) != SALTS_OK) {
    free(receipt);
    return NULL;
  }
  return receipt;
}

static size_t redis_stream_group_reply_payload_bytes(
    const redis_reply_t *fields, int *valid) {
  size_t total = 0u;
  size_t index;
  *valid = 0;
  if (fields == NULL || fields->type != REDIS_REPLY_ARRAY ||
      fields->elements == NULL || (fields->element_count % 2u) != 0u)
    return 0u;
  for (index = 1u; index < fields->element_count; index += 2u) {
    const char *value;
    size_t length;
    if (redis_stream_group_reply_text(
            fields->elements[index], &value, &length) != SALTS_OK)
      return 0u;
    if (length > SIZE_MAX - total) return 0u;
    total += length;
  }
  *valid = 1;
  return total;
}

static int redis_stream_group_copy_fields(
    redis_stream_group_receipt *receipt, const redis_reply_t *fields,
    size_t max_fields) {
  size_t field_count;
  size_t index;
  if (receipt == NULL || fields == NULL ||
      fields->type != REDIS_REPLY_ARRAY ||
      fields->elements == NULL || (fields->element_count % 2u) != 0u)
    return SALTS_EPROTO;
  field_count = fields->element_count / 2u;
  if (field_count > max_fields) return SALTS_ENOBUFS;
  if (field_count == 0u) return SALTS_OK;

  receipt->fields = (redis_stream_group_owned_field *)calloc(
      field_count, sizeof(*receipt->fields));
  if (receipt->fields == NULL) return SALTS_ENOMEM;
  receipt->field_count = field_count;

  for (index = 0u; index < field_count; ++index) {
    const char *name;
    const char *value;
    size_t name_length;
    size_t value_length;
    redis_stream_group_owned_field *field = &receipt->fields[index];
    if (redis_stream_group_reply_text(
            fields->elements[index * 2u], &name, &name_length) != SALTS_OK ||
        redis_stream_group_reply_text(
            fields->elements[index * 2u + 1u],
            &value, &value_length) != SALTS_OK)
      return SALTS_EPROTO;
    field->name = (char *)malloc(name_length + 1u);
    field->value = (char *)malloc(value_length + 1u);
    if (field->name == NULL || field->value == NULL) return SALTS_ENOMEM;
    memcpy(field->name, name, name_length);
    field->name[name_length] = '\0';
    field->name_length = name_length;
    memcpy(field->value, value, value_length);
    field->value[value_length] = '\0';
    field->value_length = value_length;
  }
  return SALTS_OK;
}

static int redis_stream_group_parse_entry(
    redis_stream_group_impl *impl, const redis_reply_t *entry,
    int claimed, redis_stream_group_receipt **out_receipt) {
  redis_stream_group_receipt *receipt;
  const redis_reply_t *fields;
  size_t payload_bytes;
  int valid;
  int status;

  *out_receipt = NULL;
  if (entry == NULL || entry->type != REDIS_REPLY_ARRAY ||
      entry->element_count != 2u || entry->elements == NULL)
    return SALTS_EPROTO;

  receipt = redis_stream_group_receipt_new_id(
      impl, REDIS_STREAM_GROUP_RECEIPT_RECORD, entry->elements[0]);
  if (receipt == NULL) return SALTS_ENOMEM;
  fields = entry->elements[1];
  payload_bytes = redis_stream_group_reply_payload_bytes(fields, &valid);
  if (!valid) {
    redis_stream_group_receipt_free_local(receipt);
    return SALTS_EPROTO;
  }
  receipt->payload_bytes = payload_bytes;
  if (fields->element_count / 2u > impl->max_fields_per_record ||
      payload_bytes > impl->remaining_payload_bytes) {
    receipt->kind = REDIS_STREAM_GROUP_RECEIPT_PAYLOAD_LIMIT;
  } else {
    status = redis_stream_group_copy_fields(
        receipt, fields, impl->max_fields_per_record);
    if (status != SALTS_OK) {
      redis_stream_group_receipt_free_local(receipt);
      return status;
    }
    impl->remaining_payload_bytes -= payload_bytes;
  }
  if (claimed) {
    receipt->needs_delivery_count = 1;
  } else {
    receipt->delivery_count = 1u;
  }
  *out_receipt = receipt;
  return SALTS_OK;
}

static int redis_stream_group_parse_fetch_reply(
    redis_stream_group_impl *impl, const redis_reply_t *reply) {
  const redis_reply_t *stream_reply;
  const redis_reply_t *entries;
  const char *stream_name;
  size_t stream_name_length;
  size_t index;
  if (reply == NULL) return SALTS_EPROTO;
  if (reply->type == REDIS_REPLY_NULL) return SALTS_OK;
  if (reply->type != REDIS_REPLY_ARRAY) return SALTS_EPROTO;
  if (reply->element_count == 0u) return SALTS_OK;
  if (reply->element_count != 1u || reply->elements == NULL)
    return SALTS_EPROTO;
  stream_reply = reply->elements[0];
  if (stream_reply == NULL || stream_reply->type != REDIS_REPLY_ARRAY ||
      stream_reply->element_count != 2u || stream_reply->elements == NULL)
    return SALTS_EPROTO;
  if (redis_stream_group_reply_text(
          stream_reply->elements[0], &stream_name,
          &stream_name_length) != SALTS_OK ||
      stream_name_length != impl->stream_key_length ||
      memcmp(stream_name, impl->stream_key, stream_name_length) != 0)
    return SALTS_EPROTO;
  entries = stream_reply->elements[1];
  if (entries == NULL || entries->type != REDIS_REPLY_ARRAY ||
      (entries->element_count != 0u && entries->elements == NULL))
    return SALTS_EPROTO;
  if (entries->element_count > impl->budget.max_records)
    return SALTS_EPROTO;
  for (index = 0u; index < entries->element_count; ++index) {
    redis_stream_group_receipt *receipt = NULL;
    int status = redis_stream_group_parse_entry(
        impl, entries->elements[index], 0, &receipt);
    if (status != SALTS_OK) return status;
    redis_stream_group_queue_push(impl, receipt);
  }
  return SALTS_OK;
}

static int redis_stream_group_parse_claim_reply(
    redis_stream_group_impl *impl, const redis_reply_t *reply) {
  const redis_reply_t *entries;
  size_t index;
  if (reply == NULL || reply->type != REDIS_REPLY_ARRAY ||
      (reply->element_count != 2u && reply->element_count != 3u) ||
      reply->elements == NULL)
    return SALTS_EPROTO;

  if (redis_stream_group_copy_id(
          reply->elements[0], impl->claim_cursor,
          &impl->claim_cursor_length) != SALTS_OK)
    return SALTS_EPROTO;

  entries = reply->elements[1];
  if (entries == NULL || entries->type != REDIS_REPLY_ARRAY ||
      (entries->element_count != 0u && entries->elements == NULL) ||
      entries->element_count > impl->budget.max_records)
    return SALTS_EPROTO;

  for (index = 0u; index < entries->element_count; ++index) {
    redis_stream_group_receipt *receipt = NULL;
    int status = redis_stream_group_parse_entry(
        impl, entries->elements[index], 1, &receipt);
    if (status != SALTS_OK) return status;
    redis_stream_group_queue_push(impl, receipt);
  }

  if (reply->element_count == 3u) {
    const redis_reply_t *deleted = reply->elements[2];
    if (deleted == NULL || deleted->type != REDIS_REPLY_ARRAY ||
        (deleted->element_count != 0u && deleted->elements == NULL))
      return SALTS_EPROTO;
    for (index = 0u; index < deleted->element_count; ++index) {
      redis_stream_group_receipt *receipt =
          redis_stream_group_receipt_new_id(
              impl, REDIS_STREAM_GROUP_RECEIPT_DATA_LOSS,
              deleted->elements[index]);
      if (receipt == NULL) return SALTS_ENOMEM;
      redis_stream_group_queue_push(impl, receipt);
    }
  }
  return SALTS_OK;
}

static int redis_stream_group_parse_pending_reply(
    redis_stream_group_impl *impl, redis_stream_group_receipt *receipt,
    const redis_reply_t *reply) {
  const redis_reply_t *entry;
  const redis_reply_t *delivery;
  if (impl == NULL || receipt == NULL || reply == NULL ||
      reply->type != REDIS_REPLY_ARRAY || reply->elements == NULL ||
      reply->element_count != 1u)
    return SALTS_EPROTO;
  entry = reply->elements[0];
  if (entry == NULL || entry->type != REDIS_REPLY_ARRAY ||
      entry->elements == NULL || entry->element_count != 4u)
    return SALTS_EPROTO;
  delivery = entry->elements[3];
  if (delivery == NULL || delivery->type != REDIS_REPLY_INTEGER ||
      delivery->integer <= 0)
    return SALTS_EPROTO;
  receipt->delivery_count = (uint64_t)delivery->integer;
  receipt->needs_delivery_count = 0;
  if (receipt->delivery_count > (uint64_t)impl->max_delivery_attempts)
    receipt->kind = REDIS_STREAM_GROUP_RECEIPT_DELIVERY_LIMIT;
  return SALTS_OK;
}

static redis_stream_group_step redis_stream_group_native_error(
    redis_cflow_stream_step native) {
  redis_stream_group_step step = REDIS_STREAM_GROUP_STEP_INIT;
  step.kind = REDIS_STREAM_GROUP_ERROR;
  step.status = native.status != SALTS_OK ? native.status : SALTS_EPROTO;
  step.outcome = native.outcome;
  step.server_error = native.server_error;
  redis_reply_free(native.item);
  return step;
}

static redis_stream_group_step redis_stream_group_error(int status) {
  redis_stream_group_step step = REDIS_STREAM_GROUP_STEP_INIT;
  step.kind = REDIS_STREAM_GROUP_ERROR;
  step.status = status;
  return step;
}

static redis_stream_group_step redis_stream_group_done(void) {
  redis_stream_group_step step = REDIS_STREAM_GROUP_STEP_INIT;
  step.kind = REDIS_STREAM_GROUP_DONE;
  step.status = SALTS_OK;
  step.outcome = REDIS_COMMAND_REPLIED;
  return step;
}

static void redis_stream_group_reset_operation(redis_stream_group_impl *impl) {
  impl->operation = REDIS_STREAM_GROUP_OPERATION_IDLE;
  memset(&impl->budget, 0, sizeof(impl->budget));
  impl->remaining_payload_bytes = 0u;
  impl->zero_budget = 0;
  impl->main_item_seen = 0;
  impl->main_complete = 0;
  impl->inspection_active = 0;
  impl->ack_item_seen = 0;
  impl->ack_result_seen = 0;
  impl->ack_receipt = NULL;
}

static int redis_stream_group_budget_valid(
    const redis_stream_group_impl *impl,
    const redis_stream_group_budget *budget, int claim) {
  if (impl == NULL || budget == NULL) return 0;
  if (budget->max_records > impl->max_records_per_fetch ||
      (claim && budget->max_records > impl->max_claim_batch) ||
      budget->max_reply_bytes > impl->max_reply_bytes ||
      budget->max_payload_bytes > impl->max_payload_bytes)
    return 0;
  return 1;
}

static int redis_stream_group_open_fetch(
    redis_stream_group_impl *impl) {
  const char *argv[9];
  size_t lengths[9];
  char count_text[32];
  int written = snprintf(count_text, sizeof(count_text), "%zu",
                         impl->budget.max_records);
  if (written <= 0 || (size_t)written >= sizeof(count_text))
    return SALTS_EINVAL;
  argv[0] = "XREADGROUP"; lengths[0] = 10u;
  argv[1] = "GROUP"; lengths[1] = 5u;
  argv[2] = impl->group; lengths[2] = impl->group_length;
  argv[3] = impl->consumer; lengths[3] = impl->consumer_length;
  argv[4] = "COUNT"; lengths[4] = 5u;
  argv[5] = count_text; lengths[5] = (size_t)written;
  argv[6] = "STREAMS"; lengths[6] = 7u;
  argv[7] = impl->stream_key; lengths[7] = impl->stream_key_length;
  argv[8] = ">"; lengths[8] = 1u;
  return redis_stream_group_command_open(
      impl, 9, argv, lengths, impl->budget.max_reply_bytes);
}

static int redis_stream_group_open_claim(
    redis_stream_group_impl *impl) {
  const char *argv[8];
  size_t lengths[8];
  char idle_text[32];
  char count_text[32];
  int idle_written = snprintf(
      idle_text, sizeof(idle_text), "%" PRIu64, impl->min_idle_ms);
  int count_written = snprintf(
      count_text, sizeof(count_text), "%zu", impl->budget.max_records);
  if (idle_written <= 0 || (size_t)idle_written >= sizeof(idle_text) ||
      count_written <= 0 || (size_t)count_written >= sizeof(count_text))
    return SALTS_EINVAL;
  argv[0] = "XAUTOCLAIM"; lengths[0] = 10u;
  argv[1] = impl->stream_key; lengths[1] = impl->stream_key_length;
  argv[2] = impl->group; lengths[2] = impl->group_length;
  argv[3] = impl->consumer; lengths[3] = impl->consumer_length;
  argv[4] = idle_text; lengths[4] = (size_t)idle_written;
  argv[5] = impl->claim_cursor; lengths[5] = impl->claim_cursor_length;
  argv[6] = "COUNT"; lengths[6] = 5u;
  argv[7] = count_text; lengths[7] = (size_t)count_written;
  return redis_stream_group_command_open(
      impl, 8, argv, lengths, impl->budget.max_reply_bytes);
}

static int redis_stream_group_open_pending_inspection(
    redis_stream_group_impl *impl, redis_stream_group_receipt *receipt) {
  const char *argv[7];
  size_t lengths[7];
  argv[0] = "XPENDING"; lengths[0] = 8u;
  argv[1] = impl->stream_key; lengths[1] = impl->stream_key_length;
  argv[2] = impl->group; lengths[2] = impl->group_length;
  argv[3] = receipt->id; lengths[3] = receipt->id_length;
  argv[4] = receipt->id; lengths[4] = receipt->id_length;
  argv[5] = "1"; lengths[5] = 1u;
  argv[6] = impl->consumer; lengths[6] = impl->consumer_length;
  return redis_stream_group_command_open(
      impl, 7, argv, lengths, impl->budget.max_reply_bytes);
}

static int redis_stream_group_open_ack(
    redis_stream_group_impl *impl, redis_stream_group_receipt *receipt) {
  const char *argv[4];
  size_t lengths[4];
  argv[0] = "XACK"; lengths[0] = 4u;
  argv[1] = impl->stream_key; lengths[1] = impl->stream_key_length;
  argv[2] = impl->group; lengths[2] = impl->group_length;
  argv[3] = receipt->id; lengths[3] = receipt->id_length;
  return redis_stream_group_command_open(
      impl, 4, argv, lengths, impl->max_reply_bytes);
}

int redis_stream_group_init(
    redis_stream_group *owner, const redis_stream_group_config *config) {
  redis_stream_group_impl *impl;
  if (owner == NULL || owner->impl != NULL || config == NULL ||
      !redis_stream_group_source_valid(config->source) ||
      config->stream_key == NULL || config->stream_key_length == 0u ||
      config->stream_key_length > REDIS_STREAM_GROUP_TEXT_MAX_BYTES ||
      config->group == NULL || config->group_length == 0u ||
      config->group_length > REDIS_STREAM_GROUP_TEXT_MAX_BYTES ||
      config->consumer == NULL || config->consumer_length == 0u ||
      config->consumer_length > REDIS_STREAM_GROUP_TEXT_MAX_BYTES ||
      config->min_idle_ms == 0u || config->max_delivery_attempts == 0u ||
      (config->retention_policy !=
           REDIS_STREAM_GROUP_RETENTION_PRESERVE_PENDING &&
       config->retention_policy !=
           REDIS_STREAM_GROUP_RETENTION_ALLOW_PENDING_LOSS) ||
      config->max_records_per_fetch == 0u || config->max_claim_batch == 0u ||
      config->max_reply_bytes == 0u || config->max_payload_bytes == 0u ||
      config->max_fields_per_record == 0u)
    return SALTS_EINVAL;

  impl = (redis_stream_group_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  impl->stream_key = redis_stream_group_copy_bytes(
      config->stream_key, config->stream_key_length);
  impl->group = redis_stream_group_copy_bytes(
      config->group, config->group_length);
  impl->consumer = redis_stream_group_copy_bytes(
      config->consumer, config->consumer_length);
  if (impl->stream_key == NULL || impl->group == NULL ||
      impl->consumer == NULL) {
    free(impl->consumer);
    free(impl->group);
    free(impl->stream_key);
    free(impl);
    return SALTS_ENOMEM;
  }

  impl->source = config->source;
  impl->stream_key_length = config->stream_key_length;
  impl->group_length = config->group_length;
  impl->consumer_length = config->consumer_length;
  impl->min_idle_ms = config->min_idle_ms;
  impl->max_delivery_attempts = config->max_delivery_attempts;
  impl->retention_policy = config->retention_policy;
  impl->max_records_per_fetch = config->max_records_per_fetch;
  impl->max_claim_batch = config->max_claim_batch;
  impl->max_reply_bytes = config->max_reply_bytes;
  impl->max_payload_bytes = config->max_payload_bytes;
  impl->max_fields_per_record = config->max_fields_per_record;
  memcpy(impl->claim_cursor, "0-0", 4u);
  impl->claim_cursor_length = 3u;
  redis_stream_group_command_reset(&impl->command);
  redis_stream_group_reset_operation(impl);
  owner->impl = impl;
  return SALTS_OK;
}

int redis_stream_group_rebind(
    redis_stream_group *owner, redis_stream_group_source source) {
  redis_stream_group_impl *impl = redis_stream_group_get(owner);
  if (impl == NULL || !redis_stream_group_source_valid(source))
    return SALTS_EINVAL;
  if (impl->operation != REDIS_STREAM_GROUP_OPERATION_IDLE ||
      impl->command.kind != REDIS_STREAM_GROUP_COMMAND_NONE ||
      impl->queue_head != NULL)
    return SALTS_EBUSY;
  impl->source = source;
  return SALTS_OK;
}

int redis_stream_group_fetch_begin(
    redis_stream_group *owner, const redis_stream_group_budget *budget) {
  redis_stream_group_impl *impl = redis_stream_group_get(owner);
  int status;
  if (!redis_stream_group_budget_valid(impl, budget, 0))
    return SALTS_EINVAL;
  if (impl->operation != REDIS_STREAM_GROUP_OPERATION_IDLE)
    return SALTS_EBUSY;
  impl->operation = REDIS_STREAM_GROUP_OPERATION_FETCH;
  impl->budget = *budget;
  impl->remaining_payload_bytes = budget->max_payload_bytes;
  impl->zero_budget = budget->max_records == 0u ||
                      budget->max_reply_bytes == 0u ||
                      budget->max_payload_bytes == 0u;
  if (impl->zero_budget) return SALTS_OK;
  status = redis_stream_group_open_fetch(impl);
  if (status != SALTS_OK) {
    redis_stream_group_reset_operation(impl);
    return status;
  }
  return SALTS_OK;
}

int redis_stream_group_claim_begin(
    redis_stream_group *owner, const redis_stream_group_budget *budget) {
  redis_stream_group_impl *impl = redis_stream_group_get(owner);
  int status;
  if (!redis_stream_group_budget_valid(impl, budget, 1))
    return SALTS_EINVAL;
  if (impl->operation != REDIS_STREAM_GROUP_OPERATION_IDLE)
    return SALTS_EBUSY;
  impl->operation = REDIS_STREAM_GROUP_OPERATION_CLAIM;
  impl->budget = *budget;
  impl->remaining_payload_bytes = budget->max_payload_bytes;
  impl->zero_budget = budget->max_records == 0u ||
                      budget->max_reply_bytes == 0u ||
                      budget->max_payload_bytes == 0u;
  if (impl->zero_budget) return SALTS_OK;
  status = redis_stream_group_open_claim(impl);
  if (status != SALTS_OK) {
    redis_stream_group_reset_operation(impl);
    return status;
  }
  return SALTS_OK;
}

int redis_stream_group_ack_begin(
    redis_stream_group *owner, redis_stream_group_receipt *receipt) {
  redis_stream_group_impl *impl = redis_stream_group_get(owner);
  int status;
  if (impl == NULL || receipt == NULL || receipt->owner != impl)
    return SALTS_EINVAL;
  if (impl->operation != REDIS_STREAM_GROUP_OPERATION_IDLE)
    return SALTS_EBUSY;
  if (receipt->kind == REDIS_STREAM_GROUP_RECEIPT_DATA_LOSS)
    return SALTS_ENOENT;

  impl->operation = REDIS_STREAM_GROUP_OPERATION_ACK;
  impl->ack_receipt = receipt;
  if (receipt->acknowledged) {
    impl->zero_budget = 1;
    return SALTS_OK;
  }
  status = redis_stream_group_open_ack(impl, receipt);
  if (status != SALTS_OK) {
    redis_stream_group_reset_operation(impl);
    return status;
  }
  return SALTS_OK;
}

static redis_stream_group_step redis_stream_group_finish_error(
    redis_stream_group_impl *impl, redis_stream_group_step step) {
  (void)redis_stream_group_command_destroy(&impl->command);
  redis_stream_group_queue_clear(impl);
  redis_stream_group_reset_operation(impl);
  return step;
}

static redis_stream_group_step redis_stream_group_drive_main(
    redis_stream_group_impl *impl) {
  for (;;) {
    redis_cflow_stream_step native =
        redis_stream_group_command_next(&impl->command);
    if (native.kind == REDIS_CFLOW_STREAM_WAIT) {
      redis_stream_group_step step = REDIS_STREAM_GROUP_STEP_INIT;
      step.kind = REDIS_STREAM_GROUP_WAIT;
      step.status = SALTS_OK;
      step.waitable = native.waitable;
      step.outcome = native.outcome;
      return step;
    }
    if (native.kind == REDIS_CFLOW_STREAM_ERROR)
      return redis_stream_group_native_error(native);
    if (native.kind == REDIS_CFLOW_STREAM_ITEM) {
      int status;
      if (impl->main_item_seen) {
        redis_reply_free(native.item);
        return redis_stream_group_error(SALTS_EPROTO);
      }
      impl->main_item_seen = 1;
      if (impl->operation == REDIS_STREAM_GROUP_OPERATION_FETCH)
        status = redis_stream_group_parse_fetch_reply(impl, native.item);
      else
        status = redis_stream_group_parse_claim_reply(impl, native.item);
      redis_reply_free(native.item);
      if (status != SALTS_OK)
        return redis_stream_group_error(status);
      continue;
    }
    if (native.kind == REDIS_CFLOW_STREAM_DONE) {
      int status;
      if (!impl->main_item_seen)
        return redis_stream_group_error(SALTS_EPROTO);
      status = redis_stream_group_command_destroy(&impl->command);
      if (status != SALTS_OK)
        return redis_stream_group_error(status);
      impl->main_complete = 1;
      return redis_stream_group_done();
    }
    redis_reply_free(native.item);
    return redis_stream_group_error(SALTS_EPROTO);
  }
}

static redis_stream_group_step redis_stream_group_drive_pending(
    redis_stream_group_impl *impl, redis_stream_group_receipt *receipt) {
  for (;;) {
    redis_cflow_stream_step native =
        redis_stream_group_command_next(&impl->command);
    if (native.kind == REDIS_CFLOW_STREAM_WAIT) {
      redis_stream_group_step step = REDIS_STREAM_GROUP_STEP_INIT;
      step.kind = REDIS_STREAM_GROUP_WAIT;
      step.status = SALTS_OK;
      step.waitable = native.waitable;
      step.outcome = native.outcome;
      return step;
    }
    if (native.kind == REDIS_CFLOW_STREAM_ERROR)
      return redis_stream_group_native_error(native);
    if (native.kind == REDIS_CFLOW_STREAM_ITEM) {
      int status;
      if (!impl->inspection_active) {
        redis_reply_free(native.item);
        return redis_stream_group_error(SALTS_EPROTO);
      }
      status = redis_stream_group_parse_pending_reply(
          impl, receipt, native.item);
      redis_reply_free(native.item);
      if (status != SALTS_OK)
        return redis_stream_group_error(status);
      impl->inspection_active = 0;
      continue;
    }
    if (native.kind == REDIS_CFLOW_STREAM_DONE) {
      int status;
      if (receipt->needs_delivery_count)
        return redis_stream_group_error(SALTS_EPROTO);
      status = redis_stream_group_command_destroy(&impl->command);
      if (status != SALTS_OK)
        return redis_stream_group_error(status);
      return redis_stream_group_done();
    }
    redis_reply_free(native.item);
    return redis_stream_group_error(SALTS_EPROTO);
  }
}

static redis_stream_group_step redis_stream_group_drive_ack(
    redis_stream_group_impl *impl) {
  for (;;) {
    redis_cflow_stream_step native =
        redis_stream_group_command_next(&impl->command);
    if (native.kind == REDIS_CFLOW_STREAM_WAIT) {
      redis_stream_group_step step = REDIS_STREAM_GROUP_STEP_INIT;
      step.kind = REDIS_STREAM_GROUP_WAIT;
      step.status = SALTS_OK;
      step.waitable = native.waitable;
      step.outcome = native.outcome;
      return step;
    }
    if (native.kind == REDIS_CFLOW_STREAM_ERROR)
      return redis_stream_group_native_error(native);
    if (native.kind == REDIS_CFLOW_STREAM_ITEM) {
      if (impl->ack_item_seen || native.item == NULL ||
          native.item->type != REDIS_REPLY_INTEGER ||
          native.item->integer < 0 || native.item->integer > 1) {
        redis_reply_free(native.item);
        return redis_stream_group_error(SALTS_EPROTO);
      }
      impl->ack_item_seen = 1;
      impl->ack_result_seen = 1;
      redis_reply_free(native.item);
      continue;
    }
    if (native.kind == REDIS_CFLOW_STREAM_DONE) {
      int status;
      if (!impl->ack_result_seen)
        return redis_stream_group_error(SALTS_EPROTO);
      status = redis_stream_group_command_destroy(&impl->command);
      if (status != SALTS_OK)
        return redis_stream_group_error(status);
      impl->ack_receipt->acknowledged = 1;
      redis_stream_group_reset_operation(impl);
      return redis_stream_group_done();
    }
    redis_reply_free(native.item);
    return redis_stream_group_error(SALTS_EPROTO);
  }
}

redis_stream_group_step redis_stream_group_next(
    redis_stream_group *owner) {
  redis_stream_group_impl *impl = redis_stream_group_get(owner);
  if (impl == NULL) return redis_stream_group_error(SALTS_EINVAL);
  if (impl->operation == REDIS_STREAM_GROUP_OPERATION_IDLE)
    return redis_stream_group_error(SALTS_EINVAL);

  if (impl->zero_budget) {
    if (impl->operation == REDIS_STREAM_GROUP_OPERATION_ACK &&
        impl->ack_receipt != NULL)
      impl->ack_receipt->acknowledged = 1;
    redis_stream_group_reset_operation(impl);
    return redis_stream_group_done();
  }

  if (impl->operation == REDIS_STREAM_GROUP_OPERATION_ACK) {
    redis_stream_group_step step = redis_stream_group_drive_ack(impl);
    if (step.kind == REDIS_STREAM_GROUP_ERROR)
      return redis_stream_group_finish_error(impl, step);
    return step;
  }

  for (;;) {
    redis_stream_group_receipt *receipt = impl->queue_head;
    if (receipt != NULL) {
      if (impl->operation == REDIS_STREAM_GROUP_OPERATION_CLAIM &&
          receipt->needs_delivery_count) {
        redis_stream_group_step step;
        int status;
        if (impl->command.kind == REDIS_STREAM_GROUP_COMMAND_NONE) {
          status = redis_stream_group_open_pending_inspection(impl, receipt);
          if (status != SALTS_OK)
            return redis_stream_group_finish_error(
                impl, redis_stream_group_error(status));
          impl->inspection_active = 1;
        }
        step = redis_stream_group_drive_pending(impl, receipt);
        if (step.kind == REDIS_STREAM_GROUP_WAIT) return step;
        if (step.kind == REDIS_STREAM_GROUP_ERROR)
          return redis_stream_group_finish_error(impl, step);
        continue;
      }

      receipt = redis_stream_group_queue_pop(impl);
      ++impl->active_receipts;
      {
        redis_stream_group_step step = REDIS_STREAM_GROUP_STEP_INIT;
        step.kind = REDIS_STREAM_GROUP_VALUE;
        step.status = SALTS_OK;
        step.outcome = REDIS_COMMAND_REPLIED;
        step.receipt = receipt;
        return step;
      }
    }

    if (impl->main_complete) {
      redis_stream_group_reset_operation(impl);
      return redis_stream_group_done();
    }

    {
      redis_stream_group_step step = redis_stream_group_drive_main(impl);
      if (step.kind == REDIS_STREAM_GROUP_WAIT) return step;
      if (step.kind == REDIS_STREAM_GROUP_ERROR)
        return redis_stream_group_finish_error(impl, step);
    }
  }
}

int redis_stream_group_cancel(redis_stream_group *owner) {
  redis_stream_group_impl *impl = redis_stream_group_get(owner);
  int status;
  if (impl == NULL) return SALTS_EINVAL;
  if (impl->command.kind != REDIS_STREAM_GROUP_COMMAND_NONE) {
    status = redis_stream_group_command_cancel(&impl->command);
    if (status != SALTS_OK) return status;
    status = redis_stream_group_command_destroy(&impl->command);
    if (status != SALTS_OK) return status;
  }
  redis_stream_group_queue_clear(impl);
  redis_stream_group_reset_operation(impl);
  return SALTS_OK;
}

int redis_stream_group_receipt_release(
    redis_stream_group_receipt *receipt) {
  redis_stream_group_impl *owner;
  if (receipt == NULL || receipt->owner == NULL)
    return SALTS_EINVAL;
  owner = receipt->owner;
  if (owner->operation == REDIS_STREAM_GROUP_OPERATION_ACK &&
      owner->ack_receipt == receipt)
    return SALTS_EBUSY;
  if (owner->active_receipts == 0u) return SALTS_EPROTO;
  --owner->active_receipts;
  receipt->owner = NULL;
  redis_stream_group_receipt_free_local(receipt);
  return SALTS_OK;
}

int redis_stream_group_receipt_retry(
    redis_stream_group_receipt *receipt) {
  return redis_stream_group_receipt_release(receipt);
}

redis_stream_group_receipt_kind redis_stream_group_receipt_kindof(
    const redis_stream_group_receipt *receipt) {
  return receipt != NULL ? receipt->kind
                         : REDIS_STREAM_GROUP_RECEIPT_DATA_LOSS;
}

int redis_stream_group_receipt_id(
    const redis_stream_group_receipt *receipt,
    const char **id, size_t *id_length) {
  if (receipt == NULL || id == NULL || id_length == NULL)
    return SALTS_EINVAL;
  *id = receipt->id;
  *id_length = receipt->id_length;
  return SALTS_OK;
}

uint64_t redis_stream_group_receipt_delivery_count(
    const redis_stream_group_receipt *receipt) {
  return receipt != NULL ? receipt->delivery_count : 0u;
}

size_t redis_stream_group_receipt_payload_bytes(
    const redis_stream_group_receipt *receipt) {
  return receipt != NULL ? receipt->payload_bytes : 0u;
}

size_t redis_stream_group_receipt_field_count(
    const redis_stream_group_receipt *receipt) {
  return receipt != NULL ? receipt->field_count : 0u;
}

int redis_stream_group_receipt_field_at(
    const redis_stream_group_receipt *receipt, size_t index,
    redis_stream_group_field_view *field) {
  const redis_stream_group_owned_field *source;
  if (receipt == NULL || field == NULL || index >= receipt->field_count)
    return SALTS_EINVAL;
  source = &receipt->fields[index];
  field->name = source->name;
  field->name_length = source->name_length;
  field->value = source->value;
  field->value_length = source->value_length;
  return SALTS_OK;
}

int redis_stream_group_receipt_find(
    const redis_stream_group_receipt *receipt,
    const char *name, size_t name_length,
    const char **value, size_t *value_length) {
  size_t index;
  if (receipt == NULL || name == NULL || name_length == 0u ||
      value == NULL || value_length == NULL)
    return SALTS_EINVAL;
  for (index = 0u; index < receipt->field_count; ++index) {
    const redis_stream_group_owned_field *field = &receipt->fields[index];
    if (field->name_length == name_length &&
        memcmp(field->name, name, name_length) == 0) {
      *value = field->value;
      *value_length = field->value_length;
      return SALTS_OK;
    }
  }
  *value = NULL;
  *value_length = 0u;
  return SALTS_ENOENT;
}

int redis_stream_group_receipt_u64(
    const redis_stream_group_receipt *receipt,
    const char *name, size_t name_length, uint64_t *value) {
  const char *text;
  size_t length;
  size_t index;
  uint64_t parsed = 0u;
  int status;
  if (value == NULL) return SALTS_EINVAL;
  status = redis_stream_group_receipt_find(
      receipt, name, name_length, &text, &length);
  if (status != SALTS_OK) return status;
  if (length == 0u) return SALTS_EPROTO;
  for (index = 0u; index < length; ++index) {
    uint64_t digit;
    if (text[index] < '0' || text[index] > '9') return SALTS_EPROTO;
    digit = (uint64_t)(text[index] - '0');
    if (parsed > (UINT64_MAX - digit) / 10u) return SALTS_EPROTO;
    parsed = parsed * 10u + digit;
  }
  *value = parsed;
  return SALTS_OK;
}

int redis_stream_group_receipt_acknowledged(
    const redis_stream_group_receipt *receipt) {
  return receipt != NULL && receipt->acknowledged != 0;
}

int redis_stream_group_destroy(redis_stream_group *owner) {
  redis_stream_group_impl *impl = redis_stream_group_get(owner);
  int status;
  if (impl == NULL) return SALTS_EINVAL;
  status = redis_stream_group_cancel(owner);
  if (status != SALTS_OK) return status;
  if (impl->active_receipts != 0u) return SALTS_EBUSY;
  free(impl->consumer);
  free(impl->group);
  free(impl->stream_key);
  free(impl);
  owner->impl = NULL;
  return SALTS_OK;
}

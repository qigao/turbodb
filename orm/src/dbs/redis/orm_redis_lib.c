#include "orm_redis_lib.h"

#include <redis_reply.h>

static orm_redis_reply_kind orm_redis_lib_kind(void *context,
                                               const void *reply_) {
  const redis_reply_t *reply = (const redis_reply_t *)reply_;
  (void)context;
  if (reply == NULL)
    return ORM_REDIS_REPLY_INVALID;
  switch (reply->type) {
    case REDIS_REPLY_NULL:
      return ORM_REDIS_REPLY_NULL;
    case REDIS_REPLY_INTEGER:
      return ORM_REDIS_REPLY_INTEGER;
    case REDIS_REPLY_STRING:
    case REDIS_REPLY_BULK_STRING:
      return ORM_REDIS_REPLY_STRING;
    case REDIS_REPLY_ARRAY:
      return ORM_REDIS_REPLY_ARRAY;
    default:
      return ORM_REDIS_REPLY_INVALID;
  }
}

static int64_t orm_redis_lib_integer(void *context, const void *reply) {
  (void)context;
  return ((const redis_reply_t *)reply)->integer;
}

static const unsigned char *orm_redis_lib_bytes(void *context,
                                                 const void *reply,
                                                 size_t *size) {
  const redis_reply_t *value = (const redis_reply_t *)reply;
  (void)context;
  if (size == NULL || value == NULL ||
      (value->type != REDIS_REPLY_STRING &&
       value->type != REDIS_REPLY_BULK_STRING))
    return NULL;
  *size = value->len;
  return (const unsigned char *)value->str;
}

static size_t orm_redis_lib_child_count(void *context, const void *reply) {
  (void)context;
  return ((const redis_reply_t *)reply)->element_count;
}

static const void *orm_redis_lib_child(void *context, const void *reply,
                                       size_t index) {
  const redis_reply_t *value = (const redis_reply_t *)reply;
  (void)context;
  if (value == NULL || value->type != REDIS_REPLY_ARRAY ||
      index >= value->element_count || value->elements == NULL)
    return NULL;
  return value->elements[index];
}

static const orm_redis_reply_ops orm_redis_lib_ops = {
    sizeof(orm_redis_reply_ops), ORM_REDIS_REPLY_OPS_ABI_VERSION,
    orm_redis_lib_kind, orm_redis_lib_integer, orm_redis_lib_bytes,
    orm_redis_lib_child_count, orm_redis_lib_child};

const orm_redis_reply_ops *orm_redis_lib_reply_ops(void) {
  return &orm_redis_lib_ops;
}

#include "orm_mongo_lib.h"

#include <mongoc/mongoc.h>

#include <stdio.h>
#include <stdlib.h>

typedef struct orm_mongo_lib_state {
  mongoc_cursor_t *cursor;
  char error_message[ORM_C_ERROR_MESSAGE_CAPACITY];
} orm_mongo_lib_state;

static void orm_mongo_lib_set_error(orm_error_t *error,
                                    orm_status_t status,
                                    const char *message) {
  if (error == NULL || error->struct_size < sizeof(*error))
    return;
  error->status = status;
  (void)snprintf(error->message, sizeof(error->message), "%s",
                 status == ORM_STATUS_OK
                     ? ""
                     : (message != NULL ? message
                                        : orm_status_message(status)));
}

static orm_status_t orm_mongo_lib_error_status(const bson_error_t *error) {
  if (error->domain == MONGOC_ERROR_SERVER_SELECTION ||
      error->domain == MONGOC_ERROR_STREAM ||
      error->domain == MONGOC_ERROR_CLIENT_AUTHENTICATE)
    return ORM_STATUS_CONNECTION_ERROR;
  return ORM_STATUS_DATASTORE_ERROR;
}

static orm_mongo_driver_step orm_mongo_lib_next(void *context,
                                                 const void **document) {
  orm_mongo_lib_state *state = (orm_mongo_lib_state *)context;
  orm_mongo_driver_step step = ORM_MONGO_DRIVER_STEP_INIT;
  const bson_t *next = NULL;
  bson_error_t error;
  if (state == NULL || document == NULL) {
    step.kind = ORM_MONGO_DRIVER_ERROR;
    step.status = ORM_STATUS_INVALID_ARGUMENT;
    step.message = "invalid MongoDB driver resume";
    return step;
  }
  if (mongoc_cursor_next(state->cursor, &next)) {
    *document = next;
    step.kind = ORM_MONGO_DRIVER_ROW;
    return step;
  }
  if (!mongoc_cursor_error(state->cursor, &error))
    return step;
  (void)snprintf(state->error_message, sizeof(state->error_message),
                 "iterate MongoDB cursor: %s", error.message);
  step.kind = ORM_MONGO_DRIVER_ERROR;
  step.status = orm_mongo_lib_error_status(&error);
  step.message = state->error_message;
  return step;
}

static orm_status_t orm_mongo_lib_find(void *context, const void *document_,
                                        const unsigned char *path,
                                        size_t path_size,
                                        orm_mongo_value *out) {
  const bson_t *document = (const bson_t *)document_;
  bson_iter_t root;
  bson_iter_t found;
  bson_type_t type;
  (void)context;
  if (document == NULL || path == NULL || path_size == 0u || out == NULL)
    return ORM_STATUS_INVALID_ARGUMENT;
  *out = (orm_mongo_value)ORM_MONGO_VALUE_INIT;
  if (!bson_iter_init(&root, document) ||
      !bson_iter_find_descendant(&root, (const char *)path, &found))
    return ORM_STATUS_OK;
  type = bson_iter_type(&found);
  switch (type) {
    case BSON_TYPE_NULL:
    case BSON_TYPE_UNDEFINED:
      return ORM_STATUS_OK;
    case BSON_TYPE_BOOL:
      out->kind = ORM_MONGO_VALUE_BOOL;
      out->data.boolean = bson_iter_bool(&found);
      return ORM_STATUS_OK;
    case BSON_TYPE_INT32:
      out->kind = ORM_MONGO_VALUE_SINT;
      out->data.sint = bson_iter_int32(&found);
      return ORM_STATUS_OK;
    case BSON_TYPE_INT64:
      out->kind = ORM_MONGO_VALUE_SINT;
      out->data.sint = bson_iter_int64(&found);
      return ORM_STATUS_OK;
    case BSON_TYPE_DOUBLE:
      out->kind = ORM_MONGO_VALUE_FLOAT;
      out->data.floating = bson_iter_double(&found);
      return ORM_STATUS_OK;
    case BSON_TYPE_UTF8: {
      uint32_t size = 0u;
      out->kind = ORM_MONGO_VALUE_STRING;
      out->data.slice.data =
          (const unsigned char *)bson_iter_utf8(&found, &size);
      out->data.slice.size = size;
      return out->data.slice.data != NULL || size == 0u
                 ? ORM_STATUS_OK
                 : ORM_STATUS_DATASTORE_ERROR;
    }
    case BSON_TYPE_BINARY: {
      bson_subtype_t subtype;
      uint32_t size = 0u;
      const uint8_t *data = NULL;
      bson_iter_binary(&found, &subtype, &size, &data);
      (void)subtype;
      out->kind = ORM_MONGO_VALUE_BYTES;
      out->data.slice.data = data;
      out->data.slice.size = size;
      return data != NULL || size == 0u ? ORM_STATUS_OK
                                        : ORM_STATUS_DATASTORE_ERROR;
    }
    default:
      return ORM_STATUS_TYPE_ERROR;
  }
}

static void orm_mongo_lib_destroy(void *context) {
  orm_mongo_lib_state *state = (orm_mongo_lib_state *)context;
  if (state == NULL)
    return;
  mongoc_cursor_destroy(state->cursor);
  free(state);
}

static const orm_mongo_driver_ops orm_mongo_lib_ops = {
    sizeof(orm_mongo_driver_ops), ORM_MONGO_DRIVER_OPS_ABI_VERSION,
    orm_mongo_lib_next, orm_mongo_lib_find, orm_mongo_lib_destroy};

orm_status_t orm_mongo_driver_from_cursor(orm_mongo_driver *out_driver,
                                          void **native_cursor,
                                          orm_error_t *error) {
  orm_mongo_lib_state *state;
  if (out_driver == NULL || out_driver->ops != NULL ||
      out_driver->context != NULL || native_cursor == NULL ||
      *native_cursor == NULL) {
    orm_mongo_lib_set_error(error, ORM_STATUS_INVALID_ARGUMENT,
                            "invalid MongoDB native cursor ownership");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  state = (orm_mongo_lib_state *)calloc(1u, sizeof(*state));
  if (state == NULL) {
    orm_mongo_lib_set_error(error, ORM_STATUS_OUT_OF_MEMORY, NULL);
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  state->cursor = (mongoc_cursor_t *)*native_cursor;
  *native_cursor = NULL;
  out_driver->ops = &orm_mongo_lib_ops;
  out_driver->context = state;
  orm_mongo_lib_set_error(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

#ifndef ORM_MONGO_QUERY_H
#define ORM_MONGO_QUERY_H

#include "orm_internal.h"

#include <bson/bson.h>

typedef struct orm_mongo_settings {
  tstr uri;
  tstr database;
  tstr id_column;
} orm_mongo_settings;

orm_status_t orm_mongo_append_filter(bson_t *out,
                                     const orm_query_plan *plan,
                                     const orm_mongo_settings *settings,
                                     const orm_limits *limits,
                                     orm_error_t *error);
orm_status_t orm_mongo_append_find_options(
    bson_t *out, const orm_query_plan *plan,
    const orm_mongo_settings *settings, orm_error_t *error);
orm_status_t orm_mongo_append_insert_document(
    bson_t *out, const orm_query_plan *plan,
    const orm_mongo_settings *settings, orm_error_t *error);
orm_status_t orm_mongo_append_update_document(
    bson_t *out, const orm_query_plan *plan,
    const orm_mongo_settings *settings, orm_error_t *error);

#endif

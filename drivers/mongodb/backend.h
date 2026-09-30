#ifndef ORM_MONGODB_BACKEND_H
#define ORM_MONGODB_BACKEND_H

#include "orm_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

orm_status_t orm_mongo_backend_create(
    const orm_config_t *config, const orm_limits *limits,
    orm_backend *out_backend, orm_error_t *error);

#ifdef __cplusplus
}
#endif

#endif

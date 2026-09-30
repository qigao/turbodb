#ifndef ORM_REDIS_LIB_H
#define ORM_REDIS_LIB_H

#include "orm_redis_cursor.h"

#ifdef __cplusplus
extern "C" {
#endif

const orm_redis_reply_ops *orm_redis_lib_reply_ops(void);

#ifdef __cplusplus
}
#endif

#endif

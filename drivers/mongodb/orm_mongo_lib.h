#ifndef ORM_MONGO_LIB_H
#define ORM_MONGO_LIB_H

#include "orm_mongo_cursor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Success moves and clears *native_cursor. */
orm_status_t orm_mongo_driver_from_cursor(orm_mongo_driver *out_driver,
                                          void **native_cursor,
                                          orm_error_t *error);

#ifdef __cplusplus
}
#endif

#endif

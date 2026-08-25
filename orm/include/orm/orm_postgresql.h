#ifndef ORM_POSTGRESQL_H
#define ORM_POSTGRESQL_H

#include "orm.h"

#if defined(ORM_POSTGRESQL_STATIC)
  #define ORM_POSTGRESQL_C_API
#elif defined(_WIN32)
  #if defined(ORM_POSTGRESQL_BUILD)
    #define ORM_POSTGRESQL_C_API __declspec(dllexport)
  #else
    #define ORM_POSTGRESQL_C_API __declspec(dllimport)
  #endif
#elif defined(ORM_POSTGRESQL_BUILD) && (defined(__GNUC__) || defined(__clang__))
  #define ORM_POSTGRESQL_C_API __attribute__((visibility("default")))
#else
  #define ORM_POSTGRESQL_C_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Registers the optional PostgreSQL driver with the linked ORM core.
 * Registration is process-wide, thread-safe, and idempotent.
 */
ORM_POSTGRESQL_C_API orm_status_t ORM_C_CALL
orm_postgresql_register(orm_error_t *error);

#ifdef __cplusplus
}
#endif

#endif

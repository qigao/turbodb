#ifndef ORM_POSTGRESQL_H
#define ORM_POSTGRESQL_H

#include "orm.h"

#if defined(ORM_POSTGRESQL_STATIC)
  #define ORM_POSTGRESQL_API
#elif defined(_WIN32)
  #if defined(ORM_POSTGRESQL_BUILD)
    #define ORM_POSTGRESQL_API __declspec(dllexport)
  #else
    #define ORM_POSTGRESQL_API __declspec(dllimport)
  #endif
#elif defined(ORM_POSTGRESQL_BUILD) && (defined(__GNUC__) || defined(__clang__))
  #define ORM_POSTGRESQL_API __attribute__((visibility("default")))
#else
  #define ORM_POSTGRESQL_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Creates a connection through the PostgreSQL component. The config and its
 * option views are borrowed only for this call. On failure, *out_connection is
 * set to NULL when out_connection is non-NULL.
 */
ORM_POSTGRESQL_API orm_status_t ORM_C_CALL orm_postgresql_connect(const orm_config_t *config,
                                                                  orm_connection_t **out_connection,
                                                                  orm_error_t *error);

#ifdef __cplusplus
}
#endif

#endif

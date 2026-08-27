#ifndef ORM_COMMAND_SOURCE_H
#define ORM_COMMAND_SOURCE_H

#include "orm.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { ORM_COMMAND_DRIVER_OPS_ABI_VERSION = 1u };

typedef struct orm_command_driver_result {
  orm_status_t status;
  uint64_t affected_rows;
  const char *message;
} orm_command_driver_result;

#define ORM_COMMAND_DRIVER_RESULT_INIT { ORM_STATUS_OK, 0u, NULL }

typedef struct orm_command_driver_ops {
  size_t struct_size;
  uint32_t abi_version;
  orm_command_driver_result (*execute)(void *context);
  void (*destroy)(void *context);
} orm_command_driver_ops;

typedef struct orm_command_driver {
  const orm_command_driver_ops *ops;
  void *context;
} orm_command_driver;

/* Success moves and clears driver. */
orm_status_t orm_command_source_init(cflow_source *out_source,
                                     orm_command_driver *driver,
                                     orm_error_t *error);

#ifdef __cplusplus
}
#endif

#endif

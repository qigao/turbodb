#ifndef ORM_MODULE_LOADER_H
#define ORM_MODULE_LOADER_H

#include "../abi/orm_internal.h"

typedef struct orm_module_handle {
  void *native;
} orm_module_handle;

orm_status_t orm_module_open_absolute(const char *utf8_path,
                                      orm_module_handle *out,
                                      orm_error_t *error);
orm_status_t orm_module_symbol(const orm_module_handle *module,
                               const char *name,
                               void *out_function, size_t function_bytes,
                               orm_error_t *error);
void orm_module_close(orm_module_handle *module);

#endif

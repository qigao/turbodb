#ifndef TURBODB_DBTOOL_CLI_H
#define TURBODB_DBTOOL_CLI_H

#include "dbtool_schema_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

dbtool_status dbtool_cli_execute(int argc, const char *const *argv,
                                 dbtool_driver_kind driver,
                                 const dbtool_schema_driver_ops *ops,
                                 dbtool_apply_result *result,
                                 dbtool_error *error);
void dbtool_cli_print_help(dbtool_driver_kind driver, const char *program);
void dbtool_cli_print_error(const dbtool_error *error);

#ifdef __cplusplus
}
#endif

#endif

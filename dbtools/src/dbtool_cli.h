#ifndef TURBODB_DBTOOL_CLI_H
#define TURBODB_DBTOOL_CLI_H

#include "dbtool_schema_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NULL ops selects the explicitly configured Plugin. Non-NULL operations are
 * borrowed for this call and let embedded callers supply an existing backend.
 * --driver selects sqlite, pg or mysql. selected_driver is optional and is
 * written after a successful command parse; parse failure or help leaves it unchanged. */
dbtool_status dbtool_cli_execute(int argc, const char *const *argv,
                                 dbtool_driver_kind *selected_driver,
                                 const dbtool_schema_driver_ops *ops,
                                 dbtool_apply_result *result,
                                 dbtool_error *error);
const char *dbtool_cli_driver_name(dbtool_driver_kind driver);
void dbtool_cli_print_help(const char *program);
void dbtool_cli_print_error(const dbtool_error *error);

#ifdef __cplusplus
}
#endif

#endif

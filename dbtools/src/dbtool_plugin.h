#ifndef TURBODB_DBTOOL_PLUGIN_H
#define TURBODB_DBTOOL_PLUGIN_H

#include "dbtool_schema_driver.h"
#include <salts/plugin.h>

typedef struct dbtool_plugin {
  salts_plugin_registry registry;
  salts_plugin_ref ref;
  salts_plugin_lease lease;
  const dbtool_schema_driver_ops *ops;
  bool started;
  bool stopping;
} dbtool_plugin;

/* Initialize with {0}. Load exactly one absolute path and verify its driver ID
 * and schema contract. Call close even after a failed load. The operations and
 * all plugin-owned contexts must be finished before close; no call may race it. */
dbtool_status dbtool_plugin_load(dbtool_plugin *plugin, const char *path,
                                const char *driver_id, dbtool_error *error);
/* Failure retains the unfinished lifecycle state so the caller can retry.
 * Success clears the handle. No module scan or alternate ABI is attempted. */
dbtool_status dbtool_plugin_close(dbtool_plugin *plugin, dbtool_error *error);

#endif

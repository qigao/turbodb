/**
 * @file redis_sentinel.h
 * @brief Redis Sentinel service discovery and failover client
 */

#ifndef REDIS_SENTINEL_H
#define REDIS_SENTINEL_H

#include "platform.h"
#include "redis_export.h"
#include "redis_client.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REDIS_SENTINEL_MAX_ENDPOINTS 64u

typedef struct redis_sentinel_s redis_sentinel_t;

/** Sentinel and data-node configuration. */
typedef struct {
  const char **sentinel_hosts;
  uint16_t *sentinel_ports;
  size_t sentinel_count;
  const char *service_name;

  /* Credentials used when talking to Sentinel instances. */
  const char *sentinel_username;
  const char *sentinel_password;

  /* Credentials and database used by discovered Redis masters. */
  const char *username;
  const char *password;
  int database;

  size_t min_connections;
  size_t max_connections;
  uint32_t sentinel_connect_timeout_ms;
  uint32_t sentinel_command_timeout_ms;
  uint32_t connect_timeout_ms;
  uint32_t command_timeout_ms;
  uint32_t idle_timeout_ms;
  uint32_t topology_refresh_ms;
} redis_sentinel_config_t;

#define REDIS_SENTINEL_CONFIG_DEFAULT                                                              \
  {.sentinel_hosts = NULL,                                                                         \
   .sentinel_ports = NULL,                                                                         \
   .sentinel_count = 0,                                                                            \
   .service_name = NULL,                                                                           \
   .sentinel_username = NULL,                                                                      \
   .sentinel_password = NULL,                                                                      \
   .username = NULL,                                                                               \
   .password = NULL,                                                                               \
   .database = 0,                                                                                  \
   .min_connections = 2,                                                                           \
   .max_connections = 10,                                                                          \
   .sentinel_connect_timeout_ms = 1000,                                                            \
   .sentinel_command_timeout_ms = 2000,                                                            \
   .connect_timeout_ms = 5000,                                                                     \
   .command_timeout_ms = 5000,                                                                     \
   .idle_timeout_ms = 60000,                                                                       \
   .topology_refresh_ms = 30000}

typedef struct {
  uint64_t commands_sent;
  uint64_t commands_failed;
  uint64_t discovery_attempts;
  uint64_t discovery_failures;
  uint64_t topology_refreshes;
  uint64_t failovers;
  uint64_t safe_retries;
} redis_sentinel_stats_t;

/**
 * Create a Sentinel client and copy its configuration.
 *
 * The object is event-loop affine and is not thread-safe. connect(), refresh(),
 * and command functions must run inside a CoroNet coroutine.
 */
REDIS_API redis_sentinel_t *redis_sentinel_create(const redis_sentinel_config_t *config);

/** Discover and validate the current master, then start its connection pool. */
REDIS_API int redis_sentinel_connect(redis_sentinel_t *sentinel);

/** Re-query Sentinel and atomically replace the current validated master pool. */
REDIS_API int redis_sentinel_refresh(redis_sentinel_t *sentinel);

/** Stop accepting commands and retire the current pool generation. */
REDIS_API void redis_sentinel_disconnect(redis_sentinel_t *sentinel);

/**
 * Destroy the Sentinel client.
 *
 * Pool generations borrowed by in-flight commands are drained before their
 * storage is released. The pointer must not be used after this call.
 */
REDIS_API void redis_sentinel_destroy(redis_sentinel_t *sentinel);

/**
 * Execute a binary-safe command against the discovered master.
 *
 * READONLY, MASTERDOWN, and commands rejected before sending may be retried
 * once after a successful master switch. Uncertain sends and unknown replies
 * are never retried. `out` owns its reply and must be cleared with
 * redis_command_result_clear().
 */
REDIS_API int redis_sentinel_commandv_result(redis_sentinel_t *sentinel, int argc,
                                             const char **argv, const size_t *argvlen,
                                             redis_command_result_t *out);

/** Callback variant; returns 0 whenever a final Redis reply was received. */
REDIS_API int redis_sentinel_commandv(redis_sentinel_t *sentinel, int argc, const char **argv,
                                      const size_t *argvlen, redis_command_cb_t callback,
                                      void *user_data);

/** Common command helpers. */
REDIS_API int redis_sentinel_set(redis_sentinel_t *sentinel, const char *key, const char *value,
                                 redis_command_cb_t callback, void *user_data);
REDIS_API int redis_sentinel_get(redis_sentinel_t *sentinel, const char *key,
                                 redis_command_cb_t callback, void *user_data);
REDIS_API int redis_sentinel_del(redis_sentinel_t *sentinel, const char *key,
                                 redis_command_cb_t callback, void *user_data);

/** Copy the current master endpoint into caller-owned storage. */
REDIS_API int redis_sentinel_get_master(const redis_sentinel_t *sentinel, char *host,
                                        size_t host_size, uint16_t *port);

REDIS_API int redis_sentinel_is_healthy(const redis_sentinel_t *sentinel);
REDIS_API void redis_sentinel_get_stats(const redis_sentinel_t *sentinel,
                                        redis_sentinel_stats_t *stats);
REDIS_API void redis_sentinel_reset_stats(redis_sentinel_t *sentinel);

#ifdef __cplusplus
}
#endif

#endif /* REDIS_SENTINEL_H */

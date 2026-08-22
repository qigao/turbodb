#ifndef REDIS_CLIENT_H
#define REDIS_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include "redis_export.h"
#include "platform.h"
#include "turbo_thread.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct redis_client_s redis_client_t;
typedef struct redis_command_s redis_command_t;
typedef struct redis_reply_s redis_reply_t;
typedef struct redis_subscription_s redis_subscription_t;
typedef struct coro_context_s coro_context_t;
typedef struct coro_socket_s coro_socket_t;

/* Pub/Sub message callback (forward declaration) */
typedef void (*redis_pubsub_cb_t)(redis_client_t *client,
                                  const char *channel,
                                  const void *message, size_t len,
                                  void *user_data);

/* RESP reply types */
typedef enum {
    REDIS_REPLY_STRING,      /* Simple string: +OK\r\n */
    REDIS_REPLY_ERROR,       /* Error: -ERR message\r\n */
    REDIS_REPLY_INTEGER,     /* Integer: :1000\r\n */
    REDIS_REPLY_BULK_STRING, /* Bulk string: $6\r\nfoobar\r\n */
    REDIS_REPLY_ARRAY,       /* Array: *2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n */
    REDIS_REPLY_NULL         /* Null: $-1\r\n */
} redis_reply_type_t;

/* Redis reply structure */
struct redis_reply_s {
    redis_reply_type_t type;
    int64_t integer;           /* For REDIS_REPLY_INTEGER */
    char *str;                 /* For STRING, ERROR, BULK_STRING */
    size_t len;                /* Length of str */
    redis_reply_t **elements;  /* For REDIS_REPLY_ARRAY */
    size_t element_count;      /* Number of array elements */
};

/** Redis command transport/application completion state. */
typedef enum {
    REDIS_COMMAND_NOT_SENT,
    REDIS_COMMAND_SEND_UNCERTAIN,
    REDIS_COMMAND_REPLY_UNKNOWN,
    REDIS_COMMAND_REPLIED
} redis_command_outcome_t;

/** Stable classification of the first token in a Redis error reply. */
typedef enum {
    REDIS_SERVER_ERROR_NONE,
    REDIS_SERVER_ERROR_ERR,
    REDIS_SERVER_ERROR_BUSY_GROUP,
    REDIS_SERVER_ERROR_NO_GROUP,
    REDIS_SERVER_ERROR_WRONG_TYPE,
    REDIS_SERVER_ERROR_NO_AUTH,
    REDIS_SERVER_ERROR_MOVED,
    REDIS_SERVER_ERROR_ASK,
    REDIS_SERVER_ERROR_TRY_AGAIN,
    REDIS_SERVER_ERROR_CLUSTER_DOWN,
    REDIS_SERVER_ERROR_READ_ONLY,
    REDIS_SERVER_ERROR_NO_SCRIPT,
    REDIS_SERVER_ERROR_LOADING,
    REDIS_SERVER_ERROR_OOM,
    REDIS_SERVER_ERROR_EXEC_ABORT,
    REDIS_SERVER_ERROR_MASTER_DOWN,
    REDIS_SERVER_ERROR_MISCONF,
    REDIS_SERVER_ERROR_UNKNOWN
} redis_server_error_t;

/**
 * Owned result of one command. `reply` must be released with
 * redis_command_result_clear().
 */
typedef struct {
    int status;
    redis_command_outcome_t outcome;
    redis_server_error_t server_error;
    redis_reply_t *reply;
} redis_command_result_t;

#define REDIS_COMMAND_RESULT_INIT \
    { 0, REDIS_COMMAND_NOT_SENT, REDIS_SERVER_ERROR_NONE, NULL }

/* Command callback */
typedef void (*redis_command_cb_t)(redis_client_t *client, redis_reply_t *reply, void *user_data);

/* Connection callback */
typedef void (*redis_connect_cb_t)(redis_client_t *client, int status, void *user_data);

/* Redis client configuration */
typedef struct {
    const char *host;
    uint16_t port;
    const char *username;      /* Optional ACL username; requires password */
    const char *password;      /* Optional auth password */
    int database;              /* Database number (0-15) */
    uint32_t timeout_ms;       /* Connection timeout */
    uint32_t command_timeout_ms; /* Command timeout */
    size_t max_pipeline;       /* Max pipelined commands */
    int cluster_readonly;      /* Send READONLY when preparing this connection */
} redis_config_t;

/* Redis client structure */
struct redis_client_s {
    coro_context_t *ctx;
    coro_socket_t *socket;
    int owns_socket;
    redis_config_t config;

    /* Connection state */
    int is_connected;
    int is_authenticated;
    int selected_db;
    int is_cluster_readonly;

    /* Compatibility-only queue state retained for public struct stability. */
    redis_command_t *command_queue;
    redis_command_t *command_queue_tail;
    size_t queued_commands;

    /* Current parsing state */
    char *recv_buffer;
    size_t recv_buffer_size;
    size_t recv_buffer_used;

    /* Callbacks */
    redis_connect_cb_t connect_cb;
    void *connect_user_data;

    /* Pub/Sub state */
    int is_subscriber;              /* In subscriber mode */
    redis_subscription_t *subscriptions;  /* Active subscriptions */
    redis_pubsub_cb_t pubsub_cb;    /* Global pubsub callback */
    void *pubsub_user_data;

    /* User data */
    void *user_data;

    /* Serializes cross-thread wait interruption with socket publish/take. */
    turbo_mutex_t socket_mutex;
};

/* API Functions */

/**
 * Create Redis client with default configuration
 * @param host Redis server host
 * @param port Redis server port
 * @return Redis client instance or NULL on error
 */
REDIS_API redis_client_t* redis_client_create(const char *host, uint16_t port);

/**
 * Create Redis client with custom configuration
 * @param config Redis configuration
 * @return Redis client instance or NULL on error
 */
REDIS_API redis_client_t* redis_client_create_with_config(const redis_config_t *config);

/**
 * Connect to Redis server
 * @param client Redis client
 * @param callback Connection callback
 * @param user_data User data for callback
 * @return 0 on success, negative on error
 */
REDIS_API int redis_client_connect(redis_client_t *client, redis_connect_cb_t callback, void *user_data);

/**
 * Complete configured AUTH/SELECT setup on an already connected socket.
 *
 * This is used by protocol-aware connection pools after transport connect.
 * It is idempotent for the state tracked by `client` and returns a Redis,
 * protocol, or transport error without hiding an uncertain command outcome.
 */
REDIS_API int redis_client_prepare(redis_client_t *client);

/**
 * Attach an externally-owned CoroNet socket to a Redis client.
 *
 * The client will use the socket for command I/O. Ownership remains with the
 * caller unless transferred via `take_ownership`.
 */
REDIS_API int redis_client_attach_socket(redis_client_t *client,
                                         coro_context_t *ctx,
                                         coro_socket_t *socket,
                                         int take_ownership);

/**
 * Detach the current socket from a Redis client without destroying it.
 *
 * Returns the detached socket or NULL if none.
 */
REDIS_API coro_socket_t *redis_client_detach_socket(redis_client_t *client);

/**
 * Execute Redis command
 * @param client Redis client
 * @param callback Command callback
 * @param user_data User data for callback
 * @param format Command format string (e.g., "SET %s %s")
 * @param ... Command arguments
 * @return 0 on success, negative on error
 */
REDIS_API int redis_command(redis_client_t *client, redis_command_cb_t callback,
                  void *user_data, const char *format, ...);

/**
 * Execute Redis command with argv
 * @param client Redis client
 * @param argc Number of arguments
 * @param argv Array of argument strings
 * @param argvlen Array of argument lengths
 * @param callback Command callback
 * @param user_data User data for callback
 * @return 0 on success, negative on error
 */
REDIS_API int redis_commandv(redis_client_t *client, int argc, const char **argv,
                   const size_t *argvlen, redis_command_cb_t callback, void *user_data);

/**
 * Execute one command and retain its exact completion state.
 *
 * On return, `out` owns `out->reply`. Call redis_command_result_clear() even
 * after failure. REDIS_COMMAND_SEND_UNCERTAIN and
 * REDIS_COMMAND_REPLY_UNKNOWN mean that a mutating command must not be retried
 * blindly. The call suspends the current coroutine while waiting for I/O.
 *
 * @param client Connected Redis client
 * @param argc Number of command arguments
 * @param argv Argument byte strings
 * @param argvlen Optional lengths; NULL treats arguments as null-terminated
 * @param out Required owned result initialized by this function
 * @return TURBO_OK for a non-error reply, TURBO_EIO for a Redis error reply,
 *         or a transport/validation error
 */
REDIS_API int redis_commandv_result(redis_client_t *client, int argc,
                                    const char **argv, const size_t *argvlen,
                                    redis_command_result_t *out);

/** Release an owned command reply and reset the result to NOT_SENT. */
REDIS_API void redis_command_result_clear(redis_command_result_t *result);

/** Classify a RESP error by its stable leading token; non-errors return NONE. */
REDIS_API redis_server_error_t redis_server_error_classify(const redis_reply_t *reply);

/**
 * Disconnect from Redis server
 * @param client Redis client
 */
REDIS_API void redis_client_disconnect(redis_client_t *client);

/**
 * Interrupt the current CoroNet socket wait from another thread.
 *
 * Socket ownership remains with the Redis client. Returns TURBO_ENOTCONN when
 * no socket is currently published.
 */
REDIS_API int redis_client_interrupt(redis_client_t *client, int status);

/**
 * Destroy Redis client
 * @param client Redis client
 */
REDIS_API void redis_client_destroy(redis_client_t *client);

/**
 * Free Redis reply
 * @param reply Redis reply to free
 */
REDIS_API void redis_reply_free(redis_reply_t *reply);

/**
 * Get error message from client
 * @param client Redis client
 * @return Error message or NULL
 */
REDIS_API const char* redis_client_get_error(redis_client_t *client);

/* Convenience functions for common commands */

/**
 * SET key value
 */
REDIS_API int redis_set(redis_client_t *client, const char *key, const char *value,
              redis_command_cb_t callback, void *user_data);

/**
 * GET key
 */
REDIS_API int redis_get(redis_client_t *client, const char *key,
              redis_command_cb_t callback, void *user_data);

/**
 * DEL key [key ...]
 */
REDIS_API int redis_del(redis_client_t *client, int key_count, const char **keys,
              redis_command_cb_t callback, void *user_data);

/**
 * EXISTS key
 */
REDIS_API int redis_exists(redis_client_t *client, const char *key,
                 redis_command_cb_t callback, void *user_data);

/**
 * EXPIRE key seconds
 */
REDIS_API int redis_expire(redis_client_t *client, const char *key, int seconds,
                 redis_command_cb_t callback, void *user_data);

/**
 * INCR key
 */
REDIS_API int redis_incr(redis_client_t *client, const char *key,
               redis_command_cb_t callback, void *user_data);

/**
 * LPUSH key value [value ...]
 */
REDIS_API int redis_lpush(redis_client_t *client, const char *key, int value_count, const char **values,
                redis_command_cb_t callback, void *user_data);

/**
 * RPUSH key value [value ...]
 */
REDIS_API int redis_rpush(redis_client_t *client, const char *key, int value_count, const char **values,
                redis_command_cb_t callback, void *user_data);

/**
 * LPOP key
 */
REDIS_API int redis_lpop(redis_client_t *client, const char *key,
               redis_command_cb_t callback, void *user_data);

/**
 * RPOP key
 */
REDIS_API int redis_rpop(redis_client_t *client, const char *key,
               redis_command_cb_t callback, void *user_data);

/**
 * HSET key field value
 */
REDIS_API int redis_hset(redis_client_t *client, const char *key, const char *field, const char *value,
               redis_command_cb_t callback, void *user_data);

/**
 * HGET key field
 */
REDIS_API int redis_hget(redis_client_t *client, const char *key, const char *field,
               redis_command_cb_t callback, void *user_data);

/**
 * SADD key member [member ...]
 */
REDIS_API int redis_sadd(redis_client_t *client, const char *key, int member_count, const char **members,
               redis_command_cb_t callback, void *user_data);

/**
 * SMEMBERS key
 */
REDIS_API int redis_smembers(redis_client_t *client, const char *key,
                   redis_command_cb_t callback, void *user_data);

/**
 * PING
 */
REDIS_API int redis_ping(redis_client_t *client, redis_command_cb_t callback, void *user_data);

/* =============================================================================
 * Bloom Filter API
 * =============================================================================
 */

/**
 * BF.RESERVE key error_rate capacity
 */
REDIS_API int redis_bf_reserve(redis_client_t *client, const char *key, double error_rate, int capacity,
                     redis_command_cb_t callback, void *user_data);

/**
 * BF.ADD key item
 */
REDIS_API int redis_bf_add(redis_client_t *client, const char *key, const char *item,
                 redis_command_cb_t callback, void *user_data);

/**
 * BF.EXISTS key item
 */
REDIS_API int redis_bf_exists(redis_client_t *client, const char *key, const char *item,
                    redis_command_cb_t callback, void *user_data);

/**
 * BF.MADD key item [item ...]
 */
REDIS_API int redis_bf_madd(redis_client_t *client, const char *key, int item_count, const char **items,
                  redis_command_cb_t callback, void *user_data);

/* =============================================================================
 * Redis Streams API (for reliable message queuing)
 * =============================================================================
 */

/**
 * Stream entry structure
 */
typedef struct {
    char *id;                  /* Entry ID (e.g., "1234567890123-0") */
    char **fields;             /* Field names */
    char **values;             /* Field values */
    size_t *value_lens;        /* Value lengths (for binary data) */
    size_t field_count;        /* Number of fields */
} redis_stream_entry_t;

/**
 * Stream read result
 */
typedef struct {
    char *stream_name;         /* Stream key name */
    redis_stream_entry_t *entries;
    size_t entry_count;
} redis_stream_result_t;

/** Owned result of XREAD/XREADGROUP. */
typedef struct {
    redis_command_result_t command;
    redis_stream_result_t *streams;
    size_t stream_count;
} redis_stream_read_result_t;

#define REDIS_STREAM_READ_RESULT_INIT \
    { REDIS_COMMAND_RESULT_INIT, NULL, 0u }

/**
 * Stream message callback
 */
typedef void (*redis_stream_cb_t)(redis_client_t *client,
                                   redis_stream_result_t *results,
                                   size_t result_count,
                                   void *user_data);

/**
 * XADD key [MAXLEN ~ count] * field value [field value ...]
 * Add entry to stream
 *
 * @param client Redis client
 * @param key Stream key
 * @param maxlen Max stream length (0 = unlimited)
 * @param field_count Number of field-value pairs
 * @param fields Array of field names
 * @param values Array of values
 * @param value_lens Array of value lengths (NULL for null-terminated strings)
 * @param callback Callback receives entry ID
 * @param user_data User data
 * @return 0 on success
 */
REDIS_API int redis_xadd(redis_client_t *client, const char *key, size_t maxlen,
               size_t field_count, const char **fields,
               const char **values, const size_t *value_lens,
               redis_command_cb_t callback, void *user_data);

/** Owned-result variant of redis_xadd(); clear `out` after use. */
REDIS_API int redis_xadd_result(redis_client_t *client, const char *key, size_t maxlen,
                                size_t field_count, const char **fields,
                                const char **values, const size_t *value_lens,
                                redis_command_result_t *out);

/**
 * XREAD [COUNT count] [BLOCK ms] STREAMS key [key ...] id [id ...]
 * Read from streams
 *
 * @param client Redis client
 * @param count Max entries to read (0 = all available)
 * @param block_ms Block timeout in ms (0 = no block, -1 = forever)
 * @param stream_count Number of streams
 * @param keys Stream keys
 * @param ids Entry IDs ("$" for new, "0" for all)
 * @param callback Stream callback
 * @param user_data User data
 * @return 0 on success
 */
REDIS_API int redis_xread(redis_client_t *client, size_t count, int block_ms,
                size_t stream_count, const char **keys, const char **ids,
                redis_stream_cb_t callback, void *user_data);

/** Owned-result variant of redis_xread(); clear `out` after use. */
REDIS_API int redis_xread_result(redis_client_t *client, size_t count, int block_ms,
                                 size_t stream_count, const char **keys,
                                 const char **ids, redis_stream_read_result_t *out);

/**
 * XGROUP CREATE key group id [MKSTREAM]
 * Create consumer group
 *
 * @param client Redis client
 * @param key Stream key
 * @param group Group name
 * @param id Start ID ("$" for new messages, "0" for all)
 * @param mkstream Create stream if not exists
 * @param callback Command callback
 * @param user_data User data
 * @return 0 on success
 */
REDIS_API int redis_xgroup_create(redis_client_t *client, const char *key,
                        const char *group, const char *id, int mkstream,
                        redis_command_cb_t callback, void *user_data);

/** Owned-result variant of redis_xgroup_create(); clear `out` after use. */
REDIS_API int redis_xgroup_create_result(redis_client_t *client, const char *key,
                                         const char *group, const char *id,
                                         int mkstream, redis_command_result_t *out);

/**
 * XREADGROUP GROUP group consumer [COUNT count] [BLOCK ms] STREAMS key [key ...] id [id ...]
 * Read from stream as consumer group member
 *
 * @param client Redis client
 * @param group Group name
 * @param consumer Consumer name
 * @param count Max entries (0 = all)
 * @param block_ms Block timeout (0 = no block)
 * @param stream_count Number of streams
 * @param keys Stream keys
 * @param ids Entry IDs (">" for new messages)
 * @param callback Stream callback
 * @param user_data User data
 * @return 0 on success
 */
REDIS_API int redis_xreadgroup(redis_client_t *client, const char *group, const char *consumer,
                     size_t count, int block_ms,
                     size_t stream_count, const char **keys, const char **ids,
                     redis_stream_cb_t callback, void *user_data);

/** Owned-result variant of redis_xreadgroup(); clear `out` after use. */
REDIS_API int redis_xreadgroup_result(redis_client_t *client, const char *group,
                                      const char *consumer, size_t count,
                                      int block_ms, size_t stream_count,
                                      const char **keys, const char **ids,
                                      redis_stream_read_result_t *out);

/** Release all owned stream entries plus the underlying command reply. */
REDIS_API void redis_stream_read_result_clear(redis_stream_read_result_t *result);

/**
 * XACK key group id [id ...]
 * Acknowledge processed messages
 *
 * @param client Redis client
 * @param key Stream key
 * @param group Group name
 * @param id_count Number of IDs
 * @param ids Entry IDs to acknowledge
 * @param callback Callback receives count of acknowledged
 * @param user_data User data
 * @return 0 on success
 */
REDIS_API int redis_xack(redis_client_t *client, const char *key, const char *group,
               size_t id_count, const char **ids,
               redis_command_cb_t callback, void *user_data);

/** Owned-result variant of redis_xack(); clear `out` after use. */
REDIS_API int redis_xack_result(redis_client_t *client, const char *key,
                                const char *group, size_t id_count,
                                const char **ids, redis_command_result_t *out);

/**
 * XDEL key id [id ...]
 * Delete entries from stream
 */
REDIS_API int redis_xdel(redis_client_t *client, const char *key,
               size_t id_count, const char **ids,
               redis_command_cb_t callback, void *user_data);

/**
 * XLEN key
 * Get stream length
 */
REDIS_API int redis_xlen(redis_client_t *client, const char *key,
               redis_command_cb_t callback, void *user_data);

/**
 * XTRIM key MAXLEN [~] count
 * Trim stream to max length
 */
REDIS_API int redis_xtrim(redis_client_t *client, const char *key, size_t maxlen,
                redis_command_cb_t callback, void *user_data);

/**
 * Free stream entry
 */
REDIS_API void redis_stream_entry_free(redis_stream_entry_t *entry);

/**
 * @brief Transfer one owned Stream field value out of an entry.
 *
 * On success, the selected value is removed from @p entry, so a later
 * redis_stream_entry_free() or redis_stream_result_free() will not release it.
 * The caller owns @p out_value and must release it with
 * redis_stream_value_free(). Binary and empty values are supported.
 *
 * @param entry Owned Stream entry to modify
 * @param index Field value index
 * @param out_value Receives the transferred value
 * @param out_len Receives the binary value length
 * @return TURBO_OK on success, TURBO_EINVAL for invalid arguments,
 *         TURBO_ERANGE for an invalid index, or TURBO_ENOENT when the value
 *         is absent or was already transferred
 *
 * @code
 * char *value = NULL;
 * size_t value_len = 0;
 * if (redis_stream_entry_take_value(entry, 0, &value, &value_len) == TURBO_OK) {
 *   consume_binary(value, value_len);
 *   redis_stream_value_free(value);
 * }
 * @endcode
 */
REDIS_API int redis_stream_entry_take_value(redis_stream_entry_t *entry, size_t index,
                                             char **out_value, size_t *out_len);

/**
 * @brief Release a value returned by redis_stream_entry_take_value().
 * @param value Transferred Stream value; NULL is accepted
 */
REDIS_API void redis_stream_value_free(void *value);

/**
 * Free stream result
 */
REDIS_API void redis_stream_result_free(redis_stream_result_t *result, size_t count);

/**
 * Decode an owned RESP XREAD/XREADGROUP reply into typed Stream results.
 *
 * The returned array is independent of `reply` and must be released with
 * redis_stream_result_free(). A NULL Redis reply decodes as an empty result.
 *
 * @param reply RESP reply to decode
 * @param out Receives an owned result array
 * @param out_count Receives the number of stream results
 * @return TURBO_OK on success or a validation/allocation/protocol error
 */
REDIS_API int redis_stream_reply_decode(const redis_reply_t *reply,
                                        redis_stream_result_t **out,
                                        size_t *out_count);

/**
 * XRANGE key start end [COUNT count]
 * Read entries in ascending ID order.
 *
 * @param client  Redis client
 * @param key     Stream key
 * @param start   Start ID ("-" for the oldest)
 * @param end     End ID ("+" for the newest)
 * @param count   Max entries to return (0 = unlimited)
 * @param callback Stream callback
 * @param user_data User data
 * @return 0 on success
 */
REDIS_API int redis_xrange(redis_client_t *client, const char *key,
               const char *start, const char *end, size_t count,
               redis_stream_cb_t callback, void *user_data);

/**
 * XREVRANGE key end start [COUNT count]
 * Read entries in descending ID order.
 *
 * @param client  Redis client
 * @param key     Stream key
 * @param end     End ID ("+" for the newest)
 * @param start   Start ID ("-" for the oldest)
 * @param count   Max entries to return (0 = unlimited)
 * @param callback Stream callback
 * @param user_data User data
 * @return 0 on success
 */
REDIS_API int redis_xrevrange(redis_client_t *client, const char *key,
                  const char *end, const char *start, size_t count,
                  redis_stream_cb_t callback, void *user_data);

/**
 * Pending entry (PEL) descriptor returned by XPENDING summary or detail form.
 * Summary form (consumer == NULL): id/consumer/idle_ms/delivery_count are set.
 * Detail form: all fields are set.
 */
typedef struct {
    char    *id;             /* Entry ID */
    char    *consumer;       /* Owner consumer name */
    int64_t  idle_ms;        /* Milliseconds since last delivery */
    int64_t  delivery_count; /* Number of times delivered */
} redis_xpending_entry_t;

/**
 * XPENDING key group [[IDLE min-idle-time] start end count [consumer]]
 * Inspect the Pending Entries List (PEL).
 *
 * Summary form  : pass start=NULL, end=NULL, count=0, consumer=NULL.
 *                 The raw RESP reply is delivered via the generic callback.
 * Detail form   : pass non-NULL start/end and count > 0.
 *                 Parsed results are delivered as redis_xpending_entry_t[].
 *
 * For simplicity this binding always uses the detail form when start != NULL.
 *
 * @param client   Redis client
 * @param key      Stream key
 * @param group    Consumer group
 * @param start    Start ID (e.g. "-"); NULL → summary form
 * @param end      End ID (e.g. "+")
 * @param count    Max entries; 0 in summary form
 * @param consumer Filter by consumer name (NULL = all consumers)
 * @param callback Generic command callback (receives raw RESP array)
 * @param user_data User data
 * @return 0 on success
 */
REDIS_API int redis_xpending(redis_client_t *client, const char *key,
                const char *group,
                const char *start, const char *end, size_t count,
                const char *consumer,
                redis_command_cb_t callback, void *user_data);

/**
 * XCLAIM key group consumer min-idle-time id [id ...]
 * Transfer ownership of pending messages whose idle time exceeds min_idle_ms.
 *
 * @param client      Redis client
 * @param key         Stream key
 * @param group       Consumer group
 * @param consumer    New owner consumer
 * @param min_idle_ms Minimum idle time threshold (ms)
 * @param id_count    Number of entry IDs
 * @param ids         Entry IDs to claim
 * @param callback    Stream callback (receives claimed entries)
 * @param user_data   User data
 * @return 0 on success
 */
REDIS_API int redis_xclaim(redis_client_t *client, const char *key,
               const char *group, const char *consumer,
               int64_t min_idle_ms,
               size_t id_count, const char **ids,
               redis_stream_cb_t callback, void *user_data);

/**
 * XAUTOCLAIM key group consumer min-idle-time start [COUNT count]
 * Automatically claim idle pending messages (Redis 6.2+).
 *
 * @param client      Redis client
 * @param key         Stream key
 * @param group       Consumer group
 * @param consumer    New owner consumer
 * @param min_idle_ms Minimum idle time threshold (ms)
 * @param start       Start ID ("0-0" for beginning)
 * @param count       Max entries to claim (0 = server default)
 * @param callback    Stream callback (receives claimed entries)
 * @param user_data   User data
 * @return 0 on success
 */
REDIS_API int redis_xautoclaim(redis_client_t *client, const char *key,
                   const char *group, const char *consumer,
                   int64_t min_idle_ms, const char *start, size_t count,
                   redis_stream_cb_t callback, void *user_data);

/**
 * XGROUP SETID key group id
 * Reposition the last-delivered-ID of a consumer group.
 *
 * @param id  New last-delivered ID; "$" = latest, "0" = replay all
 */
REDIS_API int redis_xgroup_setid(redis_client_t *client, const char *key,
                     const char *group, const char *id,
                     redis_command_cb_t callback, void *user_data);

/**
 * XGROUP DESTROY key group
 * Delete a consumer group and its PEL.
 */
REDIS_API int redis_xgroup_destroy(redis_client_t *client, const char *key,
                       const char *group,
                       redis_command_cb_t callback, void *user_data);

/**
 * XGROUP CREATECONSUMER key group consumer  (Redis 6.2+)
 * Explicitly create a consumer without requiring it to read first.
 */
REDIS_API int redis_xgroup_createconsumer(redis_client_t *client, const char *key,
                              const char *group, const char *consumer,
                              redis_command_cb_t callback, void *user_data);

/**
 * XGROUP DELCONSUMER key group consumer
 * Remove a consumer and release its PEL entries.
 */
REDIS_API int redis_xgroup_delconsumer(redis_client_t *client, const char *key,
                           const char *group, const char *consumer,
                           redis_command_cb_t callback, void *user_data);

/* =============================================================================
 * Redis Pub/Sub API (for simple real-time broadcast)
 * =============================================================================
 */

/**
 * PUBLISH channel message
 * Publish message to channel
 *
 * @param client Redis client
 * @param channel Channel name
 * @param message Message data
 * @param len Message length
 * @param callback Callback receives subscriber count
 * @param user_data User data
 * @return 0 on success
 */
REDIS_API int redis_publish(redis_client_t *client, const char *channel,
                  const void *message, size_t len,
                  redis_command_cb_t callback, void *user_data);

/**
 * SUBSCRIBE channel [channel ...]
 * Subscribe to channels
 *
 * @param client Redis client
 * @param channel_count Number of channels
 * @param channels Channel names
 * @param on_message Message callback
 * @param user_data User data
 * @return 0 on success
 */
REDIS_API int redis_subscribe(redis_client_t *client, size_t channel_count,
                    const char **channels, redis_pubsub_cb_t on_message,
                    void *user_data);

/**
 * PSUBSCRIBE pattern [pattern ...]
 * Subscribe to channel patterns
 */
REDIS_API int redis_psubscribe(redis_client_t *client, size_t pattern_count,
                     const char **patterns, redis_pubsub_cb_t on_message,
                     void *user_data);

/**
 * UNSUBSCRIBE [channel ...]
 * Unsubscribe from channels
 */
REDIS_API int redis_unsubscribe(redis_client_t *client, size_t channel_count,
                      const char **channels);

/**
 * PUNSUBSCRIBE [pattern ...]
 * Unsubscribe from patterns
 */
REDIS_API int redis_punsubscribe(redis_client_t *client, size_t pattern_count,
                       const char **patterns);

/* =============================================================================
 * Extended String API
 * =============================================================================
 */

/** MSET key value [key value ...] */
REDIS_API int redis_mset(redis_client_t *client,
               int pair_count, const char **keys, const char **values,
               redis_command_cb_t callback, void *user_data);

/** MGET key [key ...] */
REDIS_API int redis_mget(redis_client_t *client,
               int key_count, const char **keys,
               redis_command_cb_t callback, void *user_data);

/** SETNX key value  (SET if Not eXists) */
REDIS_API int redis_setnx(redis_client_t *client, const char *key, const char *value,
                redis_command_cb_t callback, void *user_data);

/** SETEX key seconds value */
REDIS_API int redis_setex(redis_client_t *client, const char *key, int seconds,
                const char *value,
                redis_command_cb_t callback, void *user_data);

/** PSETEX key milliseconds value */
REDIS_API int redis_psetex(redis_client_t *client, const char *key, int64_t ms,
                 const char *value,
                 redis_command_cb_t callback, void *user_data);

/** GETSET key value  (atomic get-then-set; deprecated in Redis 6.2 but still usable) */
REDIS_API int redis_getset(redis_client_t *client, const char *key, const char *value,
                 redis_command_cb_t callback, void *user_data);

/** GETDEL key  (Redis 6.2+) */
REDIS_API int redis_getdel(redis_client_t *client, const char *key,
                 redis_command_cb_t callback, void *user_data);

/** INCRBY key increment */
REDIS_API int redis_incrby(redis_client_t *client, const char *key, int64_t increment,
                 redis_command_cb_t callback, void *user_data);

/** DECRBY key decrement */
REDIS_API int redis_decrby(redis_client_t *client, const char *key, int64_t decrement,
                 redis_command_cb_t callback, void *user_data);

/** DECR key */
REDIS_API int redis_decr(redis_client_t *client, const char *key,
               redis_command_cb_t callback, void *user_data);

/** INCRBYFLOAT key increment */
REDIS_API int redis_incrbyfloat(redis_client_t *client, const char *key, double increment,
                     redis_command_cb_t callback, void *user_data);

/** APPEND key value */
REDIS_API int redis_append(redis_client_t *client, const char *key, const char *value,
                 redis_command_cb_t callback, void *user_data);

/** STRLEN key */
REDIS_API int redis_strlen(redis_client_t *client, const char *key,
                 redis_command_cb_t callback, void *user_data);

/** GETRANGE key start end */
REDIS_API int redis_getrange(redis_client_t *client, const char *key,
                   int64_t start, int64_t end,
                   redis_command_cb_t callback, void *user_data);

/** SETRANGE key offset value */
REDIS_API int redis_setrange(redis_client_t *client, const char *key,
                   int64_t offset, const char *value,
                   redis_command_cb_t callback, void *user_data);

/* =============================================================================
 * Extended List API
 * =============================================================================
 */

/** LLEN key */
REDIS_API int redis_llen(redis_client_t *client, const char *key,
               redis_command_cb_t callback, void *user_data);

/** LRANGE key start stop */
REDIS_API int redis_lrange(redis_client_t *client, const char *key,
                 int64_t start, int64_t stop,
                 redis_command_cb_t callback, void *user_data);

/** LINDEX key index */
REDIS_API int redis_lindex(redis_client_t *client, const char *key, int64_t index,
                 redis_command_cb_t callback, void *user_data);

/** LSET key index value */
REDIS_API int redis_lset(redis_client_t *client, const char *key, int64_t index,
               const char *value,
               redis_command_cb_t callback, void *user_data);

/** LREM key count value */
REDIS_API int redis_lrem(redis_client_t *client, const char *key, int64_t count,
               const char *value,
               redis_command_cb_t callback, void *user_data);

/**
 * LINSERT key BEFORE|AFTER pivot value
 * @param before  1 = BEFORE, 0 = AFTER
 */
REDIS_API int redis_linsert(redis_client_t *client, const char *key, int before,
                  const char *pivot, const char *value,
                  redis_command_cb_t callback, void *user_data);

/** LTRIM key start stop */
REDIS_API int redis_ltrim(redis_client_t *client, const char *key,
                int64_t start, int64_t stop,
                redis_command_cb_t callback, void *user_data);

/**
 * BLPOP key [key ...] timeout
 * @param timeout_sec  0 = block forever
 */
REDIS_API int redis_blpop(redis_client_t *client,
                int key_count, const char **keys, double timeout_sec,
                redis_command_cb_t callback, void *user_data);

/** BRPOP key [key ...] timeout */
REDIS_API int redis_brpop(redis_client_t *client,
                int key_count, const char **keys, double timeout_sec,
                redis_command_cb_t callback, void *user_data);

/** LMOVE source destination LEFT|RIGHT LEFT|RIGHT  (Redis 6.2+) */
REDIS_API int redis_lmove(redis_client_t *client,
                const char *src, const char *dst,
                const char *wherefrom, const char *whereto,
                redis_command_cb_t callback, void *user_data);

/* =============================================================================
 * Extended Hash API
 * =============================================================================
 */

/**
 * HMSET key field value [field value ...]
 * @param pair_count  number of field-value pairs
 */
REDIS_API int redis_hmset(redis_client_t *client, const char *key,
                int pair_count, const char **fields, const char **values,
                redis_command_cb_t callback, void *user_data);

/**
 * HMGET key field [field ...]
 * @param field_count  number of fields
 */
REDIS_API int redis_hmget(redis_client_t *client, const char *key,
                int field_count, const char **fields,
                redis_command_cb_t callback, void *user_data);

/** HGETALL key  — returns alternating field/value bulk strings */
REDIS_API int redis_hgetall(redis_client_t *client, const char *key,
                  redis_command_cb_t callback, void *user_data);

/** HKEYS key */
REDIS_API int redis_hkeys(redis_client_t *client, const char *key,
                redis_command_cb_t callback, void *user_data);

/** HVALS key */
REDIS_API int redis_hvals(redis_client_t *client, const char *key,
                redis_command_cb_t callback, void *user_data);

/** HLEN key */
REDIS_API int redis_hlen(redis_client_t *client, const char *key,
               redis_command_cb_t callback, void *user_data);

/** HEXISTS key field */
REDIS_API int redis_hexists(redis_client_t *client, const char *key, const char *field,
                  redis_command_cb_t callback, void *user_data);

/** HDEL key field [field ...] */
REDIS_API int redis_hdel(redis_client_t *client, const char *key,
               int field_count, const char **fields,
               redis_command_cb_t callback, void *user_data);

/** HINCRBY key field increment */
REDIS_API int redis_hincrby(redis_client_t *client, const char *key,
                  const char *field, int64_t increment,
                  redis_command_cb_t callback, void *user_data);

/** HINCRBYFLOAT key field increment */
REDIS_API int redis_hincrbyfloat(redis_client_t *client, const char *key,
                      const char *field, double increment,
                      redis_command_cb_t callback, void *user_data);

/** HSETNX key field value */
REDIS_API int redis_hsetnx(redis_client_t *client, const char *key,
                 const char *field, const char *value,
                 redis_command_cb_t callback, void *user_data);

/* =============================================================================
 * Extended Set API
 * =============================================================================
 */

/** SREM key member [member ...] */
REDIS_API int redis_srem(redis_client_t *client, const char *key,
               int member_count, const char **members,
               redis_command_cb_t callback, void *user_data);

/** SCARD key */
REDIS_API int redis_scard(redis_client_t *client, const char *key,
                redis_command_cb_t callback, void *user_data);

/** SISMEMBER key member */
REDIS_API int redis_sismember(redis_client_t *client, const char *key, const char *member,
                    redis_command_cb_t callback, void *user_data);

/** SMISMEMBER key member [member ...]  (Redis 6.2+) */
REDIS_API int redis_smismember(redis_client_t *client, const char *key,
                    int member_count, const char **members,
                    redis_command_cb_t callback, void *user_data);

/** SPOP key [count] */
REDIS_API int redis_spop(redis_client_t *client, const char *key, int count,
               redis_command_cb_t callback, void *user_data);

/** SRANDMEMBER key [count] */
REDIS_API int redis_srandmember(redis_client_t *client, const char *key, int count,
                     redis_command_cb_t callback, void *user_data);

/** SUNION key [key ...] */
REDIS_API int redis_sunion(redis_client_t *client,
                 int key_count, const char **keys,
                 redis_command_cb_t callback, void *user_data);

/** SINTER key [key ...] */
REDIS_API int redis_sinter(redis_client_t *client,
                 int key_count, const char **keys,
                 redis_command_cb_t callback, void *user_data);

/** SDIFF key [key ...] */
REDIS_API int redis_sdiff(redis_client_t *client,
                int key_count, const char **keys,
                redis_command_cb_t callback, void *user_data);

/** SUNIONSTORE destination key [key ...] */
REDIS_API int redis_sunionstore(redis_client_t *client, const char *dest,
                     int key_count, const char **keys,
                     redis_command_cb_t callback, void *user_data);

/** SINTERSTORE destination key [key ...] */
REDIS_API int redis_sinterstore(redis_client_t *client, const char *dest,
                     int key_count, const char **keys,
                     redis_command_cb_t callback, void *user_data);

/** SDIFFSTORE destination key [key ...] */
REDIS_API int redis_sdiffstore(redis_client_t *client, const char *dest,
                    int key_count, const char **keys,
                    redis_command_cb_t callback, void *user_data);

/* =============================================================================
 * Sorted Set API
 * =============================================================================
 */

/**
 * ZADD key [NX|XX] [GT|LT] [CH] [INCR] score member [score member ...]
 * Simplified form: ZADD key score member [score member ...]
 *
 * @param pair_count  number of score-member pairs
 * @param scores      array of score strings (e.g. "1.5")
 * @param members     array of member strings
 */
REDIS_API int redis_zadd(redis_client_t *client, const char *key,
               int pair_count, const char **scores, const char **members,
               redis_command_cb_t callback, void *user_data);

/** ZREM key member [member ...] */
REDIS_API int redis_zrem(redis_client_t *client, const char *key,
               int member_count, const char **members,
               redis_command_cb_t callback, void *user_data);

/** ZSCORE key member */
REDIS_API int redis_zscore(redis_client_t *client, const char *key, const char *member,
                 redis_command_cb_t callback, void *user_data);

/** ZMSCORE key member [member ...]  (Redis 6.2+) */
REDIS_API int redis_zmscore(redis_client_t *client, const char *key,
                  int member_count, const char **members,
                  redis_command_cb_t callback, void *user_data);

/** ZINCRBY key increment member */
REDIS_API int redis_zincrby(redis_client_t *client, const char *key,
                  double increment, const char *member,
                  redis_command_cb_t callback, void *user_data);

/** ZRANK key member */
REDIS_API int redis_zrank(redis_client_t *client, const char *key, const char *member,
                redis_command_cb_t callback, void *user_data);

/** ZREVRANK key member */
REDIS_API int redis_zrevrank(redis_client_t *client, const char *key, const char *member,
                   redis_command_cb_t callback, void *user_data);

/** ZCARD key */
REDIS_API int redis_zcard(redis_client_t *client, const char *key,
                redis_command_cb_t callback, void *user_data);

/** ZCOUNT key min max */
REDIS_API int redis_zcount(redis_client_t *client, const char *key,
                 const char *min, const char *max,
                 redis_command_cb_t callback, void *user_data);

/**
 * ZRANGE key start stop [WITHSCORES]
 * @param withscores  1 = append WITHSCORES
 */
REDIS_API int redis_zrange(redis_client_t *client, const char *key,
                 int64_t start, int64_t stop, int withscores,
                 redis_command_cb_t callback, void *user_data);

/** ZREVRANGE key start stop [WITHSCORES] */
REDIS_API int redis_zrevrange(redis_client_t *client, const char *key,
                   int64_t start, int64_t stop, int withscores,
                   redis_command_cb_t callback, void *user_data);

/** ZRANGEBYSCORE key min max [WITHSCORES] [LIMIT offset count] */
REDIS_API int redis_zrangebyscore(redis_client_t *client, const char *key,
                       const char *min, const char *max,
                       int withscores,
                       int use_limit, int64_t offset, int64_t limit_count,
                       redis_command_cb_t callback, void *user_data);

/** ZREVRANGEBYSCORE key max min [WITHSCORES] [LIMIT offset count] */
REDIS_API int redis_zrevrangebyscore(redis_client_t *client, const char *key,
                          const char *max, const char *min,
                          int withscores,
                          int use_limit, int64_t offset, int64_t limit_count,
                          redis_command_cb_t callback, void *user_data);

/** ZPOPMIN key [count] */
REDIS_API int redis_zpopmin(redis_client_t *client, const char *key, int count,
                  redis_command_cb_t callback, void *user_data);

/** ZPOPMAX key [count] */
REDIS_API int redis_zpopmax(redis_client_t *client, const char *key, int count,
                  redis_command_cb_t callback, void *user_data);

/** ZRANGEBYLEX key min max [LIMIT offset count] */
REDIS_API int redis_zrangebylex(redis_client_t *client, const char *key,
                     const char *min, const char *max,
                     int use_limit, int64_t offset, int64_t limit_count,
                     redis_command_cb_t callback, void *user_data);

/** ZLEXCOUNT key min max */
REDIS_API int redis_zlexcount(redis_client_t *client, const char *key,
                   const char *min, const char *max,
                   redis_command_cb_t callback, void *user_data);

/* =============================================================================
 * Key Management API
 * =============================================================================
 */

/** TYPE key */
REDIS_API int redis_type(redis_client_t *client, const char *key,
               redis_command_cb_t callback, void *user_data);

/** TTL key  (seconds; -1 = no expire, -2 = not exist) */
REDIS_API int redis_ttl(redis_client_t *client, const char *key,
              redis_command_cb_t callback, void *user_data);

/** PTTL key  (milliseconds) */
REDIS_API int redis_pttl(redis_client_t *client, const char *key,
               redis_command_cb_t callback, void *user_data);

/** PERSIST key  (remove TTL) */
REDIS_API int redis_persist(redis_client_t *client, const char *key,
                  redis_command_cb_t callback, void *user_data);

/** EXPIREAT key unix-timestamp */
REDIS_API int redis_expireat(redis_client_t *client, const char *key, int64_t timestamp,
                   redis_command_cb_t callback, void *user_data);

/** PEXPIRE key milliseconds */
REDIS_API int redis_pexpire(redis_client_t *client, const char *key, int64_t ms,
                  redis_command_cb_t callback, void *user_data);

/** PEXPIREAT key unix-timestamp-ms */
REDIS_API int redis_pexpireat(redis_client_t *client, const char *key, int64_t ts_ms,
                   redis_command_cb_t callback, void *user_data);

/** RENAME key newkey */
REDIS_API int redis_rename(redis_client_t *client, const char *key, const char *newkey,
                 redis_command_cb_t callback, void *user_data);

/** RENAMENX key newkey */
REDIS_API int redis_renamenx(redis_client_t *client, const char *key, const char *newkey,
                   redis_command_cb_t callback, void *user_data);

/** UNLINK key [key ...]  (async DEL) */
REDIS_API int redis_unlink(redis_client_t *client, int key_count, const char **keys,
                 redis_command_cb_t callback, void *user_data);

/**
 * SCAN cursor [MATCH pattern] [COUNT count] [TYPE type]
 * @param cursor   cursor string ("0" to start)
 * @param pattern  MATCH glob (NULL = no filter)
 * @param count    hint (0 = server default)
 * @param type     TYPE filter string (NULL = no filter)
 */
REDIS_API int redis_scan(redis_client_t *client,
               const char *cursor, const char *pattern,
               size_t count, const char *type,
               redis_command_cb_t callback, void *user_data);

/** KEYS pattern */
REDIS_API int redis_keys(redis_client_t *client, const char *pattern,
               redis_command_cb_t callback, void *user_data);

/**
 * COPY source destination [DB db] [REPLACE]
 * @param dest_db  -1 = same DB
 * @param replace  1 = overwrite destination
 */
REDIS_API int redis_copy(redis_client_t *client, const char *src, const char *dst,
               int dest_db, int replace,
               redis_command_cb_t callback, void *user_data);

/** OBJECT ENCODING key */
REDIS_API int redis_object_encoding(redis_client_t *client, const char *key,
                         redis_command_cb_t callback, void *user_data);

/** OBJECT REFCOUNT key */
REDIS_API int redis_object_refcount(redis_client_t *client, const char *key,
                         redis_command_cb_t callback, void *user_data);

/** OBJECT IDLETIME key */
REDIS_API int redis_object_idletime(redis_client_t *client, const char *key,
                         redis_command_cb_t callback, void *user_data);

/* =============================================================================
 * Server Commands API
 * =============================================================================
 */

/** SELECT index */
REDIS_API int redis_select(redis_client_t *client, int db,
                 redis_command_cb_t callback, void *user_data);

/** DBSIZE */
REDIS_API int redis_dbsize(redis_client_t *client,
                 redis_command_cb_t callback, void *user_data);

/**
 * FLUSHDB [ASYNC|SYNC]
 * @param async  1 = ASYNC, 0 = SYNC
 */
REDIS_API int redis_flushdb(redis_client_t *client, int async,
                  redis_command_cb_t callback, void *user_data);

/** FLUSHALL [ASYNC|SYNC] */
REDIS_API int redis_flushall(redis_client_t *client, int async,
                   redis_command_cb_t callback, void *user_data);

/** INFO [section] — section=NULL returns all sections */
REDIS_API int redis_info(redis_client_t *client, const char *section,
               redis_command_cb_t callback, void *user_data);

/** CONFIG GET parameter */
REDIS_API int redis_config_get(redis_client_t *client, const char *parameter,
                    redis_command_cb_t callback, void *user_data);

/** CONFIG SET parameter value */
REDIS_API int redis_config_set(redis_client_t *client, const char *parameter,
                    const char *value,
                    redis_command_cb_t callback, void *user_data);

/** CONFIG RESETSTAT */
REDIS_API int redis_config_resetstat(redis_client_t *client,
                          redis_command_cb_t callback, void *user_data);

/** DEBUG SLEEP seconds */
REDIS_API int redis_debug_sleep(redis_client_t *client, double seconds,
                     redis_command_cb_t callback, void *user_data);

/** TIME — returns [unix-seconds, microseconds] */
REDIS_API int redis_time(redis_client_t *client,
               redis_command_cb_t callback, void *user_data);

/** LASTSAVE */
REDIS_API int redis_lastsave(redis_client_t *client,
                   redis_command_cb_t callback, void *user_data);

/** BGSAVE */
REDIS_API int redis_bgsave(redis_client_t *client,
                 redis_command_cb_t callback, void *user_data);

/** BGREWRITEAOF */
REDIS_API int redis_bgrewriteaof(redis_client_t *client,
                      redis_command_cb_t callback, void *user_data);

/** SAVE */
REDIS_API int redis_save(redis_client_t *client,
               redis_command_cb_t callback, void *user_data);

/* =============================================================================
 * Transaction API
 * =============================================================================
 */

/** MULTI — begin transaction */
REDIS_API int redis_multi(redis_client_t *client,
                redis_command_cb_t callback, void *user_data);

/** EXEC — execute queued commands */
REDIS_API int redis_exec(redis_client_t *client,
               redis_command_cb_t callback, void *user_data);

/** DISCARD — discard queued commands */
REDIS_API int redis_discard(redis_client_t *client,
                  redis_command_cb_t callback, void *user_data);

/** WATCH key [key ...] — optimistic locking */
REDIS_API int redis_watch(redis_client_t *client, int key_count, const char **keys,
                redis_command_cb_t callback, void *user_data);

/** UNWATCH */
REDIS_API int redis_unwatch(redis_client_t *client,
                  redis_command_cb_t callback, void *user_data);

/* =============================================================================
 * Scripting API
 * =============================================================================
 */

/**
 * EVAL script numkeys key [key ...] arg [arg ...]
 * @param script     Lua script text
 * @param key_count  number of keys
 * @param keys       key array (may be NULL when key_count == 0)
 * @param arg_count  number of args
 * @param args       arg array (may be NULL when arg_count == 0)
 */
REDIS_API int redis_eval(redis_client_t *client,
               const char *script,
               int key_count, const char **keys,
               int arg_count, const char **args,
               redis_command_cb_t callback, void *user_data);

/**
 * Result-preserving EVAL variant.
 *
 * `out` owns the reply and must be cleared with
 * redis_command_result_clear(). Mutating scripts must not be retried when the
 * outcome is SEND_UNCERTAIN or REPLY_UNKNOWN.
 */
REDIS_API int redis_eval_result(redis_client_t *client,
                      const char *script,
                      int key_count, const char **keys,
                      int arg_count, const char **args,
                      redis_command_result_t *out);

/**
 * EVALSHA sha1 numkeys key [key ...] arg [arg ...]
 */
REDIS_API int redis_evalsha(redis_client_t *client,
                  const char *sha1,
                  int key_count, const char **keys,
                  int arg_count, const char **args,
                  redis_command_cb_t callback, void *user_data);

/** Result-preserving EVALSHA variant. */
REDIS_API int redis_evalsha_result(redis_client_t *client,
                         const char *sha1,
                         int key_count, const char **keys,
                         int arg_count, const char **args,
                         redis_command_result_t *out);

/** SCRIPT LOAD script */
REDIS_API int redis_script_load(redis_client_t *client, const char *script,
                     redis_command_cb_t callback, void *user_data);

/** Result-preserving SCRIPT LOAD variant. */
REDIS_API int redis_script_load_result(redis_client_t *client,
                             const char *script,
                             redis_command_result_t *out);

/** SCRIPT EXISTS sha1 [sha1 ...] */
REDIS_API int redis_script_exists(redis_client_t *client,
                       int sha_count, const char **sha1s,
                       redis_command_cb_t callback, void *user_data);

/** SCRIPT FLUSH */
REDIS_API int redis_script_flush(redis_client_t *client,
                      redis_command_cb_t callback, void *user_data);

/* =============================================================================
 * HyperLogLog API
 * =============================================================================
 */

/** PFADD key element [element ...] */
REDIS_API int redis_pfadd(redis_client_t *client, const char *key,
                int element_count, const char **elements,
                redis_command_cb_t callback, void *user_data);

/** PFCOUNT key [key ...] */
REDIS_API int redis_pfcount(redis_client_t *client, int key_count, const char **keys,
                  redis_command_cb_t callback, void *user_data);

/** PFMERGE destkey sourcekey [sourcekey ...] */
REDIS_API int redis_pfmerge(redis_client_t *client, const char *destkey,
                  int src_key_count, const char **src_keys,
                  redis_command_cb_t callback, void *user_data);

/* =============================================================================
 * Geo API
 * =============================================================================
 */

/** GEOADD key longitude latitude member [longitude latitude member ...] */
REDIS_API int redis_geoadd(redis_client_t *client, const char *key,
                 int item_count, const double *longitudes, const double *latitudes, const char **members,
                 redis_command_cb_t callback, void *user_data);

/** GEODIST key member1 member2 [m|km|ft|mi] */
REDIS_API int redis_geodist(redis_client_t *client, const char *key,
                  const char *member1, const char *member2, const char *unit,
                  redis_command_cb_t callback, void *user_data);

/** GEOPOS key member [member ...] */
REDIS_API int redis_geopos(redis_client_t *client, const char *key,
                 int member_count, const char **members,
                 redis_command_cb_t callback, void *user_data);

/** GEORADIUS key longitude latitude radius m|km|ft|mi [WITHCOORD] [WITHDIST] [WITHHASH] [COUNT count] [ASC|DESC] */
REDIS_API int redis_georadius(redis_client_t *client, const char *key,
                    double longitude, double latitude, double radius, const char *unit,
                    int withcoord, int withdist, int withhash,
                    int count, const char *order,
                    redis_command_cb_t callback, void *user_data);

/** GEORADIUSBYMEMBER key member radius m|km|ft|mi [WITHCOORD] [WITHDIST] [WITHHASH] [COUNT count] [ASC|DESC] */
REDIS_API int redis_georadiusbymember(redis_client_t *client, const char *key,
                            const char *member, double radius, const char *unit,
                            int withcoord, int withdist, int withhash,
                            int count, const char *order,
                            redis_command_cb_t callback, void *user_data);

/** GEOSEARCH key [FROMMEMBER member | FROMLONLAT longitude latitude] [BYRADIUS radius m|km|ft|mi | BYBOX width height m|km|ft|mi] [ASC|DESC] [COUNT count] (Redis 6.2+) */
REDIS_API int redis_geosearch(redis_client_t *client, const char *key,
                    const char *from_member, const double *from_lonlat,
                    const double *by_radius, const char *radius_unit,
                    const double *by_box, const char *box_unit,
                    const char *order, int count,
                    redis_command_cb_t callback, void *user_data);

/* =============================================================================
 * Bitmaps API
 * =============================================================================
 */

/** SETBIT key offset value */
REDIS_API int redis_setbit(redis_client_t *client, const char *key, int64_t offset, int value,
                 redis_command_cb_t callback, void *user_data);

/** GETBIT key offset */
REDIS_API int redis_getbit(redis_client_t *client, const char *key, int64_t offset,
                 redis_command_cb_t callback, void *user_data);

/** BITCOUNT key [start end [BYTE|BIT]] */
REDIS_API int redis_bitcount(redis_client_t *client, const char *key,
                   int has_range, int64_t start, int64_t end, const char *unit,
                   redis_command_cb_t callback, void *user_data);

/** BITOP operation destkey key [key ...] */
REDIS_API int redis_bitop(redis_client_t *client, const char *operation, const char *destkey,
                int key_count, const char **keys,
                redis_command_cb_t callback, void *user_data);

/** BITPOS key bit [start [end [BYTE|BIT]]] */
REDIS_API int redis_bitpos(redis_client_t *client, const char *key, int bit,
                 int has_range, int64_t start, int64_t end, const char *unit,
                 redis_command_cb_t callback, void *user_data);

/** BITFIELD key [GET type offset] [SET type offset value] [INCRBY type offset increment] [OVERFLOW WRAP|SAT|FAIL] */
REDIS_API int redis_bitfield(redis_client_t *client, const char *key,
                   int command_count, const char **commands,
                   redis_command_cb_t callback, void *user_data);

/* =============================================================================
 * Client/Connection API
 * =============================================================================
 */

/** AUTH [username] password */
REDIS_API int redis_auth(redis_client_t *client, const char *username, const char *password,
               redis_command_cb_t callback, void *user_data);

/** CLIENT SETNAME connection-name */
REDIS_API int redis_client_setname(redis_client_t *client, const char *name,
                         redis_command_cb_t callback, void *user_data);

/** CLIENT GETNAME */
REDIS_API int redis_client_getname(redis_client_t *client,
                         redis_command_cb_t callback, void *user_data);

/** CLIENT LIST */
REDIS_API int redis_client_list(redis_client_t *client,
                       redis_command_cb_t callback, void *user_data);

/** CLIENT KILL filter value [filter value ...] */
REDIS_API int redis_client_kill(redis_client_t *client, int filter_count, const char **filters, const char **values,
                       redis_command_cb_t callback, void *user_data);

/** HELLO [protover [AUTH username password] [SETNAME name]] */
REDIS_API int redis_hello(redis_client_t *client, const char *protover, const char *username, const char *password, const char *clientname,
                 redis_command_cb_t callback, void *user_data);

/** SHUTDOWN [NOSAVE|SAVE] */
REDIS_API int redis_shutdown(redis_client_t *client, const char *save_mode,
                   redis_command_cb_t callback, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* REDIS_CLIENT_H */

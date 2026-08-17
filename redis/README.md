# Redis Client (RESP Protocol)

Async Redis client implementing the RESP (REdis Serialization Protocol) protocol, built on netcore's async_client_lib.

## Features

- **Full RESP Protocol Support**: Simple strings, errors, integers, bulk strings, arrays, null values
- **Async Operations**: Non-blocking I/O using netcore's event loop
- **Pipelined Commands**: Queue multiple commands for efficient execution
- **Redis Streams**: XADD, XREAD, XREADGROUP, XACK for reliable messaging
- **Pub/Sub**: PUBLISH, SUBSCRIBE, PSUBSCRIBE for real-time broadcast
- **Common Commands**: Built-in support for GET, SET, LPUSH, HSET, SADD, and more
- **Authentication**: Password and Redis ACL username/password authentication
- **Database Selection**: Select different Redis databases (0-15)
- **Explicit Outcomes**: Distinguishes not-sent, uncertain-send, lost-reply, and replied commands
- **Sentinel Failover**: Discovers and validates non-clustered Redis masters by service name
- **Zero External Dependencies**: Pure RESP implementation


## Usage

### Basic Example

```c
#include "redis_client.h"
#include <stdio.h>

void on_connected(redis_client_t *client, int status, void *data) {
    if (status == 0) {
        printf("Connected to Redis\n");
        redis_set(client, "mykey", "myvalue", on_set_reply, NULL);
    }
}

void on_set_reply(redis_client_t *client, redis_reply_t *reply, void *data) {
    if (reply->type == REDIS_REPLY_STRING) {
        printf("SET result: %s\n", reply->str);
    }
}

void on_get_reply(redis_client_t *client, redis_reply_t *reply, void *data) {
    if (reply->type == REDIS_REPLY_BULK_STRING) {
        printf("GET result: %s\n", reply->str);
    } else if (reply->type == REDIS_REPLY_NULL) {
        printf("Key not found\n");
    }
}

int main() {
    redis_client_t *client = redis_client_create("localhost", 6379);
    redis_client_connect(client, on_connected, NULL);

    // Run event loop...

    redis_client_destroy(client);
    return 0;
}
```

### Advanced Configuration

```c
redis_config_t config = {
    .host = "redis.example.com",
    .port = 6379,
    .username = "worker",  // optional Redis ACL username
    .password = "secret",
    .database = 1,
    .timeout_ms = 5000,
    .command_timeout_ms = 5000,
    .max_pipeline = 100
};

redis_client_t *client = redis_client_create_with_config(&config);
```

## Redis Streams

Reliable message queuing with consumer groups.

### Producer: XADD

```c
void on_xadd(redis_client_t *client, redis_reply_t *reply, void *data) {
    if (reply->type == REDIS_REPLY_BULK_STRING) {
        printf("Entry ID: %s\n", reply->str);  // e.g., "1234567890123-0"
    }
}

const char *fields[] = {"sensor", "temperature", "value"};
const char *values[] = {"sensor-1", "25.5", "celsius"};

redis_xadd(client, "mystream", 100000,  // MAXLEN ~ 100000
           3, fields, values, NULL,      // 3 field-value pairs
           on_xadd, NULL);
```

### Consumer: XREAD

```c
void on_stream_messages(redis_client_t *client,
                        redis_stream_result_t *results,
                        size_t result_count,
                        void *user_data) {
    for (size_t i = 0; i < result_count; i++) {
        printf("Stream: %s\n", results[i].stream_name);

        for (size_t j = 0; j < results[i].entry_count; j++) {
            redis_stream_entry_t *entry = &results[i].entries[j];
            printf("  ID: %s\n", entry->id);

            for (size_t k = 0; k < entry->field_count; k++) {
                printf("    %s = %.*s\n",
                       entry->fields[k],
                       (int)entry->value_lens[k],
                       entry->values[k]);
            }
        }
    }
}

const char *keys[] = {"mystream"};
const char *ids[] = {"$"};  // Only new messages

redis_xread(client, 10, 5000,     // COUNT 10, BLOCK 5000ms
            1, keys, ids,          // 1 stream
            on_stream_messages, NULL);
```

### Consumer Groups: XREADGROUP

```c
// Create consumer group (once)
redis_xgroup_create(client, "mystream", "mygroup", "0", 1, callback, NULL);

// Read as consumer
const char *keys[] = {"mystream"};
const char *ids[] = {">"};  // Only new messages for this group

redis_xreadgroup(client, "mygroup", "consumer-1",
                 10, 5000,           // COUNT, BLOCK
                 1, keys, ids,
                 on_stream_messages, NULL);

// Acknowledge processed messages
const char *ack_ids[] = {"1234567890123-0"};
redis_xack(client, "mystream", "mygroup", 1, ack_ids, callback, NULL);
```

### Stream Management

```c
// Get stream length
redis_xlen(client, "mystream", callback, NULL);

// Trim stream
redis_xtrim(client, "mystream", 10000, callback, NULL);  // Keep ~10000 entries

// Delete entries
const char *del_ids[] = {"1234567890123-0", "1234567890123-1"};
redis_xdel(client, "mystream", 2, del_ids, callback, NULL);
```

## Pub/Sub

Real-time message broadcasting (fire-and-forget).

### Publisher

```c
void on_publish(redis_client_t *client, redis_reply_t *reply, void *data) {
    printf("Message delivered to %lld subscribers\n", reply->integer);
}

const char *message = "Hello, subscribers!";
redis_publish(client, "news:tech", message, strlen(message), on_publish, NULL);
```

### Subscriber

```c
void on_message(redis_client_t *client,
                const char *channel,
                const void *message, size_t len,
                void *user_data) {
    printf("[%s] %.*s\n", channel, (int)len, (char *)message);
}

// Subscribe to specific channels
const char *channels[] = {"news:tech", "news:sports"};
redis_subscribe(client, 2, channels, on_message, NULL);

// Subscribe to patterns
const char *patterns[] = {"news:*", "alerts:*"};
redis_psubscribe(client, 2, patterns, on_message, NULL);

// Unsubscribe
redis_unsubscribe(client, 2, channels);
redis_punsubscribe(client, 2, patterns);
```

## String Commands

```c
redis_set(client, "key", "value", callback, NULL);
redis_get(client, "key", callback, NULL);
redis_del(client, 2, (const char *[]){"key1", "key2"}, callback, NULL);
redis_exists(client, "key", callback, NULL);
redis_expire(client, "key", 3600, callback, NULL);  // TTL 1 hour
redis_incr(client, "counter", callback, NULL);
```

## List Commands

```c
const char *values[] = {"item1", "item2", "item3"};
redis_lpush(client, "mylist", 3, values, callback, NULL);
redis_rpush(client, "mylist", 3, values, callback, NULL);
redis_lpop(client, "mylist", callback, NULL);
redis_rpop(client, "mylist", callback, NULL);
```

## Hash Commands

```c
redis_hset(client, "user:1000", "name", "John Doe", callback, NULL);
redis_hget(client, "user:1000", "name", callback, NULL);
```

## Set Commands

```c
const char *members[] = {"member1", "member2", "member3"};
redis_sadd(client, "myset", 3, members, callback, NULL);
redis_smembers(client, "myset", callback, NULL);
```

## Custom Commands

```c
// Using format string
redis_command(client, callback, user_data, "ZADD %s %d %s",
              "myset", 100, "member");

// Using argv (supports binary data)
const char *argv[] = {"SET", "binkey", "\x00\x01\x02"};
size_t argvlen[] = {3, 6, 3};
redis_commandv(client, 3, argv, argvlen, callback, user_data);
```

## API Reference

### Connection

| Function | Description |
|----------|-------------|
| `redis_client_create(host, port)` | Create client with defaults |
| `redis_client_create_with_config(config)` | Create with custom config |
| `redis_client_connect(client, cb, data)` | Connect to server |
| `redis_client_disconnect(client)` | Disconnect |
| `redis_client_destroy(client)` | Free resources |

### Streams

| Function | Description |
|----------|-------------|
| `redis_xadd(...)` | Add entry to stream |
| `redis_xread(...)` | Read from streams |
| `redis_xreadgroup(...)` | Read as consumer group member |
| `redis_xgroup_create(...)` | Create consumer group |
| `redis_xack(...)` | Acknowledge messages |
| `redis_xdel(...)` | Delete entries |
| `redis_xlen(...)` | Get stream length |
| `redis_xtrim(...)` | Trim stream |

### Pub/Sub

| Function | Description |
|----------|-------------|
| `redis_publish(...)` | Publish message |
| `redis_subscribe(...)` | Subscribe to channels |
| `redis_psubscribe(...)` | Subscribe to patterns |
| `redis_unsubscribe(...)` | Unsubscribe from channels |
| `redis_punsubscribe(...)` | Unsubscribe from patterns |

### Data Types

| Function | Description |
|----------|-------------|
| `redis_set/get/del/exists/expire/incr` | Strings |
| `redis_lpush/rpush/lpop/rpop` | Lists |
| `redis_hset/hget` | Hashes |
| `redis_sadd/smembers` | Sets |
| `redis_ping` | Health check |

## Reply Types

```c
typedef enum {
    REDIS_REPLY_STRING,      // +OK\r\n
    REDIS_REPLY_ERROR,       // -ERR message\r\n
    REDIS_REPLY_INTEGER,     // :1000\r\n
    REDIS_REPLY_BULK_STRING, // $6\r\nfoobar\r\n
    REDIS_REPLY_ARRAY,       // *2\r\n...
    REDIS_REPLY_NULL         // $-1\r\n
} redis_reply_type_t;

struct redis_reply_s {
    redis_reply_type_t type;
    int64_t integer;           // For INTEGER
    char *str;                 // For STRING, ERROR, BULK_STRING
    size_t len;                // Length of str
    redis_reply_t **elements;  // For ARRAY
    size_t element_count;
};
```

## Stream Entry Structure

```c
typedef struct {
    char *id;                  // Entry ID (e.g., "1234567890123-0")
    char **fields;             // Field names
    char **values;             // Field values
    size_t *value_lens;        // Value lengths (for binary)
    size_t field_count;
} redis_stream_entry_t;

typedef struct {
    char *stream_name;
    redis_stream_entry_t *entries;
    size_t entry_count;
} redis_stream_result_t;
```

## Error Handling

```c
redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
const char *argv[] = {"SET", "job:42", "done"};
int rc = redis_commandv_result(client, 3, argv, NULL, &result);

if (rc == TURBO_EIO && result.server_error != REDIS_SERVER_ERROR_NONE) {
    /* Redis replied with an application/server error. */
} else if (result.outcome == REDIS_COMMAND_SEND_UNCERTAIN ||
           result.outcome == REDIS_COMMAND_REPLY_UNKNOWN) {
    /* The write may have committed. Reconcile before retrying. */
} else if (rc == TURBO_OK) {
    /* Consume result.reply while result remains alive. */
}
redis_command_result_clear(&result);
```

`redis_client_connect()` performs configured AUTH and SELECT before reporting a
ready connection. A Redis error reply is returned as `TURBO_EIO`, retained in
`result.reply`, and classified in `result.server_error`; it is not logged by the
RESP parser or converted into an empty Stream result.

## Streams vs Pub/Sub

| Feature | Streams | Pub/Sub |
|---------|---------|---------|
| Persistence | Yes | No |
| Message replay | Yes | No |
| Consumer groups | Yes | No |
| Acknowledgment | Yes | No |
| Blocking read | Yes | N/A |
| Use case | Reliable queuing | Real-time broadcast |

**Use Streams for:**
- Job queues
- Event sourcing
- Reliable messaging (QoS 1/2)

**Use Pub/Sub for:**
- Real-time notifications
- Chat/presence
- Fire-and-forget updates

## Thread Safety

The Redis client is **not thread-safe**. Each thread should have its own client instance.

## Connection Pool

Connection pooling for high-throughput scenarios with read/write splitting.

### Features

- Configurable min/max connections
- Read/write splitting (master + replicas)
- Automatic connection acquisition/release
- AUTH/ACL and SELECT initialization once per physical connection
- Pipeline batching support
- Health monitoring and statistics

### Basic Usage

```c
#include "redis_pool.h"

redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
config.master_host = "redis-master";
config.master_port = 6379;
config.username = "worker";       /* Optional Redis ACL user */
config.password = "secret";
config.database = 4;
config.min_connections = 5;
config.max_connections = 20;
config.command_timeout_ms = 3000;

redis_pool_t *pool = redis_pool_create(&config);
redis_pool_start(pool);

// Commands auto-acquire and release connections
redis_pool_set(pool, "key", "value", callback, ctx);
redis_pool_get(pool, "key", callback, ctx);

redis_pool_destroy(pool);
```

`redis_pool_start()` must run inside a CoroNet coroutine. Opening the pool
connects its minimum physical connections and completes configured AUTH and
SELECT before any connection can be borrowed. A connection whose RESP stream
becomes invalid is discarded instead of being returned to the idle pool.
`cluster_readonly` additionally sends and validates `READONLY` once for every
new physical connection; Redis Cluster uses this when constructing replica
pools.

For code that needs exact retry semantics, use the owned result API:

```c
redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
const char *argv[] = {"SET", "job:42", "done"};
int rc = redis_pool_commandv_result(pool, 0, 3, argv, NULL, &result);

if (result.outcome == REDIS_COMMAND_SEND_UNCERTAIN ||
    result.outcome == REDIS_COMMAND_REPLY_UNKNOWN) {
    /* Reconcile the write; do not retry blindly. */
} else if (rc == TURBO_OK) {
    /* Consume result.reply. */
}
redis_command_result_clear(&result);
```

### Read Replicas

```c
const char *replicas[] = {"replica1", "replica2"};
uint16_t ports[] = {6379, 6379};

redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
config.master_host = "redis-master";
config.replica_hosts = replicas;
config.replica_ports = ports;
config.replica_count = 2;

redis_pool_t *pool = redis_pool_create(&config);
redis_pool_start(pool);

// Writes go to master
redis_pool_set(pool, "key", "value", cb, NULL);

// Reads routed to replicas
redis_pool_get(pool, "key", cb, NULL);
```

### Pipeline Batching

```c
redis_pipeline_t *pipeline = redis_pool_pipeline_create(pool);

redis_pipeline_add(pipeline, cb, NULL, "SET key1 value1");
redis_pipeline_add(pipeline, cb, NULL, "SET key2 value2");
redis_pipeline_add(pipeline, cb, NULL, "GET key1");

redis_pipeline_execute(pipeline);
redis_pipeline_destroy(pipeline);
```

### Pool Statistics

```c
redis_pool_stats_t stats;
redis_pool_get_stats(pool, &stats);

printf("Total: %zu, Active: %zu, Idle: %zu\n",
       stats.total_connections,
       stats.active_connections,
       stats.idle_connections);
printf("Commands: %llu sent, %llu failed\n",
       stats.commands_sent,
       stats.commands_failed);
```

### Pool API Reference

| Function | Description |
|----------|-------------|
| `redis_pool_create(config)` | Create connection pool |
| `redis_pool_start(pool)` | Start pool, create min connections |
| `redis_pool_stop(pool)` | Stop pool, close all connections |
| `redis_pool_destroy(pool)` | Free pool resources |
| `redis_pool_acquire(pool, read_only)` | Get connection from pool |
| `redis_pool_release(pool, conn)` | Return connection to pool |
| `redis_pool_command(...)` | Execute command (auto-acquire) |
| `redis_pool_set/get/del/hset/hget(...)` | Convenience functions |
| `redis_pool_get_stats(pool, stats)` | Get pool statistics |
| `redis_pool_is_healthy(pool)` | Check pool health |

## Redis Sentinel

Sentinel mode adds service discovery and failover for a non-clustered Redis
master. It queries multiple Sentinel endpoints in order, verifies every reported
data endpoint with `ROLE`, and only then publishes a new connection-pool
generation. Existing commands keep their old generation alive until their
borrowed connection is returned.

```c
#include "redis_sentinel.h"

const char *sentinel_hosts[] = {"sentinel-a", "sentinel-b", "sentinel-c"};
uint16_t sentinel_ports[] = {26379, 26379, 26379};

redis_sentinel_config_t config = REDIS_SENTINEL_CONFIG_DEFAULT;
config.sentinel_hosts = sentinel_hosts;
config.sentinel_ports = sentinel_ports;
config.sentinel_count = 3;
config.service_name = "cache-primary";
config.sentinel_username = "sentinel-client"; /* Optional Sentinel ACL */
config.sentinel_password = "sentinel-secret";
config.username = "application";              /* Optional Redis ACL */
config.password = "redis-secret";
config.database = 4;

redis_sentinel_t *sentinel = redis_sentinel_create(&config);

/* connect(), refresh(), and commands run inside a CoroNet coroutine. */
if (sentinel && redis_sentinel_connect(sentinel) == TURBO_OK) {
    redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
    const char *argv[] = {"SET", "job:42", "done"};

    int rc = redis_sentinel_commandv_result(sentinel, 3, argv, NULL, &result);
    if (rc == TURBO_OK) {
        /* Consume result.reply. */
    }
    redis_command_result_clear(&result);
}

redis_sentinel_destroy(sentinel);
```

Sentinel and Redis data-node credentials are independent. Discovery uses
`SENTINEL get-master-addr-by-name`; a reported endpoint is never accepted until
it answers `ROLE` as a master. An unavailable or stale Sentinel causes the next
configured endpoint to be tried.

On a disconnect, `READONLY`, or `MASTERDOWN`, the client refreshes topology.
Commands rejected before sending and explicit `READONLY`/`MASTERDOWN` replies
may be retried once after an actual master switch. An uncertain send or unknown
reply is returned unchanged because the write may already have committed. The
exact-result API must be used when callers need to reconcile that state.

Periodic validation is controlled by `topology_refresh_ms`; zero disables it.
The initial implementation routes all commands to the authoritative master and
does not subscribe to Sentinel `+switch-master` events or route reads to
replicas. Manual and failure-triggered refresh remain available without those
optional paths. See the [Redis Sentinel client specification](https://redis.io/docs/latest/develop/reference/sentinel-clients/).

| Function | Description |
|----------|-------------|
| `redis_sentinel_create(config)` | Copy Sentinel and data-node configuration |
| `redis_sentinel_connect(sentinel)` | Discover, validate, and connect to the master |
| `redis_sentinel_refresh(sentinel)` | Refresh and atomically switch master generation |
| `redis_sentinel_commandv_result(...)` | Binary-safe command with exact completion state |
| `redis_sentinel_commandv(...)` | Callback command API |
| `redis_sentinel_set/get/del(...)` | Common command helpers |
| `redis_sentinel_get_master(...)` | Copy the current validated master endpoint |
| `redis_sentinel_get_stats(...)` | Read discovery, failover, and command statistics |
| `redis_sentinel_disconnect(sentinel)` | Retire the current generation |
| `redis_sentinel_destroy(sentinel)` | Drain generations and release the client |

## Redis Cluster

Horizontal scaling across multiple masters with automatic slot routing.

### Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      Application                             │
└─────────────────────────┬───────────────────────────────────┘
                          │ CRC16(key) % 16384
┌─────────────────────────▼───────────────────────────────────┐
│                    Redis Cluster                             │
├───────────────────┬───────────────────┬─────────────────────┤
│     Master 1      │     Master 2      │      Master 3       │
│   slots 0-5460    │  slots 5461-10922 │  slots 10923-16383  │
└───────────────────┴───────────────────┴─────────────────────┘
```

### Features

- CRC16 hash slot routing (16384 slots)
- Hash tags for co-located keys
- Per-node connection pooling
- MOVED/ASK redirection handling
- Automatic topology discovery

Topology discovery and refresh use `CLUSTER SHARDS`. `CLUSTER SLOTS` is used
only when the server explicitly reports SHARDS as an unsupported command/subcommand (Redis
before 7); parse errors, ACL failures, and other server errors never trigger a
fallback. A candidate topology is accepted only when its RESP structure is
valid, every slot is covered, all master pools are ready, and—when replica
routing is enabled—every serving shard has at least one online replica whose
pool completed `READONLY`. Only then does it atomically replace the previous
topology. See the [Redis CLUSTER SHARDS contract](https://redis.io/docs/latest/commands/cluster-shards/)
and [READONLY contract](https://redis.io/docs/latest/commands/readonly/).

### Basic Usage

```c
#include "redis_cluster.h"

const char *seeds[] = {"redis1", "redis2", "redis3"};
uint16_t ports[] = {7000, 7000, 7000};

redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
config.seed_hosts = seeds;
config.seed_ports = ports;
config.seed_count = 3;
config.connections_per_node = 10;

redis_cluster_t *cluster = redis_cluster_create(&config);
// Must run inside a CoroNet coroutine.
redis_cluster_connect(cluster);

// Keys auto-route to correct master
redis_cluster_set(cluster, "user:1000", "data", cb, NULL);
redis_cluster_get(cluster, "user:1000", cb, NULL);

redis_cluster_destroy(cluster);
```

### Hash Tags

Use hash tags `{tag}` to ensure related keys go to the same slot:

```c
// All these keys go to same slot (hash of "user:123")
redis_cluster_set(cluster, "user:{user:123}:profile", "...", cb, NULL);
redis_cluster_set(cluster, "user:{user:123}:settings", "...", cb, NULL);
redis_cluster_set(cluster, "user:{user:123}:orders", "...", cb, NULL);

// Multi-key operations require same slot
const char *keys[] = {"{user:1}:a", "{user:1}:b", "{user:1}:c"};
redis_cluster_mget(cluster, 3, keys, cb, NULL);
redis_cluster_mdelete(cluster, 3, keys, cb, NULL);
```

### Hash Slot Calculation

```c
// Get slot for a key
uint16_t slot = redis_cluster_keyslot("mykey", 5);
printf("Key 'mykey' -> slot %u\n", slot);

// Get node for slot
const redis_cluster_node_t *node = redis_cluster_get_node(cluster, slot);
printf("Slot %u -> %s:%u\n", slot, node->host, node->port);
```

### Cluster Statistics

```c
redis_cluster_stats_t stats;
redis_cluster_get_stats(cluster, &stats);

printf("Masters: %zu, Replicas: %zu\n",
       stats.master_count, stats.replica_count);
printf("Commands: %llu, Redirections: %llu\n",
       stats.commands_sent, stats.redirections);
```

### Cluster API Reference

| Function | Description |
|----------|-------------|
| `redis_cluster_create(config)` | Create cluster client |
| `redis_cluster_connect(cluster)` | Connect and discover topology |
| `redis_cluster_disconnect(cluster)` | Disconnect from all nodes |
| `redis_cluster_destroy(cluster)` | Free cluster resources |
| `redis_cluster_refresh(cluster)` | Refresh cluster topology |
| `redis_cluster_command(...)` | Execute command (auto-route) |
| `redis_cluster_command_key(...)` | Execute with explicit key |
| `redis_cluster_commandv_result(...)` | Binary-safe command with exact outcome |
| `redis_cluster_read_commandv(...)` | Declare a binary-safe command read-only |
| `redis_cluster_read_commandv_result(...)` | Read-only command with exact outcome |
| `redis_cluster_set/get/del/hset/hget(...)` | Convenience functions |
| `redis_cluster_keyslot(key, len)` | Calculate hash slot |
| `redis_cluster_get_node(cluster, slot)` | Get node for slot |
| `redis_cluster_mget/mdelete(...)` | Multi-key (same slot) |
| `redis_cluster_is_healthy(cluster)` | Check all slots covered |

### Cluster Configuration

| Option | Default | Description |
|--------|---------|-------------|
| `seed_hosts/ports` | (required) | Initial nodes to connect |
| `seed_count` | (required) | Number of seed nodes |
| `username` | NULL | Optional Redis ACL username; requires `password` |
| `password` | NULL | Authentication password |
| `connections_per_node` | 5 | Pool size per routable node |
| `connect_timeout_ms` | 5000 | Connection timeout |
| `command_timeout_ms` | 5000 | Command timeout |
| `topology_refresh_ms` | 30000 | Topology refresh interval |
| `max_redirections` | 5 | Max MOVED/ASK follows |
| `route_reads_to_replicas` | 0 | Route declared read-only commands to online replicas |

MOVED is retried only after a complete Redis error reply and updates the
reported slot. ASK sends `ASKING` and the redirected command on the same
physical connection without changing the cached owner. An uncertain send or
unknown reply is never retried automatically; callers that need this distinction
should use `redis_cluster_commandv_result()`.

Generic command APIs remain master-routed because the client cannot safely
infer whether module/custom commands mutate state. Convenience reads (`GET`,
`HGET`, `MGET`, `XREAD`) and the explicit `redis_cluster_read_commandv*()` APIs
are replica-eligible. Enabling replica routing is strict: a shard without an
online replica makes topology connection/refresh fail instead of silently
falling back to its master. Replica reads may be stale by Redis design.

## Pool vs Sentinel vs Cluster

| Feature | Pool | Sentinel | Cluster |
|---------|------|----------|---------|
| Scaling | Vertical (static replicas) | Single-master HA | Horizontal (sharding) |
| Write capacity | Single master | Single master | Multiple masters |
| Data distribution | Full copy | Full copy | Partitioned by slot |
| Master discovery | Static config | Sentinel service name | Cluster topology |
| Multi-key ops | Any keys | Any keys | Same slot only |
| Setup complexity | Low | Medium | Medium |
| Use case | Static endpoints/read scaling | Automatic failover | Write scaling |

**Use Pool for:**
- Read-heavy workloads
- Simple master-replica setup
- Single-node Redis

**Use Sentinel for:**
- Automatic failover without sharding
- Stable service names over changing master endpoints
- Existing primary/replica deployments managed by Sentinel

**Use Cluster for:**
- Write-heavy workloads
- Large datasets (>memory)
- Horizontal scaling needs

## Dependencies

- netcore (async_client_lib)
- No external dependencies

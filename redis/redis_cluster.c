/**
 * @file redis_cluster.c
 * @brief Redis Cluster Client Implementation
 */

#include "redis_cluster.h"
#include "redis_internal.h"
#include "redis_pool.h"
#include "turbo_error.h"
#include "turbo_str.h"
#include <fmt.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* =============================================================================
 * CRC16 Implementation (XMODEM)
 * =============================================================================
 */

static const uint16_t crc16_table[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
    0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52b5, 0x4294, 0x72f7, 0x62d6,
    0x9339, 0x8318, 0xb37b, 0xa35a, 0xd3bd, 0xc39c, 0xf3ff, 0xe3de,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64e6, 0x74c7, 0x44a4, 0x5485,
    0xa56a, 0xb54b, 0x8528, 0x9509, 0xe5ee, 0xf5cf, 0xc5ac, 0xd58d,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76d7, 0x66f6, 0x5695, 0x46b4,
    0xb75b, 0xa77a, 0x9719, 0x8738, 0xf7df, 0xe7fe, 0xd79d, 0xc7bc,
    0x48c4, 0x58e5, 0x6886, 0x78a7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xc9cc, 0xd9ed, 0xe98e, 0xf9af, 0x8948, 0x9969, 0xa90a, 0xb92b,
    0x5af5, 0x4ad4, 0x7ab7, 0x6a96, 0x1a71, 0x0a50, 0x3a33, 0x2a12,
    0xdbfd, 0xcbdc, 0xfbbf, 0xeb9e, 0x9b79, 0x8b58, 0xbb3b, 0xab1a,
    0x6ca6, 0x7c87, 0x4ce4, 0x5cc5, 0x2c22, 0x3c03, 0x0c60, 0x1c41,
    0xedae, 0xfd8f, 0xcdec, 0xddcd, 0xad2a, 0xbd0b, 0x8d68, 0x9d49,
    0x7e97, 0x6eb6, 0x5ed5, 0x4ef4, 0x3e13, 0x2e32, 0x1e51, 0x0e70,
    0xff9f, 0xefbe, 0xdfdd, 0xcffc, 0xbf1b, 0xaf3a, 0x9f59, 0x8f78,
    0x9188, 0x81a9, 0xb1ca, 0xa1eb, 0xd10c, 0xc12d, 0xf14e, 0xe16f,
    0x1080, 0x00a1, 0x30c2, 0x20e3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83b9, 0x9398, 0xa3fb, 0xb3da, 0xc33d, 0xd31c, 0xe37f, 0xf35e,
    0x02b1, 0x1290, 0x22f3, 0x32d2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xb5ea, 0xa5cb, 0x95a8, 0x8589, 0xf56e, 0xe54f, 0xd52c, 0xc50d,
    0x34e2, 0x24c3, 0x14a0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xa7db, 0xb7fa, 0x8799, 0x97b8, 0xe75f, 0xf77e, 0xc71d, 0xd73c,
    0x26d3, 0x36f2, 0x0691, 0x16b0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xd94c, 0xc96d, 0xf90e, 0xe92f, 0x99c8, 0x89e9, 0xb98a, 0xa9ab,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18c0, 0x08e1, 0x3882, 0x28a3,
    0xcb7d, 0xdb5c, 0xeb3f, 0xfb1e, 0x8bf9, 0x9bd8, 0xabbb, 0xbb9a,
    0x4a75, 0x5a54, 0x6a37, 0x7a16, 0x0af1, 0x1ad0, 0x2ab3, 0x3a92,
    0xfd2e, 0xed0f, 0xdd6c, 0xcd4d, 0xbdaa, 0xad8b, 0x9de8, 0x8dc9,
    0x7c26, 0x6c07, 0x5c64, 0x4c45, 0x3ca2, 0x2c83, 0x1ce0, 0x0cc1,
    0xef1f, 0xff3e, 0xcf5d, 0xdf7c, 0xaf9b, 0xbfba, 0x8fd9, 0x9ff8,
    0x6e17, 0x7e36, 0x4e55, 0x5e74, 0x2e93, 0x3eb2, 0x0ed1, 0x1ef0
};

#define calc_argc_mul_add redis_argc_mul_add
#define alloc_argv redis_argv_alloc

static uint16_t crc16(const char *buf, size_t len) {
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc = (crc << 8) ^ crc16_table[((crc >> 8) ^ (uint8_t)buf[i]) & 0xff];
    }
    return crc;
}

/* =============================================================================
 * Internal Structures
 * =============================================================================
 */

typedef struct cluster_node_s {
    redis_cluster_node_t info;
    redis_pool_t *pool;
    int pool_started;
    struct cluster_node_s *master;
    struct cluster_node_s *replica_head;
    struct cluster_node_s *replica_next;
    struct cluster_node_s *next_read_replica;
    struct cluster_node_s *next;
} cluster_node_t;

struct redis_cluster_s {
    redis_cluster_config_t config;

    /* Node management */
    cluster_node_t *nodes;
    size_t node_count;

    /* Slot mapping: slot -> node */
    cluster_node_t *slots[REDIS_CLUSTER_SLOTS];

    /* State */
    int connected;
    uint64_t last_topology_refresh_ns;

    /* Statistics */
    redis_cluster_stats_t stats;
};

/* =============================================================================
 * Helper Functions
 * =============================================================================
 */

uint16_t redis_cluster_keyslot(const char *key, size_t len) {
    size_t s, e;

    /* Find hash tag: {tag} */
    for (s = 0; s < len; s++) {
        if (key[s] == '{') break;
    }

    if (s < len) {
        for (e = s + 1; e < len; e++) {
            if (key[e] == '}') break;
        }
        /* Use content between {} if non-empty */
        if (e < len && e > s + 1) {
            return crc16(key + s + 1, e - s - 1) & 0x3FFF;
        }
    }

    return crc16(key, len) & 0x3FFF;
}

static cluster_node_t *find_node(redis_cluster_t *cluster,
                                  const char *host, uint16_t port) {
    cluster_node_t *node = cluster->nodes;
    while (node) {
        if (node->info.port == port &&
            strcmp(node->info.host, host) == 0) {
            return node;
        }
        node = node->next;
    }
    return NULL;
}

static cluster_node_t *create_node(redis_cluster_t *cluster,
                                    const char *host, uint16_t port,
                                    const char *node_id, int is_master) {
    if (!cluster || !host || !host[0] || host[0] == '?' || port == 0) return NULL;
    cluster_node_t *node = calloc(1, sizeof(cluster_node_t));
    if (!node) return NULL;

    node->info.host = tstr_dup(host);
    node->info.port = port;
    node->info.node_id = node_id ? tstr_dup(node_id) : NULL;
    if (!node->info.host || (node_id && !node->info.node_id)) {
        tstr_free((tstr)node->info.host);
        tstr_free((tstr)node->info.node_id);
        free(node);
        return NULL;
    }
    node->info.is_master = is_master;
    node->info.slot_start = -1;
    node->info.slot_end = -1;

    /* Create connection pool for this node */
    redis_pool_config_t pool_config = {
        .master_host = host,
        .master_port = port,
        .username = cluster->config.username,
        .password = cluster->config.password,
        .database = 0,
        .min_connections = 1,
        .max_connections = cluster->config.connections_per_node,
        .connect_timeout_ms = cluster->config.connect_timeout_ms,
        .command_timeout_ms = cluster->config.command_timeout_ms,
        .idle_timeout_ms = 60000,
        .health_check_ms = 30000,
        .cluster_readonly = !is_master && cluster->config.route_reads_to_replicas
    };

    node->pool = redis_pool_create(&pool_config);
    if (!node->pool) {
        tstr_free((tstr)node->info.host);
        tstr_free((tstr)node->info.node_id);
        free(node);
        return NULL;
    }

    return node;
}

static int start_node_pool(cluster_node_t *node) {
    int rc;
    if (!node || !node->pool) return TURBO_EINVAL;
    if (node->pool_started) return TURBO_OK;
    rc = redis_pool_start(node->pool);
    if (rc != TURBO_OK) return rc;
    node->pool_started = 1;
    return TURBO_OK;
}

static void destroy_node(cluster_node_t *node) {
    if (!node) return;
    if (node->pool) {
        redis_pool_destroy(node->pool);
    }
    tstr_free((tstr)node->info.host);
    tstr_free((tstr)node->info.node_id);
    free(node);
}

static void recount_nodes(redis_cluster_t *cluster) {
    cluster_node_t *node;
    if (!cluster) return;
    cluster->stats.master_count = 0;
    cluster->stats.replica_count = 0;
    for (node = cluster->nodes; node; node = node->next) {
        if (node->info.is_master) cluster->stats.master_count++;
        else cluster->stats.replica_count++;
    }
}

static void recompute_master_slot_bounds(redis_cluster_t *cluster) {
    cluster_node_t *node;
    if (!cluster) return;
    for (node = cluster->nodes; node; node = node->next) {
        if (node->info.is_master) {
            node->info.slot_start = -1;
            node->info.slot_end = -1;
        }
    }
    for (int slot = 0; slot < REDIS_CLUSTER_SLOTS; ++slot) {
        node = cluster->slots[slot];
        if (!node) continue;
        if (node->info.slot_start < 0) node->info.slot_start = slot;
        node->info.slot_end = slot;
    }
}

static void clear_nodes(redis_cluster_t *cluster) {
    cluster_node_t *node = cluster->nodes;
    while (node) {
        cluster_node_t *next = node->next;
        destroy_node(node);
        node = next;
    }
    cluster->nodes = NULL;
    cluster->node_count = 0;

    memset(cluster->slots, 0, sizeof(cluster->slots));
}

static int attach_replica(cluster_node_t *master, cluster_node_t *replica) {
    cluster_node_t *current;
    if (!master || !replica || master == replica || !master->info.is_master)
        return TURBO_EPROTO;
    if (replica->master && replica->master != master) return TURBO_EPROTO;
    if (replica->master == master) return TURBO_OK;

    replica->master = master;
    for (current = master->replica_head; current; current = current->replica_next)
        if (current == replica) return TURBO_OK;
    replica->replica_next = master->replica_head;
    master->replica_head = replica;
    if (!master->next_read_replica) master->next_read_replica = replica;
    return TURBO_OK;
}

static int assign_slot_range(redis_cluster_t *cluster, cluster_node_t *master,
                             int start, int end) {
    if (!cluster || !master || !master->info.is_master || start < 0 ||
        end < start || end >= REDIS_CLUSTER_SLOTS)
        return TURBO_EPROTO;
    for (int slot = start; slot <= end; ++slot) {
        if (cluster->slots[slot] && cluster->slots[slot] != master)
            return TURBO_EPROTO;
        cluster->slots[slot] = master;
    }
    if (master->info.slot_start < 0 || start < master->info.slot_start)
        master->info.slot_start = start;
    if (end > master->info.slot_end) master->info.slot_end = end;
    return TURBO_OK;
}

/* =============================================================================
 * Topology Discovery
 * =============================================================================
 */

static const char *topology_endpoint(const redis_reply_t *reply,
                                     const char *source_host) {
    if (!reply || !source_host) return NULL;
    if (reply->type == REDIS_REPLY_NULL) return source_host;
    if ((reply->type != REDIS_REPLY_BULK_STRING &&
         reply->type != REDIS_REPLY_STRING) || !reply->str)
        return NULL;
    if (reply->len == 0) return source_host;
    if (reply->len == 1 && reply->str[0] == '?') return NULL;
    return reply->str;
}

static int parse_cluster_slots(redis_cluster_t *cluster,
                               const redis_reply_t *reply,
                               const char *source_host) {
    if (!cluster || !reply || reply->type != REDIS_REPLY_ARRAY ||
        reply->element_count == 0 || !source_host) {
        return TURBO_EPROTO;
    }

    /* CLUSTER SLOTS returns:
     * [[start, end, [master_ip, master_port, node_id], [replica...]], ...] */

    for (size_t i = 0; i < reply->element_count; i++) {
        const redis_reply_t *slot_range = reply->elements[i];
        if (!slot_range || slot_range->type != REDIS_REPLY_ARRAY ||
            slot_range->element_count < 3) {
            return TURBO_EPROTO;
        }

        if (!slot_range->elements[0] || !slot_range->elements[1] ||
            slot_range->elements[0]->type != REDIS_REPLY_INTEGER ||
            slot_range->elements[1]->type != REDIS_REPLY_INTEGER) {
            return TURBO_EPROTO;
        }
        if (slot_range->elements[0]->integer < 0 ||
            slot_range->elements[0]->integer >= REDIS_CLUSTER_SLOTS ||
            slot_range->elements[1]->integer < slot_range->elements[0]->integer ||
            slot_range->elements[1]->integer >= REDIS_CLUSTER_SLOTS) {
            return TURBO_EPROTO;
        }
        int start = (int)slot_range->elements[0]->integer;
        int end = (int)slot_range->elements[1]->integer;

        const redis_reply_t *master_info = slot_range->elements[2];
        if (!master_info || master_info->type != REDIS_REPLY_ARRAY ||
            master_info->element_count < 2 || !master_info->elements[1] ||
            master_info->elements[1]->type != REDIS_REPLY_INTEGER ||
            master_info->elements[1]->integer <= 0 ||
            master_info->elements[1]->integer > UINT16_MAX) {
            return TURBO_EPROTO;
        }

        const char *host = topology_endpoint(master_info->elements[0], source_host);
        int port = (int)master_info->elements[1]->integer;
        const char *node_id = NULL;
        if (!host) return TURBO_EPROTO;
        if (master_info->element_count > 2 && master_info->elements[2] &&
            master_info->elements[2]->type == REDIS_REPLY_BULK_STRING) {
            node_id = master_info->elements[2]->str;
        }

        cluster_node_t *node = find_node(cluster, host, (uint16_t)port);
        if (node && !node->info.is_master) return TURBO_EPROTO;
        if (!node) {
            node = create_node(cluster, host, (uint16_t)port, node_id, 1);
            if (!node) return TURBO_ENOMEM;
            node->next = cluster->nodes;
            cluster->nodes = node;
            cluster->node_count++;
        }

        node->info.is_master = 1;
        if (assign_slot_range(cluster, node, start, end) != TURBO_OK)
            return TURBO_EPROTO;

        for (size_t r = 3; r < slot_range->element_count; r++) {
            const redis_reply_t *replica_info = slot_range->elements[r];
            if (!replica_info || replica_info->type != REDIS_REPLY_ARRAY ||
                replica_info->element_count < 2 || !replica_info->elements[1] ||
                replica_info->elements[1]->type != REDIS_REPLY_INTEGER ||
                replica_info->elements[1]->integer <= 0 ||
                replica_info->elements[1]->integer > UINT16_MAX) {
                return TURBO_EPROTO;
            }

            const char *r_host = topology_endpoint(replica_info->elements[0], source_host);
            int r_port = (int)replica_info->elements[1]->integer;
            const char *r_node_id = NULL;
            if (!r_host) return TURBO_EPROTO;
            if (replica_info->element_count > 2 && replica_info->elements[2] &&
                replica_info->elements[2]->type == REDIS_REPLY_BULK_STRING) {
                r_node_id = replica_info->elements[2]->str;
            }

            cluster_node_t *replica = find_node(cluster, r_host, (uint16_t)r_port);
            if (!replica) {
                replica = create_node(cluster, r_host, (uint16_t)r_port, r_node_id, 0);
                if (!replica) return TURBO_ENOMEM;
                replica->next = cluster->nodes;
                cluster->nodes = replica;
                cluster->node_count++;
            }
            if (replica->info.is_master || attach_replica(node, replica) != TURBO_OK)
                return TURBO_EPROTO;
        }
    }

    for (int slot = 0; slot < REDIS_CLUSTER_SLOTS; ++slot) {
        if (!cluster->slots[slot]) return TURBO_EPROTO;
    }
    return TURBO_OK;
}

static int reply_text_equal(const redis_reply_t *reply, const char *text) {
    size_t len;
    if (!reply || !text ||
        (reply->type != REDIS_REPLY_STRING &&
         reply->type != REDIS_REPLY_BULK_STRING) || !reply->str)
        return 0;
    len = strlen(text);
    return reply->len == len && memcmp(reply->str, text, len) == 0;
}

static const redis_reply_t *reply_map_value(const redis_reply_t *map,
                                            const char *key) {
    if (!map || map->type != REDIS_REPLY_ARRAY ||
        (map->element_count & 1u) != 0)
        return NULL;
    for (size_t i = 0; i < map->element_count; i += 2)
        if (reply_text_equal(map->elements[i], key)) return map->elements[i + 1];
    return NULL;
}

typedef struct {
    const char *host;
    const char *node_id;
    uint16_t port;
    int is_master;
    int online;
} topology_node_view_t;

static int parse_shards_node(const redis_reply_t *reply,
                             const char *source_host,
                             topology_node_view_t *view) {
    const redis_reply_t *endpoint;
    const redis_reply_t *port;
    const redis_reply_t *node_id;
    const redis_reply_t *role;
    const redis_reply_t *health;
    if (!reply || !source_host || !view || reply->type != REDIS_REPLY_ARRAY ||
        (reply->element_count & 1u) != 0)
        return TURBO_EPROTO;

    memset(view, 0, sizeof(*view));
    endpoint = reply_map_value(reply, "endpoint");
    port = reply_map_value(reply, "port");
    node_id = reply_map_value(reply, "id");
    role = reply_map_value(reply, "role");
    health = reply_map_value(reply, "health");
    if (!endpoint || !port || !node_id || !role || !health ||
        port->type != REDIS_REPLY_INTEGER || port->integer <= 0 ||
        port->integer > UINT16_MAX ||
        (node_id->type != REDIS_REPLY_STRING &&
         node_id->type != REDIS_REPLY_BULK_STRING) || !node_id->str)
        return TURBO_EPROTO;

    if (reply_text_equal(role, "master")) view->is_master = 1;
    else if (!reply_text_equal(role, "replica")) return TURBO_EPROTO;
    if (reply_text_equal(health, "online")) view->online = 1;
    else if (!reply_text_equal(health, "failed") &&
             !reply_text_equal(health, "loading"))
        return TURBO_EPROTO;

    view->host = topology_endpoint(endpoint, source_host);
    view->node_id = node_id->str;
    view->port = (uint16_t)port->integer;
    return TURBO_OK;
}

static int parse_cluster_shards(redis_cluster_t *cluster,
                                const redis_reply_t *reply,
                                const char *source_host) {
    if (!cluster || !reply || reply->type != REDIS_REPLY_ARRAY ||
        reply->element_count == 0 || !source_host)
        return TURBO_EPROTO;

    for (size_t i = 0; i < reply->element_count; ++i) {
        const redis_reply_t *shard = reply->elements[i];
        const redis_reply_t *slot_ranges = reply_map_value(shard, "slots");
        const redis_reply_t *nodes = reply_map_value(shard, "nodes");
        cluster_node_t *master = NULL;
        topology_node_view_t master_view = {0};
        int master_seen = 0;

        if (!slot_ranges || slot_ranges->type != REDIS_REPLY_ARRAY ||
            (slot_ranges->element_count & 1u) != 0 || !nodes ||
            nodes->type != REDIS_REPLY_ARRAY || nodes->element_count == 0)
            return TURBO_EPROTO;

        for (size_t n = 0; n < nodes->element_count; ++n) {
            topology_node_view_t view;
            if (parse_shards_node(nodes->elements[n], source_host, &view) != TURBO_OK)
                return TURBO_EPROTO;
            if (!view.is_master || !view.online) continue;
            if (!view.host || master_seen) return TURBO_EPROTO;
            master_seen = 1;
            master_view = view;
        }
        if (!master_seen) return TURBO_EPROTO;
        master = find_node(cluster, master_view.host, master_view.port);
        if (master && !master->info.is_master) return TURBO_EPROTO;
        if (!master) {
            master = create_node(cluster, master_view.host, master_view.port,
                                 master_view.node_id, 1);
            if (!master) return TURBO_ENOMEM;
            master->next = cluster->nodes;
            cluster->nodes = master;
            cluster->node_count++;
        }

        for (size_t s = 0; s < slot_ranges->element_count; s += 2) {
            const redis_reply_t *start = slot_ranges->elements[s];
            const redis_reply_t *end = slot_ranges->elements[s + 1];
            if (!start || !end || start->type != REDIS_REPLY_INTEGER ||
                end->type != REDIS_REPLY_INTEGER || start->integer < 0 ||
                start->integer >= REDIS_CLUSTER_SLOTS ||
                end->integer < start->integer ||
                end->integer >= REDIS_CLUSTER_SLOTS ||
                assign_slot_range(cluster, master, (int)start->integer,
                                  (int)end->integer) != TURBO_OK)
                return TURBO_EPROTO;
        }

        for (size_t n = 0; n < nodes->element_count; ++n) {
            topology_node_view_t view;
            cluster_node_t *replica;
            if (parse_shards_node(nodes->elements[n], source_host, &view) != TURBO_OK)
                return TURBO_EPROTO;
            if (view.is_master || !view.online || !view.host) continue;
            replica = find_node(cluster, view.host, view.port);
            if (!replica) {
                replica = create_node(cluster, view.host, view.port,
                                      view.node_id, 0);
                if (!replica) return TURBO_ENOMEM;
                replica->next = cluster->nodes;
                cluster->nodes = replica;
                cluster->node_count++;
            }
            if (replica->info.is_master ||
                attach_replica(master, replica) != TURBO_OK)
                return TURBO_EPROTO;
        }
    }

    for (int slot = 0; slot < REDIS_CLUSTER_SLOTS; ++slot)
        if (!cluster->slots[slot]) return TURBO_EPROTO;
    return TURBO_OK;
}

static int shards_command_is_unsupported(const redis_command_result_t *result) {
    static const char unknown_command[] = "ERR unknown command";
    static const char unknown_subcommand[] =
        "ERR Unknown subcommand or wrong number of arguments for 'SHARDS'";
    if (!result || result->outcome != REDIS_COMMAND_REPLIED ||
        result->server_error != REDIS_SERVER_ERROR_ERR || !result->reply ||
        result->reply->type != REDIS_REPLY_ERROR || !result->reply->str)
        return 0;
    return (result->reply->len >= sizeof(unknown_command) - 1u &&
            memcmp(result->reply->str, unknown_command,
                   sizeof(unknown_command) - 1u) == 0) ||
           (result->reply->len >= sizeof(unknown_subcommand) - 1u &&
            memcmp(result->reply->str, unknown_subcommand,
                   sizeof(unknown_subcommand) - 1u) == 0);
}

static int query_topology(redis_cluster_t *cluster, const char *host,
                          uint16_t port, redis_cluster_t *candidate) {
    redis_config_t config = {
        .host = host,
        .port = port,
        .username = cluster->config.username,
        .password = cluster->config.password,
        .database = 0,
        .timeout_ms = cluster->config.connect_timeout_ms,
        .command_timeout_ms = cluster->config.command_timeout_ms,
        .max_pipeline = 1
    };
    redis_client_t *client = redis_client_create_with_config(&config);
    redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
    const char *shards_argv[] = {"CLUSTER", "SHARDS"};
    const char *slots_argv[] = {"CLUSTER", "SLOTS"};
    int rc;

    if (!client) return TURBO_ENOMEM;
    rc = redis_client_connect(client, NULL, NULL);
    if (rc == TURBO_OK)
        rc = redis_commandv_result(client, 2, shards_argv, NULL, &result);
    if (rc == TURBO_OK)
        rc = parse_cluster_shards(candidate, result.reply, host);
    else if (shards_command_is_unsupported(&result)) {
        redis_command_result_clear(&result);
        rc = redis_commandv_result(client, 2, slots_argv, NULL, &result);
        if (rc == TURBO_OK)
            rc = parse_cluster_slots(candidate, result.reply, host);
    }
    redis_command_result_clear(&result);
    redis_client_destroy(client);
    return rc;
}

static int start_topology(redis_cluster_t *candidate) {
    for (cluster_node_t *node = candidate->nodes; node; node = node->next) {
        if (!node->info.is_master || node->info.slot_start < 0) continue;
        if (start_node_pool(node) != TURBO_OK) return TURBO_ENOTCONN;
        if (!candidate->config.route_reads_to_replicas) continue;
        if (!node->replica_head) return TURBO_ENOTCONN;
        for (cluster_node_t *replica = node->replica_head; replica;
             replica = replica->replica_next)
            if (start_node_pool(replica) != TURBO_OK) return TURBO_ENOTCONN;
    }
    return TURBO_OK;
}

static void commit_topology(redis_cluster_t *cluster,
                            redis_cluster_t *candidate) {
    redis_cluster_disconnect(cluster);
    clear_nodes(cluster);
    cluster->nodes = candidate->nodes;
    cluster->node_count = candidate->node_count;
    memcpy(cluster->slots, candidate->slots, sizeof(cluster->slots));
    candidate->nodes = NULL;
    candidate->node_count = 0;
    memset(candidate->slots, 0, sizeof(candidate->slots));
    cluster->connected = 1;
    cluster->last_topology_refresh_ns = turbo_hrtime();
    cluster->stats.topology_refreshes++;
    recount_nodes(cluster);
}

static int try_topology_endpoint(redis_cluster_t *cluster, const char *host,
                                 uint16_t port) {
    redis_cluster_t *candidate = calloc(1, sizeof(*candidate));
    int rc;
    if (!candidate) return TURBO_ENOMEM;
    candidate->config = cluster->config;
    rc = query_topology(cluster, host, port, candidate);
    if (rc == TURBO_OK) rc = start_topology(candidate);
    if (rc == TURBO_OK) commit_topology(cluster, candidate);
    clear_nodes(candidate);
    free(candidate);
    return rc;
}

static int discover_topology(redis_cluster_t *cluster) {
    cluster_node_t *node;
    if (!cluster || cluster->config.seed_count == 0) return TURBO_EINVAL;

    for (node = cluster->nodes; node; node = node->next) {
        if (try_topology_endpoint(cluster, node->info.host,
                                  node->info.port) == TURBO_OK)
            return TURBO_OK;
    }
    for (size_t i = 0; i < cluster->config.seed_count; i++) {
        if (try_topology_endpoint(cluster, cluster->config.seed_hosts[i],
                                  cluster->config.seed_ports[i]) == TURBO_OK)
            return TURBO_OK;
    }

    return TURBO_ENOTCONN;
}

/* =============================================================================
 * Cluster Lifecycle
 * =============================================================================
 */

redis_cluster_t *redis_cluster_create(const redis_cluster_config_t *config) {
    if (!config || config->seed_count == 0 || !config->seed_hosts || !config->seed_ports ||
        (config->username && !config->password) || config->max_redirections < 0) {
        return NULL;
    }
    for (size_t i = 0; i < config->seed_count; ++i) {
        if (!config->seed_hosts[i] || !config->seed_hosts[i][0] || config->seed_ports[i] == 0)
            return NULL;
    }

    redis_cluster_t *cluster = calloc(1, sizeof(redis_cluster_t));
    if (!cluster) return NULL;

    /* Copy configuration */
    cluster->config = *config;
    cluster->config.seed_hosts = NULL;
    cluster->config.seed_ports = NULL;
    cluster->config.username = NULL;
    cluster->config.password = NULL;

    /* Copy seed hosts */
    cluster->config.seed_hosts = calloc(config->seed_count, sizeof(char *));
    cluster->config.seed_ports = calloc(config->seed_count, sizeof(uint16_t));

    if (!cluster->config.seed_hosts || !cluster->config.seed_ports) {
        free(cluster->config.seed_hosts);
        free(cluster->config.seed_ports);
        free(cluster);
        return NULL;
    }

    for (size_t i = 0; i < config->seed_count; i++) {
        cluster->config.seed_hosts[i] = tstr_dup(config->seed_hosts[i]);
        if (!cluster->config.seed_hosts[i]) {
            redis_cluster_destroy(cluster);
            return NULL;
        }
        cluster->config.seed_ports[i] = config->seed_ports[i];
    }

    if (config->username) {
        cluster->config.username = tstr_dup(config->username);
        if (!cluster->config.username) {
            redis_cluster_destroy(cluster);
            return NULL;
        }
    }

    if (config->password) {
        cluster->config.password = tstr_dup(config->password);
        if (!cluster->config.password) {
            redis_cluster_destroy(cluster);
            return NULL;
        }
    }

    /* Set defaults */
    if (cluster->config.connections_per_node == 0) {
        cluster->config.connections_per_node = 5;
    }
    if (cluster->config.connect_timeout_ms == 0) {
        cluster->config.connect_timeout_ms = 5000;
    }
    if (cluster->config.command_timeout_ms == 0) {
        cluster->config.command_timeout_ms = 5000;
    }
    if (cluster->config.topology_refresh_ms == 0) {
        cluster->config.topology_refresh_ms = 30000;
    }
    if (cluster->config.max_redirections == 0) {
        cluster->config.max_redirections = 5;
    }

    return cluster;
}

int redis_cluster_connect(redis_cluster_t *cluster) {
    if (!cluster) return -1;
    return discover_topology(cluster) == TURBO_OK ? 0 : -1;
}

void redis_cluster_disconnect(redis_cluster_t *cluster) {
    if (!cluster) return;

    cluster_node_t *node = cluster->nodes;
    while (node) {
        if (node->pool) {
            redis_pool_stop(node->pool);
            node->pool_started = 0;
        }
        node = node->next;
    }

    cluster->connected = 0;
}

void redis_cluster_destroy(redis_cluster_t *cluster) {
    if (!cluster) return;

    redis_cluster_disconnect(cluster);
    clear_nodes(cluster);

    /* Free configuration */
    for (size_t i = 0; i < cluster->config.seed_count; i++) {
        tstr_free((tstr)cluster->config.seed_hosts[i]);
    }
    free(cluster->config.seed_hosts);
    free(cluster->config.seed_ports);
    tstr_free((tstr)cluster->config.username);
    tstr_free((tstr)cluster->config.password);

    free(cluster);
}

int redis_cluster_refresh(redis_cluster_t *cluster) {
    if (!cluster || !cluster->connected) return -1;
    return discover_topology(cluster) == TURBO_OK ? 0 : -1;
}

/* =============================================================================
 * Command Routing
 * =============================================================================
 */

const redis_cluster_node_t *redis_cluster_get_node(redis_cluster_t *cluster,
                                                    uint16_t slot) {
    if (!cluster || slot >= REDIS_CLUSTER_SLOTS) return NULL;
    cluster_node_t *node = cluster->slots[slot];
    return node ? &node->info : NULL;
}

typedef enum {
    CLUSTER_REDIRECT_NONE,
    CLUSTER_REDIRECT_MOVED,
    CLUSTER_REDIRECT_ASK
} cluster_redirect_kind_t;

typedef struct {
    cluster_redirect_kind_t kind;
    uint16_t slot;
    tstr host;
    uint16_t port;
} cluster_redirect_t;

static void cluster_redirect_clear(cluster_redirect_t *redirect) {
    if (!redirect) return;
    tstr_free(redirect->host);
    memset(redirect, 0, sizeof(*redirect));
}

static int parse_redirection(const redis_command_result_t *result,
                             const char *source_host,
                             cluster_redirect_t *redirect) {
    const char *text;
    const char *cursor;
    const char *endpoint;
    const char *colon;
    char *endptr;
    long slot;
    long port;
    size_t host_len;

    if (!result || !source_host || !redirect || !result->reply ||
        result->reply->type != REDIS_REPLY_ERROR || !result->reply->str)
        return TURBO_EINVAL;
    memset(redirect, 0, sizeof(*redirect));
    text = result->reply->str;
    if (result->server_error == REDIS_SERVER_ERROR_MOVED) {
        redirect->kind = CLUSTER_REDIRECT_MOVED;
        cursor = text + 6;
    } else if (result->server_error == REDIS_SERVER_ERROR_ASK) {
        redirect->kind = CLUSTER_REDIRECT_ASK;
        cursor = text + 4;
    } else {
        return TURBO_EINVAL;
    }

    errno = 0;
    slot = strtol(cursor, &endptr, 10);
    if (errno || endptr == cursor || *endptr != ' ' || slot < 0 ||
        slot >= REDIS_CLUSTER_SLOTS)
        return TURBO_EPROTO;
    endpoint = endptr + 1;
    colon = strrchr(endpoint, ':');
    if (!colon) return TURBO_EPROTO;
    errno = 0;
    port = strtol(colon + 1, &endptr, 10);
    if (errno || endptr == colon + 1 || *endptr != '\0' || port <= 0 ||
        port > UINT16_MAX)
        return TURBO_EPROTO;

    host_len = (size_t)(colon - endpoint);
    if (host_len == 0) {
        redirect->host = tstr_dup(source_host);
    } else if (host_len >= 2 && endpoint[0] == '[' &&
               endpoint[host_len - 1] == ']') {
        redirect->host = tstr_dup_len(endpoint + 1, host_len - 2);
    } else {
        redirect->host = tstr_dup_len(endpoint, host_len);
    }
    if (!redirect->host || !redirect->host[0] || redirect->host[0] == '?') {
        cluster_redirect_clear(redirect);
        return TURBO_EPROTO;
    }
    redirect->slot = (uint16_t)slot;
    redirect->port = (uint16_t)port;
    return TURBO_OK;
}

static cluster_node_t *ensure_redirect_node(redis_cluster_t *cluster,
                                            const cluster_redirect_t *redirect) {
    cluster_node_t *node;
    int created = 0;
    if (!cluster || !redirect || !redirect->host) return NULL;
    node = find_node(cluster, redirect->host, redirect->port);
    if (!node) {
        node = create_node(cluster, redirect->host, redirect->port, NULL, 1);
        if (!node) return NULL;
        created = 1;
    }
    if (start_node_pool(node) != TURBO_OK) {
        if (created) destroy_node(node);
        return NULL;
    }
    if (created) {
        node->next = cluster->nodes;
        cluster->nodes = node;
        cluster->node_count++;
    }
    if (redirect->kind == CLUSTER_REDIRECT_MOVED) {
        node->info.is_master = 1;
        cluster->slots[redirect->slot] = node;
        recompute_master_slot_bounds(cluster);
        recount_nodes(cluster);
    }
    return node;
}

static cluster_node_t *select_command_node(redis_cluster_t *cluster,
                                           cluster_node_t *master,
                                           int read_only) {
    cluster_node_t *replica;
    if (!cluster || !master) return NULL;
    if (!read_only || !cluster->config.route_reads_to_replicas) return master;
    replica = master->next_read_replica ? master->next_read_replica
                                        : master->replica_head;
    if (!replica) return NULL;
    master->next_read_replica = replica->replica_next
                                    ? replica->replica_next
                                    : master->replica_head;
    return replica;
}

static int cluster_send_commandv(redis_cluster_t *cluster, int argc,
                                 const char **argv, const size_t *argvlen,
                                 const char *key, size_t key_len,
                                 int read_only,
                                 redis_command_cb_t callback, void *user_data,
                                 redis_command_result_t *out) {
    redis_pool_conn_t *conn;
    redis_client_t *client;
    cluster_node_t *node;
    redis_command_result_t asking = REDIS_COMMAND_RESULT_INIT;
    cluster_redirect_t redirect = {0};
    const char *asking_argv[] = {"ASKING"};
    uint16_t slot;
    int ask = 0;
    int redirections = 0;
    int rc;

    if (!out) return TURBO_EINVAL;
    *out = (redis_command_result_t)REDIS_COMMAND_RESULT_INIT;
    if (!cluster || argc <= 0 || !argv || !key) {
        out->status = TURBO_EINVAL;
        return out->status;
    }
    if (!cluster->connected) {
        out->status = TURBO_ENOTCONN;
        return out->status;
    }
    for (int i = 0; i < argc; ++i) {
        if (!argv[i]) {
            out->status = TURBO_EINVAL;
            return out->status;
        }
    }

    if (cluster->config.topology_refresh_ms > 0 &&
        cluster->last_topology_refresh_ns > 0 &&
        turbo_hrtime() - cluster->last_topology_refresh_ns >=
            (uint64_t)cluster->config.topology_refresh_ms * UINT64_C(1000000) &&
        discover_topology(cluster) != TURBO_OK) {
        out->status = TURBO_ENOTCONN;
        cluster->stats.commands_failed++;
        return out->status;
    }

    slot = redis_cluster_keyslot(key, key_len);
    node = select_command_node(cluster, cluster->slots[slot], read_only);
    if (!node || !node->pool || !node->pool_started) {
        out->status = TURBO_ENOTCONN;
        cluster->stats.commands_failed++;
        return out->status;
    }

    for (;;) {
        conn = redis_pool_acquire(node->pool, 0);
        if (!conn) {
            out->status = TURBO_ENOTCONN;
            cluster->stats.commands_failed++;
            return out->status;
        }
        client = redis_pool_conn_client(conn);

        if (ask) {
            rc = redis_commandv_result(client, 1, asking_argv, NULL, &asking);
            if (rc != TURBO_OK || !asking.reply ||
                asking.reply->type != REDIS_REPLY_STRING ||
                asking.reply->len != 2 || !asking.reply->str ||
                memcmp(asking.reply->str, "OK", 2) != 0) {
                redis_command_result_clear(&asking);
                redis_pool_release(node->pool, conn);
                out->status = rc == TURBO_OK ? TURBO_EPROTO : rc;
                cluster->stats.commands_failed++;
                return out->status;
            }
            redis_command_result_clear(&asking);
        }

        rc = redis_commandv_result(client, argc, argv, argvlen, out);
        if (out->server_error != REDIS_SERVER_ERROR_MOVED &&
            out->server_error != REDIS_SERVER_ERROR_ASK) {
            if (out->outcome == REDIS_COMMAND_REPLIED && callback)
                callback(client, out->reply, user_data);
            redis_pool_release(node->pool, conn);
            if (out->outcome == REDIS_COMMAND_REPLIED)
                cluster->stats.commands_sent++;
            if (rc != TURBO_OK) cluster->stats.commands_failed++;
            return rc;
        }

        rc = parse_redirection(out, node->info.host, &redirect);
        if (rc != TURBO_OK || redirect.slot != slot) {
            out->status = TURBO_EPROTO;
            if (callback) callback(client, out->reply, user_data);
            redis_pool_release(node->pool, conn);
            cluster->stats.commands_sent++;
            cluster->stats.commands_failed++;
            cluster_redirect_clear(&redirect);
            return out->status;
        }
        cluster->stats.redirections++;
        if (redirections++ >= cluster->config.max_redirections) {
            out->status = TURBO_ELOOP;
            if (callback) callback(client, out->reply, user_data);
            redis_pool_release(node->pool, conn);
            cluster->stats.commands_sent++;
            cluster->stats.commands_failed++;
            cluster_redirect_clear(&redirect);
            return out->status;
        }

        cluster_node_t *target = ensure_redirect_node(cluster, &redirect);
        if (!target) {
            out->status = TURBO_ENOTCONN;
            if (callback) callback(client, out->reply, user_data);
            redis_pool_release(node->pool, conn);
            cluster->stats.commands_sent++;
            cluster->stats.commands_failed++;
            cluster_redirect_clear(&redirect);
            return out->status;
        }
        ask = redirect.kind == CLUSTER_REDIRECT_ASK;
        cluster_redirect_clear(&redirect);
        redis_command_result_clear(out);
        redis_pool_release(node->pool, conn);
        node = target;
    }
}

#define CLUSTER_FORMAT_MAX_ARGS 32
#define CLUSTER_FORMAT_ARG_SIZE 256

static int cluster_format_argv(const char *format, va_list ap,
                               const char **argv,
                               char arg_buf[CLUSTER_FORMAT_MAX_ARGS]
                                           [CLUSTER_FORMAT_ARG_SIZE]) {
    int argc = 0;
    const char *p = format;
    if (!format || !argv || !arg_buf) return -1;

    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (argc >= CLUSTER_FORMAT_MAX_ARGS) return -1;

        if (*p == '%') {
            p++;
            if (*p == 's') {
                argv[argc] = va_arg(ap, const char *);
                if (!argv[argc]) return -1;
                argc++;
            } else if (*p == 'd') {
                int value = va_arg(ap, int);
                fmt(arg_buf[argc], sizeof(arg_buf[argc]), "{}", value);
                argv[argc] = arg_buf[argc];
                argc++;
            } else {
                return -1;
            }
            p++;
        } else {
            const char *start = p;
            size_t len;
            while (*p && *p != ' ' && *p != '%') p++;
            len = (size_t)(p - start);
            if (len == 0 || len >= CLUSTER_FORMAT_ARG_SIZE) return -1;
            memcpy(arg_buf[argc], start, len);
            arg_buf[argc][len] = '\0';
            argv[argc] = arg_buf[argc];
            argc++;
        }
    }
    return argc;
}

static int cluster_command_key_route_v(redis_cluster_t *cluster, const char *key,
                                       int read_only,
                                       redis_command_cb_t callback,
                                       void *user_data, const char *format,
                                       va_list ap) {
    const char *argv[CLUSTER_FORMAT_MAX_ARGS];
    char arg_buf[CLUSTER_FORMAT_MAX_ARGS][CLUSTER_FORMAT_ARG_SIZE];
    redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
    int argc;
    int rc;

    if (!cluster || !cluster->connected || !key || !format) return -1;
    argc = cluster_format_argv(format, ap, argv, arg_buf);
    if (argc <= 0) return -1;
    rc = cluster_send_commandv(cluster, argc, argv, NULL, key, strlen(key),
                               read_only, callback, user_data, &result);
    (void)rc;
    rc = result.outcome == REDIS_COMMAND_REPLIED ? 0 : -1;
    redis_command_result_clear(&result);
    return rc;
}

static int cluster_command_key_route(redis_cluster_t *cluster, const char *key,
                                     int read_only,
                                     redis_command_cb_t callback,
                                     void *user_data, const char *format, ...) {
    va_list ap;
    int rc;
    va_start(ap, format);
    rc = cluster_command_key_route_v(cluster, key, read_only, callback,
                                     user_data, format, ap);
    va_end(ap);
    return rc;
}

int redis_cluster_command_key(redis_cluster_t *cluster, const char *key,
                              redis_command_cb_t callback, void *user_data,
                              const char *format, ...) {
    va_list ap;
    int rc;
    va_start(ap, format);
    rc = cluster_command_key_route_v(cluster, key, 0, callback, user_data,
                                     format, ap);
    va_end(ap);
    return rc;
}

int redis_cluster_command(redis_cluster_t *cluster, redis_command_cb_t callback,
                          void *user_data, const char *format, ...) {
    const char *argv[CLUSTER_FORMAT_MAX_ARGS];
    char arg_buf[CLUSTER_FORMAT_MAX_ARGS][CLUSTER_FORMAT_ARG_SIZE];
    redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
    va_list ap;
    int argc;
    int rc;

    if (!cluster || !cluster->connected || !format) return -1;
    va_start(ap, format);
    argc = cluster_format_argv(format, ap, argv, arg_buf);
    va_end(ap);
    if (argc < 2) return -1;
    rc = cluster_send_commandv(cluster, argc, argv, NULL, argv[1],
                               strlen(argv[1]), 0, callback, user_data, &result);
    (void)rc;
    rc = result.outcome == REDIS_COMMAND_REPLIED ? 0 : -1;
    redis_command_result_clear(&result);
    return rc;
}

int redis_cluster_commandv(redis_cluster_t *cluster, int argc, const char **argv,
                           const size_t *argvlen, int key_index,
                           redis_command_cb_t callback, void *user_data) {
    redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
    int kidx = key_index >= 0 ? key_index : 1;
    int rc;
    if (!cluster || !cluster->connected || !argv || argc <= 0 || kidx < 0 ||
        kidx >= argc || !argv[kidx])
        return -1;
    rc = cluster_send_commandv(cluster, argc, argv, argvlen, argv[kidx],
                               argvlen ? argvlen[kidx] : strlen(argv[kidx]),
                               0, callback, user_data, &result);
    (void)rc;
    rc = result.outcome == REDIS_COMMAND_REPLIED ? 0 : -1;
    redis_command_result_clear(&result);
    return rc;
}

int redis_cluster_commandv_result(redis_cluster_t *cluster, int argc,
                                  const char **argv, const size_t *argvlen,
                                  int key_index, redis_command_result_t *out) {
    int kidx = key_index >= 0 ? key_index : 1;
    if (!out) return TURBO_EINVAL;
    *out = (redis_command_result_t)REDIS_COMMAND_RESULT_INIT;
    if (!cluster || !argv || argc <= 0 || kidx < 0 || kidx >= argc ||
        !argv[kidx]) {
        out->status = TURBO_EINVAL;
        return out->status;
    }
    if (!cluster->connected) {
        out->status = TURBO_ENOTCONN;
        return out->status;
    }
    return cluster_send_commandv(cluster, argc, argv, argvlen, argv[kidx],
                                 argvlen ? argvlen[kidx] : strlen(argv[kidx]),
                                 0, NULL, NULL, out);
}

int redis_cluster_read_commandv(redis_cluster_t *cluster, int argc,
                                const char **argv, const size_t *argvlen,
                                int key_index, redis_command_cb_t callback,
                                void *user_data) {
    redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
    int kidx = key_index >= 0 ? key_index : 1;
    int rc;
    if (!cluster || !cluster->connected || !argv || argc <= 0 || kidx < 0 ||
        kidx >= argc || !argv[kidx])
        return -1;
    rc = cluster_send_commandv(cluster, argc, argv, argvlen, argv[kidx],
                               argvlen ? argvlen[kidx] : strlen(argv[kidx]),
                               1, callback, user_data, &result);
    (void)rc;
    rc = result.outcome == REDIS_COMMAND_REPLIED ? 0 : -1;
    redis_command_result_clear(&result);
    return rc;
}

int redis_cluster_read_commandv_result(redis_cluster_t *cluster, int argc,
                                       const char **argv,
                                       const size_t *argvlen, int key_index,
                                       redis_command_result_t *out) {
    int kidx = key_index >= 0 ? key_index : 1;
    if (!out) return TURBO_EINVAL;
    *out = (redis_command_result_t)REDIS_COMMAND_RESULT_INIT;
    if (!cluster || !argv || argc <= 0 || kidx < 0 || kidx >= argc ||
        !argv[kidx]) {
        out->status = TURBO_EINVAL;
        return out->status;
    }
    if (!cluster->connected) {
        out->status = TURBO_ENOTCONN;
        return out->status;
    }
    return cluster_send_commandv(cluster, argc, argv, argvlen, argv[kidx],
                                 argvlen ? argvlen[kidx] : strlen(argv[kidx]),
                                 1, NULL, NULL, out);
}

/* =============================================================================
 * Convenience Functions
 * =============================================================================
 */

int redis_cluster_set(redis_cluster_t *cluster, const char *key, const char *value,
                      redis_command_cb_t callback, void *user_data) {
    return redis_cluster_command_key(cluster, key, callback, user_data,
                                      "SET %s %s", key, value);
}

int redis_cluster_get(redis_cluster_t *cluster, const char *key,
                      redis_command_cb_t callback, void *user_data) {
    return cluster_command_key_route(cluster, key, 1, callback, user_data,
                                     "GET %s", key);
}

int redis_cluster_del(redis_cluster_t *cluster, const char *key,
                      redis_command_cb_t callback, void *user_data) {
    return redis_cluster_command_key(cluster, key, callback, user_data,
                                      "DEL %s", key);
}

int redis_cluster_expire(redis_cluster_t *cluster, const char *key, int seconds,
                         redis_command_cb_t callback, void *user_data) {
    return redis_cluster_command_key(cluster, key, callback, user_data,
                                      "EXPIRE %s %d", key, seconds);
}

int redis_cluster_incr(redis_cluster_t *cluster, const char *key,
                       redis_command_cb_t callback, void *user_data) {
    return redis_cluster_command_key(cluster, key, callback, user_data,
                                      "INCR %s", key);
}

int redis_cluster_hset(redis_cluster_t *cluster, const char *key,
                       const char *field, const char *value,
                       redis_command_cb_t callback, void *user_data) {
    return redis_cluster_command_key(cluster, key, callback, user_data,
                                      "HSET %s %s %s", key, field, value);
}

int redis_cluster_hget(redis_cluster_t *cluster, const char *key,
                       const char *field, redis_command_cb_t callback,
                       void *user_data) {
    return cluster_command_key_route(cluster, key, 1, callback, user_data,
                                     "HGET %s %s", key, field);
}

int redis_cluster_hdel(redis_cluster_t *cluster, const char *key,
                       const char *field, redis_command_cb_t callback,
                       void *user_data) {
    return redis_cluster_command_key(cluster, key, callback, user_data,
                                      "HDEL %s %s", key, field);
}

int redis_cluster_xadd(redis_cluster_t *cluster, const char *key, size_t maxlen,
                       size_t field_count, const char **fields,
                       const char **values, const size_t *value_lens,
                       redis_command_cb_t callback, void *user_data) {
    const char **argv;
    size_t *argvlen;
    char maxlen_str[32];
    int argc;
    size_t idx = 0;
    int result;

    if (!cluster || !cluster->connected || !key || !fields || !values ||
        field_count == 0 || field_count > (size_t)INT_MAX)
        return -1;
    for (size_t i = 0; i < field_count; ++i)
        if (!fields[i] || !values[i]) return -1;

    if (calc_argc_mul_add(3, (int)field_count, 2, &argc) < 0) return -1;
    if (maxlen > 0) {
      if (calc_argc_mul_add(argc, 3, 1, &argc) < 0) return -1;
    }
    argv = alloc_argv(argc);
    if ((size_t)argc > SIZE_MAX / sizeof(size_t)) {
      redis_argv_free(argv);
      return -1;
    }
    argvlen = malloc((size_t)argc * sizeof(*argvlen));
    if (!argv || !argvlen) {
        redis_argv_free(argv);
        free(argvlen);
        return -1;
    }
    argv[idx] = "XADD"; argvlen[idx++] = 4;
    argv[idx] = key; argvlen[idx++] = strlen(key);
    if (maxlen > 0) {
        argv[idx] = "MAXLEN"; argvlen[idx++] = 6;
        argv[idx] = "~"; argvlen[idx++] = 1;
        fmt(maxlen_str, sizeof(maxlen_str), "{}", maxlen);
        argv[idx] = maxlen_str; argvlen[idx++] = strlen(maxlen_str);
    }
    argv[idx] = "*"; argvlen[idx++] = 1;
    for (size_t i = 0; i < field_count; ++i) {
        argv[idx] = fields[i]; argvlen[idx++] = strlen(fields[i]);
        argv[idx] = values[i];
        argvlen[idx++] = value_lens ? value_lens[i] : strlen(values[i]);
    }
    result = redis_cluster_commandv(cluster, (int)idx, argv, argvlen, 1,
                                    callback, user_data);
    redis_argv_free(argv);
    free(argvlen);
    return result;
}

typedef struct {
    redis_stream_cb_t callback;
    void *user_data;
    int decode_status;
} cluster_stream_callback_t;

static void on_cluster_stream_reply(redis_client_t *client,
                                    redis_reply_t *reply, void *user_data) {
    cluster_stream_callback_t *ctx = (cluster_stream_callback_t *)user_data;
    redis_stream_result_t *streams = NULL;
    size_t stream_count = 0;
    ctx->decode_status = redis_stream_reply_decode(reply, &streams, &stream_count);
    if (ctx->decode_status == TURBO_OK && ctx->callback)
        ctx->callback(client, streams, stream_count, ctx->user_data);
    redis_stream_result_free(streams, stream_count);
}

int redis_cluster_xread(redis_cluster_t *cluster, const char *key,
                        size_t count, int block_ms, const char *last_id,
                        redis_stream_cb_t callback, void *user_data) {
    const char *argv[8];
    char count_str[32];
    char block_str[32];
    size_t idx = 0;
    redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
    cluster_stream_callback_t callback_ctx = {callback, user_data, TURBO_EPROTO};
    int rc;

    if (!cluster || !cluster->connected || !key) return -1;
    argv[idx++] = "XREAD";
    if (count > 0) {
        argv[idx++] = "COUNT";
        fmt(count_str, sizeof(count_str), "{}", count);
        argv[idx++] = count_str;
    }
    if (block_ms >= 0) {
        argv[idx++] = "BLOCK";
        fmt(block_str, sizeof(block_str), "{}", block_ms);
        argv[idx++] = block_str;
    }
    argv[idx++] = "STREAMS";
    argv[idx++] = key;
    argv[idx++] = last_id ? last_id : "$";

    rc = cluster_send_commandv(cluster, (int)idx, argv, NULL, key, strlen(key),
                               1, on_cluster_stream_reply, &callback_ctx, &result);
    redis_command_result_clear(&result);
    return rc == TURBO_OK && callback_ctx.decode_status == TURBO_OK ? 0 : -1;
}

/* =============================================================================
 * Multi-Key Operations
 * =============================================================================
 */

static int verify_same_slot(redis_cluster_t *cluster, int key_count,
                            const char **keys) {
    if (key_count <= 0 || !keys) return -1;

    uint16_t first_slot = redis_cluster_keyslot(keys[0], strlen(keys[0]));

    for (int i = 1; i < key_count; i++) {
        uint16_t slot = redis_cluster_keyslot(keys[i], strlen(keys[i]));
        if (slot != first_slot) {
            return -1;  /* Keys in different slots */
        }
    }

    return first_slot;
}

int redis_cluster_mdelete(redis_cluster_t *cluster, int key_count,
                          const char **keys, redis_command_cb_t callback,
                          void *user_data) {
    const char **argv;
    int argc;
    int result;
    if (!cluster || !cluster->connected || key_count <= 0 || !keys) return -1;

    int slot = verify_same_slot(cluster, key_count, keys);
    if (slot < 0) {
        /* Keys span multiple slots - not allowed */
        return -1;
    }

    (void)slot;
    if (calc_argc_mul_add(1, key_count, 1, &argc) < 0) return -1;
    argv = alloc_argv(argc);
    if (!argv) return -1;
    argv[0] = "DEL";
    for (int i = 0; i < key_count; i++) argv[i + 1] = keys[i];
    result = redis_cluster_commandv(cluster, argc, argv, NULL, 1,
                                    callback, user_data);
    redis_argv_free(argv);
    return result;
}

int redis_cluster_mget(redis_cluster_t *cluster, int key_count,
                       const char **keys, redis_command_cb_t callback,
                       void *user_data) {
    const char **argv;
    int argc;
    int result;
    if (!cluster || !cluster->connected || key_count <= 0 || !keys) return -1;

    int slot = verify_same_slot(cluster, key_count, keys);
    if (slot < 0) {
        /* Keys span multiple slots - not allowed */
        return -1;
    }

    (void)slot;
    if (calc_argc_mul_add(1, key_count, 1, &argc) < 0) return -1;
    argv = alloc_argv(argc);
    if (!argv) return -1;
    argv[0] = "MGET";
    for (int i = 0; i < key_count; i++) argv[i + 1] = keys[i];
    result = redis_cluster_read_commandv(cluster, argc, argv, NULL, 1,
                                         callback, user_data);
    redis_argv_free(argv);
    return result;
}

/* =============================================================================
 * Statistics & Health
 * =============================================================================
 */

void redis_cluster_get_stats(redis_cluster_t *cluster, redis_cluster_stats_t *stats) {
    if (!cluster || !stats) return;

    *stats = cluster->stats;

    /* Count total connections across all nodes */
    stats->total_connections = 0;
    cluster_node_t *node = cluster->nodes;
    while (node) {
        if (node->pool) {
            redis_pool_stats_t pool_stats;
            redis_pool_get_stats(node->pool, &pool_stats);
            stats->total_connections += pool_stats.total_connections;
        }
        node = node->next;
    }
}

void redis_cluster_reset_stats(redis_cluster_t *cluster) {
    if (!cluster) return;

    cluster->stats.commands_sent = 0;
    cluster->stats.commands_failed = 0;
    cluster->stats.redirections = 0;
}

int redis_cluster_is_healthy(redis_cluster_t *cluster) {
    if (!cluster || !cluster->connected) return 0;

    /* Check all slots are covered */
    for (int i = 0; i < REDIS_CLUSTER_SLOTS; i++) {
        if (!cluster->slots[i]) return 0;
    }

    for (cluster_node_t *node = cluster->nodes; node; node = node->next) {
        if (node->info.is_master &&
            (!node->pool_started || !redis_pool_is_healthy(node->pool)))
            return 0;
    }

    return 1;
}

void redis_cluster_node_count(redis_cluster_t *cluster, size_t *masters,
                              size_t *replicas) {
    if (!cluster) {
        if (masters) *masters = 0;
        if (replicas) *replicas = 0;
        return;
    }

    if (masters) *masters = cluster->stats.master_count;
    if (replicas) *replicas = cluster->stats.replica_count;
}

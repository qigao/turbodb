#include "redis_cluster.h"

#include "turbo_error.h"
#include "turbo_str.h"

#include <stdlib.h>
#include <string.h>

typedef enum redis_cluster_phase {
  REDIS_CLUSTER_PHASE_SEED_CONNECT = 0,
  REDIS_CLUSTER_PHASE_DISCOVERY,
  REDIS_CLUSTER_PHASE_NODE_CONNECT,
  REDIS_CLUSTER_PHASE_READY,
  REDIS_CLUSTER_PHASE_FAILED,
  REDIS_CLUSTER_PHASE_CLOSING,
  REDIS_CLUSTER_PHASE_CLOSED
} redis_cluster_phase;

typedef struct redis_cluster_node_impl {
  redis_cluster_node view;
  tstr host;
  tstr node_id;
  redis_pool pool;
} redis_cluster_node_impl;

typedef struct redis_cluster_impl {
  redis_io_runtime *runtime;
  tstr *seed_hosts;
  uint16_t *seed_ports;
  size_t seed_count;
  tstr username;
  tstr password;
  size_t connections_per_node;
  size_t max_nodes;
  cflow_io_lease_id base_lease_id;
  size_t address_capacity;
  size_t max_command_bytes;
  size_t initial_buffer_bytes;
  size_t max_buffer_bytes;
  size_t receive_chunk_bytes;
  size_t topology_reply_bytes;
  uint64_t cancel_timeout_ns;
  redis_cluster_node_impl *nodes;
  size_t node_count;
  redis_cluster_node_impl **slots;
  redis_pool seed_pool;
  redis_pool_stream discovery;
  int discovery_item_seen;
  size_t connect_node;
  redis_cluster_phase phase;
} redis_cluster_impl;

static const uint16_t redis_cluster_crc16_table[256] = {
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
    0x6e17, 0x7e36, 0x4e55, 0x5e74, 0x2e93, 0x3eb2, 0x0ed1, 0x1ef0};

static uint16_t redis_cluster_crc16(const char *data, size_t length) {
  uint16_t crc = 0u;
  size_t index;
  for (index = 0; index < length; ++index)
    crc = (uint16_t)((crc << 8u) ^
                     redis_cluster_crc16_table[
                         ((crc >> 8u) ^ (uint8_t)data[index]) & 0xffu]);
  return crc;
}

uint16_t redis_cluster_keyslot(const char *key, size_t length) {
  size_t start;
  size_t end;
  if (!key) return 0u;
  for (start = 0u; start < length && key[start] != '{'; ++start) {
  }
  if (start < length) {
    for (end = start + 1u; end < length && key[end] != '}'; ++end) {
    }
    if (end < length && end > start + 1u)
      return (uint16_t)(redis_cluster_crc16(key + start + 1u,
                                            end - start - 1u) &
                        0x3fffu);
  }
  return (uint16_t)(redis_cluster_crc16(key, length) & 0x3fffu);
}

static redis_cluster_impl *redis_cluster_get(redis_cluster *cluster) {
  return cluster ? (redis_cluster_impl *)cluster->impl : NULL;
}

static const redis_cluster_impl *redis_cluster_get_const(
    const redis_cluster *cluster) {
  return cluster ? (const redis_cluster_impl *)cluster->impl : NULL;
}

static redis_cluster_connect_step redis_cluster_result(
    const redis_cluster_impl *impl, redis_cluster_connect_step_kind kind,
    int status, cflow_waitable waitable) {
  redis_cluster_connect_step step;
  memset(&step, 0, sizeof(step));
  step.kind = kind;
  step.status = status;
  step.waitable = waitable;
  step.node_count = impl ? impl->node_count : 0u;
  return step;
}

static tstr redis_cluster_copy(const char *value) {
  return value ? tstr_new_len(value, strlen(value)) : NULL;
}

static redis_pool_config redis_cluster_pool_config(
    const redis_cluster_impl *impl, const char *host, uint16_t port,
    size_t node_index, size_t capacity) {
  redis_pool_config config = REDIS_POOL_CONFIG_INIT;
  config.runtime = impl->runtime;
  config.host = host;
  config.port = port;
  config.username = impl->username;
  config.password = impl->password;
  config.connection_capacity = capacity;
  config.base_lease_id =
      impl->base_lease_id + 1u + node_index * impl->connections_per_node;
  config.address_capacity = impl->address_capacity;
  config.max_command_bytes = impl->max_command_bytes;
  config.initial_buffer_bytes = impl->initial_buffer_bytes;
  config.max_buffer_bytes = impl->max_buffer_bytes;
  config.receive_chunk_bytes = impl->receive_chunk_bytes;
  config.prepare_reply_bytes = impl->topology_reply_bytes;
  config.cancel_timeout_ns = impl->cancel_timeout_ns;
  return config;
}

static int redis_cluster_reply_string(const redis_reply_t *reply,
                                      const char **value) {
  if (!reply || !value ||
      (reply->type != REDIS_REPLY_STRING &&
       reply->type != REDIS_REPLY_BULK_STRING) ||
      !reply->str || reply->len == 0u)
    return TURBO_EPROTO;
  *value = reply->str;
  return TURBO_OK;
}

static int redis_cluster_find_or_add_node(
    redis_cluster_impl *impl, const char *host, uint16_t port,
    const char *node_id, redis_cluster_node_impl **out_node) {
  size_t index;
  redis_cluster_node_impl *node;
  tstr host_copy;
  tstr node_id_copy = NULL;
  for (index = 0u; index < impl->node_count; ++index) {
    node = &impl->nodes[index];
    if (node->view.port == port && strcmp(node->host, host) == 0) {
      *out_node = node;
      return TURBO_OK;
    }
  }
  if (impl->node_count == impl->max_nodes) return TURBO_ENOBUFS;
  host_copy = redis_cluster_copy(host);
  if (!host_copy) return TURBO_ENOMEM;
  if (node_id) {
    node_id_copy = redis_cluster_copy(node_id);
    if (!node_id_copy) {
      tstr_free(host_copy);
      return TURBO_ENOMEM;
    }
  }
  node = &impl->nodes[impl->node_count];
  node->host = host_copy;
  node->node_id = node_id_copy;
  node->view.host = node->host;
  node->view.node_id = node->node_id;
  node->view.port = port;
  node->view.slot_start = UINT16_MAX;
  node->view.slot_end = 0u;
  ++impl->node_count;
  *out_node = node;
  return TURBO_OK;
}

static int redis_cluster_parse_slots(redis_cluster_impl *impl,
                                     const redis_reply_t *reply) {
  size_t range_index;
  if (!reply || reply->type != REDIS_REPLY_ARRAY ||
      reply->element_count == 0u)
    return TURBO_EPROTO;
  memset(impl->slots, 0,
         REDIS_CLUSTER_SLOT_COUNT * sizeof(*impl->slots));
  for (range_index = 0u; range_index < reply->element_count; ++range_index) {
    const redis_reply_t *range = reply->elements[range_index];
    const redis_reply_t *endpoint;
    const char *host;
    const char *node_id = NULL;
    uint16_t start;
    uint16_t end;
    uint16_t port;
    redis_cluster_node_impl *node;
    size_t slot;
    if (!range || range->type != REDIS_REPLY_ARRAY ||
        range->element_count < 3u || !range->elements[0] ||
        !range->elements[1] ||
        range->elements[0]->type != REDIS_REPLY_INTEGER ||
        range->elements[1]->type != REDIS_REPLY_INTEGER ||
        range->elements[0]->integer < 0 ||
        range->elements[1]->integer < range->elements[0]->integer ||
        range->elements[1]->integer >= REDIS_CLUSTER_SLOT_COUNT)
      return TURBO_EPROTO;
    endpoint = range->elements[2];
    if (!endpoint || endpoint->type != REDIS_REPLY_ARRAY ||
        endpoint->element_count < 2u ||
        redis_cluster_reply_string(endpoint->elements[0], &host) != TURBO_OK ||
        !endpoint->elements[1] ||
        endpoint->elements[1]->type != REDIS_REPLY_INTEGER ||
        endpoint->elements[1]->integer <= 0 ||
        endpoint->elements[1]->integer > UINT16_MAX)
      return TURBO_EPROTO;
    if (endpoint->element_count > 2u && endpoint->elements[2] &&
        endpoint->elements[2]->type != REDIS_REPLY_NULL &&
        redis_cluster_reply_string(endpoint->elements[2], &node_id) != TURBO_OK)
      return TURBO_EPROTO;
    start = (uint16_t)range->elements[0]->integer;
    end = (uint16_t)range->elements[1]->integer;
    port = (uint16_t)endpoint->elements[1]->integer;
    {
      int status =
          redis_cluster_find_or_add_node(impl, host, port, node_id, &node);
      if (status != TURBO_OK) return status;
    }
    if (start < node->view.slot_start) node->view.slot_start = start;
    if (end > node->view.slot_end) node->view.slot_end = end;
    for (slot = start; slot <= end; ++slot) {
      if (impl->slots[slot] && impl->slots[slot] != node)
        return TURBO_EPROTO;
      impl->slots[slot] = node;
    }
  }
  for (range_index = 0u; range_index < REDIS_CLUSTER_SLOT_COUNT;
       ++range_index) {
    if (!impl->slots[range_index]) return TURBO_EPROTO;
  }
  return TURBO_OK;
}

int redis_cluster_init(redis_cluster *cluster,
                       const redis_cluster_config *config) {
  redis_cluster_impl *impl;
  size_t index;
  size_t lease_span;
  if (!cluster || cluster->impl || !config ||
      !redis_io_runtime_valid(config->runtime) || !config->seed_hosts ||
      !config->seed_ports || config->seed_count == 0u ||
      config->connections_per_node == 0u || config->max_nodes == 0u ||
      config->base_lease_id == 0u || config->address_capacity == 0u ||
      config->max_command_bytes == 0u ||
      config->initial_buffer_bytes == 0u ||
      config->max_buffer_bytes < config->initial_buffer_bytes ||
      config->receive_chunk_bytes == 0u ||
      config->topology_reply_bytes == 0u || config->cancel_timeout_ns == 0u ||
      config->max_nodes >
          (SIZE_MAX - 1u) / config->connections_per_node)
    return TURBO_EINVAL;
  lease_span = 1u + config->max_nodes * config->connections_per_node;
  if (lease_span - 1u > UINT64_MAX - config->base_lease_id ||
      config->max_nodes > SIZE_MAX / sizeof(redis_cluster_node_impl) ||
      config->seed_count > SIZE_MAX / sizeof(tstr) ||
      config->seed_count > SIZE_MAX / sizeof(uint16_t))
    return TURBO_EINVAL;
  impl = (redis_cluster_impl *)calloc(1, sizeof(*impl));
  if (!impl) return TURBO_ENOMEM;
  impl->seed_hosts = (tstr *)calloc(config->seed_count, sizeof(tstr));
  impl->seed_ports =
      (uint16_t *)calloc(config->seed_count, sizeof(uint16_t));
  impl->nodes = (redis_cluster_node_impl *)calloc(config->max_nodes,
                                                   sizeof(*impl->nodes));
  impl->slots = (redis_cluster_node_impl **)calloc(
      REDIS_CLUSTER_SLOT_COUNT, sizeof(*impl->slots));
  impl->username = redis_cluster_copy(config->username);
  impl->password = redis_cluster_copy(config->password);
  if (!impl->seed_hosts || !impl->seed_ports || !impl->nodes || !impl->slots ||
      (config->username && !impl->username) ||
      (config->password && !impl->password))
    goto no_memory;
  for (index = 0u; index < config->seed_count; ++index) {
    if (!config->seed_hosts[index] || config->seed_hosts[index][0] == '\0' ||
        config->seed_ports[index] == 0u)
      goto invalid;
    impl->seed_hosts[index] = redis_cluster_copy(config->seed_hosts[index]);
    if (!impl->seed_hosts[index]) goto no_memory;
    impl->seed_ports[index] = config->seed_ports[index];
  }
  impl->runtime = config->runtime;
  impl->seed_count = config->seed_count;
  impl->connections_per_node = config->connections_per_node;
  impl->max_nodes = config->max_nodes;
  impl->base_lease_id = config->base_lease_id;
  impl->address_capacity = config->address_capacity;
  impl->max_command_bytes = config->max_command_bytes;
  impl->initial_buffer_bytes = config->initial_buffer_bytes;
  impl->max_buffer_bytes = config->max_buffer_bytes;
  impl->receive_chunk_bytes = config->receive_chunk_bytes;
  impl->topology_reply_bytes = config->topology_reply_bytes;
  impl->cancel_timeout_ns = config->cancel_timeout_ns;
  impl->phase = REDIS_CLUSTER_PHASE_SEED_CONNECT;
  {
    redis_pool_config seed_config = redis_cluster_pool_config(
        impl, impl->seed_hosts[0], impl->seed_ports[0], 0u, 1u);
    seed_config.base_lease_id = impl->base_lease_id;
    if (redis_pool_init(&impl->seed_pool, &seed_config) != TURBO_OK)
      goto invalid;
  }
  cluster->impl = impl;
  return TURBO_OK;

invalid:
  for (index = 0u; index < config->seed_count; ++index)
    tstr_free(impl->seed_hosts ? impl->seed_hosts[index] : NULL);
  tstr_free(impl->password);
  tstr_free(impl->username);
  free(impl->slots);
  free(impl->nodes);
  free(impl->seed_ports);
  free(impl->seed_hosts);
  free(impl);
  return TURBO_EINVAL;
no_memory:
  for (index = 0u; index < config->seed_count; ++index)
    tstr_free(impl->seed_hosts ? impl->seed_hosts[index] : NULL);
  tstr_free(impl->password);
  tstr_free(impl->username);
  free(impl->slots);
  free(impl->nodes);
  free(impl->seed_ports);
  free(impl->seed_hosts);
  free(impl);
  return TURBO_ENOMEM;
}

redis_cluster_connect_step redis_cluster_connect_next(
    redis_cluster *cluster) {
  static const char *topology_command[] = {"CLUSTER", "SLOTS"};
  redis_cluster_impl *impl = redis_cluster_get(cluster);
  cflow_waitable empty_waitable;
  memset(&empty_waitable, 0, sizeof(empty_waitable));
  if (!impl)
    return redis_cluster_result(NULL, REDIS_CLUSTER_CONNECT_ERROR,
                                TURBO_EINVAL, empty_waitable);
  if (impl->phase == REDIS_CLUSTER_PHASE_READY)
    return redis_cluster_result(impl, REDIS_CLUSTER_CONNECT_DONE, TURBO_OK,
                                empty_waitable);
  if (impl->phase == REDIS_CLUSTER_PHASE_SEED_CONNECT) {
    redis_pool_connect_step seed = redis_pool_connect_next(&impl->seed_pool);
    if (seed.kind == REDIS_POOL_CONNECT_WAIT)
      return redis_cluster_result(impl, REDIS_CLUSTER_CONNECT_WAIT, TURBO_OK,
                                  seed.waitable);
    if (seed.kind == REDIS_POOL_CONNECT_ERROR) {
      if (seed.status != TURBO_EBUSY)
        impl->phase = REDIS_CLUSTER_PHASE_FAILED;
      return redis_cluster_result(impl, REDIS_CLUSTER_CONNECT_ERROR,
                                  seed.status, empty_waitable);
    }
    {
      int status = redis_pool_command_open(
          &impl->seed_pool, 2, topology_command, NULL,
          impl->topology_reply_bytes, &impl->discovery);
      if (status != TURBO_OK) {
        impl->phase = REDIS_CLUSTER_PHASE_FAILED;
        return redis_cluster_result(impl, REDIS_CLUSTER_CONNECT_ERROR,
                                    status, empty_waitable);
      }
    }
    impl->phase = REDIS_CLUSTER_PHASE_DISCOVERY;
  }
  if (impl->phase == REDIS_CLUSTER_PHASE_DISCOVERY) {
    for (;;) {
      redis_cflow_stream_step discovered =
          redis_pool_stream_next(&impl->discovery);
      if (discovered.kind == REDIS_CFLOW_STREAM_WAIT)
        return redis_cluster_result(impl, REDIS_CLUSTER_CONNECT_WAIT, TURBO_OK,
                                    discovered.waitable);
      if (discovered.kind == REDIS_CFLOW_STREAM_ERROR) {
        int status = discovered.status;
        redis_reply_free(discovered.item);
        impl->phase = REDIS_CLUSTER_PHASE_FAILED;
        return redis_cluster_result(impl, REDIS_CLUSTER_CONNECT_ERROR,
                                    status, empty_waitable);
      }
      if (discovered.kind == REDIS_CFLOW_STREAM_ITEM) {
        int status = redis_cluster_parse_slots(impl, discovered.item);
        redis_reply_free(discovered.item);
        if (status != TURBO_OK) {
          (void)redis_pool_stream_destroy(&impl->discovery);
          impl->phase = REDIS_CLUSTER_PHASE_FAILED;
          return redis_cluster_result(impl, REDIS_CLUSTER_CONNECT_ERROR,
                                      status, empty_waitable);
        }
        impl->discovery_item_seen = 1;
        continue;
      }
      if (!impl->discovery_item_seen) goto failed_protocol;
      if (redis_pool_stream_destroy(&impl->discovery) != TURBO_OK ||
          redis_pool_close(&impl->seed_pool) != TURBO_OK ||
          redis_pool_destroy(&impl->seed_pool) != TURBO_OK)
        goto failed_cleanup;
      impl->phase = REDIS_CLUSTER_PHASE_NODE_CONNECT;
      break;
    }
  }
  while (impl->phase == REDIS_CLUSTER_PHASE_NODE_CONNECT &&
         impl->connect_node < impl->node_count) {
    redis_cluster_node_impl *node = &impl->nodes[impl->connect_node];
    if (!node->pool.impl) {
      redis_pool_config config = redis_cluster_pool_config(
          impl, node->host, node->view.port, impl->connect_node,
          impl->connections_per_node);
      int status = redis_pool_init(&node->pool, &config);
      if (status != TURBO_OK) {
        impl->phase = REDIS_CLUSTER_PHASE_FAILED;
        return redis_cluster_result(impl, REDIS_CLUSTER_CONNECT_ERROR,
                                    status, empty_waitable);
      }
    }
    {
      redis_pool_connect_step connected = redis_pool_connect_next(&node->pool);
      if (connected.kind == REDIS_POOL_CONNECT_WAIT)
        return redis_cluster_result(impl, REDIS_CLUSTER_CONNECT_WAIT, TURBO_OK,
                                    connected.waitable);
      if (connected.kind == REDIS_POOL_CONNECT_ERROR) {
        if (connected.status != TURBO_EBUSY)
          impl->phase = REDIS_CLUSTER_PHASE_FAILED;
        return redis_cluster_result(impl, REDIS_CLUSTER_CONNECT_ERROR,
                                    connected.status, empty_waitable);
      }
    }
    impl->connect_node++;
  }
  impl->phase = REDIS_CLUSTER_PHASE_READY;
  return redis_cluster_result(impl, REDIS_CLUSTER_CONNECT_DONE, TURBO_OK,
                              empty_waitable);

failed_protocol:
  impl->phase = REDIS_CLUSTER_PHASE_FAILED;
  return redis_cluster_result(impl, REDIS_CLUSTER_CONNECT_ERROR,
                              TURBO_EPROTO, empty_waitable);
failed_cleanup:
  impl->phase = REDIS_CLUSTER_PHASE_FAILED;
  return redis_cluster_result(impl, REDIS_CLUSTER_CONNECT_ERROR,
                              TURBO_EBUSY, empty_waitable);
}

int redis_cluster_command_open(redis_cluster *cluster, const char *key,
                               size_t key_length, int argc,
                               const char **argv, const size_t *argvlen,
                               size_t max_reply_bytes,
                               redis_pool_stream *out_stream) {
  redis_cluster_impl *impl = redis_cluster_get(cluster);
  uint16_t slot;
  if (!impl || impl->phase != REDIS_CLUSTER_PHASE_READY)
    return impl ? TURBO_ESHUTDOWN : TURBO_EINVAL;
  if (!key || key_length == 0u) return TURBO_EINVAL;
  slot = redis_cluster_keyslot(key, key_length);
  if (!impl->slots[slot]) return TURBO_ENOTCONN;
  return redis_pool_command_open(&impl->slots[slot]->pool, argc, argv, argvlen,
                                 max_reply_bytes, out_stream);
}

const redis_cluster_node *redis_cluster_node_for_slot(
    const redis_cluster *cluster, uint16_t slot) {
  const redis_cluster_impl *impl = redis_cluster_get_const(cluster);
  if (!impl || slot >= REDIS_CLUSTER_SLOT_COUNT || !impl->slots[slot])
    return NULL;
  return &impl->slots[slot]->view;
}

int redis_cluster_ready(const redis_cluster *cluster) {
  const redis_cluster_impl *impl = redis_cluster_get_const(cluster);
  return impl && impl->phase == REDIS_CLUSTER_PHASE_READY;
}

int redis_cluster_close(redis_cluster *cluster) {
  redis_cluster_impl *impl = redis_cluster_get(cluster);
  size_t index;
  int result = TURBO_OK;
  if (!impl) return TURBO_EINVAL;
  if (impl->phase == REDIS_CLUSTER_PHASE_CLOSED) return TURBO_OK;
  impl->phase = REDIS_CLUSTER_PHASE_CLOSING;
  if (impl->discovery.impl) {
    int status = redis_pool_stream_destroy(&impl->discovery);
    if (status != TURBO_OK) return status;
  }
  if (impl->seed_pool.impl) {
    int status = redis_pool_close(&impl->seed_pool);
    if (status != TURBO_OK) return status;
    status = redis_pool_destroy(&impl->seed_pool);
    if (status != TURBO_OK) return status;
  }
  for (index = 0u; index < impl->node_count; ++index) {
    if (impl->nodes[index].pool.impl) {
      int status = redis_pool_close(&impl->nodes[index].pool);
      if (status != TURBO_OK && result == TURBO_OK) result = status;
    }
  }
  if (result == TURBO_OK) impl->phase = REDIS_CLUSTER_PHASE_CLOSED;
  return result;
}

int redis_cluster_destroy(redis_cluster *cluster) {
  redis_cluster_impl *impl = redis_cluster_get(cluster);
  size_t index;
  int status;
  if (!impl) return TURBO_EINVAL;
  status = redis_cluster_close(cluster);
  if (status != TURBO_OK) return status;
  for (index = 0u; index < impl->node_count; ++index) {
    if (impl->nodes[index].pool.impl) {
      status = redis_pool_destroy(&impl->nodes[index].pool);
      if (status != TURBO_OK) return status;
    }
    tstr_free(impl->nodes[index].node_id);
    tstr_free(impl->nodes[index].host);
  }
  for (index = 0u; index < impl->seed_count; ++index)
    tstr_free(impl->seed_hosts[index]);
  tstr_free(impl->password);
  tstr_free(impl->username);
  free(impl->slots);
  free(impl->nodes);
  free(impl->seed_ports);
  free(impl->seed_hosts);
  free(impl);
  cluster->impl = NULL;
  return TURBO_OK;
}

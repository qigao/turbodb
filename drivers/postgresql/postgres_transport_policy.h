#ifndef ORM_POSTGRES_TRANSPORT_POLICY_H
#define ORM_POSTGRES_TRANSPORT_POLICY_H

#include <orm.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ORM_POSTGRES_TRANSPORT_POLICY_VERSION UINT32_C(1)
#define ORM_POSTGRES_TRANSPORT_HOST_CAPACITY UINT32_C(254)
#define ORM_POSTGRES_TRANSPORT_PATH_CAPACITY UINT32_C(4096)
#define ORM_POSTGRES_TRANSPORT_MIN_BUFFER_BYTES UINT32_C(4096)
#define ORM_POSTGRES_TRANSPORT_MAX_BUFFER_BYTES UINT32_C(1048576)
#define ORM_POSTGRES_TRANSPORT_DEFAULT_BUFFER_BYTES UINT32_C(65536)

typedef int32_t orm_postgres_transport_mode;
enum {
  ORM_POSTGRES_TRANSPORT_DISABLED = 0,
  ORM_POSTGRES_TRANSPORT_DIRECT_TLS = 1
};

typedef struct orm_postgres_transport_policy {
  uint32_t struct_size;
  uint32_t version;
  orm_postgres_transport_mode mode;
  uint16_t remote_port;
  uint16_t reserved;
  uint32_t connect_timeout_ms;
  uint32_t handshake_timeout_ms;
  uint32_t idle_timeout_ms;
  uint32_t ingress_buffer_bytes;
  uint32_t egress_buffer_bytes;
  char remote_host[ORM_POSTGRES_TRANSPORT_HOST_CAPACITY + 1u];
  char server_name[ORM_POSTGRES_TRANSPORT_HOST_CAPACITY + 1u];
  char ca_file[ORM_POSTGRES_TRANSPORT_PATH_CAPACITY + 1u];
  char ca_path[ORM_POSTGRES_TRANSPORT_PATH_CAPACITY + 1u];
} orm_postgres_transport_policy;

#define ORM_POSTGRES_TRANSPORT_POLICY_INIT                                      \
  {                                                                             \
    sizeof(orm_postgres_transport_policy), ORM_POSTGRES_TRANSPORT_POLICY_VERSION,\
    ORM_POSTGRES_TRANSPORT_DISABLED, 0u, 0u, 0u, 0u, 0u,                       \
    ORM_POSTGRES_TRANSPORT_DEFAULT_BUFFER_BYTES,                                \
    ORM_POSTGRES_TRANSPORT_DEFAULT_BUFFER_BYTES, {0}, {0}, {0}, {0}             \
  }

int orm_postgres_transport_option_name(orm_string_view_t keyword);
orm_status_t orm_postgres_transport_policy_parse(
    const orm_config_t *config, orm_postgres_transport_policy *out,
    orm_error_t *error);

#ifdef __cplusplus
}
#endif

#endif /* ORM_POSTGRES_TRANSPORT_POLICY_H */

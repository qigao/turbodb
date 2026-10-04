#include "postgres_direct_tls_bridge.h"

#include <salts_buffer.h>
#include <salts_error.h>

#include <stdio.h>
#include <string.h>

static native_io_backend_kind orm_postgres_bridge_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static int orm_postgres_bridge_connection_equal(
    cnet_connection left, cnet_connection right) {
  return left.slot == right.slot && left.generation == right.generation;
}

static void orm_postgres_bridge_fail(
    orm_postgres_direct_tls_bridge *bridge, int status) {
  if (bridge == NULL || bridge->state == ORM_POSTGRES_BRIDGE_CLOSED)
    return;
  if (bridge->failure_status == SALTS_OK)
    bridge->failure_status = status != SALTS_OK ? status : SALTS_EIO;
  bridge->state = ORM_POSTGRES_BRIDGE_FAILED;
}

static int orm_postgres_bridge_send(
    orm_postgres_direct_tls_bridge *bridge,
    cnet_connection target, const cnet_receive_view *view) {
  mem_buffer_t *buffer;
  int status;
  if (bridge == NULL || view == NULL || view->kind != CNET_MESSAGE_BYTES ||
      view->data == NULL || view->size == 0u)
    return SALTS_EINVAL;
  if (view->size > bridge->policy.ingress_buffer_bytes &&
      view->size > bridge->policy.egress_buffer_bytes)
    return SALTS_ENOBUFS;
  buffer = mem_get_buffer(mem_global(), view->size);
  if (buffer == NULL)
    return SALTS_ENOMEM;
  memcpy(mem_buffer_data(buffer), view->data, view->size);
  mem_set_used(buffer, view->size);
  status = cnet_send_buffer(&bridge->client, target, buffer);
  mem_buffer_release(buffer);
  return status;
}

static void orm_postgres_bridge_on_state(
    void *user, cnet_connection connection, cnet_connection_state state,
    const cnet_error *error) {
  orm_postgres_direct_tls_bridge *bridge =
      (orm_postgres_direct_tls_bridge *)user;
  if (bridge == NULL)
    return;

  if (error != NULL && error->status != SALTS_OK) {
    orm_postgres_bridge_fail(bridge, error->status);
    return;
  }

  if (orm_postgres_bridge_connection_equal(connection, bridge->remote)) {
    if (state == CNET_CONNECTION_TLS_HANDSHAKING) {
      bridge->state = ORM_POSTGRES_BRIDGE_TLS_HANDSHAKING;
      return;
    }
    if (state == CNET_CONNECTION_CONNECTED) {
      bridge->remote_tls_ready = 1;
      if (bridge->local_live) {
        const int local_status = cnet_receive(&bridge->client, bridge->local, 1u);
        const int remote_status = cnet_receive(&bridge->client, bridge->remote, 1u);
        if (local_status != SALTS_OK || remote_status != SALTS_OK) {
          orm_postgres_bridge_fail(
              bridge, local_status != SALTS_OK ? local_status : remote_status);
          return;
        }
        bridge->receive_started = 1;
        bridge->state = ORM_POSTGRES_BRIDGE_FORWARDING;
      }
      return;
    }
    if (state == CNET_CONNECTION_FAILED || state == CNET_CONNECTION_CLOSED) {
      bridge->remote_live = 0;
      if (bridge->state != ORM_POSTGRES_BRIDGE_CLOSING)
        orm_postgres_bridge_fail(bridge, SALTS_ENOTCONN);
    }
    return;
  }

  if (orm_postgres_bridge_connection_equal(connection, bridge->local)) {
    if (state == CNET_CONNECTION_CONNECTED) {
      bridge->local_live = 1;
      bridge->state = bridge->remote_tls_ready
                          ? ORM_POSTGRES_BRIDGE_FORWARDING
                          : ORM_POSTGRES_BRIDGE_LOCAL_ACCEPTED;
      if (bridge->remote_tls_ready && !bridge->receive_started) {
        const int local_status = cnet_receive(&bridge->client, bridge->local, 1u);
        const int remote_status = cnet_receive(&bridge->client, bridge->remote, 1u);
        if (local_status != SALTS_OK || remote_status != SALTS_OK) {
          orm_postgres_bridge_fail(
              bridge, local_status != SALTS_OK ? local_status : remote_status);
          return;
        }
        bridge->receive_started = 1;
      }
      return;
    }
    if (state == CNET_CONNECTION_FAILED || state == CNET_CONNECTION_CLOSED) {
      bridge->local_live = 0;
      if (bridge->state != ORM_POSTGRES_BRIDGE_CLOSING)
        orm_postgres_bridge_fail(bridge, SALTS_ENOTCONN);
    }
  }
}

static void orm_postgres_bridge_on_receive(
    void *user, cnet_connection connection, const cnet_receive_view *view) {
  orm_postgres_direct_tls_bridge *bridge =
      (orm_postgres_direct_tls_bridge *)user;
  cnet_connection target;
  int status;
  if (bridge == NULL || bridge->state != ORM_POSTGRES_BRIDGE_FORWARDING ||
      !bridge->remote_tls_ready) {
    orm_postgres_bridge_fail(bridge, SALTS_EPROTO);
    return;
  }
  if (orm_postgres_bridge_connection_equal(connection, bridge->local))
    target = bridge->remote;
  else if (orm_postgres_bridge_connection_equal(connection, bridge->remote))
    target = bridge->local;
  else {
    orm_postgres_bridge_fail(bridge, SALTS_EPROTO);
    return;
  }
  status = orm_postgres_bridge_send(bridge, target, view);
  if (status != SALTS_OK) {
    orm_postgres_bridge_fail(bridge, status);
    return;
  }
  status = cnet_receive(&bridge->client, connection, 1u);
  if (status != SALTS_OK)
    orm_postgres_bridge_fail(bridge, status);
}

static cnet_client_config orm_postgres_bridge_client_config(
    const orm_postgres_transport_policy *policy) {
  cnet_client_config config = {0};
  config.backend = orm_postgres_bridge_backend();
  config.connection_capacity = 2u;
  config.command_capacity = 16u;
  config.request_capacity = 8u;
  config.completion_batch_capacity = 8u;
  config.event_capacity = 16u;
  config.max_send_bytes =
      policy->ingress_buffer_bytes > policy->egress_buffer_bytes
          ? policy->ingress_buffer_bytes
          : policy->egress_buffer_bytes;
  config.receive_buffer_bytes = config.max_send_bytes;
  config.connect_timeout_ms = policy->connect_timeout_ms;
  config.read_timeout_ms = policy->idle_timeout_ms;
  config.write_timeout_ms = policy->idle_timeout_ms;
  config.tls_io_buffer_bytes =
      config.max_send_bytes < CNET_TLS_MIN_IO_BUFFER_BYTES
          ? CNET_TLS_MIN_IO_BUFFER_BYTES
          : config.max_send_bytes;
  config.tls_handshake_timeout_ms = policy->handshake_timeout_ms;
  return config;
}

int orm_postgres_direct_tls_bridge_init(
    orm_postgres_direct_tls_bridge *bridge,
    const orm_postgres_transport_policy *policy) {
  static const char *const alpn[] = {"postgresql"};
  cnet_client_config client_config;
  cnet_listener_config listener_config;
  cnet_tls_client_config tls_config;
  cnet_connect_options connect_options;
  cnet_observer observer;
  char uri[ORM_POSTGRES_TRANSPORT_HOST_CAPACITY + 32u];
  int written;
  int status;

  if (bridge == NULL || policy == NULL ||
      policy->mode != ORM_POSTGRES_TRANSPORT_DIRECT_TLS ||
      policy->remote_host[0] == '\0' || policy->remote_port == 0u ||
      policy->server_name[0] == '\0')
    return SALTS_EINVAL;

  memset(bridge, 0, sizeof(*bridge));
  bridge->policy = *policy;
  bridge->failure_status = SALTS_OK;

  client_config = orm_postgres_bridge_client_config(policy);
  status = cnet_client_init(&bridge->client, &client_config);
  if (status != SALTS_OK)
    return status;

  listener_config = (cnet_listener_config){
      .backend = client_config.backend,
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = 1u};
  status = cnet_listener_init(&bridge->listener, &listener_config);
  if (status != SALTS_OK)
    goto fail;
  status = cnet_listener_port(&bridge->listener, &bridge->local_port);
  if (status != SALTS_OK)
    goto fail;
  bridge->state = ORM_POSTGRES_BRIDGE_LISTENING;

  observer = (cnet_observer){
      .on_state = orm_postgres_bridge_on_state,
      .on_receive = orm_postgres_bridge_on_receive,
      .user = bridge};

  tls_config = (cnet_tls_client_config){
      .size = sizeof(tls_config),
      .server_name = policy->server_name,
      .ca_file = policy->ca_file[0] != '\0' ? policy->ca_file : NULL,
      .ca_path = policy->ca_path[0] != '\0' ? policy->ca_path : NULL,
      .alpn_protocols = alpn,
      .alpn_protocol_count = 1u};

  written = snprintf(uri, sizeof(uri), "tls://%s:%u",
                     policy->remote_host, (unsigned)policy->remote_port);
  if (written <= 0 || (size_t)written >= sizeof(uri)) {
    status = SALTS_ERANGE;
    goto fail;
  }
  connect_options = (cnet_connect_options){
      .uri = uri, .observer = observer, .tls = &tls_config};
  status = cnet_connect(&bridge->client, &connect_options, &bridge->remote);
  if (status != SALTS_OK)
    goto fail;
  bridge->remote_live = 1;
  bridge->state = ORM_POSTGRES_BRIDGE_REMOTE_CONNECTING;
  return SALTS_OK;

fail:
  (void)orm_postgres_direct_tls_bridge_destroy(bridge);
  return status;
}

int orm_postgres_direct_tls_bridge_progress(
    orm_postgres_direct_tls_bridge *bridge, uint32_t timeout_ms) {
  size_t events = 0u;
  int ready = 0;
  int status;
  if (bridge == NULL || bridge->client.impl == NULL)
    return SALTS_EINVAL;
  if (bridge->state == ORM_POSTGRES_BRIDGE_FAILED)
    return bridge->failure_status;

  if (!bridge->local_live && bridge->listener.impl != NULL) {
    status = cnet_listener_wait(&bridge->listener, 0u, &ready);
    if (status != SALTS_OK)
      return status;
    if (ready) {
      cnet_accepted_stream accepted = CNET_ACCEPTED_STREAM_INIT;
      cnet_observer observer = {
          .on_state = orm_postgres_bridge_on_state,
          .on_receive = orm_postgres_bridge_on_receive,
          .user = bridge};
      status = cnet_listener_accept_detached(&bridge->listener, &accepted);
      if (status != SALTS_OK)
        return status;
      status = cnet_client_adopt_accepted(
          &bridge->client, &accepted, &observer, &bridge->local);
      if (status != SALTS_OK)
        return status;
      bridge->local_live = 1;
      (void)cnet_listener_close(&bridge->listener);
      bridge->state = bridge->remote_tls_ready
                          ? ORM_POSTGRES_BRIDGE_FORWARDING
                          : ORM_POSTGRES_BRIDGE_LOCAL_ACCEPTED;
    }
  }

  status = cnet_client_poll(&bridge->client, timeout_ms, &events);
  if (status != SALTS_OK)
    return status;
  if (bridge->state == ORM_POSTGRES_BRIDGE_FAILED)
    return bridge->failure_status;
  return SALTS_OK;
}

int orm_postgres_direct_tls_bridge_close(
    orm_postgres_direct_tls_bridge *bridge) {
  if (bridge == NULL)
    return SALTS_EINVAL;
  if (bridge->state == ORM_POSTGRES_BRIDGE_CLOSED)
    return SALTS_EALREADY;
  bridge->state = ORM_POSTGRES_BRIDGE_CLOSING;
  if (bridge->listener.impl != NULL)
    (void)cnet_listener_close(&bridge->listener);
  if (bridge->local_live)
    (void)cnet_close(&bridge->client, bridge->local);
  if (bridge->remote_live)
    (void)cnet_close(&bridge->client, bridge->remote);
  return SALTS_OK;
}

int orm_postgres_direct_tls_bridge_destroy(
    orm_postgres_direct_tls_bridge *bridge) {
  int status = SALTS_OK;
  int next;
  if (bridge == NULL)
    return SALTS_EINVAL;
  if (bridge->listener.impl != NULL) {
    (void)cnet_listener_close(&bridge->listener);
    next = cnet_listener_destroy(&bridge->listener);
    if (status == SALTS_OK && next != SALTS_OK)
      status = next;
  }
  if (bridge->client.impl != NULL) {
    next = cnet_client_stop(
        &bridge->client,
        bridge->policy.handshake_timeout_ms != 0u
            ? bridge->policy.handshake_timeout_ms
            : 5000u);
    if (status == SALTS_OK && next != SALTS_OK)
      status = next;
    next = cnet_client_destroy(&bridge->client);
    if (status == SALTS_OK && next != SALTS_OK)
      status = next;
  }
  memset(bridge, 0, sizeof(*bridge));
  bridge->state = ORM_POSTGRES_BRIDGE_CLOSED;
  return status;
}

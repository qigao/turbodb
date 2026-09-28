#include "session.h"

#include "auth/auth.h"
#include "wire/handshake.h"
#include "wire/packet.h"
#include "wire/result.h"

#include <cnet/cnet.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define MYSQL_SESSION_CONTROL_CAPACITY 4096u
#define MYSQL_SESSION_PACKET_CAPACITY (MYSQL_SESSION_CONTROL_CAPACITY + 4u)
#define MYSQL_SESSION_DEFAULT_TIMEOUT_MS 5000u
#define MYSQL_SESSION_MAX_PACKET_SIZE UINT32_C(0x01000000)
#define MYSQL_COM_PING UINT8_C(0x0e)

typedef enum mysql_session_phase_t {
  MYSQL_PHASE_WAIT_TCP = 0,
  MYSQL_PHASE_WAIT_GREETING,
  MYSQL_PHASE_WAIT_SSL_SEND,
  MYSQL_PHASE_WAIT_TLS,
  MYSQL_PHASE_WAIT_HANDSHAKE_SEND,
  MYSQL_PHASE_WAIT_AUTH,
  MYSQL_PHASE_WAIT_AUTH_SEND,
  MYSQL_PHASE_WAIT_PING_SEND,
  MYSQL_PHASE_WAIT_PING_REPLY,
  MYSQL_PHASE_DONE,
  MYSQL_PHASE_FAILED
} mysql_session_phase_t;

typedef enum mysql_session_send_kind_t {
  MYSQL_SEND_NONE = 0,
  MYSQL_SEND_SSL_REQUEST,
  MYSQL_SEND_HANDSHAKE_RESPONSE,
  MYSQL_SEND_AUTH_RESPONSE,
  MYSQL_SEND_PING
} mysql_session_send_kind_t;

typedef struct mysql_session_t {
  const mysql_session_config_t *config;
  mysql_session_error_t *error;
  cnet_client client;
  cnet_connection connection;
  mysql_wire_packet_stream_t stream;
  mysql_wire_greeting_t greeting;
  uint32_t client_capabilities;
  mysql_session_phase_t phase;
  mysql_session_send_kind_t pending_send;
  uint8_t pending_sequence;
  uint8_t control[MYSQL_SESSION_CONTROL_CAPACITY];
  size_t control_used;
  uint8_t message_last_sequence;
  char auth_plugin[MYSQL_WIRE_AUTH_PLUGIN_NAME_CAPACITY];
  uint8_t auth_nonce[MYSQL_AUTH_NONCE_BYTES];
  size_t auth_nonce_size;
  bool tls_established;
} mysql_session_t;

static void mysql_session_set_error(
    mysql_session_t *session, mysql_session_status_t status,
    const char *stage, const char *message) {
  if (session == NULL)
    return;
  session->phase = MYSQL_PHASE_FAILED;
  if (session->error == NULL)
    return;
  session->error->status = status;
  if (stage != NULL && stage != session->error->stage)
    (void)snprintf(session->error->stage, sizeof(session->error->stage),
                   "%s", stage);
  if (message != NULL && message != session->error->message)
    (void)snprintf(session->error->message, sizeof(session->error->message),
                   "%s", message);
}

static bool mysql_session_waits_for_receive(mysql_session_phase_t phase) {
  return phase == MYSQL_PHASE_WAIT_GREETING ||
         phase == MYSQL_PHASE_WAIT_AUTH ||
         phase == MYSQL_PHASE_WAIT_PING_REPLY;
}

static int mysql_session_request_receive(mysql_session_t *session) {
  int status;
  status = cnet_receive(&session->client, session->connection, 1u);
  if (status != SALTS_OK) {
    if (session->error != NULL)
      session->error->cnet_status = status;
    mysql_session_set_error(session, MYSQL_SESSION_IO,
                            "receive", "CNet receive admission failed");
  }
  return status;
}

static int mysql_session_send_packet(
    mysql_session_t *session, uint8_t sequence_id,
    const uint8_t *payload, size_t payload_size,
    mysql_session_send_kind_t kind) {
  uint8_t packet[MYSQL_SESSION_PACKET_CAPACITY];
  size_t total;
  int status;

  if (payload_size > MYSQL_SESSION_CONTROL_CAPACITY ||
      payload_size > MYSQL_WIRE_PACKET_MAX_PAYLOAD ||
      (payload == NULL && payload_size != 0u)) {
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "send", "control packet exceeds M2 bound");
    return SALTS_EMSGSIZE;
  }

  packet[0] = (uint8_t)(payload_size & 0xffu);
  packet[1] = (uint8_t)((payload_size >> 8u) & 0xffu);
  packet[2] = (uint8_t)((payload_size >> 16u) & 0xffu);
  packet[3] = sequence_id;
  if (payload_size != 0u)
    memcpy(packet + 4u, payload, payload_size);
  total = payload_size + 4u;

  status = cnet_send(&session->client, session->connection, packet, total);
  if (status != SALTS_OK) {
    if (session->error != NULL)
      session->error->cnet_status = status;
    mysql_session_set_error(session, MYSQL_SESSION_IO,
                            "send", "CNet send admission failed");
    return status;
  }
  session->pending_send = kind;
  session->pending_sequence = sequence_id;
  return SALTS_OK;
}

static mysql_wire_status_t mysql_session_build_auth(
    mysql_session_t *session, const char *plugin,
    const uint8_t *nonce, size_t nonce_size,
    uint8_t out[MYSQL_AUTH_RESPONSE_CAPACITY], size_t *out_size) {
  mysql_wire_status_t status =
      mysql_auth_build_response(plugin, session->config->password,
                                nonce, nonce_size, out, out_size);
  if (status != MYSQL_WIRE_STATUS_OK)
    mysql_session_set_error(session, MYSQL_SESSION_AUTH,
                            "auth", "unsupported or invalid authentication challenge");
  return status;
}

static int mysql_session_send_handshake_response(mysql_session_t *session) {
  uint8_t auth[MYSQL_AUTH_RESPONSE_CAPACITY];
  uint8_t payload[MYSQL_SESSION_CONTROL_CAPACITY];
  size_t auth_size = 0u;
  size_t payload_size = 0u;
  mysql_wire_status_t wire_status;
  uint8_t sequence = (uint8_t)(session->pending_sequence + 1u);

  wire_status = mysql_session_build_auth(
      session, session->auth_plugin, session->auth_nonce,
      session->auth_nonce_size, auth, &auth_size);
  if (wire_status != MYSQL_WIRE_STATUS_OK)
    return SALTS_EINVAL;

  wire_status = mysql_wire_build_handshake_response(
      session->client_capabilities, MYSQL_SESSION_MAX_PACKET_SIZE,
      session->greeting.character_set, session->config->username,
      auth, auth_size,
      session->config->database != NULL &&
              session->config->database[0] != '\0'
          ? session->config->database
          : NULL,
      session->auth_plugin, payload, sizeof(payload), &payload_size);
  if (wire_status != MYSQL_WIRE_STATUS_OK) {
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "handshake-response",
                            "failed to encode HandshakeResponse41");
    return SALTS_EINVAL;
  }

  mysql_wire_packet_stream_reset(&session->stream,
                                 (uint8_t)(sequence + 1u));
  session->phase = MYSQL_PHASE_WAIT_HANDSHAKE_SEND;
  return mysql_session_send_packet(
      session, sequence, payload, payload_size,
      MYSQL_SEND_HANDSHAKE_RESPONSE);
}

static void mysql_session_begin_tls(mysql_session_t *session) {
  cnet_tls_client_config tls_config = {
      .size = sizeof(tls_config),
      .ca_file = session->config->ca_file,
      .server_name = session->config->server_name};
  cnet_start_tls_options options = CNET_START_TLS_OPTIONS_INIT;
  int status;

  options.tls = &tls_config;
  session->phase = MYSQL_PHASE_WAIT_TLS;
  status = cnet_start_tls(&session->client, session->connection, &options);
  if (status != SALTS_OK) {
    if (session->error != NULL)
      session->error->cnet_status = status;
    mysql_session_set_error(session, MYSQL_SESSION_IO,
                            "start-tls", "CNet STARTTLS admission failed");
  }
}

static void mysql_session_handle_greeting(
    mysql_session_t *session, const uint8_t *payload, size_t payload_size,
    uint8_t sequence_id) {
  uint8_t ssl_request[32];
  size_t ssl_size = 0u;
  mysql_wire_status_t status;
  bool use_database =
      session->config->database != NULL &&
      session->config->database[0] != '\0';

  status = mysql_wire_parse_greeting(
      payload, payload_size, &session->greeting);
  if (status != MYSQL_WIRE_STATUS_OK) {
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "greeting", "invalid HandshakeV10 packet");
    return;
  }
  status = mysql_wire_select_client_capabilities(
      &session->greeting, true, use_database,
      &session->client_capabilities);
  if (status != MYSQL_WIRE_STATUS_OK) {
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "capabilities",
                            "server lacks required Protocol41/TLS/plugin-auth capabilities");
    return;
  }
  if (session->greeting.auth_plugin_data_length != MYSQL_AUTH_NONCE_BYTES ||
      session->greeting.auth_plugin[0] == '\0') {
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "greeting-auth", "invalid authentication plugin challenge");
    return;
  }

  (void)snprintf(session->auth_plugin, sizeof(session->auth_plugin),
                 "%s", session->greeting.auth_plugin);
  memcpy(session->auth_nonce, session->greeting.auth_plugin_data,
         MYSQL_AUTH_NONCE_BYTES);
  session->auth_nonce_size = MYSQL_AUTH_NONCE_BYTES;

  status = mysql_wire_build_ssl_request(
      session->client_capabilities, MYSQL_SESSION_MAX_PACKET_SIZE,
      session->greeting.character_set, ssl_request, sizeof(ssl_request),
      &ssl_size);
  if (status != MYSQL_WIRE_STATUS_OK) {
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "ssl-request", "failed to encode SSLRequest");
    return;
  }

  session->phase = MYSQL_PHASE_WAIT_SSL_SEND;
  (void)mysql_session_send_packet(
      session, (uint8_t)(sequence_id + 1u),
      ssl_request, ssl_size, MYSQL_SEND_SSL_REQUEST);
}

static void mysql_session_send_auth_reply(
    mysql_session_t *session, uint8_t server_sequence,
    const char *plugin, const uint8_t *nonce, size_t nonce_size) {
  uint8_t auth[MYSQL_AUTH_RESPONSE_CAPACITY + 1u];
  size_t auth_size = 0u;
  uint8_t client_sequence = (uint8_t)(server_sequence + 1u);

  if (mysql_session_build_auth(
          session, plugin, nonce, nonce_size,
          auth, &auth_size) != MYSQL_WIRE_STATUS_OK)
    return;

  mysql_wire_packet_stream_reset(
      &session->stream, (uint8_t)(client_sequence + 1u));
  session->phase = MYSQL_PHASE_WAIT_AUTH_SEND;
  (void)mysql_session_send_packet(
      session, client_sequence, auth, auth_size, MYSQL_SEND_AUTH_RESPONSE);
}

static void mysql_session_send_clear_password(
    mysql_session_t *session, uint8_t server_sequence) {
  uint8_t password[MYSQL_SESSION_CONTROL_CAPACITY];
  size_t size = strlen(session->config->password) + 1u;
  uint8_t client_sequence = (uint8_t)(server_sequence + 1u);

  if (!session->tls_established) {
    mysql_session_set_error(session, MYSQL_SESSION_AUTH,
                            "full-auth",
                            "clear password authentication requires verified TLS");
    return;
  }
  if (size > sizeof(password)) {
    mysql_session_set_error(session, MYSQL_SESSION_AUTH,
                            "full-auth", "password exceeds M2 control bound");
    return;
  }
  memcpy(password, session->config->password, size);

  mysql_wire_packet_stream_reset(
      &session->stream, (uint8_t)(client_sequence + 1u));
  session->phase = MYSQL_PHASE_WAIT_AUTH_SEND;
  (void)mysql_session_send_packet(
      session, client_sequence, password, size, MYSQL_SEND_AUTH_RESPONSE);
  memset(password, 0, size);
}

static void mysql_session_send_ping(mysql_session_t *session) {
  const uint8_t command = MYSQL_COM_PING;
  mysql_wire_packet_stream_reset(&session->stream, UINT8_C(1));
  session->phase = MYSQL_PHASE_WAIT_PING_SEND;
  (void)mysql_session_send_packet(
      session, UINT8_C(0), &command, 1u, MYSQL_SEND_PING);
}

static void mysql_session_handle_auth(
    mysql_session_t *session, const uint8_t *payload, size_t payload_size,
    uint8_t sequence_id) {
  if (payload_size == 0u) {
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "auth-reply", "empty authentication reply");
    return;
  }

  if (payload[0] == UINT8_C(0x00)) {
    mysql_wire_ok_packet_t ok;
    if (mysql_wire_decode_ok_packet(
            payload, payload_size, session->client_capabilities,
            &ok) != MYSQL_WIRE_STATUS_OK) {
      mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                              "auth-ok", "invalid authentication OK packet");
      return;
    }
    mysql_session_send_ping(session);
    return;
  }

  if (payload[0] == UINT8_C(0xff)) {
    mysql_wire_err_packet_t server_error;
    if (mysql_wire_decode_err_packet(
            payload, payload_size, session->client_capabilities,
            &server_error) == MYSQL_WIRE_STATUS_OK &&
        session->error != NULL) {
      session->error->server_error = server_error.error_code;
      if (server_error.has_sql_state)
        memcpy(session->error->sql_state, server_error.sql_state,
               sizeof(server_error.sql_state));
      (void)snprintf(session->error->message,
                     sizeof(session->error->message), "%.*s",
                     (int)(server_error.message.length <
                                   sizeof(session->error->message) - 1u
                               ? server_error.message.length
                               : sizeof(session->error->message) - 1u),
                     (const char *)server_error.message.data);
    }
    mysql_session_set_error(session, MYSQL_SESSION_AUTH,
                            "auth-server", session->error != NULL &&
                                                   session->error->message[0] != '\0'
                                               ? session->error->message
                                               : "server rejected authentication");
    return;
  }

  if (payload[0] == UINT8_C(0x01) && payload_size >= 2u) {
    if (payload[1] == UINT8_C(0x03)) {
      session->phase = MYSQL_PHASE_WAIT_AUTH;
      return;
    }
    if (payload[1] == UINT8_C(0x04)) {
      mysql_session_send_clear_password(session, sequence_id);
      return;
    }
    mysql_session_set_error(session, MYSQL_SESSION_AUTH,
                            "auth-more", "unsupported caching_sha2_password continuation");
    return;
  }

  if (payload[0] == UINT8_C(0xfe)) {
    mysql_wire_auth_switch_t request;
    if (mysql_wire_parse_auth_switch(
            payload, payload_size, &request) != MYSQL_WIRE_STATUS_OK ||
        request.data_length != MYSQL_AUTH_NONCE_BYTES) {
      mysql_session_set_error(session, MYSQL_SESSION_AUTH,
                              "auth-switch", "invalid AuthSwitchRequest");
      return;
    }
    (void)snprintf(session->auth_plugin, sizeof(session->auth_plugin),
                   "%s", request.plugin);
    memcpy(session->auth_nonce, request.data, request.data_length);
    session->auth_nonce_size = request.data_length;
    mysql_session_send_auth_reply(
        session, sequence_id, session->auth_plugin,
        session->auth_nonce, session->auth_nonce_size);
    return;
  }

  mysql_session_set_error(session, MYSQL_SESSION_AUTH,
                          "auth-reply", "unsupported authentication packet");
}

static void mysql_session_handle_ping_reply(
    mysql_session_t *session, const uint8_t *payload, size_t payload_size) {
  mysql_wire_ok_packet_t ok;
  mysql_wire_err_packet_t server_error;

  if (payload_size != 0u && payload[0] == UINT8_C(0xff) &&
      mysql_wire_decode_err_packet(
          payload, payload_size, session->client_capabilities,
          &server_error) == MYSQL_WIRE_STATUS_OK) {
    if (session->error != NULL) {
      session->error->server_error = server_error.error_code;
      if (server_error.has_sql_state)
        memcpy(session->error->sql_state, server_error.sql_state,
               sizeof(server_error.sql_state));
    }
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "ping", "server rejected COM_PING");
    return;
  }

  if (mysql_wire_decode_ok_packet(
          payload, payload_size, session->client_capabilities,
          &ok) != MYSQL_WIRE_STATUS_OK) {
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "ping", "invalid COM_PING response");
    return;
  }
  session->phase = MYSQL_PHASE_DONE;
}

static void mysql_session_handle_message(
    mysql_session_t *session, const uint8_t *payload, size_t payload_size,
    uint8_t sequence_id) {
  switch (session->phase) {
    case MYSQL_PHASE_WAIT_GREETING:
      mysql_session_handle_greeting(
          session, payload, payload_size, sequence_id);
      break;
    case MYSQL_PHASE_WAIT_AUTH:
      mysql_session_handle_auth(
          session, payload, payload_size, sequence_id);
      break;
    case MYSQL_PHASE_WAIT_PING_REPLY:
      mysql_session_handle_ping_reply(session, payload, payload_size);
      break;
    default:
      mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                              "state", "unexpected MySQL packet for session phase");
      break;
  }
}

static void mysql_session_on_receive(
    void *user, cnet_connection connection, const cnet_receive_view *view) {
  mysql_session_t *session = (mysql_session_t *)user;
  const uint8_t *input;
  size_t offset = 0u;

  if (session == NULL || view == NULL ||
      connection.slot != session->connection.slot ||
      connection.generation != session->connection.generation ||
      view->kind != CNET_MESSAGE_BYTES) {
    if (session != NULL)
      mysql_session_set_error(session, MYSQL_SESSION_IO,
                              "receive-callback", "invalid CNet receive callback");
    return;
  }

  input = (const uint8_t *)view->data;
  while (offset < view->size && session->phase != MYSQL_PHASE_FAILED) {
    mysql_wire_packet_event_t event;
    size_t consumed = 0u;
    mysql_wire_status_t status = mysql_wire_packet_stream_feed(
        &session->stream, input + offset, view->size - offset,
        &consumed, &event);
    offset += consumed;

    if (status != MYSQL_WIRE_STATUS_OK &&
        status != MYSQL_WIRE_STATUS_NEED_MORE) {
      mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                              "packet", "invalid MySQL packet framing");
      return;
    }

    if (event.length != 0u) {
      if (session->control_used >
              sizeof(session->control) - event.length) {
        mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                                "packet", "control message exceeds M2 bound");
        return;
      }
      memcpy(session->control + session->control_used,
             event.data, event.length);
      session->control_used += event.length;
      session->message_last_sequence = event.sequence_id;
    } else if (event.packet_end) {
      session->message_last_sequence = event.sequence_id;
    }

    if (event.message_end) {
      uint8_t last_sequence = session->message_last_sequence;
      mysql_session_handle_message(
          session, session->control, session->control_used, last_sequence);
      session->control_used = 0u;
      if (session->phase == MYSQL_PHASE_FAILED ||
          session->phase == MYSQL_PHASE_DONE)
        break;
    }

    if (status == MYSQL_WIRE_STATUS_NEED_MORE && consumed == 0u)
      break;
  }

  if (session->phase != MYSQL_PHASE_FAILED &&
      session->phase != MYSQL_PHASE_DONE &&
      mysql_session_waits_for_receive(session->phase))
    (void)mysql_session_request_receive(session);
}

static void mysql_session_on_send(
    void *user, cnet_connection connection, size_t size) {
  mysql_session_t *session = (mysql_session_t *)user;
  mysql_session_send_kind_t kind;

  if (session == NULL || size == 0u ||
      connection.slot != session->connection.slot ||
      connection.generation != session->connection.generation) {
    if (session != NULL)
      mysql_session_set_error(session, MYSQL_SESSION_IO,
                              "send-callback", "invalid CNet send callback");
    return;
  }

  kind = session->pending_send;
  session->pending_send = MYSQL_SEND_NONE;

  switch (kind) {
    case MYSQL_SEND_SSL_REQUEST:
      mysql_session_begin_tls(session);
      break;
    case MYSQL_SEND_HANDSHAKE_RESPONSE:
    case MYSQL_SEND_AUTH_RESPONSE:
      session->phase = MYSQL_PHASE_WAIT_AUTH;
      (void)mysql_session_request_receive(session);
      break;
    case MYSQL_SEND_PING:
      session->phase = MYSQL_PHASE_WAIT_PING_REPLY;
      (void)mysql_session_request_receive(session);
      break;
    default:
      mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                              "send-state", "unexpected MySQL send completion");
      break;
  }
}

static void mysql_session_on_state(
    void *user, cnet_connection connection, cnet_connection_state state,
    const cnet_error *cnet_error) {
  mysql_session_t *session = (mysql_session_t *)user;

  if (session == NULL)
    return;
  if (connection.slot != session->connection.slot ||
      connection.generation != session->connection.generation) {
    mysql_session_set_error(session, MYSQL_SESSION_IO,
                            "state-callback", "CNet connection handle changed");
    return;
  }

  if (state == CNET_CONNECTION_CONNECTED) {
    if (session->phase == MYSQL_PHASE_WAIT_TCP) {
      session->phase = MYSQL_PHASE_WAIT_GREETING;
      (void)mysql_session_request_receive(session);
      return;
    }
    if (session->phase == MYSQL_PHASE_WAIT_TLS) {
      session->tls_established = true;
      (void)mysql_session_send_handshake_response(session);
      return;
    }
    return;
  }

  if (state == CNET_CONNECTION_TLS_HANDSHAKING)
    return;

  if (state == CNET_CONNECTION_FAILED) {
    if (session->error != NULL && cnet_error != NULL) {
      session->error->cnet_status = cnet_error->status;
      session->error->cnet_native_status = cnet_error->native_status;
      if (cnet_error->stage != NULL)
        (void)snprintf(session->error->stage,
                       sizeof(session->error->stage), "%s",
                       cnet_error->stage);
    }
    mysql_session_set_error(session, MYSQL_SESSION_IO,
                            session->error != NULL &&
                                    session->error->stage[0] != '\0'
                                ? session->error->stage
                                : "cnet",
                            "CNet connection failed");
    return;
  }

  if (state == CNET_CONNECTION_CLOSED &&
      session->phase != MYSQL_PHASE_DONE &&
      session->phase != MYSQL_PHASE_FAILED)
    mysql_session_set_error(session, MYSQL_SESSION_IO,
                            "closed", "connection closed before COM_PING completed");
}

static cnet_client_config mysql_session_cnet_config(uint32_t timeout_ms) {
  cnet_client_config config = {
#if defined(_WIN32)
      .backend = NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
      .backend = NATIVE_IO_BACKEND_EPOLL,
#else
      .backend = NATIVE_IO_BACKEND_KQUEUE,
#endif
      .connection_capacity = 1u,
      .command_capacity = 32u,
      .request_capacity = 16u,
      .completion_batch_capacity = 16u,
      .event_capacity = 32u,
      .max_send_bytes = MYSQL_SESSION_PACKET_CAPACITY,
      .receive_buffer_bytes = MYSQL_SESSION_PACKET_CAPACITY,
      .connect_timeout_ms = timeout_ms,
      .read_timeout_ms = timeout_ms,
      .write_timeout_ms = timeout_ms,
      .tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES,
      .tls_handshake_timeout_ms = timeout_ms,
      .command_buffer_bytes = 0u,
      .event_buffer_bytes = 0u};
  return config;
}

mysql_session_status_t mysql_session_connect_and_ping(
    const mysql_session_config_t *config, mysql_session_error_t *error) {
  mysql_session_t session;
  cnet_client_config client_config;
  cnet_connect_options options;
  char uri[512];
  uint32_t timeout_ms;
  uint32_t elapsed = 0u;
  int status;
  mysql_session_status_t result;

  if (error != NULL)
    memset(error, 0, sizeof(*error));
  if (config == NULL || config->host == NULL || config->host[0] == '\0' ||
      config->port == 0u || config->username == NULL ||
      config->password == NULL || config->ca_file == NULL ||
      config->ca_file[0] == '\0' || config->server_name == NULL ||
      config->server_name[0] == '\0') {
    if (error != NULL) {
      error->status = MYSQL_SESSION_INVALID;
      (void)snprintf(error->stage, sizeof(error->stage), "%s", "config");
      (void)snprintf(error->message, sizeof(error->message), "%s",
                     "host/port/user/password/CA/server_name are required");
    }
    return MYSQL_SESSION_INVALID;
  }

  memset(&session, 0, sizeof(session));
  session.config = config;
  session.error = error;
  session.phase = MYSQL_PHASE_WAIT_TCP;
  timeout_ms = config->timeout_ms != 0u
                   ? config->timeout_ms
                   : MYSQL_SESSION_DEFAULT_TIMEOUT_MS;

  {
    int uri_size = snprintf(uri, sizeof(uri), "tcp://%s:%u",
                            config->host, (unsigned int)config->port);
    if (uri_size <= 0 || (size_t)uri_size >= sizeof(uri)) {
      if (error != NULL)
        error->status = MYSQL_SESSION_INVALID;
      return MYSQL_SESSION_INVALID;
    }
  }

  if (mysql_wire_packet_stream_init(
          &session.stream, UINT8_C(0),
          MYSQL_SESSION_CONTROL_CAPACITY) != MYSQL_WIRE_STATUS_OK) {
    if (error != NULL)
      error->status = MYSQL_SESSION_PROTOCOL;
    return MYSQL_SESSION_PROTOCOL;
  }

  client_config = mysql_session_cnet_config(timeout_ms);
  status = cnet_client_init(&session.client, &client_config);
  if (status != SALTS_OK) {
    if (error != NULL) {
      error->status = MYSQL_SESSION_IO;
      error->cnet_status = status;
      (void)snprintf(error->stage, sizeof(error->stage), "%s", "client-init");
    }
    return MYSQL_SESSION_IO;
  }

  options = (cnet_connect_options){
      .uri = uri,
      .observer = {.on_state = mysql_session_on_state,
                   .on_receive = mysql_session_on_receive,
                   .user = &session,
                   .on_send = mysql_session_on_send}};
  status = cnet_connect(&session.client, &options, &session.connection);
  if (status != SALTS_OK) {
    if (error != NULL) {
      error->status = MYSQL_SESSION_IO;
      error->cnet_status = status;
      (void)snprintf(error->stage, sizeof(error->stage), "%s", "connect");
    }
    (void)cnet_client_stop(&session.client, timeout_ms);
    (void)cnet_client_destroy(&session.client);
    return MYSQL_SESSION_IO;
  }

  while (session.phase != MYSQL_PHASE_DONE &&
         session.phase != MYSQL_PHASE_FAILED &&
         elapsed < timeout_ms) {
    size_t events = 0u;
    uint32_t slice = timeout_ms - elapsed > 10u
                         ? 10u
                         : timeout_ms - elapsed;
    status = cnet_client_poll(&session.client, slice, &events);
    if (status != SALTS_OK) {
      if (error != NULL)
        error->cnet_status = status;
      mysql_session_set_error(&session, MYSQL_SESSION_IO,
                              "poll", "CNet progress failed");
      break;
    }
    elapsed += slice;
  }

  if (session.phase != MYSQL_PHASE_DONE &&
      session.phase != MYSQL_PHASE_FAILED)
    mysql_session_set_error(&session, MYSQL_SESSION_TIMEOUT,
                            "timeout", "MySQL connect/ping deadline expired");

  result = session.phase == MYSQL_PHASE_DONE
               ? MYSQL_SESSION_OK
               : (error != NULL && error->status != MYSQL_SESSION_OK
                      ? error->status
                      : MYSQL_SESSION_PROTOCOL);

  if (session.phase == MYSQL_PHASE_DONE)
    (void)cnet_close(&session.client, session.connection);
  status = cnet_client_stop(&session.client, timeout_ms);
  if (status != SALTS_OK && result == MYSQL_SESSION_OK) {
    result = MYSQL_SESSION_IO;
    if (error != NULL) {
      error->status = result;
      error->cnet_status = status;
      (void)snprintf(error->stage, sizeof(error->stage), "%s", "client-stop");
    }
  }
  (void)cnet_client_destroy(&session.client);
  return result;
}

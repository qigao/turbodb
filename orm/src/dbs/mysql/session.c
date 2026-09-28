#include "session.h"
#include "session_cursor.h"
#include "session_transaction.h"
#include "transaction_control.h"

#include "auth/auth.h"
#include "wire/handshake.h"
#include "wire/packet.h"
#include "wire/query.h"
#include "wire/result.h"
#include "wire/row.h"
#include "wire/statement.h"

#include <cnet/cnet.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MYSQL_SESSION_CONTROL_CAPACITY 4096u
#define MYSQL_SESSION_PACKET_CAPACITY (MYSQL_SESSION_CONTROL_CAPACITY + 4u)
#define MYSQL_SESSION_DEFAULT_TIMEOUT_MS 5000u
#define MYSQL_SESSION_MAX_PACKET_SIZE UINT32_C(0x01000000)
#define MYSQL_COM_PING UINT8_C(0x0e)
#define MYSQL_SESSION_PROBE_COLUMN_COUNT 4u

static const uint8_t MYSQL_SESSION_PROBE_SQL[] =
    "SELECT s,u,txt,decv FROM m3_probe WHERE s=?";

static const uint8_t MYSQL_TXN_READ_UNCOMMITTED[] =
    "SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED";
static const uint8_t MYSQL_TXN_READ_COMMITTED[] =
    "SET TRANSACTION ISOLATION LEVEL READ COMMITTED";
static const uint8_t MYSQL_TXN_REPEATABLE_READ[] =
    "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ";
static const uint8_t MYSQL_TXN_SERIALIZABLE[] =
    "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE";
static const uint8_t MYSQL_TXN_BEGIN[] = "START TRANSACTION";
static const uint8_t MYSQL_TXN_COMMIT[] = "COMMIT";
static const uint8_t MYSQL_TXN_ROLLBACK[] = "ROLLBACK";

typedef enum mysql_session_action_t {
  MYSQL_SESSION_ACTION_PING = 0,
  MYSQL_SESSION_ACTION_PREPARED_PROBE,
  MYSQL_SESSION_ACTION_PREPARED_CURSOR,
  MYSQL_SESSION_ACTION_PREPARED_COMMAND,
  MYSQL_SESSION_ACTION_TRANSACTION
} mysql_session_action_t;

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
  MYSQL_PHASE_WAIT_CONTROL_SEND,
  MYSQL_PHASE_WAIT_CONTROL_REPLY,
  MYSQL_PHASE_TRANSACTION_READY,
  MYSQL_PHASE_WAIT_PREPARE_SEND,
  MYSQL_PHASE_WAIT_PREPARE_OK,
  MYSQL_PHASE_WAIT_PREPARE_PARAM_DEF,
  MYSQL_PHASE_WAIT_PREPARE_COLUMN_DEF,
  MYSQL_PHASE_WAIT_EXECUTE_SEND,
  MYSQL_PHASE_WAIT_COMMAND_REPLY,
  MYSQL_PHASE_WAIT_RESULT_COLUMN_COUNT,
  MYSQL_PHASE_WAIT_RESULT_COLUMN_DEF,
  MYSQL_PHASE_WAIT_RESULT_ROW,
  MYSQL_PHASE_CURSOR_READY,
  MYSQL_PHASE_CURSOR_ROW_READY,
  MYSQL_PHASE_WAIT_CLOSE_SEND,
  MYSQL_PHASE_DONE,
  MYSQL_PHASE_FAILED
} mysql_session_phase_t;

typedef enum mysql_session_send_kind_t {
  MYSQL_SEND_NONE = 0,
  MYSQL_SEND_SSL_REQUEST,
  MYSQL_SEND_HANDSHAKE_RESPONSE,
  MYSQL_SEND_AUTH_RESPONSE,
  MYSQL_SEND_PING,
  MYSQL_SEND_CONTROL,
  MYSQL_SEND_PREPARE,
  MYSQL_SEND_EXECUTE,
  MYSQL_SEND_CLOSE
} mysql_session_send_kind_t;

typedef enum mysql_transaction_step_t {
  MYSQL_TRANSACTION_STEP_NONE = 0,
  MYSQL_TRANSACTION_STEP_SET_ISOLATION,
  MYSQL_TRANSACTION_STEP_BEGIN,
  MYSQL_TRANSACTION_STEP_COMMAND,
  MYSQL_TRANSACTION_STEP_SAVEPOINT,
  MYSQL_TRANSACTION_STEP_ROLLBACK_TO_SAVEPOINT,
  MYSQL_TRANSACTION_STEP_RELEASE_SAVEPOINT,
  MYSQL_TRANSACTION_STEP_COMMIT,
  MYSQL_TRANSACTION_STEP_ROLLBACK
} mysql_transaction_step_t;

typedef struct mysql_session_t {
  const mysql_session_config_t *config;
  mysql_session_error_t *error;
  mysql_session_action_t action;
  mysql_session_prepared_probe_t *probe;
  mysql_session_command_result_t *command_result;
  const uint8_t *prepared_sql;
  size_t prepared_sql_size;
  const mysql_stmt_value_t *prepared_values;
  size_t prepared_value_count;
  mysql_transaction_step_t transaction_step;
  const uint8_t *control_sql;
  size_t control_sql_size;
  bool transaction_active;
  mysql_session_cursor_limits_t cursor_limits;
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
  mysql_stmt_prepare_ok_t prepare_ok;
  size_t metadata_index;
  uint64_t result_column_count;
  mysql_column_definition_t result_columns[MYSQL_SESSION_PROBE_COLUMN_COUNT];
  uint32_t result_row_count;
  mysql_column_definition_t *cursor_columns;
  unsigned char *cursor_metadata;
  size_t cursor_metadata_used;
  uint8_t *cursor_message;
  size_t cursor_message_capacity;
  size_t command_capacity;
  uint8_t deferred[MYSQL_SESSION_PACKET_CAPACITY];
  size_t deferred_size;
  size_t cursor_row_size;
  uint32_t timeout_ms;
  bool cancel_requested;
  mysql_session_error_t owned_error;
} mysql_session_t;

struct mysql_transaction_session_t {
  mysql_session_t session;
  size_t max_command_bytes;
};

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
         phase == MYSQL_PHASE_WAIT_PING_REPLY ||
         phase == MYSQL_PHASE_WAIT_CONTROL_REPLY ||
         phase == MYSQL_PHASE_WAIT_PREPARE_OK ||
         phase == MYSQL_PHASE_WAIT_PREPARE_PARAM_DEF ||
         phase == MYSQL_PHASE_WAIT_PREPARE_COLUMN_DEF ||
         phase == MYSQL_PHASE_WAIT_COMMAND_REPLY ||
         phase == MYSQL_PHASE_WAIT_RESULT_COLUMN_COUNT ||
         phase == MYSQL_PHASE_WAIT_RESULT_COLUMN_DEF ||
         phase == MYSQL_PHASE_WAIT_RESULT_ROW;
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
  uint8_t stack_packet[MYSQL_SESSION_PACKET_CAPACITY];
  uint8_t *packet = stack_packet;
  const size_t command_capacity =
      session->command_capacity != 0u
          ? session->command_capacity
          : MYSQL_SESSION_CONTROL_CAPACITY;
  size_t total;
  int status;

  if (payload_size > command_capacity ||
      payload_size > MYSQL_WIRE_PACKET_MAX_PAYLOAD ||
      payload_size > SIZE_MAX - 4u ||
      (payload == NULL && payload_size != 0u)) {
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "send", "MySQL command exceeds configured bound");
    return SALTS_EMSGSIZE;
  }

  total = payload_size + 4u;
  if (total > sizeof(stack_packet)) {
    packet = (uint8_t *)malloc(total);
    if (packet == NULL) {
      mysql_session_set_error(session, MYSQL_SESSION_IO,
                              "send", "allocate bounded MySQL command packet");
      return SALTS_ENOMEM;
    }
  }

  packet[0] = (uint8_t)(payload_size & 0xffu);
  packet[1] = (uint8_t)((payload_size >> 8u) & 0xffu);
  packet[2] = (uint8_t)((payload_size >> 16u) & 0xffu);
  packet[3] = sequence_id;
  if (payload_size != 0u)
    memcpy(packet + 4u, payload, payload_size);

  status = cnet_send(&session->client, session->connection, packet, total);
  if (packet != stack_packet)
    free(packet);
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

static mysql_session_status_t mysql_session_isolation_sql(
    orm_isolation_t isolation,
    const uint8_t **out_sql, size_t *out_size) {
  if (out_sql == NULL || out_size == NULL)
    return MYSQL_SESSION_INVALID;

  switch (isolation) {
    case ORM_ISOLATION_READ_UNCOMMITTED:
      *out_sql = MYSQL_TXN_READ_UNCOMMITTED;
      *out_size = sizeof(MYSQL_TXN_READ_UNCOMMITTED) - 1u;
      return MYSQL_SESSION_OK;
    case ORM_ISOLATION_READ_COMMITTED:
      *out_sql = MYSQL_TXN_READ_COMMITTED;
      *out_size = sizeof(MYSQL_TXN_READ_COMMITTED) - 1u;
      return MYSQL_SESSION_OK;
    case ORM_ISOLATION_REPEATABLE_READ:
      *out_sql = MYSQL_TXN_REPEATABLE_READ;
      *out_size = sizeof(MYSQL_TXN_REPEATABLE_READ) - 1u;
      return MYSQL_SESSION_OK;
    case ORM_ISOLATION_SERIALIZABLE:
      *out_sql = MYSQL_TXN_SERIALIZABLE;
      *out_size = sizeof(MYSQL_TXN_SERIALIZABLE) - 1u;
      return MYSQL_SESSION_OK;
    case ORM_ISOLATION_SNAPSHOT:
    default:
      *out_sql = NULL;
      *out_size = 0u;
      return MYSQL_SESSION_INVALID;
  }
}

static void mysql_session_send_control(
    mysql_session_t *session,
    const uint8_t *sql, size_t sql_size) {
  uint8_t stack_payload[MYSQL_SESSION_CONTROL_CAPACITY];
  uint8_t *payload = stack_payload;
  const size_t capacity =
      session->command_capacity != 0u
          ? session->command_capacity
          : MYSQL_SESSION_CONTROL_CAPACITY;
  size_t payload_size = 0u;
  mysql_wire_status_t status;

  if (sql == NULL || sql_size == 0u || sql_size + 1u > capacity) {
    mysql_session_set_error(session, MYSQL_SESSION_INVALID,
                            "control",
                            "invalid or oversized MySQL control statement");
    return;
  }
  if (capacity > sizeof(stack_payload)) {
    payload = (uint8_t *)malloc(capacity);
    if (payload == NULL) {
      mysql_session_set_error(session, MYSQL_SESSION_IO,
                              "control",
                              "allocate MySQL control command buffer");
      return;
    }
  }

  status = mysql_wire_build_query(
      sql, sql_size, payload, capacity, &payload_size);
  if (status != MYSQL_WIRE_STATUS_OK) {
    if (payload != stack_payload)
      free(payload);
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "control",
                            "failed to encode COM_QUERY control statement");
    return;
  }

  session->control_sql = sql;
  session->control_sql_size = sql_size;
  mysql_wire_packet_stream_reset(&session->stream, UINT8_C(1));
  session->phase = MYSQL_PHASE_WAIT_CONTROL_SEND;
  (void)mysql_session_send_packet(
      session, UINT8_C(0), payload, payload_size, MYSQL_SEND_CONTROL);
  if (payload != stack_payload)
    free(payload);
}

static void mysql_session_send_prepare(mysql_session_t *session) {
  uint8_t stack_payload[MYSQL_SESSION_CONTROL_CAPACITY];
  uint8_t *payload = stack_payload;
  const uint8_t *sql = MYSQL_SESSION_PROBE_SQL;
  size_t sql_size = sizeof(MYSQL_SESSION_PROBE_SQL) - 1u;
  const size_t capacity =
      session->command_capacity != 0u
          ? session->command_capacity
          : MYSQL_SESSION_CONTROL_CAPACITY;
  size_t payload_size = 0u;
  mysql_wire_status_t wire_status;

  if ((session->client_capabilities & MYSQL_WIRE_CLIENT_DEPRECATE_EOF) == 0u) {
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "prepare-capability",
                            "prepared statements require CLIENT_DEPRECATE_EOF");
    return;
  }
  if (session->action == MYSQL_SESSION_ACTION_PREPARED_CURSOR ||
      session->action == MYSQL_SESSION_ACTION_PREPARED_COMMAND ||
      session->action == MYSQL_SESSION_ACTION_TRANSACTION) {
    sql = session->prepared_sql;
    sql_size = session->prepared_sql_size;
  }
  if (sql == NULL || sql_size == 0u || sql_size + 1u > capacity) {
    mysql_session_set_error(session, MYSQL_SESSION_INVALID,
                            "prepare", "invalid or oversized prepared SQL");
    return;
  }
  if (capacity > sizeof(stack_payload)) {
    payload = (uint8_t *)malloc(capacity);
    if (payload == NULL) {
      mysql_session_set_error(session, MYSQL_SESSION_IO,
                              "prepare", "allocate prepared command buffer");
      return;
    }
  }

  wire_status = mysql_wire_build_stmt_prepare(
      sql, sql_size, payload, capacity, &payload_size);
  if (wire_status != MYSQL_WIRE_STATUS_OK) {
    if (payload != stack_payload)
      free(payload);
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "prepare", "failed to encode COM_STMT_PREPARE");
    return;
  }

  mysql_wire_packet_stream_reset(&session->stream, UINT8_C(1));
  session->phase = MYSQL_PHASE_WAIT_PREPARE_SEND;
  (void)mysql_session_send_packet(
      session, UINT8_C(0), payload, payload_size, MYSQL_SEND_PREPARE);
  if (payload != stack_payload)
    free(payload);
}

static void mysql_session_send_execute(mysql_session_t *session) {
  const mysql_stmt_value_t probe_value = {
      .kind = MYSQL_STMT_VALUE_SINT64,
      .data.sint64_value = INT64_C(-42)};
  const mysql_stmt_value_t *values = &probe_value;
  size_t value_count = 1u;
  uint8_t stack_payload[MYSQL_SESSION_CONTROL_CAPACITY];
  uint8_t *payload = stack_payload;
  const size_t capacity =
      session->command_capacity != 0u
          ? session->command_capacity
          : MYSQL_SESSION_CONTROL_CAPACITY;
  size_t payload_size = 0u;
  mysql_wire_status_t wire_status;

  if (session->action == MYSQL_SESSION_ACTION_PREPARED_CURSOR ||
      session->action == MYSQL_SESSION_ACTION_PREPARED_COMMAND ||
      session->action == MYSQL_SESSION_ACTION_TRANSACTION) {
    values = session->prepared_values;
    value_count = session->prepared_value_count;
  }
  if (capacity > sizeof(stack_payload)) {
    payload = (uint8_t *)malloc(capacity);
    if (payload == NULL) {
      mysql_session_set_error(session, MYSQL_SESSION_IO,
                              "execute", "allocate execute command buffer");
      return;
    }
  }

  wire_status = mysql_wire_build_stmt_execute(
      session->prepare_ok.statement_id, values, value_count,
      payload, capacity, &payload_size);
  if (wire_status != MYSQL_WIRE_STATUS_OK) {
    if (payload != stack_payload)
      free(payload);
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "execute", "failed to encode COM_STMT_EXECUTE");
    return;
  }

  mysql_wire_packet_stream_reset(&session->stream, UINT8_C(1));
  session->phase = MYSQL_PHASE_WAIT_EXECUTE_SEND;
  (void)mysql_session_send_packet(
      session, UINT8_C(0), payload, payload_size, MYSQL_SEND_EXECUTE);
  if (payload != stack_payload)
    free(payload);
}

static void mysql_session_send_close(mysql_session_t *session) {
  uint8_t payload[8];
  size_t payload_size = 0u;

  if (mysql_wire_build_stmt_close(
          session->prepare_ok.statement_id,
          payload, sizeof(payload), &payload_size) != MYSQL_WIRE_STATUS_OK) {
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "close", "failed to encode COM_STMT_CLOSE");
    return;
  }

  session->phase = MYSQL_PHASE_WAIT_CLOSE_SEND;
  (void)mysql_session_send_packet(
      session, UINT8_C(0), payload, payload_size, MYSQL_SEND_CLOSE);
}

static void mysql_session_begin_action(mysql_session_t *session) {
  if (session->action == MYSQL_SESSION_ACTION_PING) {
    mysql_session_send_ping(session);
    return;
  }
  if (session->action == MYSQL_SESSION_ACTION_PREPARED_PROBE ||
      session->action == MYSQL_SESSION_ACTION_PREPARED_CURSOR ||
      session->action == MYSQL_SESSION_ACTION_PREPARED_COMMAND) {
    mysql_session_send_prepare(session);
    return;
  }
  if (session->action == MYSQL_SESSION_ACTION_TRANSACTION) {
    session->transaction_step = MYSQL_TRANSACTION_STEP_SET_ISOLATION;
    mysql_session_send_control(
        session, session->control_sql, session->control_sql_size);
    return;
  }
  mysql_session_set_error(session, MYSQL_SESSION_INVALID,
                          "action", "invalid MySQL session action");
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
    mysql_session_begin_action(session);
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

static bool mysql_session_decode_server_error(
    mysql_session_t *session, const uint8_t *payload, size_t payload_size,
    const char *stage) {
  mysql_wire_err_packet_t server_error;
  if (payload_size == 0u || payload[0] != UINT8_C(0xff))
    return false;
  if (mysql_wire_decode_err_packet(
          payload, payload_size, session->client_capabilities,
          &server_error) == MYSQL_WIRE_STATUS_OK &&
      session->error != NULL) {
    session->error->server_error = server_error.error_code;
    if (server_error.has_sql_state)
      memcpy(session->error->sql_state, server_error.sql_state,
             sizeof(server_error.sql_state));
  }
  mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                          stage, "server rejected prepared statement operation");
  return true;
}

static void mysql_session_handle_prepare_ok(
    mysql_session_t *session, const uint8_t *payload, size_t payload_size) {
  if (mysql_session_decode_server_error(
          session, payload, payload_size, "prepare-server"))
    return;
  if (mysql_wire_decode_stmt_prepare_ok(
          payload, payload_size, &session->prepare_ok) != MYSQL_WIRE_STATUS_OK) {
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "prepare-ok", "invalid COM_STMT_PREPARE_OK");
    return;
  }
  if (session->action == MYSQL_SESSION_ACTION_PREPARED_CURSOR) {
    if ((size_t)session->prepare_ok.parameter_count !=
            session->prepared_value_count ||
        session->prepare_ok.column_count == 0u ||
        (size_t)session->prepare_ok.column_count >
            session->cursor_limits.max_columns) {
      mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                              "prepare-shape",
                              "prepared cursor metadata exceeds expected shape");
      return;
    }
  } else if (session->action == MYSQL_SESSION_ACTION_PREPARED_COMMAND ||
             session->action == MYSQL_SESSION_ACTION_TRANSACTION) {
    if ((size_t)session->prepare_ok.parameter_count !=
            session->prepared_value_count ||
        session->prepare_ok.column_count != 0u) {
      mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                              "prepare-shape",
                              "prepared command unexpectedly returns columns");
      return;
    }
  } else if (session->prepare_ok.parameter_count != 1u ||
             session->prepare_ok.column_count !=
                 MYSQL_SESSION_PROBE_COLUMN_COUNT) {
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "prepare-shape",
                            "unexpected prepared metadata shape");
    return;
  }
  session->metadata_index = 0u;
  if (session->prepare_ok.parameter_count != 0u) {
    session->phase = MYSQL_PHASE_WAIT_PREPARE_PARAM_DEF;
  } else if (session->prepare_ok.column_count != 0u) {
    session->phase = MYSQL_PHASE_WAIT_PREPARE_COLUMN_DEF;
  } else {
    mysql_session_send_execute(session);
  }
}

static void mysql_session_handle_prepare_param_def(
    mysql_session_t *session, const uint8_t *payload, size_t payload_size) {
  mysql_column_definition_t definition;
  if (mysql_wire_decode_column_definition41(
          payload, payload_size, &definition) != MYSQL_WIRE_STATUS_OK) {
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "prepare-param", "invalid parameter metadata");
    return;
  }
  ++session->metadata_index;
  if (session->metadata_index == session->prepare_ok.parameter_count) {
    session->metadata_index = 0u;
    if (session->prepare_ok.column_count != 0u)
      session->phase = MYSQL_PHASE_WAIT_PREPARE_COLUMN_DEF;
    else
      mysql_session_send_execute(session);
  }
}

static void mysql_session_handle_prepare_column_def(
    mysql_session_t *session, const uint8_t *payload, size_t payload_size) {
  mysql_column_definition_t definition;
  if (mysql_wire_decode_column_definition41(
          payload, payload_size, &definition) != MYSQL_WIRE_STATUS_OK) {
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "prepare-column", "invalid prepared result metadata");
    return;
  }
  ++session->metadata_index;
  if (session->metadata_index == session->prepare_ok.column_count) {
    session->metadata_index = 0u;
    mysql_session_send_execute(session);
  }
}

static void mysql_session_handle_control_reply(
    mysql_session_t *session, const uint8_t *payload, size_t payload_size) {
  mysql_wire_ok_packet_t ok;

  if (mysql_session_decode_server_error(
          session, payload, payload_size, "control-server"))
    return;
  if (mysql_wire_decode_ok_packet(
          payload, payload_size, session->client_capabilities,
          &ok) != MYSQL_WIRE_STATUS_OK) {
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "control-ok",
                            "invalid MySQL control OK packet");
    return;
  }

  switch (session->transaction_step) {
    case MYSQL_TRANSACTION_STEP_SET_ISOLATION:
      session->transaction_step = MYSQL_TRANSACTION_STEP_BEGIN;
      mysql_session_send_control(
          session, MYSQL_TXN_BEGIN, sizeof(MYSQL_TXN_BEGIN) - 1u);
      break;
    case MYSQL_TRANSACTION_STEP_BEGIN:
      session->transaction_active = true;
      session->phase = MYSQL_PHASE_TRANSACTION_READY;
      break;
    case MYSQL_TRANSACTION_STEP_SAVEPOINT:
    case MYSQL_TRANSACTION_STEP_ROLLBACK_TO_SAVEPOINT:
    case MYSQL_TRANSACTION_STEP_RELEASE_SAVEPOINT:
      session->phase = MYSQL_PHASE_TRANSACTION_READY;
      break;
    case MYSQL_TRANSACTION_STEP_COMMIT:
    case MYSQL_TRANSACTION_STEP_ROLLBACK:
      session->transaction_active = false;
      session->phase = MYSQL_PHASE_DONE;
      break;
    default:
      mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                              "control-state",
                              "unexpected transaction control reply");
      break;
  }
}

static void mysql_session_handle_command_reply(
    mysql_session_t *session, const uint8_t *payload, size_t payload_size) {
  mysql_wire_ok_packet_t ok;

  if (mysql_session_decode_server_error(
          session, payload, payload_size, "command-server"))
    return;
  if (mysql_wire_decode_ok_packet(
          payload, payload_size, session->client_capabilities,
          &ok) != MYSQL_WIRE_STATUS_OK) {
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "command-ok",
                            "invalid prepared command OK packet");
    return;
  }
  if (session->command_result != NULL) {
    session->command_result->affected_rows = ok.affected_rows;
    session->command_result->last_insert_id = ok.last_insert_id;
    session->command_result->status_flags = ok.status_flags;
    session->command_result->warnings = ok.warnings;
  }
  mysql_session_send_close(session);
}

static void mysql_session_handle_result_column_count(
    mysql_session_t *session, const uint8_t *payload, size_t payload_size) {
  size_t offset = 0u;
  uint64_t count = 0u;
  bool is_null = false;

  if (mysql_session_decode_server_error(
          session, payload, payload_size, "execute-server"))
    return;
  if (mysql_wire_read_lenenc_uint(
          payload, payload_size, &offset, &count, &is_null) != MYSQL_WIRE_STATUS_OK ||
      is_null || offset != payload_size) {
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "result-columns", "invalid binary result column count");
    return;
  }
  if (session->action == MYSQL_SESSION_ACTION_PREPARED_CURSOR) {
    if (count == 0u ||
        count > (uint64_t)session->cursor_limits.max_columns ||
        count != (uint64_t)session->prepare_ok.column_count) {
      mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                              "result-columns",
                              "prepared cursor result column count mismatch");
      return;
    }
  } else if (count != MYSQL_SESSION_PROBE_COLUMN_COUNT) {
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "result-columns",
                            "unexpected binary result column count");
    return;
  }
  session->result_column_count = count;
  session->metadata_index = 0u;
  session->phase = MYSQL_PHASE_WAIT_RESULT_COLUMN_DEF;
}

static mysql_wire_status_t mysql_session_copy_cursor_column(
    mysql_session_t *session, size_t index,
    const mysql_column_definition_t *input) {
  mysql_column_definition_t copied;
  size_t name_size;

  if (session == NULL || input == NULL ||
      session->cursor_columns == NULL ||
      session->cursor_metadata == NULL ||
      index >= session->cursor_limits.max_columns)
    return MYSQL_WIRE_STATUS_INVALID;

  name_size = input->name.length;
  if (input->name.is_null ||
      (input->name.data == NULL && name_size != 0u))
    return MYSQL_WIRE_STATUS_INVALID;
  if (session->cursor_metadata_used >
          session->cursor_limits.max_metadata_bytes ||
      name_size > session->cursor_limits.max_metadata_bytes -
                      session->cursor_metadata_used)
    return MYSQL_WIRE_STATUS_LIMIT;

  copied = *input;
  copied.catalog = (mysql_wire_bytes_t){0};
  copied.schema = (mysql_wire_bytes_t){0};
  copied.table = (mysql_wire_bytes_t){0};
  copied.org_table = (mysql_wire_bytes_t){0};
  copied.org_name = (mysql_wire_bytes_t){0};

  if (name_size != 0u)
    memcpy(session->cursor_metadata + session->cursor_metadata_used,
           input->name.data, name_size);
  copied.name.data =
      name_size != 0u
          ? session->cursor_metadata + session->cursor_metadata_used
          : NULL;
  copied.name.length = name_size;
  copied.name.is_null = false;
  session->cursor_metadata_used += name_size;
  session->cursor_columns[index] = copied;
  return MYSQL_WIRE_STATUS_OK;
}

static void mysql_session_handle_result_column_def(
    mysql_session_t *session, const uint8_t *payload, size_t payload_size) {
  mysql_column_definition_t definition;
  mysql_wire_status_t status;

  if (session->metadata_index >= (size_t)session->result_column_count ||
      mysql_wire_decode_column_definition41(
          payload, payload_size, &definition) != MYSQL_WIRE_STATUS_OK) {
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "result-column", "invalid binary result metadata");
    return;
  }

  if (session->action == MYSQL_SESSION_ACTION_PREPARED_CURSOR) {
    status = mysql_session_copy_cursor_column(
        session, session->metadata_index, &definition);
    if (status != MYSQL_WIRE_STATUS_OK) {
      mysql_session_set_error(
          session,
          status == MYSQL_WIRE_STATUS_LIMIT
              ? MYSQL_SESSION_PROTOCOL
              : MYSQL_SESSION_PROTOCOL,
          "result-column",
          status == MYSQL_WIRE_STATUS_LIMIT
              ? "prepared cursor metadata exceeds configured bound"
              : "invalid prepared cursor metadata");
      return;
    }
  } else {
    session->result_columns[session->metadata_index] = definition;
  }

  ++session->metadata_index;
  if (session->metadata_index == session->result_column_count)
    session->phase =
        session->action == MYSQL_SESSION_ACTION_PREPARED_CURSOR
            ? MYSQL_PHASE_CURSOR_READY
            : MYSQL_PHASE_WAIT_RESULT_ROW;
}

static void mysql_session_handle_result_row(
    mysql_session_t *session, const uint8_t *payload, size_t payload_size) {
  mysql_binary_value_t values[MYSQL_SESSION_PROBE_COLUMN_COUNT];

  if (payload_size != 0u && payload[0] == UINT8_C(0xfe) &&
      payload_size >= 7u) {
    mysql_wire_ok_packet_t ok;
    if (mysql_wire_decode_ok_packet(
            payload, payload_size, session->client_capabilities,
            &ok) != MYSQL_WIRE_STATUS_OK ||
        (session->action != MYSQL_SESSION_ACTION_PREPARED_CURSOR &&
         session->result_row_count != 1u)) {
      mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                              "result-end", "invalid prepared result terminator");
      return;
    }
    mysql_session_send_close(session);
    return;
  }
  if (mysql_session_decode_server_error(
          session, payload, payload_size, "result-server"))
    return;
  if (session->action == MYSQL_SESSION_ACTION_PREPARED_CURSOR) {
    if (payload_size == 0u || payload_size > session->cursor_message_capacity) {
      mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                              "result-row",
                              "prepared cursor row exceeds configured message bound");
      return;
    }
    session->cursor_row_size = payload_size;
    ++session->result_row_count;
    session->phase = MYSQL_PHASE_CURSOR_ROW_READY;
    return;
  }
  if (session->result_row_count != 0u ||
      mysql_wire_decode_binary_row(
          payload, payload_size, session->result_columns,
          MYSQL_SESSION_PROBE_COLUMN_COUNT, values,
          MYSQL_SESSION_PROBE_COLUMN_COUNT) != MYSQL_WIRE_STATUS_OK) {
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "result-row", "invalid prepared binary row");
    return;
  }

  if (values[0].kind != MYSQL_BINARY_VALUE_SINT64 ||
      values[1].kind != MYSQL_BINARY_VALUE_UINT64 ||
      values[2].kind != MYSQL_BINARY_VALUE_BYTES ||
      values[3].kind != MYSQL_BINARY_VALUE_BYTES ||
      values[2].data.bytes.length >= sizeof(session->probe->text) ||
      values[3].data.bytes.length >= sizeof(session->probe->decimal)) {
    mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                            "result-shape", "unexpected prepared result types");
    return;
  }

  session->probe->signed_value = values[0].data.sint64_value;
  session->probe->unsigned_value = values[1].data.uint64_value;
  session->probe->text_size = values[2].data.bytes.length;
  memcpy(session->probe->text, values[2].data.bytes.data,
         values[2].data.bytes.length);
  session->probe->text[values[2].data.bytes.length] = '\0';
  session->probe->decimal_size = values[3].data.bytes.length;
  memcpy(session->probe->decimal, values[3].data.bytes.data,
         values[3].data.bytes.length);
  session->probe->decimal[values[3].data.bytes.length] = '\0';
  session->probe->row_count = 1u;
  session->result_row_count = 1u;
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
    case MYSQL_PHASE_WAIT_PREPARE_OK:
      mysql_session_handle_prepare_ok(session, payload, payload_size);
      break;
    case MYSQL_PHASE_WAIT_PREPARE_PARAM_DEF:
      mysql_session_handle_prepare_param_def(session, payload, payload_size);
      break;
    case MYSQL_PHASE_WAIT_PREPARE_COLUMN_DEF:
      mysql_session_handle_prepare_column_def(session, payload, payload_size);
      break;
    case MYSQL_PHASE_WAIT_CONTROL_REPLY:
      mysql_session_handle_control_reply(session, payload, payload_size);
      break;
    case MYSQL_PHASE_WAIT_COMMAND_REPLY:
      mysql_session_handle_command_reply(session, payload, payload_size);
      break;
    case MYSQL_PHASE_WAIT_RESULT_COLUMN_COUNT:
      mysql_session_handle_result_column_count(session, payload, payload_size);
      break;
    case MYSQL_PHASE_WAIT_RESULT_COLUMN_DEF:
      mysql_session_handle_result_column_def(session, payload, payload_size);
      break;
    case MYSQL_PHASE_WAIT_RESULT_ROW:
      mysql_session_handle_result_row(session, payload, payload_size);
      break;
    default:
      mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                              "state", "unexpected MySQL packet for session phase");
      break;
  }
}

static uint8_t *mysql_session_message_buffer(
    mysql_session_t *session) {
  return session != NULL &&
                 session->action == MYSQL_SESSION_ACTION_PREPARED_CURSOR
             ? session->cursor_message
             : (session != NULL ? session->control : NULL);
}

static size_t mysql_session_message_capacity(
    const mysql_session_t *session) {
  return session != NULL &&
                 session->action == MYSQL_SESSION_ACTION_PREPARED_CURSOR
             ? session->cursor_message_capacity
             : MYSQL_SESSION_CONTROL_CAPACITY;
}

static bool mysql_session_pause_receive(mysql_session_phase_t phase) {
  return phase == MYSQL_PHASE_CURSOR_READY ||
         phase == MYSQL_PHASE_CURSOR_ROW_READY;
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
      uint8_t *message = mysql_session_message_buffer(session);
      const size_t capacity = mysql_session_message_capacity(session);
      if (message == NULL ||
          session->control_used > capacity ||
          event.length > capacity - session->control_used) {
        mysql_session_set_error(session, MYSQL_SESSION_PROTOCOL,
                                "packet",
                                "MySQL message exceeds configured bound");
        return;
      }
      memcpy(message + session->control_used,
             event.data, event.length);
      session->control_used += event.length;
      session->message_last_sequence = event.sequence_id;
    } else if (event.packet_end) {
      session->message_last_sequence = event.sequence_id;
    }

    if (event.message_end) {
      uint8_t last_sequence = session->message_last_sequence;
      uint8_t *message = mysql_session_message_buffer(session);
      mysql_session_handle_message(
          session, message, session->control_used, last_sequence);
      session->control_used = 0u;
      if (session->phase == MYSQL_PHASE_FAILED ||
          session->phase == MYSQL_PHASE_DONE)
        break;
      if (mysql_session_pause_receive(session->phase)) {
        const size_t remaining = view->size - offset;
        if (remaining > sizeof(session->deferred)) {
          mysql_session_set_error(
              session, MYSQL_SESSION_PROTOCOL,
              "receive-deferred",
              "CNet callback remainder exceeds deferred transport bound");
          return;
        }
        if (remaining != 0u)
          memcpy(session->deferred, input + offset, remaining);
        session->deferred_size = remaining;
        break;
      }
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
    case MYSQL_SEND_CONTROL:
      session->phase = MYSQL_PHASE_WAIT_CONTROL_REPLY;
      (void)mysql_session_request_receive(session);
      break;
    case MYSQL_SEND_PREPARE:
      session->phase = MYSQL_PHASE_WAIT_PREPARE_OK;
      (void)mysql_session_request_receive(session);
      break;
    case MYSQL_SEND_EXECUTE:
      session->phase =
          session->action == MYSQL_SESSION_ACTION_PREPARED_COMMAND ||
                  session->action == MYSQL_SESSION_ACTION_TRANSACTION
              ? MYSQL_PHASE_WAIT_COMMAND_REPLY
              : MYSQL_PHASE_WAIT_RESULT_COLUMN_COUNT;
      (void)mysql_session_request_receive(session);
      break;
    case MYSQL_SEND_CLOSE:
      if (session->action == MYSQL_SESSION_ACTION_TRANSACTION &&
          session->transaction_step == MYSQL_TRANSACTION_STEP_COMMAND)
        session->phase = MYSQL_PHASE_TRANSACTION_READY;
      else
        session->phase = MYSQL_PHASE_DONE;
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
      session->phase != MYSQL_PHASE_FAILED) {
    if (session->cancel_requested) {
      session->phase = MYSQL_PHASE_DONE;
      return;
    }
    mysql_session_set_error(session, MYSQL_SESSION_IO,
                            "closed",
                            "connection closed before MySQL operation completed");
  }
}

static cnet_client_config mysql_session_cnet_config(
    uint32_t timeout_ms, size_t command_capacity) {
  const size_t max_send_bytes =
      command_capacity <= SIZE_MAX - 4u
          ? command_capacity + 4u
          : SIZE_MAX;
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
      .max_send_bytes = max_send_bytes,
      .receive_buffer_bytes = MYSQL_SESSION_PACKET_CAPACITY,
      .connect_timeout_ms = timeout_ms,
      .read_timeout_ms = timeout_ms,
      .write_timeout_ms = timeout_ms,
      .tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES,
      .tls_handshake_timeout_ms = timeout_ms,
      .command_buffer_bytes = max_send_bytes,
      .event_buffer_bytes = 0u};
  return config;
}

static bool mysql_session_config_valid(
    const mysql_session_config_t *config) {
  return config != NULL &&
         config->host != NULL && config->host[0] != '\0' &&
         config->port != 0u &&
         config->username != NULL &&
         config->password != NULL &&
         config->ca_file != NULL && config->ca_file[0] != '\0' &&
         config->server_name != NULL && config->server_name[0] != '\0';
}

static mysql_session_status_t mysql_session_start(
    mysql_session_t *session,
    const mysql_session_config_t *config,
    mysql_session_action_t action,
    mysql_session_prepared_probe_t *probe,
    mysql_session_error_t *error,
    size_t message_capacity,
    size_t command_capacity) {
  cnet_client_config client_config;
  cnet_connect_options options;
  char uri[512];
  int status;

  if (error != NULL)
    memset(error, 0, sizeof(*error));
  if (session == NULL || !mysql_session_config_valid(config) ||
      message_capacity == 0u || command_capacity == 0u ||
      command_capacity > MYSQL_WIRE_PACKET_MAX_PAYLOAD) {
    if (error != NULL) {
      error->status = MYSQL_SESSION_INVALID;
      (void)snprintf(error->stage, sizeof(error->stage), "%s", "config");
      (void)snprintf(error->message, sizeof(error->message), "%s",
                     "invalid MySQL session configuration or bounds");
    }
    return MYSQL_SESSION_INVALID;
  }

  memset(session, 0, sizeof(*session));
  session->config = config;
  session->error = error;
  session->action = action;
  session->probe = probe;
  session->phase = MYSQL_PHASE_WAIT_TCP;
  session->command_capacity = command_capacity;
  session->timeout_ms = config->timeout_ms != 0u
                            ? config->timeout_ms
                            : MYSQL_SESSION_DEFAULT_TIMEOUT_MS;

  {
    const int uri_size = snprintf(
        uri, sizeof(uri), "tcp://%s:%u",
        config->host, (unsigned int)config->port);
    if (uri_size <= 0 || (size_t)uri_size >= sizeof(uri)) {
      if (error != NULL) {
        error->status = MYSQL_SESSION_INVALID;
        (void)snprintf(error->stage, sizeof(error->stage), "%s", "uri");
      }
      return MYSQL_SESSION_INVALID;
    }
  }

  if (mysql_wire_packet_stream_init(
          &session->stream, UINT8_C(0),
          message_capacity) != MYSQL_WIRE_STATUS_OK) {
    if (error != NULL) {
      error->status = MYSQL_SESSION_PROTOCOL;
      (void)snprintf(error->stage, sizeof(error->stage), "%s", "packet-stream");
    }
    return MYSQL_SESSION_PROTOCOL;
  }

  client_config = mysql_session_cnet_config(
      session->timeout_ms, command_capacity);
  status = cnet_client_init(&session->client, &client_config);
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
                   .user = session,
                   .on_send = mysql_session_on_send}};
  status = cnet_connect(&session->client, &options, &session->connection);
  if (status != SALTS_OK) {
    if (error != NULL) {
      error->status = MYSQL_SESSION_IO;
      error->cnet_status = status;
      (void)snprintf(error->stage, sizeof(error->stage), "%s", "connect");
    }
    (void)cnet_client_stop(&session->client, session->timeout_ms);
    (void)cnet_client_destroy(&session->client);
    memset(&session->client, 0, sizeof(session->client));
    return MYSQL_SESSION_IO;
  }

  return MYSQL_SESSION_OK;
}

static mysql_session_status_t mysql_session_progress_until(
    mysql_session_t *session, mysql_session_phase_t target) {
  uint32_t elapsed = 0u;
  int status;

  if (session == NULL)
    return MYSQL_SESSION_INVALID;

  while (session->phase != target &&
         session->phase != MYSQL_PHASE_DONE &&
         session->phase != MYSQL_PHASE_FAILED &&
         elapsed < session->timeout_ms) {
    size_t events = 0u;
    const uint32_t remaining = session->timeout_ms - elapsed;
    const uint32_t slice = remaining > 10u ? 10u : remaining;
    status = cnet_client_poll(&session->client, slice, &events);
    if (status != SALTS_OK) {
      if (session->error != NULL)
        session->error->cnet_status = status;
      mysql_session_set_error(session, MYSQL_SESSION_IO,
                              "poll", "CNet progress failed");
      break;
    }
    elapsed += slice;
  }

  if (session->phase != target &&
      session->phase != MYSQL_PHASE_DONE &&
      session->phase != MYSQL_PHASE_FAILED)
    mysql_session_set_error(session, MYSQL_SESSION_TIMEOUT,
                            "timeout", "MySQL session deadline expired");

  if (session->phase == target ||
      (target == MYSQL_PHASE_DONE &&
       session->phase == MYSQL_PHASE_DONE))
    return MYSQL_SESSION_OK;

  return session->error != NULL &&
                 session->error->status != MYSQL_SESSION_OK
             ? session->error->status
             : MYSQL_SESSION_PROTOCOL;
}

static void mysql_session_release_cursor_storage(
    mysql_session_t *session) {
  if (session == NULL)
    return;
  free(session->cursor_message);
  free(session->cursor_metadata);
  free(session->cursor_columns);
  session->cursor_message = NULL;
  session->cursor_metadata = NULL;
  session->cursor_columns = NULL;
  session->cursor_message_capacity = 0u;
  session->cursor_metadata_used = 0u;
}

static void mysql_session_shutdown(
    mysql_session_t *session, bool request_close) {
  if (session == NULL)
    return;
  if (request_close &&
      session->phase != MYSQL_PHASE_FAILED)
    (void)cnet_close(&session->client, session->connection);
  (void)cnet_client_stop(&session->client, session->timeout_ms);
  (void)cnet_client_destroy(&session->client);
}

static mysql_session_status_t mysql_session_run(
    const mysql_session_config_t *config, mysql_session_action_t action,
    mysql_session_prepared_probe_t *probe, mysql_session_error_t *error) {
  mysql_session_t session;
  mysql_session_status_t result;

  result = mysql_session_start(
      &session, config, action, probe, error,
      MYSQL_SESSION_CONTROL_CAPACITY,
      MYSQL_SESSION_CONTROL_CAPACITY);
  if (result != MYSQL_SESSION_OK)
    return result;

  result = mysql_session_progress_until(
      &session, MYSQL_PHASE_DONE);
  mysql_session_shutdown(&session, session.phase == MYSQL_PHASE_DONE);
  return result;
}

static orm_status_t mysql_session_source_status(
    mysql_session_status_t status) {
  switch (status) {
    case MYSQL_SESSION_INVALID:
      return ORM_STATUS_INVALID_ARGUMENT;
    case MYSQL_SESSION_IO:
    case MYSQL_SESSION_AUTH:
    case MYSQL_SESSION_TIMEOUT:
      return ORM_STATUS_CONNECTION_ERROR;
    case MYSQL_SESSION_PROTOCOL:
      return ORM_STATUS_DATASTORE_ERROR;
    case MYSQL_SESSION_OK:
      return ORM_STATUS_OK;
    default:
      return ORM_STATUS_INTERNAL_ERROR;
  }
}

static void mysql_session_feed_deferred(mysql_session_t *session) {
  uint8_t deferred[MYSQL_SESSION_PACKET_CAPACITY];
  cnet_receive_view view;

  if (session == NULL || session->deferred_size == 0u)
    return;
  memcpy(deferred, session->deferred, session->deferred_size);
  view = (cnet_receive_view){
      .data = deferred,
      .size = session->deferred_size,
      .kind = CNET_MESSAGE_BYTES};
  session->deferred_size = 0u;
  mysql_session_on_receive(session, session->connection, &view);
}

static mysql_cursor_source_step_t mysql_session_cursor_source_next(
    void *context) {
  mysql_session_t *session = (mysql_session_t *)context;
  mysql_cursor_source_step_t step = MYSQL_CURSOR_SOURCE_STEP_INIT;
  mysql_session_status_t progress_status;

  if (session == NULL) {
    step.kind = MYSQL_CURSOR_SOURCE_ERROR;
    step.status = ORM_STATUS_INVALID_ARGUMENT;
    step.message = "invalid MySQL cursor source";
    return step;
  }

  if (session->phase == MYSQL_PHASE_DONE)
    return step;
  if (session->phase == MYSQL_PHASE_FAILED) {
    step.kind = MYSQL_CURSOR_SOURCE_ERROR;
    step.status = mysql_session_source_status(
        session->error != NULL ? session->error->status
                               : MYSQL_SESSION_PROTOCOL);
    step.message = session->error != NULL &&
                           session->error->message[0] != '\0'
                       ? session->error->message
                       : "MySQL cursor source failed";
    return step;
  }
  if (session->cancel_requested) {
    progress_status = mysql_session_progress_until(
        session, MYSQL_PHASE_DONE);
    if (progress_status == MYSQL_SESSION_OK ||
        session->phase == MYSQL_PHASE_DONE)
      return step;
    step.kind = MYSQL_CURSOR_SOURCE_ERROR;
    step.status = mysql_session_source_status(progress_status);
    step.message = session->error != NULL &&
                           session->error->message[0] != '\0'
                       ? session->error->message
                       : "cancel MySQL cursor source";
    return step;
  }

  if (session->phase != MYSQL_PHASE_CURSOR_READY) {
    step.kind = MYSQL_CURSOR_SOURCE_ERROR;
    step.status = ORM_STATUS_INVALID_STATE;
    step.message = "MySQL cursor source is not ready for demand";
    return step;
  }

  session->phase = MYSQL_PHASE_WAIT_RESULT_ROW;
  if (session->deferred_size != 0u) {
    mysql_session_feed_deferred(session);
  } else if (mysql_session_request_receive(session) != SALTS_OK) {
    /* request_receive records the session error */
  }

  if (session->phase == MYSQL_PHASE_CURSOR_ROW_READY ||
      session->phase == MYSQL_PHASE_DONE) {
    progress_status = MYSQL_SESSION_OK;
  } else if (session->phase == MYSQL_PHASE_FAILED) {
    progress_status = session->error != NULL
                          ? session->error->status
                          : MYSQL_SESSION_PROTOCOL;
  } else {
    /*
     * Deferred transport bytes may contain the result terminator immediately
     * after the row. Consuming it transitions through WAIT_CLOSE_SEND, so keep
     * driving the same session until either another row is ready or CLOSE
     * reaches DONE. Treating WAIT_CLOSE_SEND as an error drops a valid
     * deprecate-EOF terminator path.
     */
    progress_status = mysql_session_progress_until(
        session, MYSQL_PHASE_CURSOR_ROW_READY);
  }

  if (session->phase == MYSQL_PHASE_CURSOR_ROW_READY) {
    step.kind = MYSQL_CURSOR_SOURCE_ROW;
    step.row = session->cursor_message;
    step.row_size = session->cursor_row_size;
    session->phase = MYSQL_PHASE_CURSOR_READY;
    return step;
  }

  if (session->phase == MYSQL_PHASE_DONE)
    return step;

  step.kind = MYSQL_CURSOR_SOURCE_ERROR;
  step.status = mysql_session_source_status(progress_status);
  step.message = session->error != NULL &&
                         session->error->message[0] != '\0'
                     ? session->error->message
                     : "advance MySQL cursor source";
  return step;
}

static void mysql_session_cursor_source_cancel(void *context) {
  mysql_session_t *session = (mysql_session_t *)context;
  if (session == NULL || session->cancel_requested ||
      session->phase == MYSQL_PHASE_DONE ||
      session->phase == MYSQL_PHASE_FAILED)
    return;
  session->cancel_requested = true;
  (void)cnet_close(&session->client, session->connection);
}

static void mysql_session_cursor_source_destroy(void *context) {
  mysql_session_t *session = (mysql_session_t *)context;
  if (session == NULL)
    return;
  if (session->phase != MYSQL_PHASE_DONE &&
      session->phase != MYSQL_PHASE_FAILED &&
      !session->cancel_requested) {
    session->cancel_requested = true;
    (void)cnet_close(&session->client, session->connection);
  }
  (void)cnet_client_stop(&session->client, session->timeout_ms);
  (void)cnet_client_destroy(&session->client);
  mysql_session_release_cursor_storage(session);
  free(session);
}

static const mysql_cursor_source_ops_t mysql_session_cursor_source_ops = {
    sizeof(mysql_cursor_source_ops_t),
    MYSQL_CURSOR_SOURCE_OPS_ABI_VERSION,
    mysql_session_cursor_source_next,
    mysql_session_cursor_source_cancel,
    mysql_session_cursor_source_destroy};

mysql_session_status_t mysql_session_open_prepared_source(
    const mysql_session_config_t *config,
    const uint8_t *sql, size_t sql_size,
    const mysql_stmt_value_t *parameters, size_t parameter_count,
    const mysql_session_cursor_limits_t *limits,
    mysql_cursor_source_t *out_source,
    const mysql_column_definition_t **out_columns,
    size_t *out_column_count,
    mysql_session_error_t *error) {
  mysql_session_t *session = NULL;
  mysql_session_status_t status;
  size_t message_capacity;

  if (out_source != NULL)
    memset(out_source, 0, sizeof(*out_source));
  if (out_columns != NULL)
    *out_columns = NULL;
  if (out_column_count != NULL)
    *out_column_count = 0u;
  if (error != NULL)
    memset(error, 0, sizeof(*error));

  if (!mysql_session_config_valid(config) ||
      sql == NULL || sql_size == 0u ||
      (parameters == NULL && parameter_count != 0u) ||
      parameter_count > (size_t)UINT16_MAX ||
      limits == NULL ||
      limits->max_columns == 0u ||
      limits->max_metadata_bytes == 0u ||
      limits->max_row_bytes == 0u ||
      limits->max_command_bytes == 0u ||
      limits->max_command_bytes > MYSQL_WIRE_PACKET_MAX_PAYLOAD ||
      sql_size + 1u > limits->max_command_bytes ||
      out_source == NULL ||
      out_columns == NULL ||
      out_column_count == NULL) {
    if (error != NULL) {
      error->status = MYSQL_SESSION_INVALID;
      (void)snprintf(error->stage, sizeof(error->stage), "%s",
                     "cursor-config");
      (void)snprintf(error->message, sizeof(error->message), "%s",
                     "invalid prepared cursor request or bounds");
    }
    return MYSQL_SESSION_INVALID;
  }

  message_capacity = MYSQL_SESSION_CONTROL_CAPACITY;
  if (limits->max_row_bytes > message_capacity)
    message_capacity = limits->max_row_bytes;
  if (limits->max_metadata_bytes > message_capacity)
    message_capacity = limits->max_metadata_bytes;

  session = (mysql_session_t *)calloc(1u, sizeof(*session));
  if (session == NULL) {
    if (error != NULL) {
      error->status = MYSQL_SESSION_IO;
      (void)snprintf(error->stage, sizeof(error->stage), "%s",
                     "cursor-allocate");
      (void)snprintf(error->message, sizeof(error->message), "%s",
                     "allocate MySQL cursor session");
    }
    return MYSQL_SESSION_IO;
  }

  status = mysql_session_start(
      session, config, MYSQL_SESSION_ACTION_PREPARED_CURSOR,
      NULL, &session->owned_error,
      message_capacity, limits->max_command_bytes);
  if (status != MYSQL_SESSION_OK)
    goto fail;

  session->prepared_sql = sql;
  session->prepared_sql_size = sql_size;
  session->prepared_values = parameters;
  session->prepared_value_count = parameter_count;
  session->cursor_limits = *limits;
  session->cursor_message_capacity = message_capacity;

  if (limits->max_columns > SIZE_MAX / sizeof(*session->cursor_columns)) {
    status = MYSQL_SESSION_INVALID;
    mysql_session_set_error(session, status,
                            "cursor-columns",
                            "cursor column allocation exceeds platform range");
    goto fail;
  }

  session->cursor_columns = (mysql_column_definition_t *)calloc(
      limits->max_columns, sizeof(*session->cursor_columns));
  session->cursor_metadata = (unsigned char *)malloc(
      limits->max_metadata_bytes);
  session->cursor_message = (uint8_t *)malloc(message_capacity);
  if (session->cursor_columns == NULL ||
      session->cursor_metadata == NULL ||
      session->cursor_message == NULL) {
    mysql_session_set_error(session, MYSQL_SESSION_IO,
                            "cursor-storage",
                            "allocate bounded MySQL cursor storage");
    status = MYSQL_SESSION_IO;
    goto fail;
  }

  status = mysql_session_progress_until(
      session, MYSQL_PHASE_CURSOR_READY);
  if (status != MYSQL_SESSION_OK ||
      session->phase != MYSQL_PHASE_CURSOR_READY)
    goto fail;

  session->prepared_sql = NULL;
  session->prepared_sql_size = 0u;
  session->prepared_values = NULL;
  session->prepared_value_count = 0u;
  session->config = NULL;

  out_source->ops = &mysql_session_cursor_source_ops;
  out_source->context = session;
  *out_columns = session->cursor_columns;
  *out_column_count = (size_t)session->result_column_count;
  if (error != NULL)
    memset(error, 0, sizeof(*error));
  return MYSQL_SESSION_OK;

fail:
  if (error != NULL)
    *error = session->owned_error;
  mysql_session_shutdown(session, true);
  mysql_session_release_cursor_storage(session);
  free(session);
  return status != MYSQL_SESSION_OK
             ? status
             : MYSQL_SESSION_PROTOCOL;
}

mysql_session_status_t mysql_session_connect_and_ping(
    const mysql_session_config_t *config, mysql_session_error_t *error) {
  return mysql_session_run(
      config, MYSQL_SESSION_ACTION_PING, NULL, error);
}

mysql_session_status_t mysql_session_prepared_probe(
    const mysql_session_config_t *config,
    mysql_session_prepared_probe_t *out,
    mysql_session_error_t *error) {
  if (out == NULL) {
    if (error != NULL) {
      memset(error, 0, sizeof(*error));
      error->status = MYSQL_SESSION_INVALID;
      (void)snprintf(error->stage, sizeof(error->stage), "%s", "probe");
      (void)snprintf(error->message, sizeof(error->message), "%s",
                     "prepared probe output is required");
    }
    return MYSQL_SESSION_INVALID;
  }
  memset(out, 0, sizeof(*out));
  return mysql_session_run(
      config, MYSQL_SESSION_ACTION_PREPARED_PROBE, out, error);
}

mysql_session_status_t mysql_session_execute_prepared(
    const mysql_session_config_t *config,
    const uint8_t *sql, size_t sql_size,
    const mysql_stmt_value_t *parameters, size_t parameter_count,
    size_t max_command_bytes,
    mysql_session_command_result_t *out,
    mysql_session_error_t *error) {
  mysql_session_t session;
  mysql_session_status_t status;

  if (out != NULL)
    memset(out, 0, sizeof(*out));
  if (error != NULL)
    memset(error, 0, sizeof(*error));

  if (!mysql_session_config_valid(config) ||
      sql == NULL || sql_size == 0u ||
      (parameters == NULL && parameter_count != 0u) ||
      parameter_count > (size_t)UINT16_MAX ||
      max_command_bytes == 0u ||
      max_command_bytes > MYSQL_WIRE_PACKET_MAX_PAYLOAD ||
      sql_size + 1u > max_command_bytes ||
      out == NULL) {
    if (error != NULL) {
      error->status = MYSQL_SESSION_INVALID;
      (void)snprintf(error->stage, sizeof(error->stage), "%s",
                     "command-config");
      (void)snprintf(error->message, sizeof(error->message), "%s",
                     "invalid prepared command request or bounds");
    }
    return MYSQL_SESSION_INVALID;
  }

  status = mysql_session_start(
      &session, config, MYSQL_SESSION_ACTION_PREPARED_COMMAND,
      NULL, error, MYSQL_SESSION_CONTROL_CAPACITY,
      max_command_bytes);
  if (status != MYSQL_SESSION_OK)
    return status;

  session.prepared_sql = sql;
  session.prepared_sql_size = sql_size;
  session.prepared_values = parameters;
  session.prepared_value_count = parameter_count;
  session.command_result = out;

  status = mysql_session_progress_until(
      &session, MYSQL_PHASE_DONE);
  mysql_session_shutdown(&session, session.phase == MYSQL_PHASE_DONE);
  return status;
}


static void mysql_transaction_copy_error(
    mysql_transaction_session_t *transaction,
    mysql_session_error_t *error) {
  if (error == NULL)
    return;
  if (transaction == NULL) {
    memset(error, 0, sizeof(*error));
    error->status = MYSQL_SESSION_INVALID;
    return;
  }
  *error = transaction->session.owned_error;
}

mysql_session_status_t mysql_transaction_session_begin(
    const mysql_session_config_t *config,
    orm_isolation_t isolation,
    size_t max_command_bytes,
    mysql_transaction_session_t **out_transaction,
    mysql_session_error_t *error) {
  mysql_transaction_session_t *transaction = NULL;
  const uint8_t *isolation_sql = NULL;
  size_t isolation_sql_size = 0u;
  mysql_session_status_t status;

  if (out_transaction != NULL)
    *out_transaction = NULL;
  if (error != NULL)
    memset(error, 0, sizeof(*error));

  if (out_transaction == NULL ||
      max_command_bytes == 0u ||
      max_command_bytes > MYSQL_WIRE_PACKET_MAX_PAYLOAD ||
      mysql_session_isolation_sql(
          isolation, &isolation_sql, &isolation_sql_size) !=
          MYSQL_SESSION_OK ||
      isolation_sql_size + 1u > max_command_bytes) {
    if (error != NULL) {
      error->status = MYSQL_SESSION_INVALID;
      (void)snprintf(error->stage, sizeof(error->stage), "%s",
                     "transaction-config");
      (void)snprintf(error->message, sizeof(error->message), "%s",
                     isolation == ORM_ISOLATION_SNAPSHOT
                         ? "MySQL does not expose ORM snapshot isolation"
                         : "invalid transaction request or command bound");
    }
    return MYSQL_SESSION_INVALID;
  }

  transaction = (mysql_transaction_session_t *)calloc(
      1u, sizeof(*transaction));
  if (transaction == NULL) {
    if (error != NULL) {
      error->status = MYSQL_SESSION_IO;
      (void)snprintf(error->stage, sizeof(error->stage), "%s",
                     "transaction-allocate");
      (void)snprintf(error->message, sizeof(error->message), "%s",
                     "allocate MySQL transaction session");
    }
    return MYSQL_SESSION_IO;
  }

  transaction->max_command_bytes = max_command_bytes;
  status = mysql_session_start(
      &transaction->session, config,
      MYSQL_SESSION_ACTION_TRANSACTION,
      NULL, &transaction->session.owned_error,
      MYSQL_SESSION_CONTROL_CAPACITY,
      max_command_bytes);
  if (status != MYSQL_SESSION_OK) {
    mysql_transaction_copy_error(transaction, error);
    free(transaction);
    return status;
  }

  transaction->session.control_sql = isolation_sql;
  transaction->session.control_sql_size = isolation_sql_size;
  transaction->session.transaction_step =
      MYSQL_TRANSACTION_STEP_SET_ISOLATION;

  status = mysql_session_progress_until(
      &transaction->session, MYSQL_PHASE_TRANSACTION_READY);
  if (status != MYSQL_SESSION_OK ||
      transaction->session.phase != MYSQL_PHASE_TRANSACTION_READY ||
      !transaction->session.transaction_active)
    goto fail;

  transaction->session.config = NULL;
  transaction->session.control_sql = NULL;
  transaction->session.control_sql_size = 0u;
  transaction->session.error = &transaction->session.owned_error;
  if (error != NULL)
    memset(error, 0, sizeof(*error));
  *out_transaction = transaction;
  return MYSQL_SESSION_OK;

fail:
  mysql_transaction_copy_error(transaction, error);
  if (transaction != NULL) {
    mysql_session_shutdown(&transaction->session, true);
    free(transaction);
  }
  return status != MYSQL_SESSION_OK
             ? status
             : MYSQL_SESSION_PROTOCOL;
}

mysql_session_status_t mysql_transaction_session_execute_prepared(
    mysql_transaction_session_t *transaction,
    const uint8_t *sql, size_t sql_size,
    const mysql_stmt_value_t *parameters, size_t parameter_count,
    mysql_session_command_result_t *out,
    mysql_session_error_t *error) {
  mysql_session_status_t status;

  if (out != NULL)
    memset(out, 0, sizeof(*out));
  if (error != NULL)
    memset(error, 0, sizeof(*error));

  if (transaction == NULL ||
      !transaction->session.transaction_active ||
      transaction->session.phase != MYSQL_PHASE_TRANSACTION_READY ||
      sql == NULL || sql_size == 0u ||
      sql_size + 1u > transaction->max_command_bytes ||
      (parameters == NULL && parameter_count != 0u) ||
      parameter_count > (size_t)UINT16_MAX ||
      out == NULL) {
    if (error != NULL) {
      error->status = MYSQL_SESSION_INVALID;
      (void)snprintf(error->stage, sizeof(error->stage), "%s",
                     "transaction-command");
      (void)snprintf(error->message, sizeof(error->message), "%s",
                     "invalid active MySQL transaction command");
    }
    return MYSQL_SESSION_INVALID;
  }

  memset(&transaction->session.owned_error, 0,
         sizeof(transaction->session.owned_error));
  transaction->session.error = &transaction->session.owned_error;
  transaction->session.transaction_step =
      MYSQL_TRANSACTION_STEP_COMMAND;
  transaction->session.prepared_sql = sql;
  transaction->session.prepared_sql_size = sql_size;
  transaction->session.prepared_values = parameters;
  transaction->session.prepared_value_count = parameter_count;
  transaction->session.command_result = out;

  mysql_session_send_prepare(&transaction->session);
  status = mysql_session_progress_until(
      &transaction->session, MYSQL_PHASE_TRANSACTION_READY);

  transaction->session.prepared_sql = NULL;
  transaction->session.prepared_sql_size = 0u;
  transaction->session.prepared_values = NULL;
  transaction->session.prepared_value_count = 0u;
  transaction->session.command_result = NULL;

  if (status != MYSQL_SESSION_OK ||
      transaction->session.phase != MYSQL_PHASE_TRANSACTION_READY) {
    mysql_transaction_copy_error(transaction, error);
    return status != MYSQL_SESSION_OK
               ? status
               : MYSQL_SESSION_PROTOCOL;
  }

  if (error != NULL)
    memset(error, 0, sizeof(*error));
  return MYSQL_SESSION_OK;
}

static mysql_session_status_t mysql_transaction_finish(
    mysql_transaction_session_t *transaction,
    mysql_transaction_step_t step,
    const uint8_t *sql, size_t sql_size,
    mysql_session_error_t *error) {
  mysql_session_status_t status;

  if (error != NULL)
    memset(error, 0, sizeof(*error));
  if (transaction == NULL ||
      !transaction->session.transaction_active ||
      transaction->session.phase != MYSQL_PHASE_TRANSACTION_READY ||
      sql == NULL || sql_size == 0u) {
    if (error != NULL) {
      error->status = MYSQL_SESSION_INVALID;
      (void)snprintf(error->stage, sizeof(error->stage), "%s",
                     "transaction-finish");
      (void)snprintf(error->message, sizeof(error->message), "%s",
                     "MySQL transaction is not active");
    }
    return MYSQL_SESSION_INVALID;
  }

  memset(&transaction->session.owned_error, 0,
         sizeof(transaction->session.owned_error));
  transaction->session.error = &transaction->session.owned_error;
  transaction->session.transaction_step = step;
  mysql_session_send_control(
      &transaction->session, sql, sql_size);
  status = mysql_session_progress_until(
      &transaction->session, MYSQL_PHASE_DONE);
  if (status != MYSQL_SESSION_OK ||
      transaction->session.phase != MYSQL_PHASE_DONE) {
    mysql_transaction_copy_error(transaction, error);
    return status != MYSQL_SESSION_OK
               ? status
               : MYSQL_SESSION_PROTOCOL;
  }

  if (error != NULL)
    memset(error, 0, sizeof(*error));
  return MYSQL_SESSION_OK;
}

static mysql_session_status_t mysql_transaction_savepoint_control(
    mysql_transaction_session_t *transaction,
    mysql_transaction_step_t step,
    mysql_savepoint_control_t control,
    const uint8_t *name, size_t name_size,
    mysql_session_error_t *error) {
  uint8_t sql[96];
  size_t sql_size = 0u;
  mysql_wire_status_t wire_status;
  mysql_session_status_t status;

  if (error != NULL)
    memset(error, 0, sizeof(*error));
  if (transaction == NULL ||
      !transaction->session.transaction_active ||
      transaction->session.phase != MYSQL_PHASE_TRANSACTION_READY) {
    if (error != NULL) {
      error->status = MYSQL_SESSION_INVALID;
      (void)snprintf(error->stage, sizeof(error->stage), "%s",
                     "transaction-savepoint");
      (void)snprintf(error->message, sizeof(error->message), "%s",
                     "MySQL transaction is not active");
    }
    return MYSQL_SESSION_INVALID;
  }

  wire_status = mysql_transaction_build_savepoint_control(
      control, name, name_size, sql, sizeof(sql), &sql_size);
  if (wire_status != MYSQL_WIRE_STATUS_OK ||
      sql_size > transaction->max_command_bytes) {
    if (error != NULL) {
      error->status = MYSQL_SESSION_INVALID;
      (void)snprintf(error->stage, sizeof(error->stage), "%s",
                     "transaction-savepoint");
      (void)snprintf(error->message, sizeof(error->message), "%s",
                     wire_status == MYSQL_WIRE_STATUS_LIMIT ||
                             sql_size > transaction->max_command_bytes
                         ? "MySQL savepoint control exceeds configured bound"
                         : "invalid MySQL savepoint identifier");
    }
    return MYSQL_SESSION_INVALID;
  }

  memset(&transaction->session.owned_error, 0,
         sizeof(transaction->session.owned_error));
  transaction->session.error = &transaction->session.owned_error;
  transaction->session.transaction_step = step;
  mysql_session_send_control(
      &transaction->session, sql, sql_size);
  status = mysql_session_progress_until(
      &transaction->session, MYSQL_PHASE_TRANSACTION_READY);
  if (status != MYSQL_SESSION_OK ||
      transaction->session.phase != MYSQL_PHASE_TRANSACTION_READY) {
    mysql_transaction_copy_error(transaction, error);
    return status != MYSQL_SESSION_OK
               ? status
               : MYSQL_SESSION_PROTOCOL;
  }

  if (error != NULL)
    memset(error, 0, sizeof(*error));
  return MYSQL_SESSION_OK;
}

mysql_session_status_t mysql_transaction_session_savepoint(
    mysql_transaction_session_t *transaction,
    const uint8_t *name, size_t name_size,
    mysql_session_error_t *error) {
  return mysql_transaction_savepoint_control(
      transaction, MYSQL_TRANSACTION_STEP_SAVEPOINT,
      MYSQL_SAVEPOINT_CREATE, name, name_size, error);
}

mysql_session_status_t mysql_transaction_session_rollback_to_savepoint(
    mysql_transaction_session_t *transaction,
    const uint8_t *name, size_t name_size,
    mysql_session_error_t *error) {
  return mysql_transaction_savepoint_control(
      transaction, MYSQL_TRANSACTION_STEP_ROLLBACK_TO_SAVEPOINT,
      MYSQL_SAVEPOINT_ROLLBACK_TO, name, name_size, error);
}

mysql_session_status_t mysql_transaction_session_release_savepoint(
    mysql_transaction_session_t *transaction,
    const uint8_t *name, size_t name_size,
    mysql_session_error_t *error) {
  return mysql_transaction_savepoint_control(
      transaction, MYSQL_TRANSACTION_STEP_RELEASE_SAVEPOINT,
      MYSQL_SAVEPOINT_RELEASE, name, name_size, error);
}

mysql_session_status_t mysql_transaction_session_commit(
    mysql_transaction_session_t *transaction,
    mysql_session_error_t *error) {
  return mysql_transaction_finish(
      transaction, MYSQL_TRANSACTION_STEP_COMMIT,
      MYSQL_TXN_COMMIT, sizeof(MYSQL_TXN_COMMIT) - 1u, error);
}

mysql_session_status_t mysql_transaction_session_rollback(
    mysql_transaction_session_t *transaction,
    mysql_session_error_t *error) {
  return mysql_transaction_finish(
      transaction, MYSQL_TRANSACTION_STEP_ROLLBACK,
      MYSQL_TXN_ROLLBACK, sizeof(MYSQL_TXN_ROLLBACK) - 1u, error);
}

void mysql_transaction_session_destroy(
    mysql_transaction_session_t *transaction) {
  if (transaction == NULL)
    return;

  if (transaction->session.transaction_active &&
      transaction->session.phase == MYSQL_PHASE_TRANSACTION_READY) {
    mysql_session_error_t ignored;
    (void)mysql_transaction_session_rollback(
        transaction, &ignored);
  }

  mysql_session_shutdown(
      &transaction->session,
      transaction->session.phase != MYSQL_PHASE_FAILED);
  free(transaction);
}

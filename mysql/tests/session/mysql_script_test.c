#include <cnet/cnet.h>
#include <cmeta_buffer.h>
#include <tinytest.h>

typedef struct mysql_script_fake_state {
  int receive_result;
  int send_result;
  int init_result;
  size_t receive_calls;
  size_t send_calls;
  size_t init_calls;
  size_t receive_demand;
  cnet_client *receive_client;
  cnet_client *send_client;
} mysql_script_fake_state;

static mysql_script_fake_state script_fake;

static int test_receive(cnet_client *client, cnet_connection connection, size_t demand) {
  (void)connection;
  ++script_fake.receive_calls;
  script_fake.receive_client = client;
  script_fake.receive_demand = demand;
  return script_fake.receive_result;
}

static int script_init(cnet_client *client, const cnet_client_config *config) {
  (void)client;
  (void)config;
  ++script_fake.init_calls;
  return script_fake.init_result;
}

enum { SCRIPT_CAPTURE_CAPACITY = 256u };
static uint8_t sent_packet[SCRIPT_CAPTURE_CAPACITY];
static size_t sent_size;
static int test_send(cnet_client *client, cnet_connection connection, mem_buffer_t *buffer) {
  (void)connection;
  ++script_fake.send_calls;
  script_fake.send_client = client;
  sent_size = mem_buffer_used(buffer);
  if (sent_size <= sizeof(sent_packet)) memcpy(sent_packet, mem_buffer_data(buffer), sent_size);
  return script_fake.send_result;
}

#define cnet_send_buffer test_send
#define cnet_receive test_receive
#define cnet_client_init script_init
#include "../../session.c"
#undef cnet_send_buffer
#undef cnet_receive
#undef cnet_client_init

spec("MySQL native schema script protocol") {
  (void)ttest_config__;
  static mysql_session_t session;
  static mysql_session_error_t error;
  static mysql_session_config_t config;
  before_each() {
    memset(&session, 0, sizeof(session));
    memset(&error, 0, sizeof(error));
    config = (mysql_session_config_t){
        .host = "test.invalid", .port = 3306u, .username = "unit",
        .password = "fixture", .database = "unit", .ca_file = "fixture-ca.pem",
        .server_name = "test.invalid", .timeout_ms = 25u};
    sent_size = 0u;
    session.config = &config;
    session.error = &error;
    session.action = MYSQL_SESSION_ACTION_SCRIPT;
    session.phase = MYSQL_PHASE_WAIT_SCRIPT_REPLY;
    session.client_capabilities = MYSQL_WIRE_CLIENT_PROTOCOL_41;
    check_equal(mysql_wire_packet_stream_init(&session.stream, 1u,
                    MYSQL_SESSION_CONTROL_CAPACITY), MYSQL_WIRE_STATUS_OK);
    script_fake = (mysql_script_fake_state){
        .receive_result = SALTS_OK,
        .send_result = SALTS_OK,
        .init_result = SALTS_ENOMEM};
  }

  it("sends the complete script as one COM_QUERY and enters multi-result receive") {
    const uint8_t sql[] = "CREATE TABLE a(s TEXT); INSERT INTO a VALUES('a;b');";
    session.control_sql = sql;
    session.control_sql_size = sizeof(sql) - 1u;
    mysql_session_begin_action(&session);
    check_equal(session.phase, MYSQL_PHASE_WAIT_QUERY_SEND);
    check_equal(sent_size, sizeof(sql) + MYSQL_WIRE_PACKET_HEADER_SIZE);
    check_equal(sent_packet[3], (uint8_t)0u);
    check_equal(sent_packet[4], MYSQL_COM_QUERY);
    check_equal(memcmp(sent_packet + 5u, sql, sizeof(sql) - 1u), 0);
    mysql_session_on_send(&session, session.connection, sent_size);
    check_equal(session.phase, MYSQL_PHASE_WAIT_SCRIPT_REPLY);
    check_equal(script_fake.send_calls, (size_t)1u);
    check_equal(script_fake.receive_calls, (size_t)1u);
  }

  it("negotiates multi-statements only for script sessions before TLS") {
    uint8_t greeting[] = {
      0x0a,'8','.','0',0, 1,0,0,0, '1','2','3','4','5','6','7','8',0,
      0x09,0x8a, 0x2d,2,0, 0x0b,0x01, 0x15, 0,0,0,0,0,0,0,0,0,0,
      '9','0','1','2','3','4','5','6','7','8','9','0',0,
      'c','a','c','h','i','n','g','_','s','h','a','2','_','p','a','s','s','w','o','r','d',0};
    mysql_session_handle_greeting(&session, greeting, sizeof(greeting), 0u);
    check_equal(session.phase, MYSQL_PHASE_WAIT_SSL_SEND);
    check_true((session.client_capabilities & MYSQL_WIRE_CLIENT_MULTI_STATEMENTS) != 0u);
    check_true((session.client_capabilities & MYSQL_WIRE_CLIENT_SSL) != 0u);
    session.action = MYSQL_SESSION_ACTION_PING;
    mysql_session_handle_greeting(&session, greeting, sizeof(greeting), 0u);
    check_equal(session.client_capabilities & MYSQL_WIRE_CLIENT_MULTI_STATEMENTS, (uint32_t)0u);
    session.action = MYSQL_SESSION_ACTION_SCRIPT;
    greeting[23] = 0x08; /* Remove both multi-statement capability bits. */
    mysql_session_handle_greeting(&session, greeting, sizeof(greeting), 0u);
    check_equal(error.status, MYSQL_SESSION_UNSUPPORTED);
    check_equal(script_fake.send_calls, (size_t)2u);
  }

  it("counts multiple OK packets arriving together without finishing early") {
    const uint8_t packets[] = {
      7,0,0,1, 0,0,0,8,0,0,0,
      7,0,0,2, 0,0,0,0,0,0,0};
    const cnet_receive_view view = {packets, sizeof(packets), CNET_MESSAGE_BYTES};
    mysql_session_on_receive(&session, session.connection, &view);
    check_equal(session.phase, MYSQL_PHASE_DONE);
    check_equal(session.script_statements, (uint64_t)2u);
    check_equal(script_fake.receive_calls, (size_t)0u);
  }

  it("waits for a fragmented final result after the first statement") {
    const uint8_t packets[] = {
      7,0,0,1, 0,0,0,8,0,0,0,
      7,0,0,2, 0,0,0,0,0,0,0};
    const size_t split = 14u;
    cnet_receive_view view = {packets, split, CNET_MESSAGE_BYTES};
    mysql_session_on_receive(&session, session.connection, &view);
    check_equal(session.phase, MYSQL_PHASE_WAIT_SCRIPT_REPLY);
    check_equal(session.script_statements, (uint64_t)1u);
    view.data = packets + split;
    view.size = sizeof(packets) - split;
    mysql_session_on_receive(&session, session.connection, &view);
    check_equal(session.phase, MYSQL_PHASE_DONE);
    check_equal(session.script_statements, (uint64_t)2u);
    check_equal(script_fake.receive_calls, (size_t)1u);
  }

  it("preserves a later SQL error without exposing the server text") {
    const uint8_t ok[] = {0,0,0,8,0,0,0};
    const uint8_t failure[] = {0xff,0x28,0x04,'#','4','2','0','0','0','s','e','c','r','e','t'};
    mysql_session_handle_script_reply(&session, ok, sizeof(ok));
    mysql_session_handle_script_reply(&session, failure, sizeof(failure));
    check_equal(session.phase, MYSQL_PHASE_FAILED);
    check_equal(error.status, MYSQL_SESSION_SQL_ERROR);
    check_equal(error.server_error, (uint16_t)1064u);
    check_equal(error.sql_state, "42000");
    check_null(strstr(error.message, "secret"));
  }

  it("classifies valid prepared operation errors as SQL errors") {
    const uint8_t failure[] = {
        0xff, 0x28, 0x04, '#', '4', '2', '0', '0', '0',
        's', 'e', 'c', 'r', 'e', 't'};
    check_true(mysql_session_decode_server_error(
        &session, failure, sizeof(failure), "prepare-server"));
    check_equal(session.phase, MYSQL_PHASE_FAILED);
    check_equal(error.status, MYSQL_SESSION_SQL_ERROR);
    check_equal(error.server_error, (uint16_t)1064u);
    check_equal(error.sql_state, "42000");
    check_equal(error.stage, "prepare-server");
    check_null(strstr(error.message, "secret"));
  }

  it("rejects malformed prepared operation errors as protocol failures") {
    const uint8_t failure[] = {0xff, 0x28, 0x04, '#', '4', '2'};
    check_true(mysql_session_decode_server_error(
        &session, failure, sizeof(failure), "command-server"));
    check_equal(session.phase, MYSQL_PHASE_FAILED);
    check_equal(error.status, MYSQL_SESSION_PROTOCOL);
    check_equal(error.server_error, (uint16_t)0u);
    check_equal(error.stage, "command-server");
  }

  it("rejects row results and local file requests") {
    const uint8_t rows[] = {1u};
    const uint8_t local_file[] = {0xfb,'f'};
    mysql_session_handle_script_reply(&session, rows, sizeof(rows));
    check_equal(error.status, MYSQL_SESSION_UNSUPPORTED);
    session.phase = MYSQL_PHASE_WAIT_SCRIPT_REPLY;
    mysql_session_handle_script_reply(&session, local_file, sizeof(local_file));
    check_equal(error.status, MYSQL_SESSION_UNSUPPORTED);
  }

  it("rejects malformed OK packets and wrong packet sequence") {
    const uint8_t truncated[] = {0u};
    mysql_session_handle_script_reply(&session, truncated, sizeof(truncated));
    check_equal(error.status, MYSQL_SESSION_PROTOCOL);
    session.phase = MYSQL_PHASE_WAIT_SCRIPT_REPLY;
    const uint8_t packet[] = {7,0,0,2, 0,0,0,0,0,0,0};
    const cnet_receive_view view = {packet, sizeof(packet), CNET_MESSAGE_BYTES};
    mysql_session_on_receive(&session, session.connection, &view);
    check_equal(error.status, MYSQL_SESSION_PROTOCOL);
    check_equal(error.stage, "packet");
  }

  it("rejects oversized and NUL-containing scripts before transport allocation") {
    const uint8_t sql[] = {'a',0,'b'};
    uint64_t statements = 9u;
    check_equal(mysql_session_execute_script(&config, sql, SIZE_MAX, SIZE_MAX,
                    &statements, &error), MYSQL_SESSION_INVALID);
    check_equal(statements, (uint64_t)0u);
    check_equal(mysql_session_execute_script(&config, sql, sizeof(sql), sizeof(sql),
                    &statements, &error), MYSQL_SESSION_INVALID);
    check_equal(mysql_session_execute_script(&config, sql, 1u, 0u,
                    &statements, &error), MYSQL_SESSION_INVALID);
    config.ca_file = "";
    check_equal(mysql_session_execute_script(&config, sql, 1u, 1u,
                    &statements, &error), MYSQL_SESSION_INVALID);
    check_equal(script_fake.init_calls, (size_t)0u);
  }

  it("stops after send admission fails without requesting a reply") {
    const uint8_t sql[] = "create table alpha(id int);";
    session.control_sql = sql;
    session.control_sql_size = sizeof(sql) - 1u;
    script_fake.send_result = SALTS_ENOBUFS;
    mysql_session_begin_action(&session);
    check_equal(session.phase, MYSQL_PHASE_FAILED);
    check_equal(error.status, MYSQL_SESSION_IO);
    check_equal(error.cnet_status, SALTS_ENOBUFS);
    check_equal(error.stage, "send");
    check_equal(script_fake.send_calls, (size_t)1u);
    check_true(script_fake.send_client == &session.client);
    check_equal(script_fake.receive_calls, (size_t)0u);
  }

  it("reports receive admission failure after the script was sent") {
    const uint8_t sql[] = "create table alpha(id int);";
    session.control_sql = sql;
    session.control_sql_size = sizeof(sql) - 1u;
    script_fake.receive_result = SALTS_ENOBUFS;
    mysql_session_begin_action(&session);
    mysql_session_on_send(&session, session.connection, sent_size);
    check_equal(session.phase, MYSQL_PHASE_FAILED);
    check_equal(error.status, MYSQL_SESSION_IO);
    check_equal(error.cnet_status, SALTS_ENOBUFS);
    check_equal(error.stage, "receive");
    check_equal(session.script_statements, (uint64_t)0u);
    check_equal(script_fake.send_calls, (size_t)1u);
    check_equal(script_fake.receive_calls, (size_t)1u);
    check_true(script_fake.receive_client == &session.client);
    check_equal(script_fake.receive_demand, (size_t)1u);
  }

  it("fails when the connection closes with another statement result pending") {
    const uint8_t ok[] = {0,0,0,8,0,0,0};
    mysql_session_handle_script_reply(&session, ok, sizeof(ok));
    mysql_session_on_state(&session, session.connection, CNET_CONNECTION_CLOSED, NULL);
    check_equal(session.phase, MYSQL_PHASE_FAILED);
    check_equal(error.status, MYSQL_SESSION_IO);
    check_equal(error.stage, "closed");
    check_equal(session.script_statements, (uint64_t)1u);
    check_equal(script_fake.send_calls, (size_t)0u);
    check_equal(script_fake.receive_calls, (size_t)0u);
  }

  it("keeps a final successful result when the peer closes afterward") {
    const uint8_t ok[] = {0,0,0,0,0,0,0};
    mysql_session_handle_script_reply(&session, ok, sizeof(ok));
    mysql_session_on_state(&session, session.connection, CNET_CONNECTION_CLOSED, NULL);
    check_equal(session.phase, MYSQL_PHASE_DONE);
    check_equal(error.status, MYSQL_SESSION_OK);
    check_equal(session.script_statements, (uint64_t)1u);
    check_equal(script_fake.receive_calls, (size_t)0u);
  }

  it("rejects a truncated server error without publishing SQL diagnostics") {
    const uint8_t failure[] = {0xff,0x28,0x04,'#','4','2'};
    mysql_session_handle_script_reply(&session, failure, sizeof(failure));
    check_equal(session.phase, MYSQL_PHASE_FAILED);
    check_equal(error.status, MYSQL_SESSION_PROTOCOL);
    check_equal(error.stage, "script-result");
    check_equal(error.server_error, (uint16_t)0u);
    check_equal(session.script_statements, (uint64_t)0u);
    check_equal(script_fake.receive_calls, (size_t)0u);
  }

  it("returns transport failure even when optional diagnostic storage is absent") {
    const uint8_t sql[] = "create table alpha(id int);";
    uint64_t statements = UINT64_MAX;
    check_equal(mysql_session_execute_script(&config, sql, sizeof(sql) - 1u,
                    sizeof(sql), &statements, NULL), MYSQL_SESSION_IO);
    check_equal(statements, (uint64_t)0u);
    check_equal(script_fake.init_calls, (size_t)1u);
  }

  it("propagates transport allocation failure without publishing a statement count") {
    const uint8_t sql[] = "create table alpha(id int);";
    uint64_t statements = 9u;
    check_equal(mysql_session_execute_script(&config, sql, sizeof(sql) - 1u,
                    sizeof(sql), &statements, &error), MYSQL_SESSION_IO);
    check_equal(statements, (uint64_t)0u);
    check_equal(error.cnet_status, SALTS_ENOMEM);
    check_equal(script_fake.init_calls, (size_t)1u);
  }
}

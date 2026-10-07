#include "password.h"
#include <cnet/cnet.h>
#include <salts/clock.h>
#include <gmssl/mem.h>
#include <tinytest.h>
#include <stdio.h>
#include <string.h>

enum { TEST_BYTES=4096,TEST_DEADLINE_MS=10000,TEST_IO_TIMEOUT_MS=2000,
       TEST_STALLED_TLS_MS=100,TEST_MAX_PACKET=16777216,TEST_QUEUE=16,
       TEST_REQUESTS=8,TEST_VERSION_BYTES=32 };
typedef struct tls_peer {
  cnet_client *owner;
  cnet_connection connection;
  uint8_t bytes[TEST_BYTES];
  size_t size,expected,sent;
  unsigned connected,handshaking;
  bool terminal,failed;
  int status;
} tls_peer;
static cnet_client client,server;
static cnet_listener listener;
static cnet_tls_server tls_server;
static tls_peer client_peer,server_peer;
static tdsql_mysql_negotiation gate;

static void state(void *user,cnet_connection connection,cnet_connection_state next,const cnet_error *error) {
  tls_peer *peer=user; peer->connection=connection;
  if(next==CNET_CONNECTION_CONNECTED) ++peer->connected;
  if(next==CNET_CONNECTION_TLS_HANDSHAKING) ++peer->handshaking;
  if(next==CNET_CONNECTION_CLOSED || next==CNET_CONNECTION_FAILED) {
    peer->terminal=true; peer->failed=next==CNET_CONNECTION_FAILED;
    if(error) { peer->failed=true; peer->status=error->status; }
  }
}
static void receive(void *user,cnet_connection connection,const cnet_receive_view *view) {
  tls_peer *peer=user;
  if(!view || view->kind!=CNET_MESSAGE_BYTES || !view->size || peer->size>peer->expected ||
      view->size>peer->expected-peer->size || view->size>sizeof(peer->bytes)-peer->size) {
    peer->failed=true; peer->status=SALTS_EINVAL; return;
  }
  memcpy(peer->bytes+peer->size,view->data,view->size); peer->size+=view->size;
  if(peer->size<peer->expected) {
    int status=cnet_receive(peer->owner,connection,1);
    if(status!=SALTS_OK) { peer->failed=true; peer->status=status; }
  }
}
static void send_done(void *user,cnet_connection connection,size_t size) {
  (void)connection; tls_peer *peer=user; peer->sent+=size;
}
static cnet_observer observer(tls_peer *peer) {
  cnet_observer out={.on_state=state,.on_receive=receive,.on_send=send_done,.user=peer}; return out;
}
static cnet_client_config configuration(void) {
  cnet_client_config config={.backend=
#if defined(_WIN32)
    NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
    NATIVE_IO_BACKEND_EPOLL,
#else
    NATIVE_IO_BACKEND_KQUEUE,
#endif
    .connection_capacity=2,.command_capacity=TEST_QUEUE,.request_capacity=TEST_REQUESTS,
    .completion_batch_capacity=TEST_REQUESTS,.event_capacity=TEST_QUEUE,
    .max_send_bytes=TEST_BYTES,.receive_buffer_bytes=TEST_BYTES,
    .connect_timeout_ms=TEST_IO_TIMEOUT_MS,.read_timeout_ms=TEST_IO_TIMEOUT_MS,
    .write_timeout_ms=TEST_IO_TIMEOUT_MS,.tls_io_buffer_bytes=CNET_TLS_MIN_IO_BUFFER_BYTES,
    .tls_handshake_timeout_ms=TEST_IO_TIMEOUT_MS}; return config;
}
static void poll_pair(void) {
  size_t events=0;
  check_equal(cnet_client_poll(&client,1,&events),SALTS_OK);
  check_equal(cnet_client_poll(&server,1,&events),SALTS_OK);
}
static void connect_pair(uint32_t timeout) {
  cnet_client_config config=configuration(); config.tls_handshake_timeout_ms=timeout;
  cnet_tls_server_config policy={.size=sizeof(policy),.cert_file=TEST_TLS_CERT,.key_file=TEST_TLS_KEY};
  check_equal(cnet_tls_server_init(&tls_server,&policy),SALTS_OK);
  check_equal(cnet_client_init(&client,&config),SALTS_OK);
  check_equal(cnet_client_init(&server,&config),SALTS_OK);
  cnet_listener_config listen={.backend=config.backend,.host="127.0.0.1",.port=0,.backlog=2};
  check_equal(cnet_listener_init(&listener,&listen),SALTS_OK);
  uint16_t port=0; check_equal(cnet_listener_port(&listener,&port),SALTS_OK);
  char uri[TEST_VERSION_BYTES*2];
  check_greater(snprintf(uri,sizeof(uri),"tcp://127.0.0.1:%u",(unsigned)port),0);
  cnet_connect_options options={.uri=uri,.observer=observer(&client_peer)};
  check_equal(cnet_connect(&client,&options,&client_peer.connection),SALTS_OK);
  uint64_t deadline=cmeta_monotonic_ms()+TEST_DEADLINE_MS; bool accepted=false;
  while((!client_peer.connected || !server_peer.connected) && cmeta_monotonic_ms()<deadline) {
    if(!accepted) {
      int ready=0; check_equal(cnet_listener_wait(&listener,0,&ready),SALTS_OK);
      if(ready) {
        cnet_observer server_observer=observer(&server_peer);
        check_equal(cnet_listener_accept(&listener,&server,&server_observer,&server_peer.connection),SALTS_OK);
        accepted=true;
      }
    }
    poll_pair();
  }
  check_true(accepted); check_equal(client_peer.connected,1u); check_equal(server_peer.connected,1u);
}
static void exchange(tls_peer *sender,tls_peer *receiver,const uint8_t *bytes,size_t size) {
  check_less_equal(size,sizeof(receiver->bytes)); receiver->size=0; receiver->expected=size;
  size_t expected_send=sender->sent+size;
  check_equal(cnet_receive(receiver->owner,receiver->connection,1),SALTS_OK);
  mem_buffer_t *buffer=mem_get_buffer(mem_global(),size); check_not_null(buffer);
  memcpy(mem_buffer_data(buffer),bytes,size); mem_set_used(buffer,size);
  int status=cnet_send_buffer(sender->owner,sender->connection,buffer);
  mem_buffer_release(buffer); check_equal(status,SALTS_OK);
  uint64_t deadline=cmeta_monotonic_ms()+TEST_DEADLINE_MS;
  while((receiver->size<size || sender->sent<expected_send) && !receiver->terminal && !sender->terminal &&
      !receiver->failed && !sender->failed && cmeta_monotonic_ms()<deadline) poll_pair();
  check_false(receiver->failed); check_false(sender->failed);
  check_equal(receiver->size,size); check_equal(sender->sent,expected_send);
  check_equal(receiver->bytes,bytes,size);
}
static void ssl_request(void) {
  uint8_t bytes[TDSQL_MYSQL_SSL_BYTES]; size_t size=0;
  uint32_t capabilities=tdsql_mysql_server_capabilities(false);
  check_equal(tdsql_mysql_negotiation_init(&gate,capabilities),TDSQL_MYSQL_OK);
  check_equal(mysql_wire_build_ssl_request(capabilities,TEST_MAX_PACKET,TDSQL_MYSQL_BINARY_CHARSET,
    bytes,sizeof(bytes),&size),MYSQL_WIRE_STATUS_OK);
  exchange(&client_peer,&server_peer,bytes,size);
  tdsql_mysql_login login={0};
  check_equal(tdsql_mysql_negotiation_accept(&gate,server_peer.bytes,server_peer.size,1,false,&login),TDSQL_MYSQL_OK);
  check_equal(gate.phase,TDSQL_MYSQL_WAIT_TLS);
}
static void upgrade(const char *identity,const char *ca) {
  check_equal(cnet_start_tls_server(&server,server_peer.connection,&tls_server),SALTS_OK);
  cnet_tls_client_config policy={.size=sizeof(policy),.ca_file=ca,.server_name=identity};
  cnet_start_tls_options options=CNET_START_TLS_OPTIONS_INIT; options.tls=&policy;
  check_equal(cnet_start_tls(&client,client_peer.connection,&options),SALTS_OK);
  uint64_t deadline=cmeta_monotonic_ms()+TEST_DEADLINE_MS;
  while((client_peer.connected<2 || server_peer.connected<2) && !client_peer.terminal && !server_peer.terminal &&
      cmeta_monotonic_ms()<deadline) poll_pair();
}
spec("MySQL frontend real CNet TLS transport") {
  before_each() {
    client=(cnet_client){0}; server=(cnet_client){0}; listener=(cnet_listener){0}; tls_server=(cnet_tls_server){0};
    client_peer=(tls_peer){.owner=&client}; server_peer=(tls_peer){.owner=&server}; gate=(tdsql_mysql_negotiation){0};
  }
  after_each() {
    if(listener.impl) { check_equal(cnet_listener_close(&listener),SALTS_OK); check_equal(cnet_listener_destroy(&listener),SALTS_OK); }
    if(client.impl) { check_equal(cnet_client_stop(&client,TEST_IO_TIMEOUT_MS),SALTS_OK); check_equal(cnet_client_destroy(&client),SALTS_OK); }
    if(server.impl) { check_equal(cnet_client_stop(&server,TEST_IO_TIMEOUT_MS),SALTS_OK); check_equal(cnet_client_destroy(&server),SALTS_OK); }
    if(tls_server.impl) check_equal(cnet_tls_server_destroy(&tls_server),SALTS_OK);
    gmssl_secure_clear(client_peer.bytes,sizeof(client_peer.bytes)); gmssl_secure_clear(server_peer.bytes,sizeof(server_peer.bytes));
  }
  it("upgrades SSLRequest in place and verifies full credentials inside certificate-verified TLS") {
    connect_pair(TEST_IO_TIMEOUT_MS); ssl_request();
    cnet_connection original=server_peer.connection; upgrade("localhost",TEST_TLS_CA);
    check_false(client_peer.failed); check_false(server_peer.failed);
    check_equal(client_peer.connected,2u); check_equal(server_peer.connected,2u);
    check_equal(server_peer.connection.slot,original.slot); check_equal(server_peer.connection.generation,original.generation);
    check_equal(client_peer.handshaking,1u); check_equal(server_peer.handshaking,1u);
    char version[TEST_VERSION_BYTES],cipher[TEST_BYTES]; size_t length=0;
    check_equal(cnet_tls_negotiated_version(&client,client_peer.connection,version,sizeof(version),&length),SALTS_OK);
    check_equal(version,"TLSv1.3");
    check_equal(cnet_tls_negotiated_cipher(&client,client_peer.connection,cipher,sizeof(cipher),&length),SALTS_OK);
    check_greater(length,0u);
    check_equal(tdsql_mysql_negotiation_tls_ready(&gate),TDSQL_MYSQL_OK);
    uint8_t bytes[TEST_BYTES],token[TDSQL_MYSQL_AUTH_BYTES]={0}; size_t size=0;
    check_equal(mysql_wire_build_handshake_response(gate.advertised,TEST_MAX_PACKET,TDSQL_MYSQL_BINARY_CHARSET,
      "reader",token,sizeof(token),NULL,TDSQL_MYSQL_AUTH_PLUGIN,bytes,sizeof(bytes),&size),MYSQL_WIRE_STATUS_OK);
    exchange(&client_peer,&server_peer,bytes,size); tdsql_mysql_login login={0};
    check_equal(tdsql_mysql_negotiation_accept(&gate,server_peer.bytes,server_peer.size,2,true,&login),TDSQL_MYSQL_OK);
    check_equal(login.username.data,"reader",login.username.length); check_equal(gate.phase,TDSQL_MYSQL_WAIT_CREDENTIALS);
    tdsql_mysql_output out={bytes,sizeof(bytes),0}; check_equal(tdsql_mysql_full_auth_encode(&out),TDSQL_MYSQL_OK);
    exchange(&server_peer,&client_peer,bytes,out.size);
    static const uint8_t password[]="tls-test-password";
    vstr password_view={(const char *)password,sizeof(password)-1}; tdsql_mysql_password_record record={0};
    turbodb_error_t error; turbodb_error_init(&error);
    check_equal(tdsql_mysql_password_make(password_view,TDSQL_MYSQL_PASSWORD_MIN_ITERATIONS,&record,&error),TURBODB_STATUS_OK);
    exchange(&client_peer,&server_peer,password,sizeof(password)); bool verified=false;
    turbodb_status_t status=tdsql_mysql_password_verify(&record,server_peer.bytes,server_peer.size,true,&verified,&error);
    gmssl_secure_clear(&record,sizeof(record)); gmssl_secure_clear(server_peer.bytes,sizeof(server_peer.bytes));
    check_equal(status,TURBODB_STATUS_OK); check_true(verified);
  }
  it("rejects a mismatched certificate identity before permitting a login") {
    connect_pair(TEST_IO_TIMEOUT_MS); ssl_request(); upgrade("untrusted.invalid",TEST_TLS_CA);
    uint64_t deadline=cmeta_monotonic_ms()+TEST_DEADLINE_MS;
    while(!client_peer.terminal && cmeta_monotonic_ms()<deadline) poll_pair();
    check_true(client_peer.terminal); check_true(client_peer.failed); check_equal(client_peer.connected,1u);
    check_equal(gate.phase,TDSQL_MYSQL_WAIT_TLS); check_equal(server_peer.size,(size_t)TDSQL_MYSQL_SSL_BYTES);
  }
  it("rejects a server certificate signed by an untrusted key before login") {
    connect_pair(TEST_IO_TIMEOUT_MS); ssl_request(); upgrade("localhost",TEST_TLS_WRONG_CA);
    uint64_t deadline=cmeta_monotonic_ms()+TEST_DEADLINE_MS;
    while(!client_peer.terminal && cmeta_monotonic_ms()<deadline) poll_pair();
    check_true(client_peer.terminal); check_true(client_peer.failed); check_equal(client_peer.connected,1u);
    check_equal(gate.phase,TDSQL_MYSQL_WAIT_TLS); check_equal(server_peer.size,(size_t)TDSQL_MYSQL_SSL_BYTES);
  }
  it("terminates a stalled server TLS handshake without authorizing plaintext") {
    connect_pair(TEST_STALLED_TLS_MS); ssl_request();
    check_equal(cnet_start_tls_server(&server,server_peer.connection,&tls_server),SALTS_OK);
    uint64_t deadline=cmeta_monotonic_ms()+TEST_DEADLINE_MS;
    while(!server_peer.terminal && cmeta_monotonic_ms()<deadline) poll_pair();
    check_true(server_peer.terminal); check_true(server_peer.failed);
    check_equal(server_peer.status,SALTS_ETIMEDOUT); check_equal(server_peer.connected,1u);
    check_equal(gate.phase,TDSQL_MYSQL_WAIT_TLS);
  }
}

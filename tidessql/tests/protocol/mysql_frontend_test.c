#include "frontend.h"
#include <gmssl/mem.h>
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

enum { TEST_BUFFER=4096,TEST_CONNECTION_ID=42,TEST_PACKET_HEADER=4 };
static tdsql_database *database;
static char *directory;
static tdsql_mysql_account account;
static tdsql_mysql_database_binding binding;
static tdsql_mysql_auth_policy policy;
static tdsql_mysql_frontend frontend;
static turbodb_error_t error;
static uint8_t input[TEST_BUFFER],scratch[TEST_BUFFER],output_bytes[TEST_BUFFER],packet[TEST_BUFFER];
static tdsql_mysql_output output;
static int action;
static const uint8_t password[]="test-password";
static tdsql_mysql_password_record verifier(void) {
  tdsql_mysql_password_record record={.iterations=TDSQL_MYSQL_PASSWORD_MIN_ITERATIONS,
    .salt={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
    .hash={0xca,0xe9,0xc8,0x01,0x37,0x45,0x96,0xf1,0x7d,0xe4,0x8b,0xa4,0xed,0x70,0x61,0x69,
      0x2b,0x5d,0x0a,0xb4,0x33,0x93,0x2a,0x7d,0x3c,0xdf,0x69,0x8d,0xfd,0x8b,0xb3,0xe8}};
  return record;
}
static mysql_wire_bytes_t emit(uint8_t sequence) {
  output=(tdsql_mysql_output){output_bytes,sizeof(output_bytes),0}; action=TEST_CONNECTION_ID;
  check_equal(tdsql_mysql_frontend_emit(&frontend,&output,&action,&error),TURBODB_STATUS_OK);
  check_equal(action,TDSQL_MYSQL_FRONT_SEND);
  uint32_t payload_size=0; uint8_t actual_sequence=0;
  check_equal(mysql_wire_packet_header_decode(output_bytes,output.size,sequence,TEST_BUFFER,&payload_size,&actual_sequence),MYSQL_WIRE_STATUS_OK);
  check_equal(actual_sequence,sequence); check_equal(output.size,(size_t)payload_size+TEST_PACKET_HEADER);
  return (mysql_wire_bytes_t){output_bytes+TEST_PACKET_HEADER,payload_size};
}
static void acknowledge(int expected) {
  action=TEST_CONNECTION_ID;
  check_equal(tdsql_mysql_frontend_acknowledge(&frontend,&action,&error),TURBODB_STATUS_OK);
  check_equal(action,expected);
}
static size_t frame(const uint8_t *payload,size_t size,uint8_t sequence) {
  tdsql_mysql_output framed={packet,sizeof(packet),0}; uint8_t next=0;
  check_equal(tdsql_mysql_frame_encode(payload,size,sequence,&framed,&next),TDSQL_MYSQL_OK);
  return framed.size;
}
static void feed_range(const uint8_t *bytes,size_t size,size_t width,int final_action) {
  size_t offset=0;
  while(offset<size) {
    const size_t chunk=size-offset<width?size-offset:width; size_t consumed=TEST_CONNECTION_ID;
    action=TEST_CONNECTION_ID;
    check_equal(tdsql_mysql_frontend_feed(&frontend,bytes+offset,chunk,&consumed,&action,&error),TURBODB_STATUS_OK);
    check_equal(consumed,chunk); offset+=consumed;
    check_equal(action,offset==size?final_action:TDSQL_MYSQL_FRONT_RECEIVE);
  }
}
static void connect_until_password(const char *user,const char *database_name) {
  mysql_wire_bytes_t greeting=emit(0); mysql_wire_greeting_t parsed={0};
  check_equal(mysql_wire_parse_greeting(greeting.data,greeting.length,&parsed),MYSQL_WIRE_STATUS_OK);
  check_equal(parsed.connection_id,(uint32_t)TEST_CONNECTION_ID);
  check_equal(parsed.auth_plugin,TDSQL_MYSQL_AUTH_PLUGIN);
  check_equal(parsed.auth_plugin_data_length,(size_t)TDSQL_MYSQL_NONCE_BYTES);
  acknowledge(TDSQL_MYSQL_FRONT_RECEIVE);
  uint32_t capabilities=0;
  check_equal(mysql_wire_select_client_capabilities(&parsed,true,database_name!=NULL,&capabilities),MYSQL_WIRE_STATUS_OK);
  size_t size=0;
  check_equal(mysql_wire_build_ssl_request(capabilities,TEST_BUFFER,TDSQL_MYSQL_BINARY_CHARSET,
      input,sizeof(input),&size),MYSQL_WIRE_STATUS_OK);
  size=frame(input,size,1); feed_range(packet,size,1,TDSQL_MYSQL_FRONT_START_TLS);
  action=TEST_CONNECTION_ID;
  check_equal(tdsql_mysql_frontend_tls_ready(&frontend,&action,&error),TURBODB_STATUS_OK);
  check_equal(action,TDSQL_MYSQL_FRONT_RECEIVE);
  uint8_t token[TDSQL_MYSQL_AUTH_BYTES]={0}; size_t login_size=0;
  check_equal(mysql_wire_build_handshake_response(capabilities,TEST_BUFFER,TDSQL_MYSQL_BINARY_CHARSET,
      user,token,sizeof(token),database_name,TDSQL_MYSQL_AUTH_PLUGIN,input,sizeof(input),&login_size),MYSQL_WIRE_STATUS_OK);
  size=frame(input,login_size,2); feed_range(packet,size,3,TDSQL_MYSQL_FRONT_SEND);
  mysql_wire_bytes_t full=emit(3); const uint8_t expected[]={1,4};
  check_equal(full.length,sizeof(expected)); check_equal(full.data,expected,sizeof(expected));
  acknowledge(TDSQL_MYSQL_FRONT_RECEIVE);
}
static void password_reply(const uint8_t *bytes,size_t size) {
  size_t packet_size=frame(bytes,size,4); feed_range(packet,packet_size,2,TDSQL_MYSQL_FRONT_SEND);
}
static void authenticate(void) {
  connect_until_password("alice","tenant"); password_reply(password,sizeof(password));
  mysql_wire_bytes_t reply=emit(5); mysql_wire_ok_packet_t ok={0};
  check_equal(mysql_wire_decode_ok_packet(reply.data,reply.length,frontend.negotiation.header.capabilities,&ok),MYSQL_WIRE_STATUS_OK);
  check_equal(ok.status_flags,TDSQL_MYSQL_SERVER_AUTOCOMMIT);
  acknowledge(TDSQL_MYSQL_FRONT_RECEIVE); check_equal(frontend.phase,TDSQL_MYSQL_FRONT_COMMAND);
  check_not_null(frontend.connection); check_null(frontend.auth.connection);
}
static void command(const uint8_t *payload,size_t size) {
  size_t packet_size=frame(payload,size,0); feed_range(packet,packet_size,1,TDSQL_MYSQL_FRONT_SEND);
}
static void finish_one_packet(void) {
  acknowledge(TDSQL_MYSQL_FRONT_SEND);
  output=(tdsql_mysql_output){output_bytes,sizeof(output_bytes),TEST_CONNECTION_ID}; action=TEST_CONNECTION_ID;
  check_equal(tdsql_mysql_frontend_emit(&frontend,&output,&action,&error),TURBODB_STATUS_OK);
  check_equal(action,TDSQL_MYSQL_FRONT_RECEIVE); check_equal(frontend.phase,TDSQL_MYSQL_FRONT_COMMAND);
}
spec("MySQL bounded framed connection owner") {
  before_each() {
    database=NULL; directory=tt_make_temp_dir("tidessql-frontend"); check_not_null(directory);
    turbodb_error_init(&error); frontend=(tdsql_mysql_frontend){0};
    const turbodb_option_t options[]={
      {turbodb_view("path"),turbodb_view(directory)},
      {turbodb_view("column_family"),turbodb_view("frontend")},
      {turbodb_view("sql_initialize"),turbodb_view("true")}};
    tdsql_config db_config=tdsql_config_default(); db_config.options=options;
    db_config.option_count=sizeof(options)/sizeof(options[0]);
    check_equal(tdsql_database_open(&db_config,&database,&error),TURBODB_STATUS_OK);
    account=(tdsql_mysql_account){.username=turbodb_view("alice"),.password=verifier(),.databases=1,.default_database=0};
    binding=(tdsql_mysql_database_binding){turbodb_view("tenant"),database};
    tdsql_mysql_auth_policy policy_config=tdsql_mysql_auth_policy_default();
    policy_config.accounts=&account; policy_config.account_count=1; policy_config.databases=&binding; policy_config.database_count=1;
    check_equal(tdsql_mysql_auth_policy_init(&policy,&policy_config,&error),TURBODB_STATUS_OK);
    tdsql_mysql_frontend_config config=tdsql_mysql_frontend_config_default();
    config.policy=&policy; config.connection_id=TEST_CONNECTION_ID;
    check_equal(tdsql_mysql_frontend_init(&frontend,&config,input,sizeof(input),scratch,sizeof(scratch),&error),TURBODB_STATUS_OK);
  }
  after_each() {
    check_equal(tdsql_mysql_frontend_dispose(&frontend,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_database_close(database,&error),TURBODB_STATUS_OK); database=NULL;
    check_equal(tt_remove_tree(directory),0); free(directory); directory=NULL;
    gmssl_secure_clear(&account,sizeof(account));
  }
  it("runs fragmented greeting TLS login full authentication and PING through one owner") {
    authenticate(); const uint8_t ping[]={TDSQL_MYSQL_PING}; command(ping,sizeof(ping));
    mysql_wire_bytes_t reply=emit(1); mysql_wire_ok_packet_t ok={0};
    check_equal(mysql_wire_decode_ok_packet(reply.data,reply.length,frontend.negotiation.header.capabilities,&ok),MYSQL_WIRE_STATUS_OK);
    check_equal(ok.status_flags,TDSQL_MYSQL_SERVER_AUTOCOMMIT); finish_one_packet();
  }
  it("executes a framed COM_QUERY and keeps the session ready for the next command") {
    authenticate(); const uint8_t query[]={TDSQL_MYSQL_QUERY,'C','R','E','A','T','E',' ','T','A','B','L','E',' ',
      't','(','i','d',' ','B','I','G','I','N','T',' ','P','R','I','M','A','R','Y',' ','K','E','Y',')'};
    command(query,sizeof(query)); mysql_wire_bytes_t reply=emit(1); mysql_wire_ok_packet_t ok={0};
    check_equal(mysql_wire_decode_ok_packet(reply.data,reply.length,frontend.negotiation.header.capabilities,&ok),MYSQL_WIRE_STATUS_OK);
    finish_one_packet(); const uint8_t ping[]={TDSQL_MYSQL_PING}; command(ping,sizeof(ping)); (void)emit(1); finish_one_packet();
  }
  it("sends a generic access error and never creates a command owner for a wrong password") {
    connect_until_password("alice","tenant"); const uint8_t wrong[]="wrong"; password_reply(wrong,sizeof(wrong));
    mysql_wire_bytes_t reply=emit(5); mysql_wire_err_packet_t denied={0};
    check_equal(mysql_wire_decode_err_packet(reply.data,reply.length,frontend.negotiation.header.capabilities,&denied),MYSQL_WIRE_STATUS_OK);
    check_equal(denied.error_code,1045u); check_null(frontend.connection); check_null(frontend.dispatch.registry.connection);
    acknowledge(TDSQL_MYSQL_FRONT_DISCONNECT); check_equal(frontend.phase,TDSQL_MYSQL_FRONT_FAILED);
  }
  it("rejects TLS completion before SSLRequest and latches the connection failure") {
    action=TEST_CONNECTION_ID;
    check_equal(tdsql_mysql_frontend_tls_ready(&frontend,&action,&error),TURBODB_STATUS_INVALID_STATE);
    check_equal(frontend.phase,TDSQL_MYSQL_FRONT_FAILED); check_equal(action,TDSQL_MYSQL_FRONT_NONE);
  }
  it("rejects plaintext pipelining after a complete SSLRequest without retaining borrowed bytes") {
    (void)emit(0); acknowledge(TDSQL_MYSQL_FRONT_RECEIVE);
    size_t body=0; uint32_t capabilities=tdsql_mysql_server_capabilities(true);
    check_equal(mysql_wire_build_ssl_request(capabilities,TEST_BUFFER,TDSQL_MYSQL_BINARY_CHARSET,
      input,sizeof(input),&body),MYSQL_WIRE_STATUS_OK);
    size_t packet_size=frame(input,body,1); packet[packet_size++]=0xa5; size_t consumed=0;
    check_equal(tdsql_mysql_frontend_feed(&frontend,packet,packet_size,&consumed,&action,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(consumed,packet_size-1); check_equal(frontend.phase,TDSQL_MYSQL_FRONT_FAILED);
  }
  it("rejects command pipelining while a reply is active") {
    authenticate(); const uint8_t ping[]={TDSQL_MYSQL_PING}; command(ping,sizeof(ping));
    size_t packet_size=frame(ping,sizeof(ping),0),consumed=TEST_CONNECTION_ID;
    check_equal(tdsql_mysql_frontend_feed(&frontend,packet,packet_size,&consumed,&action,&error),TURBODB_STATUS_BUSY);
    check_equal(consumed,0u); (void)emit(1); finish_one_packet();
  }
  it("retries a too-small output without repeating authentication or SQL execution") {
    connect_until_password("alice","tenant"); password_reply(password,sizeof(password));
    memset(output_bytes,0xa5,sizeof(output_bytes)); uint8_t original[TEST_BUFFER]; memcpy(original,output_bytes,sizeof(original));
    output=(tdsql_mysql_output){output_bytes,TEST_PACKET_HEADER,TEST_CONNECTION_ID};
    check_equal(tdsql_mysql_frontend_emit(&frontend,&output,&action,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(output.size,(size_t)TEST_CONNECTION_ID); check_equal(output_bytes,original,sizeof(original));
    (void)emit(5); acknowledge(TDSQL_MYSQL_FRONT_RECEIVE); check_not_null(frontend.connection);
  }
  it("disconnect disposal rolls back an authenticated SQL transaction") {
    authenticate();
    const uint8_t create[]={TDSQL_MYSQL_QUERY,'C','R','E','A','T','E',' ','T','A','B','L','E',' ',
      't','(','i','d',' ','B','I','G','I','N','T',' ','P','R','I','M','A','R','Y',' ','K','E','Y',')'};
    command(create,sizeof(create)); (void)emit(1); finish_one_packet();
    const uint8_t begin[]={TDSQL_MYSQL_QUERY,'B','E','G','I','N'}; command(begin,sizeof(begin)); (void)emit(1); finish_one_packet();
    const uint8_t insert[]={TDSQL_MYSQL_QUERY,'I','N','S','E','R','T',' ','I','N','T','O',' ','t',' ','V','A','L','U','E','S','(','1',')'};
    command(insert,sizeof(insert)); (void)emit(1); finish_one_packet();
    check_equal(tdsql_mysql_frontend_dispose(&frontend,&error),TURBODB_STATUS_OK);
    tdsql_connection *check=NULL; check_equal(tdsql_database_connect(database,&check,&error),TURBODB_STATUS_OK);
    tdsql_request request=tdsql_request_default(turbodb_view("SELECT id FROM t")); tdsql_result *rows=NULL;
    check_equal(tdsql_connection_query(check,&request,&rows,&error),TURBODB_STATUS_OK);
    tdsql_row row={0}; check_equal(tdsql_result_next(rows,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,TDSQL_DONE);
    check_equal(tdsql_result_destroy_checked(rows,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_connection_close(check,&error),TURBODB_STATUS_OK);
  }
  it("rejects overlapping buffers embedded NUL versions and zero connection IDs atomically") {
    check_equal(tdsql_mysql_frontend_dispose(&frontend,&error),TURBODB_STATUS_OK);
    tdsql_mysql_frontend sentinel={.phase=TEST_CONNECTION_ID}; frontend=sentinel;
    tdsql_mysql_frontend_config config=tdsql_mysql_frontend_config_default(); config.policy=&policy;
    check_equal(tdsql_mysql_frontend_init(&frontend,&config,input,sizeof(input),scratch,sizeof(scratch),&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(&frontend,&sentinel,sizeof(sentinel)); frontend=(tdsql_mysql_frontend){0}; config.connection_id=TEST_CONNECTION_ID;
    check_equal(tdsql_mysql_frontend_init(&frontend,&config,input,sizeof(input),input+1,sizeof(input)-1,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(frontend.phase,0); const char invalid[]={'v',0,'x'}; config.server_version=(vstr){invalid,sizeof(invalid)};
    check_equal(tdsql_mysql_frontend_init(&frontend,&config,input,sizeof(input),scratch,sizeof(scratch),&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(frontend.phase,0);
  }
}

#include "handshake.h"
#include <tinytest.h>
#include <string.h>

enum { TEST_BUFFER=1024,TEST_MAX_PACKET=16777216,TEST_ID=0x04030201,
       TEST_HEADER_CHARSET=8,TEST_HEADER_FILLER=9,TEST_HEADER_PACKET=4,
       TEST_AUTH_LENGTH=39,TEST_TRAILER=77 };
static uint8_t payload[TEST_BUFFER],ssl[TDSQL_MYSQL_SSL_BYTES],encoded[TEST_BUFFER];
static uint8_t token[TDSQL_MYSQL_AUTH_BYTES];
static size_t payload_size,ssl_size;
static uint32_t advertised,selected;
static tdsql_mysql_negotiation gate;
static mysql_wire_greeting_t greeting;
static tdsql_mysql_login login,sentinel;
static void packets(const char *user,const uint8_t *auth,size_t size,const char *database,const char *plugin) {
  check_equal(mysql_wire_build_ssl_request(selected,TEST_MAX_PACKET,TDSQL_MYSQL_BINARY_CHARSET,ssl,sizeof(ssl),&ssl_size),MYSQL_WIRE_STATUS_OK);
  check_equal(mysql_wire_build_handshake_response(selected,TEST_MAX_PACKET,TDSQL_MYSQL_BINARY_CHARSET,
      user,auth,size,database,plugin,payload,sizeof(payload),&payload_size),MYSQL_WIRE_STATUS_OK);
}
static void init(bool database) {
  advertised=tdsql_mysql_server_capabilities(database); selected=advertised;
  gate=(tdsql_mysql_negotiation){0}; check_equal(tdsql_mysql_negotiation_init(&gate,advertised),TDSQL_MYSQL_OK);
}
static void begin_tls(void) {
  check_equal(tdsql_mysql_negotiation_accept(&gate,ssl,ssl_size,1,false,&login),TDSQL_MYSQL_OK);
  check_equal(gate.phase,TDSQL_MYSQL_WAIT_TLS); check_equal(&login,&sentinel,sizeof(login));
  check_equal(tdsql_mysql_negotiation_tls_ready(&gate),TDSQL_MYSQL_OK); check_equal(gate.phase,TDSQL_MYSQL_WAIT_LOGIN);
}
static void preserved(void) { check_equal(&login,&sentinel,sizeof(login)); }

spec("MySQL server TLS negotiation and handshake payloads") {
  before_each() {
    memset(token,UINT8_C(0xa5),sizeof(token)); token[0]=0; token[sizeof(token)-1]=0;
    sentinel=(tdsql_mysql_login){.header={.max_packet_size=TEST_TRAILER}}; login=sentinel;
    greeting=(mysql_wire_greeting_t){.protocol_version=10,.connection_id=TEST_ID,
        .character_set=TDSQL_MYSQL_BINARY_CHARSET,.status_flags=TDSQL_MYSQL_SERVER_AUTOCOMMIT,
        .auth_plugin_data_length=TDSQL_MYSQL_NONCE_BYTES};
    memcpy(greeting.server_version,"TidesSQL",sizeof("TidesSQL"));
    memcpy(greeting.auth_plugin,TDSQL_MYSQL_AUTH_PLUGIN,sizeof(TDSQL_MYSQL_AUTH_PLUGIN));
    memcpy(greeting.auth_plugin_data,"abcdefghijklmnopqrst",TDSQL_MYSQL_NONCE_BYTES);
    init(false); greeting.capabilities=advertised;
    packets("reader",token,sizeof(token),NULL,TDSQL_MYSQL_AUTH_PLUGIN);
  }
  it("produces an independent Protocol10 golden and the existing client can parse/select it") {
    const uint8_t golden[]={10,'T','i','d','e','s','S','Q','L',0,1,2,3,4,
      'a','b','c','d','e','f','g','h',0,5,0xaa,63,2,0,8,1,21,
      0,0,0,0,0,0,0,0,0,0,'i','j','k','l','m','n','o','p','q','r','s','t',0,
      'c','a','c','h','i','n','g','_','s','h','a','2','_','p','a','s','s','w','o','r','d',0};
    tdsql_mysql_output out={encoded,sizeof(encoded),0};
    check_equal(tdsql_mysql_greeting_encode(&greeting,&out),TDSQL_MYSQL_OK);
    check_equal(out.size,sizeof(golden)); check_equal(encoded,golden,sizeof(golden));
    mysql_wire_greeting_t client={0}; check_equal(mysql_wire_parse_greeting(encoded,out.size,&client),MYSQL_WIRE_STATUS_OK);
    check_equal(client.connection_id,TEST_ID); check_equal(client.capabilities,advertised);
    check_equal(client.auth_plugin_data,greeting.auth_plugin_data,TDSQL_MYSQL_NONCE_BYTES);
    check_equal(client.auth_plugin_data_length,(size_t)TDSQL_MYSQL_NONCE_BYTES);
    uint32_t capabilities=0;
    check_equal(mysql_wire_select_client_capabilities(&client,true,false,&capabilities),MYSQL_WIRE_STATUS_OK);
    check_equal(capabilities,advertised);
    check_equal(mysql_wire_select_client_capabilities(&client,true,true,&capabilities),MYSQL_WIRE_STATUS_INVALID);
  }
  it("atomically measures/encodes bounded greetings and rejects unterminated strings or fake features") {
    tdsql_mysql_output measured={.capacity=sizeof(encoded),.size=TEST_TRAILER};
    check_equal(tdsql_mysql_greeting_encode(&greeting,&measured),TDSQL_MYSQL_OK);
    memset(encoded,UINT8_C(0xcc),sizeof(encoded)); uint8_t original[TEST_BUFFER]; memcpy(original,encoded,sizeof(original));
    tdsql_mysql_output small={encoded,measured.size-1,TEST_TRAILER};
    check_equal(tdsql_mysql_greeting_encode(&greeting,&small),TDSQL_MYSQL_LIMIT);
    check_equal(small.size,TEST_TRAILER); check_equal(encoded,original,sizeof(original));
    small.capacity=sizeof(encoded); memset(greeting.server_version,'x',sizeof(greeting.server_version));
    check_equal(tdsql_mysql_greeting_encode(&greeting,&small),TDSQL_MYSQL_LIMIT); check_equal(encoded,original,sizeof(original));
    greeting.server_version[0]=0; check_equal(tdsql_mysql_greeting_encode(&greeting,&small),TDSQL_MYSQL_INVALID);
    greeting.server_version[0]='x'; greeting.server_version[1]=0; greeting.capabilities|=MYSQL_WIRE_CLIENT_MULTI_RESULTS;
    check_equal(tdsql_mysql_greeting_encode(&greeting,&small),TDSQL_MYSQL_UNSUPPORTED); check_equal(encoded,original,sizeof(original));
    greeting.capabilities=advertised; memset(greeting.auth_plugin,'x',sizeof(greeting.auth_plugin));
    check_equal(tdsql_mysql_greeting_encode(&greeting,&small),TDSQL_MYSQL_LIMIT);
  }
  it("parses existing client TLS/login builders and preserves binary token bytes including NUL") {
    begin_tls(); check_equal(tdsql_mysql_negotiation_accept(&gate,payload,payload_size,2,true,&login),TDSQL_MYSQL_OK);
    check_equal(gate.phase,TDSQL_MYSQL_WAIT_CREDENTIALS); check_equal(login.username.data,"reader",6);
    check_equal(login.auth_response.length,sizeof(token)); check_equal(login.auth_response.data,token,sizeof(token));
    check_true(login.auth_response.data==payload+TEST_AUTH_LENGTH+1); check_equal(login.database.length,0u);
    check_equal(login.header.capabilities,selected); check_equal(login.header.max_packet_size,TEST_MAX_PACKET);
    /* Protocol admission has no authenticated state or SQL connection. */
    check_equal(tdsql_mysql_negotiation_accept(&gate,payload,payload_size,2,true,&login),TDSQL_MYSQL_INVALID);
    check_equal(gate.phase,TDSQL_MYSQL_HANDSHAKE_FAILED);
  }
  it("allows optional database selection and empty auth without authorizing either") {
    init(true); packets("reader",NULL,0,"../../arbitrary",TDSQL_MYSQL_AUTH_PLUGIN); begin_tls();
    check_equal(tdsql_mysql_negotiation_accept(&gate,payload,payload_size,2,true,&login),TDSQL_MYSQL_OK);
    check_equal(gate.phase,TDSQL_MYSQL_WAIT_CREDENTIALS); check_equal(login.auth_response.length,0u);
    check_equal(login.database.data,"../../arbitrary",15); check_equal(login.database.length,15u);
    check_true((login.header.capabilities&MYSQL_WIRE_CLIENT_CONNECT_WITH_DB)!=0);
  }
  it("does not parse plaintext login or accept forged/duplicate TLS transitions") {
    check_equal(tdsql_mysql_negotiation_accept(&gate,payload,payload_size,1,false,&login),TDSQL_MYSQL_INVALID);
    check_equal(gate.phase,TDSQL_MYSQL_HANDSHAKE_FAILED); preserved();
    init(false); check_equal(tdsql_mysql_negotiation_tls_ready(&gate),TDSQL_MYSQL_INVALID);
    check_equal(gate.phase,TDSQL_MYSQL_HANDSHAKE_FAILED);
    init(false); check_equal(tdsql_mysql_negotiation_accept(&gate,ssl,ssl_size,1,true,&login),TDSQL_MYSQL_INVALID); preserved();
    init(false); begin_tls(); check_equal(tdsql_mysql_negotiation_tls_ready(&gate),TDSQL_MYSQL_INVALID); preserved();
    init(false); begin_tls(); check_equal(tdsql_mysql_negotiation_accept(&gate,payload,payload_size,2,false,&login),TDSQL_MYSQL_INVALID); preserved();
  }
  it("rejects incoming bytes while transport TLS establishment is pending") {
    check_equal(tdsql_mysql_negotiation_accept(&gate,ssl,ssl_size,1,false,&login),TDSQL_MYSQL_OK);
    check_equal(tdsql_mysql_negotiation_accept(&gate,payload,payload_size,2,true,&login),TDSQL_MYSQL_INVALID); preserved();
    check_equal(gate.phase,TDSQL_MYSQL_HANDSHAKE_FAILED);
    check_equal(tdsql_mysql_negotiation_tls_ready(&gate),TDSQL_MYSQL_INVALID);
  }
  it("latches wrong message sequences and publishes no partial login") {
    const uint8_t wrong[]={0,2,UINT8_C(255)};
    for(size_t i=0;i<sizeof(wrong);++i) {
      init(false); check_equal(tdsql_mysql_negotiation_accept(&gate,ssl,ssl_size,wrong[i],false,&login),TDSQL_MYSQL_SEQUENCE);
      preserved(); check_equal(gate.phase,TDSQL_MYSQL_HANDSHAKE_FAILED);
      check_equal(tdsql_mysql_negotiation_accept(&gate,ssl,ssl_size,1,false,&login),TDSQL_MYSQL_INVALID);
    }
    init(false); begin_tls(); check_equal(tdsql_mysql_negotiation_accept(&gate,payload,payload_size,1,true,&login),TDSQL_MYSQL_SEQUENCE); preserved();
  }
  it("rejects every SSL/login truncation and trailing bytes without changing outputs") {
    tdsql_mysql_login_header header={.max_packet_size=TEST_TRAILER}; const tdsql_mysql_login_header original=header;
    for(size_t i=0;i<sizeof(ssl);++i) {
      check_equal(tdsql_mysql_ssl_decode(ssl,i,advertised,&header),TDSQL_MYSQL_INVALID); check_equal(&header,&original,sizeof(header));
    }
    for(size_t i=0;i<payload_size;++i) {
      check_not_equal(tdsql_mysql_login_decode(payload,i,advertised,&login),TDSQL_MYSQL_OK); preserved();
    }
    payload[payload_size]=TEST_TRAILER;
    check_equal(tdsql_mysql_login_decode(payload,payload_size+1,advertised,&login),TDSQL_MYSQL_INVALID); preserved();
    check_equal(tdsql_mysql_ssl_decode(payload,TDSQL_MYSQL_SSL_BYTES+1,advertised,&header),TDSQL_MYSQL_INVALID);
  }
  it("rejects all unadvertised bits and required capabilities missing in client or server") {
    for(size_t bit=0;bit<32;++bit) {
      const uint32_t flag=UINT32_C(1)<<bit;
      if(!(advertised&flag)) {
        size_t offset=0;
        check_equal(mysql_wire_write_u32_le(ssl,sizeof(ssl),&offset,advertised|flag),MYSQL_WIRE_STATUS_OK);
        offset=0; check_equal(mysql_wire_write_u32_le(payload,sizeof(payload),&offset,advertised|flag),MYSQL_WIRE_STATUS_OK);
        tdsql_mysql_login_header header={0};
        check_equal(tdsql_mysql_ssl_decode(ssl,ssl_size,advertised,&header),TDSQL_MYSQL_UNSUPPORTED);
        check_equal(tdsql_mysql_login_decode(payload,payload_size,advertised,&login),TDSQL_MYSQL_UNSUPPORTED); preserved();
      }
    }
    const uint32_t required[]={MYSQL_WIRE_CLIENT_PROTOCOL_41,MYSQL_WIRE_CLIENT_SSL,MYSQL_WIRE_CLIENT_SECURE_CONNECTION,MYSQL_WIRE_CLIENT_PLUGIN_AUTH};
    for(size_t i=0;i<sizeof(required)/sizeof(required[0]);++i) {
      uint8_t bytes[TDSQL_MYSQL_SSL_BYTES]; memcpy(bytes,ssl,sizeof(bytes)); size_t offset=0;
      check_equal(mysql_wire_write_u32_le(bytes,sizeof(bytes),&offset,advertised&~required[i]),MYSQL_WIRE_STATUS_OK);
      tdsql_mysql_login_header header={0}; check_equal(tdsql_mysql_ssl_decode(bytes,sizeof(bytes),advertised,&header),TDSQL_MYSQL_UNSUPPORTED);
      tdsql_mysql_negotiation fresh={0}; check_equal(tdsql_mysql_negotiation_init(&fresh,advertised&~required[i]),TDSQL_MYSQL_INVALID); check_equal(fresh.phase,0);
    }
  }
  it("accepts binary and utf8mb4 clients but rejects other charsets and nonzero reserved bytes") {
    tdsql_mysql_login_header header={.max_packet_size=TEST_TRAILER}; const tdsql_mysql_login_header original=header;
    uint8_t bytes[TDSQL_MYSQL_SSL_BYTES]; memcpy(bytes,ssl,sizeof(bytes)); memset(bytes+TEST_HEADER_PACKET,0,sizeof(uint32_t));
    check_equal(tdsql_mysql_ssl_decode(bytes,sizeof(bytes),advertised,&header),TDSQL_MYSQL_INVALID); check_equal(&header,&original,sizeof(header));
    const uint8_t supported[]={TDSQL_MYSQL_UTF8MB4_GENERAL_CI,TDSQL_MYSQL_UTF8MB4_0900_AI_CI};
    for(size_t i=0;i<sizeof(supported)/sizeof(supported[0]);++i) {
      memcpy(bytes,ssl,sizeof(bytes)); bytes[TEST_HEADER_CHARSET]=supported[i];
      check_equal(tdsql_mysql_ssl_decode(bytes,sizeof(bytes),advertised,&header),TDSQL_MYSQL_OK);
      check_equal(header.character_set,supported[i]);
    }
    header=original;
    memcpy(bytes,ssl,sizeof(bytes)); bytes[TEST_HEADER_CHARSET]=1;
    check_equal(tdsql_mysql_ssl_decode(bytes,sizeof(bytes),advertised,&header),TDSQL_MYSQL_UNSUPPORTED);
    for(size_t i=TEST_HEADER_FILLER;i<sizeof(bytes);++i) {
      memcpy(bytes,ssl,sizeof(bytes)); bytes[i]=1;
      check_equal(tdsql_mysql_ssl_decode(bytes,sizeof(bytes),advertised,&header),TDSQL_MYSQL_INVALID); check_equal(&header,&original,sizeof(header));
    }
  }
  it("rejects changed TLS flags/packet limits before publishing borrowed credentials") {
    begin_tls(); size_t offset=0;
    check_equal(mysql_wire_write_u32_le(payload,payload_size,&offset,selected&~MYSQL_WIRE_CLIENT_DEPRECATE_EOF),MYSQL_WIRE_STATUS_OK);
    check_equal(tdsql_mysql_negotiation_accept(&gate,payload,payload_size,2,true,&login),TDSQL_MYSQL_INVALID); preserved();
    init(false); packets("reader",token,sizeof(token),NULL,TDSQL_MYSQL_AUTH_PLUGIN); begin_tls(); offset=TEST_HEADER_PACKET;
    check_equal(mysql_wire_write_u32_le(payload,payload_size,&offset,TEST_MAX_PACKET-1),MYSQL_WIRE_STATUS_OK);
    check_equal(tdsql_mysql_negotiation_accept(&gate,payload,payload_size,2,true,&login),TDSQL_MYSQL_INVALID); preserved();
  }
  it("bounds account/database/plugin strings and never accepts anonymous or another plugin") {
    char user[TDSQL_MYSQL_USERNAME_BYTES+2]; memset(user,'x',sizeof(user)); user[sizeof(user)-1]=0;
    packets(user,token,sizeof(token),NULL,TDSQL_MYSQL_AUTH_PLUGIN);
    check_equal(tdsql_mysql_login_decode(payload,payload_size,advertised,&login),TDSQL_MYSQL_LIMIT); preserved();
    user[sizeof(user)-2]=0; packets(user,token,sizeof(token),NULL,TDSQL_MYSQL_AUTH_PLUGIN);
    check_equal(tdsql_mysql_login_decode(payload,payload_size,advertised,&login),TDSQL_MYSQL_OK);
    login=sentinel; packets("",token,sizeof(token),NULL,TDSQL_MYSQL_AUTH_PLUGIN);
    check_equal(tdsql_mysql_login_decode(payload,payload_size,advertised,&login),TDSQL_MYSQL_INVALID); preserved();
    packets("reader",token,sizeof(token),NULL,"mysql_native_password");
    check_equal(tdsql_mysql_login_decode(payload,payload_size,advertised,&login),TDSQL_MYSQL_UNSUPPORTED); preserved();
    char plugin[MYSQL_WIRE_AUTH_PLUGIN_NAME_CAPACITY+1]; memset(plugin,'x',sizeof(plugin)); plugin[sizeof(plugin)-1]=0;
    packets("reader",token,sizeof(token),NULL,plugin);
    check_equal(tdsql_mysql_login_decode(payload,payload_size,advertised,&login),TDSQL_MYSQL_LIMIT); preserved();
    init(true); char database[TDSQL_MYSQL_DATABASE_BYTES+2]; memset(database,'x',sizeof(database)); database[sizeof(database)-1]=0;
    packets("reader",token,sizeof(token),database,TDSQL_MYSQL_AUTH_PLUGIN);
    check_equal(tdsql_mysql_login_decode(payload,payload_size,advertised,&login),TDSQL_MYSQL_LIMIT); preserved();
    packets("reader",token,sizeof(token),"",TDSQL_MYSQL_AUTH_PLUGIN);
    check_equal(tdsql_mysql_login_decode(payload,payload_size,advertised,&login),TDSQL_MYSQL_INVALID); preserved();
  }
  it("rejects token lengths outside caching_sha2 and does not mistake embedded NUL for truncation") {
    const size_t invalid[]={1,TDSQL_MYSQL_AUTH_BYTES-1,TDSQL_MYSQL_AUTH_BYTES+1}; uint8_t bytes[TDSQL_MYSQL_AUTH_BYTES+1]={0};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
      packets("reader",bytes,invalid[i],NULL,TDSQL_MYSQL_AUTH_PLUGIN);
      check_equal(tdsql_mysql_login_decode(payload,payload_size,advertised,&login),TDSQL_MYSQL_INVALID); preserved();
    }
    packets("reader",token,sizeof(token),NULL,TDSQL_MYSQL_AUTH_PLUGIN);
    payload[TEST_AUTH_LENGTH]=UINT8_C(255);
    check_equal(tdsql_mysql_login_decode(payload,payload_size,advertised,&login),TDSQL_MYSQL_INVALID); preserved();
  }
  it("allows clients to decline optional EOF/transaction/long flags without inventing capabilities") {
    selected&=~(MYSQL_WIRE_CLIENT_DEPRECATE_EOF|MYSQL_WIRE_CLIENT_TRANSACTIONS|MYSQL_WIRE_CLIENT_LONG_PASSWORD|MYSQL_WIRE_CLIENT_LONG_FLAG);
    packets("reader",token,sizeof(token),NULL,TDSQL_MYSQL_AUTH_PLUGIN); begin_tls();
    check_equal(tdsql_mysql_negotiation_accept(&gate,payload,payload_size,2,true,&login),TDSQL_MYSQL_OK);
    check_equal(login.header.capabilities,selected); check_false((login.header.capabilities&MYSQL_WIRE_CLIENT_DEPRECATE_EOF)!=0);
  }
}

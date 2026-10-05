#include "handshake.h"
#include <string.h>

enum { HANDSHAKE_PROTOCOL=10,HANDSHAKE_NONCE_FIRST=8,HANDSHAKE_RESERVED=10,
       HANDSHAKE_SSL_FILLER=23,HANDSHAKE_SSL_OFFSET=9,HANDSHAKE_SECOND_SEQUENCE=2 };
static uint32_t handshake_required(void) {
  return MYSQL_WIRE_CLIENT_PROTOCOL_41|MYSQL_WIRE_CLIENT_SSL|
      MYSQL_WIRE_CLIENT_SECURE_CONNECTION|MYSQL_WIRE_CLIENT_PLUGIN_AUTH;
}
uint32_t tdsql_mysql_server_capabilities(bool database) {
  return handshake_required()|MYSQL_WIRE_CLIENT_LONG_PASSWORD|MYSQL_WIRE_CLIENT_LONG_FLAG|
      MYSQL_WIRE_CLIENT_TRANSACTIONS|MYSQL_WIRE_CLIENT_DEPRECATE_EOF|
      (database?MYSQL_WIRE_CLIENT_CONNECT_WITH_DB:0);
}
static tdsql_mysql_status handshake_flags(uint32_t advertised,uint32_t client) {
  const uint32_t required=handshake_required();
  if((advertised&required)!=required) return TDSQL_MYSQL_INVALID;
  if(advertised&~tdsql_mysql_server_capabilities(true)) return TDSQL_MYSQL_UNSUPPORTED;
  if((client&required)!=required) return TDSQL_MYSQL_UNSUPPORTED;
  return client&~advertised?TDSQL_MYSQL_UNSUPPORTED:TDSQL_MYSQL_OK;
}
static tdsql_mysql_status handshake_string(const char *value,size_t capacity,size_t *length) {
  const char *end=memchr(value,0,capacity);
  if(!end) return TDSQL_MYSQL_LIMIT;
  *length=(size_t)(end-value); return TDSQL_MYSQL_OK;
}
tdsql_mysql_status tdsql_mysql_greeting_encode(const mysql_wire_greeting_t *g,tdsql_mysql_output *out) {
  if(!g || !out || g->protocol_version!=HANDSHAKE_PROTOCOL || !g->connection_id ||
      g->auth_plugin_data_length!=TDSQL_MYSQL_NONCE_BYTES ||
      g->character_set!=TDSQL_MYSQL_BINARY_CHARSET || g->status_flags!=TDSQL_MYSQL_SERVER_AUTOCOMMIT)
    return TDSQL_MYSQL_INVALID;
  tdsql_mysql_status status=handshake_flags(g->capabilities,g->capabilities);
  size_t version=0,plugin=0;
  if(status==TDSQL_MYSQL_OK) status=handshake_string(g->server_version,sizeof(g->server_version),&version);
  if(status==TDSQL_MYSQL_OK) status=handshake_string(g->auth_plugin,sizeof(g->auth_plugin),&plugin);
  if(status!=TDSQL_MYSQL_OK) return status;
  if(!version) return TDSQL_MYSQL_INVALID;
  if(plugin!=sizeof(TDSQL_MYSQL_AUTH_PLUGIN)-1 || memcmp(g->auth_plugin,TDSQL_MYSQL_AUTH_PLUGIN,plugin)) return TDSQL_MYSQL_UNSUPPORTED;
  enum { HANDSHAKE_FIXED=1+4+8+1+2+1+2+2+1+10+13 };
  const size_t size=HANDSHAKE_FIXED+version+1+plugin+1;
  if(size>out->capacity) return TDSQL_MYSQL_LIMIT;
  if(!out->data) { out->size=size; return TDSQL_MYSQL_OK; }
  uint8_t encoded[HANDSHAKE_FIXED+MYSQL_WIRE_SERVER_VERSION_CAPACITY+MYSQL_WIRE_AUTH_PLUGIN_NAME_CAPACITY];
  uint8_t *data=encoded; size_t offset=0;
  data[offset++]=HANDSHAKE_PROTOCOL; memcpy(data+offset,g->server_version,version+1); offset+=version+1;
  status=mysql_wire_write_u32_le(data,sizeof(encoded),&offset,g->connection_id);
  if(status!=TDSQL_MYSQL_OK) return status;
  memcpy(data+offset,g->auth_plugin_data,HANDSHAKE_NONCE_FIRST); offset+=HANDSHAKE_NONCE_FIRST;
  data[offset++]=0; status=mysql_wire_write_u16_le(data,sizeof(encoded),&offset,(uint16_t)g->capabilities);
  if(status!=TDSQL_MYSQL_OK) return status;
  data[offset++]=g->character_set; status=mysql_wire_write_u16_le(data,sizeof(encoded),&offset,g->status_flags);
  if(status==TDSQL_MYSQL_OK) status=mysql_wire_write_u16_le(data,sizeof(encoded),&offset,(uint16_t)(g->capabilities>>16));
  if(status!=TDSQL_MYSQL_OK) return status;
  data[offset++]=TDSQL_MYSQL_NONCE_BYTES+1; memset(data+offset,0,HANDSHAKE_RESERVED); offset+=HANDSHAKE_RESERVED;
  memcpy(data+offset,g->auth_plugin_data+HANDSHAKE_NONCE_FIRST,TDSQL_MYSQL_NONCE_BYTES-HANDSHAKE_NONCE_FIRST);
  offset+=TDSQL_MYSQL_NONCE_BYTES-HANDSHAKE_NONCE_FIRST; data[offset++]=0;
  memcpy(data+offset,g->auth_plugin,plugin+1); offset+=plugin+1;
  memcpy(out->data,data,offset); out->size=offset;
  return TDSQL_MYSQL_OK;
}
static tdsql_mysql_status handshake_header(const uint8_t *data,size_t size,uint32_t advertised,tdsql_mysql_login_header *out) {
  if(!data || !out) return TDSQL_MYSQL_INVALID;
  if(size<TDSQL_MYSQL_SSL_BYTES) return TDSQL_MYSQL_INVALID;
  tdsql_mysql_login_header header={0}; size_t offset=0;
  mysql_wire_status_t status=mysql_wire_read_u32_le(data,size,&offset,&header.capabilities);
  if(status==MYSQL_WIRE_STATUS_OK) status=mysql_wire_read_u32_le(data,size,&offset,&header.max_packet_size);
  if(status!=MYSQL_WIRE_STATUS_OK) return status;
  const tdsql_mysql_status flags=handshake_flags(advertised,header.capabilities);
  if(flags!=TDSQL_MYSQL_OK) return flags;
  header.character_set=data[offset++];
  if(!header.max_packet_size) return TDSQL_MYSQL_INVALID;
  if(header.character_set!=TDSQL_MYSQL_BINARY_CHARSET) return TDSQL_MYSQL_UNSUPPORTED;
  const uint8_t empty[HANDSHAKE_SSL_FILLER]={0};
  if(memcmp(data+HANDSHAKE_SSL_OFFSET,empty,sizeof(empty))) return TDSQL_MYSQL_INVALID;
  *out=header; return TDSQL_MYSQL_OK;
}
tdsql_mysql_status tdsql_mysql_ssl_decode(const uint8_t *data,size_t size,uint32_t advertised,tdsql_mysql_login_header *out) {
  if(size!=TDSQL_MYSQL_SSL_BYTES) return TDSQL_MYSQL_INVALID;
  return handshake_header(data,size,advertised,out);
}
static tdsql_mysql_status handshake_cstring(const uint8_t *data,size_t size,size_t *offset,size_t limit,mysql_wire_bytes_t *out) {
  if(*offset>=size) return TDSQL_MYSQL_INVALID;
  const uint8_t *start=data+*offset,*end=memchr(start,0,size-*offset);
  if(!end) return TDSQL_MYSQL_INVALID;
  const size_t length=(size_t)(end-start);
  if(length>limit) return TDSQL_MYSQL_LIMIT;
  *out=(mysql_wire_bytes_t){start,length,false}; *offset+=length+1; return TDSQL_MYSQL_OK;
}
tdsql_mysql_status tdsql_mysql_login_decode(const uint8_t *data,size_t size,uint32_t advertised,tdsql_mysql_login *out) {
  if(!out) return TDSQL_MYSQL_INVALID;
  tdsql_mysql_login login={0}; tdsql_mysql_status status=handshake_header(data,size,advertised,&login.header);
  size_t offset=TDSQL_MYSQL_SSL_BYTES;
  if(status==TDSQL_MYSQL_OK) status=handshake_cstring(data,size,&offset,TDSQL_MYSQL_USERNAME_BYTES,&login.username);
  if(status!=TDSQL_MYSQL_OK) return status;
  if(!login.username.length || offset>=size) return TDSQL_MYSQL_INVALID;
  const size_t bytes=data[offset++];
  if(bytes!=0 && bytes!=TDSQL_MYSQL_AUTH_BYTES) return TDSQL_MYSQL_INVALID;
  if(bytes>size-offset) return TDSQL_MYSQL_INVALID;
  login.auth_response=(mysql_wire_bytes_t){data+offset,bytes,false}; offset+=bytes;
  if(login.header.capabilities&MYSQL_WIRE_CLIENT_CONNECT_WITH_DB) {
    status=handshake_cstring(data,size,&offset,TDSQL_MYSQL_DATABASE_BYTES,&login.database);
    if(status!=TDSQL_MYSQL_OK) return status;
    if(!login.database.length) return TDSQL_MYSQL_INVALID;
  }
  status=handshake_cstring(data,size,&offset,MYSQL_WIRE_AUTH_PLUGIN_NAME_CAPACITY-1,&login.plugin);
  if(status!=TDSQL_MYSQL_OK) return status;
  if(login.plugin.length!=sizeof(TDSQL_MYSQL_AUTH_PLUGIN)-1 ||
      memcmp(login.plugin.data,TDSQL_MYSQL_AUTH_PLUGIN,login.plugin.length)) return TDSQL_MYSQL_UNSUPPORTED;
  if(offset!=size) return TDSQL_MYSQL_INVALID;
  *out=login; return TDSQL_MYSQL_OK;
}
tdsql_mysql_status tdsql_mysql_negotiation_init(tdsql_mysql_negotiation *out,uint32_t advertised) {
  if(!out || out->phase) return TDSQL_MYSQL_INVALID;
  const tdsql_mysql_status status=handshake_flags(advertised,advertised);
  if(status==TDSQL_MYSQL_OK) *out=(tdsql_mysql_negotiation){.advertised=advertised,.phase=TDSQL_MYSQL_WAIT_SSL};
  return status;
}
tdsql_mysql_status tdsql_mysql_negotiation_accept(tdsql_mysql_negotiation *n,const uint8_t *data,size_t size,
    uint8_t sequence,bool tls,tdsql_mysql_login *out) {
  if(!n || !n->phase) return TDSQL_MYSQL_INVALID;
  if(n->phase==TDSQL_MYSQL_HANDSHAKE_FAILED) return TDSQL_MYSQL_INVALID;
  tdsql_mysql_status status=TDSQL_MYSQL_INVALID;
  if(n->phase==TDSQL_MYSQL_WAIT_SSL && !tls) {
    if(sequence!=1) status=TDSQL_MYSQL_SEQUENCE;
    else status=tdsql_mysql_ssl_decode(data,size,n->advertised,&n->header);
    if(status==TDSQL_MYSQL_OK) { n->phase=TDSQL_MYSQL_WAIT_TLS; return status; }
  } else if(n->phase==TDSQL_MYSQL_WAIT_LOGIN && tls) {
    if(sequence!=HANDSHAKE_SECOND_SEQUENCE) status=TDSQL_MYSQL_SEQUENCE;
    else if(out) {
      tdsql_mysql_login login={0}; status=tdsql_mysql_login_decode(data,size,n->advertised,&login);
      if(status==TDSQL_MYSQL_OK && (login.header.capabilities!=n->header.capabilities ||
          login.header.max_packet_size!=n->header.max_packet_size || login.header.character_set!=n->header.character_set)) status=TDSQL_MYSQL_INVALID;
      if(status==TDSQL_MYSQL_OK) { *out=login; n->phase=TDSQL_MYSQL_WAIT_CREDENTIALS; return status; }
    }
  }
  n->phase=TDSQL_MYSQL_HANDSHAKE_FAILED; return status;
}
tdsql_mysql_status tdsql_mysql_negotiation_tls_ready(tdsql_mysql_negotiation *n) {
  if(!n || !n->phase) return TDSQL_MYSQL_INVALID;
  if(n->phase!=TDSQL_MYSQL_WAIT_TLS) { n->phase=TDSQL_MYSQL_HANDSHAKE_FAILED; return TDSQL_MYSQL_INVALID; }
  n->phase=TDSQL_MYSQL_WAIT_LOGIN; return TDSQL_MYSQL_OK;
}

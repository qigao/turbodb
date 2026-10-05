#include "frontend.h"
#include <gmssl/rand.h>
#include <stdio.h>
#include <string.h>

enum { FRONT_PROTOCOL=10,FRONT_GREETING_SEQUENCE=0 };
enum { FRONT_SEND_GREETING=1,FRONT_SEND_AUTH=2,FRONT_SEND_COMMAND=3 };
static bool front_overlap(const void *first,size_t first_size,const void *second,size_t second_size) {
  const uintptr_t a=(uintptr_t)first,b=(uintptr_t)second;
  if(first_size>UINTPTR_MAX-a || second_size>UINTPTR_MAX-b) return true;
  return a<b+second_size && b<a+first_size;
}
static turbodb_status_t front_error(turbodb_error_t *error,turbodb_status_t status,const char *reason) {
  if(error && error->struct_size>=sizeof(*error)) {
    error->status=status; (void)snprintf(error->message,sizeof(error->message),"MySQL frontend: %s",reason);
  }
  return status;
}
static turbodb_status_t front_codec(tdsql_mysql_status status,turbodb_error_t *error) {
  return status==TDSQL_MYSQL_OK?TURBODB_STATUS_OK:front_error(error,
      status==TDSQL_MYSQL_LIMIT?TURBODB_STATUS_LIMIT_EXCEEDED:
      status==TDSQL_MYSQL_BUSY?TURBODB_STATUS_BUSY:TURBODB_STATUS_INVALID_ARGUMENT,
      "wire codec rejected connection data");
}
static turbodb_status_t front_fail(tdsql_mysql_frontend *front,turbodb_status_t status,
    turbodb_error_t *error,const char *reason) {
  front->failure=status; front->phase=TDSQL_MYSQL_FRONT_FAILED;
  return front_error(error,status,reason);
}
turbodb_status_t tdsql_mysql_frontend_init(tdsql_mysql_frontend *out,const tdsql_mysql_frontend_config *config,
    uint8_t *input,size_t input_capacity,uint8_t *scratch,size_t scratch_capacity,turbodb_error_t *error) {
  if(!out || out->phase || !config || !config->policy || !config->connection_id ||
      !input || !input_capacity || !scratch || scratch_capacity<TDSQL_MYSQL_MIN_REPLY_BYTES ||
      scratch_capacity>UINT32_MAX || front_overlap(input,input_capacity,scratch,scratch_capacity) ||
      !config->server_version.data || !config->server_version.len ||
      config->server_version.len>=MYSQL_WIRE_SERVER_VERSION_CAPACITY ||
      memchr(config->server_version.data,0,config->server_version.len))
    return front_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid connection configuration or storage");
  tdsql_mysql_frontend front={.policy=config->policy,.registry=config->registry,
      .scratch=scratch,.scratch_capacity=scratch_capacity,.phase=TDSQL_MYSQL_FRONT_SEND_GREETING};
  turbodb_status_t status=front_codec(tdsql_mysql_input_init(&front.input,input,input_capacity,1),error);
  if(status!=TURBODB_STATUS_OK) return status;
  const uint32_t capabilities=tdsql_mysql_server_capabilities(true);
  if(tdsql_mysql_negotiation_init(&front.negotiation,capabilities)!=TDSQL_MYSQL_OK)
    return front_error(error,TURBODB_STATUS_INTERNAL_ERROR,"initialize negotiation gate");
  front.greeting=(mysql_wire_greeting_t){.protocol_version=FRONT_PROTOCOL,.connection_id=config->connection_id,
      .capabilities=capabilities,.character_set=TDSQL_MYSQL_BINARY_CHARSET,
      .status_flags=TDSQL_MYSQL_SERVER_AUTOCOMMIT,.auth_plugin_data_length=TDSQL_MYSQL_NONCE_BYTES};
  memcpy(front.greeting.server_version,config->server_version.data,config->server_version.len);
  memcpy(front.greeting.auth_plugin,TDSQL_MYSQL_AUTH_PLUGIN,sizeof(TDSQL_MYSQL_AUTH_PLUGIN));
  if(rand_bytes(front.greeting.auth_plugin_data,TDSQL_MYSQL_NONCE_BYTES)!=1)
    return front_error(error,TURBODB_STATUS_INTERNAL_ERROR,"generate greeting nonce");
  *out=front; return TURBODB_STATUS_OK;
}
static turbodb_status_t front_greeting(tdsql_mysql_frontend *front,tdsql_mysql_output *out,turbodb_error_t *error) {
  uint8_t payload[MYSQL_WIRE_SERVER_VERSION_CAPACITY+MYSQL_WIRE_AUTH_PLUGIN_NAME_CAPACITY+64];
  tdsql_mysql_output body={payload,sizeof(payload),0};
  turbodb_status_t status=front_codec(tdsql_mysql_greeting_encode(&front->greeting,&body),error);
  uint8_t next=0;
  if(status==TURBODB_STATUS_OK) status=front_codec(tdsql_mysql_frame_encode(payload,body.size,FRONT_GREETING_SEQUENCE,out,&next),error);
  if(status==TURBODB_STATUS_OK && next!=1) return front_error(error,TURBODB_STATUS_INTERNAL_ERROR,"invalid greeting sequence");
  return status;
}
turbodb_status_t tdsql_mysql_frontend_emit(tdsql_mysql_frontend *front,tdsql_mysql_output *out,int *action,
    turbodb_error_t *error) {
  if(action) *action=TDSQL_MYSQL_FRONT_NONE;
  if(!front || !out || !out->data || !action) return front_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid reply request");
  if(front->send_pending) return front_error(error,TURBODB_STATUS_BUSY,"reply awaits transport handoff");
  turbodb_status_t status;
  if(front->phase==TDSQL_MYSQL_FRONT_SEND_GREETING) {
    status=front_greeting(front,out,error); front->send_source=FRONT_SEND_GREETING;
  } else if(front->phase==TDSQL_MYSQL_FRONT_SEND_AUTH || front->phase==TDSQL_MYSQL_FRONT_SEND_AUTH_RESULT) {
    status=tdsql_mysql_auth_emit(&front->auth,out,error); front->send_source=FRONT_SEND_AUTH;
  } else if(front->phase==TDSQL_MYSQL_FRONT_SEND_COMMAND) {
    int reply=0; status=tdsql_mysql_dispatch_emit(&front->dispatch,out,&reply,error);
    if(status==TURBODB_STATUS_OK && reply==TDSQL_MYSQL_REPLY_COMPLETE) {
      front->phase=TDSQL_MYSQL_FRONT_COMMAND; *action=TDSQL_MYSQL_FRONT_RECEIVE; return status;
    }
    if(status==TURBODB_STATUS_OK && reply==TDSQL_MYSQL_REPLY_DISCONNECT) {
      front->phase=TDSQL_MYSQL_FRONT_FAILED; *action=TDSQL_MYSQL_FRONT_DISCONNECT; return status;
    }
    if(status==TURBODB_STATUS_OK && reply!=TDSQL_MYSQL_REPLY_PACKET)
      return front_fail(front,TURBODB_STATUS_INTERNAL_ERROR,error,"invalid dispatcher reply state");
    front->send_source=FRONT_SEND_COMMAND;
  } else return front_error(error,TURBODB_STATUS_INVALID_STATE,"no connection reply available");
  if(status!=TURBODB_STATUS_OK) return status;
  front->send_pending=true; *action=TDSQL_MYSQL_FRONT_SEND; return TURBODB_STATUS_OK;
}
turbodb_status_t tdsql_mysql_frontend_acknowledge(tdsql_mysql_frontend *front,int *action,turbodb_error_t *error) {
  if(action) *action=TDSQL_MYSQL_FRONT_NONE;
  if(!front || !action || !front->send_pending) return front_error(error,TURBODB_STATUS_INVALID_STATE,"no reply handoff pending");
  front->send_pending=false; turbodb_status_t status=TURBODB_STATUS_OK;
  if(front->send_source==FRONT_SEND_GREETING) {
    front->phase=TDSQL_MYSQL_FRONT_WAIT_SSL; *action=TDSQL_MYSQL_FRONT_RECEIVE;
  } else if(front->send_source==FRONT_SEND_AUTH) {
    status=tdsql_mysql_auth_acknowledge(&front->auth,error);
    if(status!=TURBODB_STATUS_OK) return front_fail(front,status,error,"authentication reply acknowledgement failed");
    if(front->auth.phase==TDSQL_MYSQL_AUTH_WAIT_PASSWORD) {
      front->phase=TDSQL_MYSQL_FRONT_WAIT_PASSWORD; *action=TDSQL_MYSQL_FRONT_RECEIVE;
    } else if(front->auth.phase==TDSQL_MYSQL_AUTH_FAILED) {
      front->phase=TDSQL_MYSQL_FRONT_FAILED; *action=TDSQL_MYSQL_FRONT_DISCONNECT;
    } else if(front->auth.phase==TDSQL_MYSQL_AUTH_READY) {
      tdsql_connection *connection=NULL; status=tdsql_mysql_auth_take(&front->auth,&connection,error);
      if(status!=TURBODB_STATUS_OK || connection!=front->dispatch.registry.connection)
        return front_fail(front,status==TURBODB_STATUS_OK?TURBODB_STATUS_INTERNAL_ERROR:status,error,"SDK session handoff mismatch");
      front->connection=connection; front->phase=TDSQL_MYSQL_FRONT_COMMAND; *action=TDSQL_MYSQL_FRONT_RECEIVE;
    } else return front_fail(front,TURBODB_STATUS_INTERNAL_ERROR,error,"unexpected authentication ACK state");
  } else if(front->send_source==FRONT_SEND_COMMAND) {
    status=tdsql_mysql_dispatch_acknowledge(&front->dispatch,error);
    if(status!=TURBODB_STATUS_OK) return front_fail(front,status,error,"dispatcher reply acknowledgement failed");
    front->phase=TDSQL_MYSQL_FRONT_SEND_COMMAND; *action=TDSQL_MYSQL_FRONT_SEND;
  } else return front_fail(front,TURBODB_STATUS_INTERNAL_ERROR,error,"unknown reply owner");
  front->send_source=0; return TURBODB_STATUS_OK;
}
static turbodb_status_t front_release(tdsql_mysql_frontend *front,uint8_t next,turbodb_error_t *error) {
  return front_codec(tdsql_mysql_input_release(&front->input,next),error);
}
static turbodb_status_t front_auth_dispatch(tdsql_mysql_frontend *front,turbodb_error_t *error) {
  if(front->auth.error_code || !front->auth.connection) return TURBODB_STATUS_OK;
  const bool deprecated=(front->negotiation.header.capabilities&MYSQL_WIRE_CLIENT_DEPRECATE_EOF)!=0;
  turbodb_status_t status=tdsql_mysql_dispatch_init(&front->dispatch,front->auth.connection,&front->registry,
      deprecated,front->scratch,front->scratch_capacity,error);
  if(status!=TURBODB_STATUS_OK) {
    turbodb_status_t rejected=tdsql_mysql_auth_reject(&front->auth,status,error);
    if(rejected!=TURBODB_STATUS_OK) return rejected;
  }
  return TURBODB_STATUS_OK;
}
turbodb_status_t tdsql_mysql_frontend_feed(tdsql_mysql_frontend *front,const uint8_t *bytes,size_t size,
    size_t *consumed,int *action,turbodb_error_t *error) {
  if(consumed) *consumed=0; if(action) *action=TDSQL_MYSQL_FRONT_NONE;
  if(!front || !consumed || !action || (!bytes && size)) return front_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid network input");
  if(front->send_pending || front->phase==TDSQL_MYSQL_FRONT_SEND_GREETING ||
      front->phase==TDSQL_MYSQL_FRONT_SEND_AUTH || front->phase==TDSQL_MYSQL_FRONT_SEND_AUTH_RESULT ||
      front->phase==TDSQL_MYSQL_FRONT_SEND_COMMAND)
    return front_error(error,TURBODB_STATUS_BUSY,"network input while reply active");
  if(front->phase!=TDSQL_MYSQL_FRONT_WAIT_SSL && front->phase!=TDSQL_MYSQL_FRONT_WAIT_LOGIN &&
      front->phase!=TDSQL_MYSQL_FRONT_WAIT_PASSWORD && front->phase!=TDSQL_MYSQL_FRONT_COMMAND)
    return front_fail(front,TURBODB_STATUS_INVALID_STATE,error,"network input in invalid connection phase");
  tdsql_mysql_status wire=tdsql_mysql_input_feed(&front->input,bytes,size,consumed);
  if(wire==TDSQL_MYSQL_NEED_MORE) { *action=TDSQL_MYSQL_FRONT_RECEIVE; return TURBODB_STATUS_OK; }
  if(wire!=TDSQL_MYSQL_OK) return front_fail(front,front_codec(wire,error),error,"invalid framed request");
  if(*consumed!=size)
    return front_fail(front,TURBODB_STATUS_INVALID_ARGUMENT,error,"pipelined request bytes rejected");
  mysql_wire_bytes_t message={0};
  turbodb_status_t status=front_codec(tdsql_mysql_input_message(&front->input,&message),error);
  const uint8_t next=front->input.stream.expected_sequence;
  if(status!=TURBODB_STATUS_OK) return front_fail(front,status,error,"read complete request");
  if(front->phase==TDSQL_MYSQL_FRONT_WAIT_SSL) {
    wire=tdsql_mysql_negotiation_accept(&front->negotiation,message.data,message.length,1,false,NULL);
    status=front_codec(wire,error);
    if(status==TURBODB_STATUS_OK) status=front_release(front,next,error);
    if(status!=TURBODB_STATUS_OK) return front_fail(front,status,error,"reject SSLRequest");
    front->phase=TDSQL_MYSQL_FRONT_WAIT_TLS; *action=TDSQL_MYSQL_FRONT_START_TLS;
  } else if(front->phase==TDSQL_MYSQL_FRONT_WAIT_LOGIN) {
    tdsql_mysql_login login={0};
    wire=tdsql_mysql_negotiation_accept(&front->negotiation,message.data,message.length,2,true,&login);
    status=front_codec(wire,error);
    if(status==TURBODB_STATUS_OK) status=tdsql_mysql_auth_begin(&front->auth,front->policy,&front->negotiation,&login,error);
    if(status==TURBODB_STATUS_OK) status=front_release(front,4,error);
    if(status!=TURBODB_STATUS_OK) return front_fail(front,status,error,"reject encrypted login");
    front->phase=TDSQL_MYSQL_FRONT_SEND_AUTH; *action=TDSQL_MYSQL_FRONT_SEND;
  } else if(front->phase==TDSQL_MYSQL_FRONT_WAIT_PASSWORD) {
    status=tdsql_mysql_auth_accept(&front->auth,message.data,message.length,4,front->tls,error);
    if(status==TURBODB_STATUS_OK) status=front_auth_dispatch(front,error);
    if(status==TURBODB_STATUS_OK) status=front_release(front,0,error);
    if(status!=TURBODB_STATUS_OK) return front_fail(front,status,error,"full authentication failed");
    front->phase=TDSQL_MYSQL_FRONT_SEND_AUTH_RESULT; *action=TDSQL_MYSQL_FRONT_SEND;
  } else {
    status=tdsql_mysql_dispatch_accept(&front->dispatch,message.data,message.length,next,error);
    if(status==TURBODB_STATUS_OK) status=front_release(front,0,error);
    if(status!=TURBODB_STATUS_OK) return front_fail(front,status,error,"command admission failed");
    front->phase=TDSQL_MYSQL_FRONT_SEND_COMMAND; *action=TDSQL_MYSQL_FRONT_SEND;
  }
  return TURBODB_STATUS_OK;
}
turbodb_status_t tdsql_mysql_frontend_tls_ready(tdsql_mysql_frontend *front,int *action,turbodb_error_t *error) {
  if(action) *action=TDSQL_MYSQL_FRONT_NONE;
  if(!front || !action) return front_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid TLS completion");
  if(front->phase!=TDSQL_MYSQL_FRONT_WAIT_TLS || front->tls)
    return front_fail(front,TURBODB_STATUS_INVALID_STATE,error,"unexpected TLS completion");
  turbodb_status_t status=front_codec(tdsql_mysql_negotiation_tls_ready(&front->negotiation),error);
  if(status!=TURBODB_STATUS_OK) return front_fail(front,status,error,"TLS negotiation gate failed");
  front->tls=true; front->phase=TDSQL_MYSQL_FRONT_WAIT_LOGIN; *action=TDSQL_MYSQL_FRONT_RECEIVE;
  return TURBODB_STATUS_OK;
}
turbodb_status_t tdsql_mysql_frontend_dispose(tdsql_mysql_frontend *front,turbodb_error_t *error) {
  if(!front) return TURBODB_STATUS_OK;
  if(front->cleanup_failure!=TURBODB_STATUS_OK)
    return front_error(error,front->cleanup_failure,"connection cleanup is quarantined");
  front->phase=TDSQL_MYSQL_FRONT_CLOSED;
  turbodb_status_t status=tdsql_mysql_dispatch_dispose(&front->dispatch,error);
  if(status==TURBODB_STATUS_BUSY) return status;
  if(front->connection) {
    turbodb_status_t closed=tdsql_connection_close(front->connection,status==TURBODB_STATUS_OK?error:NULL);
    if(closed==TURBODB_STATUS_OK) front->connection=NULL;
    if(status==TURBODB_STATUS_OK) status=closed;
  } else {
    turbodb_status_t closed=tdsql_mysql_auth_dispose(&front->auth,status==TURBODB_STATUS_OK?error:NULL);
    if(status==TURBODB_STATUS_OK) status=closed;
  }
  if(status!=TURBODB_STATUS_OK) {
    if(status!=TURBODB_STATUS_BUSY) front->cleanup_failure=status;
    return status;
  }
  *front=(tdsql_mysql_frontend){0}; return TURBODB_STATUS_OK;
}

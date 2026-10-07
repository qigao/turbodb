#include "server.h"
#include <cmeta_buffer.h>
#include <stdio.h>
#include <string.h>

typedef struct tdsql_mysql_server_slot {
  tdsql_mysql_server *server;
  tdsql_mysql_frontend frontend;
  cnet_connection connection;
  mem_buffer_t *workspace;
  size_t send_bytes;
  turbodb_error_t error;
  turbodb_status_t cleanup_status;
  bool active,tls_started,tls_ready,send_pending,closing,cleanup_pending,quarantined;
} tdsql_mysql_server_slot;

static turbodb_status_t server_error(turbodb_error_t *error,turbodb_status_t status,const char *message) {
  if(error) {
    turbodb_error_init(error); error->status=status;
    (void)snprintf(error->message,sizeof(error->message),"%s",message?message:"");
  }
  return status;
}
static turbodb_status_t server_network(tdsql_mysql_server *server,turbodb_error_t *error,int status,
    const char *operation) {
  if(server) server->network_failure=status;
  if(error) {
    turbodb_error_init(error); error->status=TURBODB_STATUS_CONNECTION_ERROR;
    (void)snprintf(error->message,sizeof(error->message),"CNet %s failed: %d",operation,status);
  }
  return TURBODB_STATUS_CONNECTION_ERROR;
}
static bool checked_add(size_t a,size_t b,size_t *out) {
  if(a>SIZE_MAX-b) return false; *out=a+b; return true;
}
static bool same_connection(cnet_connection a,cnet_connection b) {
  return a.slot==b.slot && a.generation==b.generation;
}
static void slot_close(tdsql_mysql_server_slot *slot) {
  if(!slot || !slot->active || slot->closing) return;
  const int status=cnet_close(&slot->server->client,slot->connection);
  if(status==SALTS_OK || status==SALTS_EALREADY || status==SALTS_ENOENT || status==SALTS_ESHUTDOWN)
    slot->closing=true;
  else {
    slot->server->network_failure=status; ++slot->server->transport_failures; slot->closing=true;
  }
}
static void slot_protocol_failure(tdsql_mysql_server_slot *slot,turbodb_status_t status) {
  if(!slot || !slot->active) return;
  if(slot->error.status==TURBODB_STATUS_OK)
    server_error(&slot->error,status,"MySQL frontend transport action failed");
  ++slot->server->protocol_failures; slot_close(slot);
}
static void slot_drive(tdsql_mysql_server_slot *slot,int action) {
  while(slot && slot->active && !slot->closing) {
    int network=SALTS_OK; turbodb_status_t status=TURBODB_STATUS_OK;
    if(action==TDSQL_MYSQL_FRONT_SEND) {
      mem_buffer_t *packet=mem_get_buffer(mem_global(),slot->server->output_bytes);
      if(!packet) { slot_protocol_failure(slot,TURBODB_STATUS_OUT_OF_MEMORY); return; }
      tdsql_mysql_output output={(uint8_t *)mem_buffer_data(packet),slot->server->output_bytes,0};
      int next=TDSQL_MYSQL_FRONT_NONE;
      status=tdsql_mysql_frontend_emit(&slot->frontend,&output,&next,&slot->error);
      if(status!=TURBODB_STATUS_OK) { mem_buffer_release(packet); slot_protocol_failure(slot,status); return; }
      if(next!=TDSQL_MYSQL_FRONT_SEND) { mem_buffer_release(packet); action=next; continue; }
      if(output.size==0 || output.size>slot->server->output_bytes) {
        mem_buffer_release(packet); slot_protocol_failure(slot,TURBODB_STATUS_INTERNAL_ERROR); return;
      }
      mem_set_used(packet,output.size); slot->send_bytes=output.size;
      network=cnet_send_buffer(&slot->server->client,slot->connection,packet);
      mem_buffer_release(packet);
      if(network!=SALTS_OK) {
        slot->server->network_failure=network; ++slot->server->transport_failures; slot_close(slot); return;
      }
      slot->send_pending=true; return;
    }
    if(action==TDSQL_MYSQL_FRONT_RECEIVE) {
      network=cnet_receive(&slot->server->client,slot->connection,1u);
      if(network!=SALTS_OK) {
        slot->server->network_failure=network; ++slot->server->transport_failures; slot_close(slot);
      }
      return;
    }
    if(action==TDSQL_MYSQL_FRONT_START_TLS) {
      network=cnet_start_tls_server(&slot->server->client,slot->connection,&slot->server->tls);
      if(network!=SALTS_OK) {
        slot->server->network_failure=network; ++slot->server->transport_failures; slot_close(slot);
      } else slot->tls_started=true;
      return;
    }
    if(action==TDSQL_MYSQL_FRONT_DISCONNECT) { slot_close(slot); return; }
    if(action==TDSQL_MYSQL_FRONT_NONE) return;
    slot_protocol_failure(slot,TURBODB_STATUS_INTERNAL_ERROR); return;
  }
}
static void slot_finish(tdsql_mysql_server_slot *slot) {
  if(!slot || !slot->active || slot->cleanup_pending || slot->quarantined) return;
  turbodb_error_t cleanup; turbodb_error_init(&cleanup);
  const turbodb_status_t status=tdsql_mysql_frontend_dispose(&slot->frontend,&cleanup);
  slot->cleanup_status=status;
  if(status==TURBODB_STATUS_BUSY) { slot->cleanup_pending=true; return; }
  if(status!=TURBODB_STATUS_OK) {
    slot->quarantined=true; ++slot->server->cleanup_failures; return;
  }
  if(slot->workspace) { mem_buffer_release(slot->workspace); slot->workspace=NULL; }
  --slot->server->active; ++slot->server->closed;
  tdsql_mysql_server *server=slot->server;
  *slot=(tdsql_mysql_server_slot){.server=server};
}
static void slot_state(void *user,cnet_connection connection,cnet_connection_state next,const cnet_error *error) {
  tdsql_mysql_server_slot *slot=user;
  if(!slot || !slot->active || !same_connection(slot->connection,connection)) return;
  if(next==CNET_CONNECTION_CONNECTED) {
    int action=TDSQL_MYSQL_FRONT_SEND;
    if(slot->tls_started) {
      if(slot->tls_ready) { slot_protocol_failure(slot,TURBODB_STATUS_INVALID_STATE); return; }
      const turbodb_status_t status=tdsql_mysql_frontend_tls_ready(&slot->frontend,&action,&slot->error);
      if(status!=TURBODB_STATUS_OK) { slot_protocol_failure(slot,status); return; }
      slot->tls_ready=true;
    }
    slot_drive(slot,action); return;
  }
  if(next==CNET_CONNECTION_FAILED) {
    ++slot->server->transport_failures;
    if(error) slot->server->network_failure=error->status;
  }
  if(next==CNET_CONNECTION_CLOSED || next==CNET_CONNECTION_FAILED) slot_finish(slot);
}
static void slot_receive(void *user,cnet_connection connection,const cnet_receive_view *view) {
  tdsql_mysql_server_slot *slot=user;
  if(!slot || !slot->active || !same_connection(slot->connection,connection) || !view ||
      view->kind!=CNET_MESSAGE_BYTES || (!view->data && view->size)) {
    slot_protocol_failure(slot,TURBODB_STATUS_INVALID_ARGUMENT); return;
  }
  size_t consumed=0; int action=TDSQL_MYSQL_FRONT_NONE;
  const turbodb_status_t status=tdsql_mysql_frontend_feed(&slot->frontend,view->data,view->size,
      &consumed,&action,&slot->error);
  if(status!=TURBODB_STATUS_OK || consumed!=view->size) {
    slot_protocol_failure(slot,status==TURBODB_STATUS_OK?TURBODB_STATUS_INVALID_ARGUMENT:status); return;
  }
  slot_drive(slot,action);
}
static void slot_send(void *user,cnet_connection connection,size_t size) {
  tdsql_mysql_server_slot *slot=user;
  if(!slot || !slot->active || !same_connection(slot->connection,connection) || !slot->send_pending ||
      size!=slot->send_bytes) { slot_protocol_failure(slot,TURBODB_STATUS_INVALID_STATE); return; }
  slot->send_pending=false; slot->send_bytes=0; int action=TDSQL_MYSQL_FRONT_NONE;
  const turbodb_status_t status=tdsql_mysql_frontend_acknowledge(&slot->frontend,&action,&slot->error);
  if(status!=TURBODB_STATUS_OK) { slot_protocol_failure(slot,status); return; }
  slot_drive(slot,action);
}
static cnet_observer slot_observer(tdsql_mysql_server_slot *slot) {
  return (cnet_observer){.on_state=slot_state,.on_receive=slot_receive,.user=slot,.on_send=slot_send};
}
static void server_partial_release(tdsql_mysql_server *server) {
  if(!server) return;
  if(server->listener.impl) { (void)cnet_listener_close(&server->listener); (void)cnet_listener_destroy(&server->listener); }
  if(server->client.impl) { (void)cnet_client_stop(&server->client,0); (void)cnet_client_destroy(&server->client); }
  if(server->tls.impl) (void)cnet_tls_server_destroy(&server->tls);
  if(server->slots.initialized) vec_destroy(&server->slots);
  *server=(tdsql_mysql_server){0};
}
turbodb_status_t tdsql_mysql_server_init(tdsql_mysql_server *out,const tdsql_mysql_server_config *config,
    turbodb_error_t *error) {
  if(!out || !config || out->initialized || !config->policy || !config->listener.host ||
      !config->server_version.data || config->server_version.len==0 ||
      config->server_version.len>=MYSQL_WIRE_SERVER_VERSION_CAPACITY ||
      memchr(config->server_version.data,0,config->server_version.len) ||
      config->max_connections==0 || config->max_connections>TDSQL_MYSQL_SERVER_MAX_CONNECTIONS ||
      config->transport.connection_capacity<config->max_connections ||
      config->input_bytes<TDSQL_MYSQL_SERVER_MIN_INPUT_BYTES ||
      config->scratch_bytes<TDSQL_MYSQL_MIN_REPLY_BYTES || config->scratch_bytes>UINT32_MAX ||
      config->output_bytes<TDSQL_MYSQL_SERVER_MIN_OUTPUT_BYTES ||
      config->output_bytes>config->transport.max_send_bytes ||
      config->transport.read_timeout_ms==0 || config->transport.write_timeout_ms==0 ||
      config->transport.tls_io_buffer_bytes<CNET_TLS_MIN_IO_BUFFER_BYTES ||
      config->transport.tls_handshake_timeout_ms==0 ||
      config->listener.backlog==0 || config->tls.size!=sizeof(cnet_tls_server_config))
    return server_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid bounded MySQL server configuration");
  tdsql_mysql_auth_policy validated={0};
  turbodb_status_t policy_status=tdsql_mysql_auth_policy_init(&validated,config->policy,error);
  if(policy_status!=TURBODB_STATUS_OK) return policy_status;
  size_t workspace=0;
  if(!checked_add(config->input_bytes,config->scratch_bytes,&workspace))
    return server_error(error,TURBODB_STATUS_OUT_OF_RANGE,"MySQL connection workspace overflows size_t");
  (void)workspace;
  tdsql_mysql_server server={.policy=config->policy,.registry=config->registry,
    .server_version=config->server_version,.max_connections=config->max_connections,
    .input_bytes=config->input_bytes,.scratch_bytes=config->scratch_bytes,
    .output_bytes=config->output_bytes,.next_connection_id=1};
  if(vec_init_bytes(&server.slots,sizeof(tdsql_mysql_server_slot),_Alignof(tdsql_mysql_server_slot),
      config->max_connections)!=STL_OK || vec_resize(&server.slots,config->max_connections)!=STL_OK) {
    server_partial_release(&server); return server_error(error,TURBODB_STATUS_OUT_OF_MEMORY,"allocate MySQL connection slots");
  }
  for(size_t i=0;i<config->max_connections;++i)
    *(tdsql_mysql_server_slot *)vec_at(&server.slots,i)=(tdsql_mysql_server_slot){.server=&server};
  int network=cnet_tls_server_init(&server.tls,&config->tls);
  if(network==SALTS_OK) network=cnet_client_init(&server.client,&config->transport);
  if(network==SALTS_OK) network=cnet_listener_init(&server.listener,&config->listener);
  if(network!=SALTS_OK) {
    server_partial_release(&server); return server_network(NULL,error,network,"server initialization");
  }
  /* Vector elements carried a temporary stack owner while construction was
   * private. Rebind only after every fallible owner is initialized. */
  server.initialized=true; *out=server;
  for(size_t i=0;i<out->max_connections;++i)
    ((tdsql_mysql_server_slot *)vec_at(&out->slots,i))->server=out;
  return TURBODB_STATUS_OK;
}
static tdsql_mysql_server_slot *server_free_slot(tdsql_mysql_server *server) {
  for(size_t i=0;i<server->max_connections;++i) {
    tdsql_mysql_server_slot *slot=vec_at(&server->slots,i);
    if(!slot->active && !slot->quarantined) return slot;
  }
  return NULL;
}
static turbodb_status_t server_accept(tdsql_mysql_server *server,turbodb_error_t *error) {
  int ready=0,network=cnet_listener_wait(&server->listener,0,&ready);
  if(network!=SALTS_OK) return server_network(server,error,network,"listener wait");
  if(!ready) return TURBODB_STATUS_OK;
  tdsql_mysql_server_slot *slot=server_free_slot(server);
  if(!slot) return TURBODB_STATUS_OK;
  size_t workspace_size=server->input_bytes+server->scratch_bytes;
  mem_buffer_t *workspace=mem_get_buffer(mem_global(),workspace_size);
  if(!workspace) return server_error(error,TURBODB_STATUS_OUT_OF_MEMORY,"allocate MySQL connection workspace");
  if(server->next_connection_id==0) { mem_buffer_release(workspace); return server_error(error,
      TURBODB_STATUS_LIMIT_EXCEEDED,"MySQL connection identifier space exhausted"); }
  turbodb_error_init(&slot->error); slot->workspace=workspace; slot->active=true;
  uint8_t *bytes=(uint8_t *)mem_buffer_data(workspace);
  tdsql_mysql_frontend_config front=tdsql_mysql_frontend_config_default();
  front.policy=server->policy; front.registry=server->registry; front.server_version=server->server_version;
  front.connection_id=server->next_connection_id++;
  turbodb_status_t status=tdsql_mysql_frontend_init(&slot->frontend,&front,bytes,server->input_bytes,
      bytes+server->input_bytes,server->scratch_bytes,&slot->error);
  if(status!=TURBODB_STATUS_OK) {
    slot->active=false; slot->workspace=NULL; mem_buffer_release(workspace); return status;
  }
  cnet_observer observer=slot_observer(slot);
  network=cnet_listener_accept(&server->listener,&server->client,&observer,&slot->connection);
  if(network!=SALTS_OK) {
    (void)tdsql_mysql_frontend_dispose(&slot->frontend,NULL); slot->active=false; slot->workspace=NULL;
    mem_buffer_release(workspace);
    if(network==SALTS_ETIMEDOUT) return TURBODB_STATUS_OK;
    return server_network(server,error,network,"listener accept");
  }
  ++server->active; ++server->accepted; return TURBODB_STATUS_OK;
}
static void server_retry_cleanup(tdsql_mysql_server *server) {
  for(size_t i=0;i<server->max_connections;++i) {
    tdsql_mysql_server_slot *slot=vec_at(&server->slots,i);
    if(!slot->cleanup_pending || slot->quarantined) continue;
    slot->cleanup_pending=false; slot_finish(slot);
  }
}
turbodb_status_t tdsql_mysql_server_poll(tdsql_mysql_server *server,uint32_t timeout_ms,size_t *events,
    turbodb_error_t *error) {
  if(events) *events=0;
  if(!server || !server->initialized || server->stopping || !events)
    return server_error(error,TURBODB_STATUS_INVALID_STATE,"MySQL server is not accepting progress");
  turbodb_status_t status=server_accept(server,error); if(status!=TURBODB_STATUS_OK) return status;
  const int network=cnet_client_poll(&server->client,timeout_ms,events);
  server_retry_cleanup(server);
  if(network!=SALTS_OK) return server_network(server,error,network,"poll");
  return TURBODB_STATUS_OK;
}
turbodb_status_t tdsql_mysql_server_port(const tdsql_mysql_server *server,uint16_t *port,turbodb_error_t *error) {
  if(port) *port=0;
  if(!server || !server->initialized || !port)
    return server_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid MySQL server port query");
  const int status=cnet_listener_port(&server->listener,port);
  return status==SALTS_OK?TURBODB_STATUS_OK:server_network(NULL,error,status,"listener port");
}
turbodb_status_t tdsql_mysql_server_stop(tdsql_mysql_server *server,uint32_t timeout_ms,turbodb_error_t *error) {
  if(!server || !server->initialized) return TURBODB_STATUS_OK;
  server->stopping=true;
  int first=SALTS_OK,status=SALTS_OK;
  if(server->listener.impl) {
    status=cnet_listener_close(&server->listener);
    if(status!=SALTS_OK && status!=SALTS_EALREADY) first=status;
  }
  status=cnet_client_stop(&server->client,timeout_ms);
  if(status==SALTS_ETIMEDOUT) {
    server->network_failure=status;
    return server_error(error,TURBODB_STATUS_BUSY,"MySQL server stop timed out; retry stop");
  }
  if(first==SALTS_OK && status!=SALTS_OK) first=status;
  server_retry_cleanup(server);
  if(server->active!=0 || server->cleanup_failures!=0)
    return server_error(error,TURBODB_STATUS_CLEANUP_FAILED,"MySQL connection cleanup did not quiesce");
  status=cnet_client_destroy(&server->client); if(first==SALTS_OK && status!=SALTS_OK) first=status;
  status=cnet_listener_destroy(&server->listener); if(first==SALTS_OK && status!=SALTS_OK) first=status;
  status=cnet_tls_server_destroy(&server->tls); if(first==SALTS_OK && status!=SALTS_OK) first=status;
  if(first!=SALTS_OK) return server_network(server,error,first,"server teardown");
  vec_destroy(&server->slots); *server=(tdsql_mysql_server){0}; return TURBODB_STATUS_OK;
}

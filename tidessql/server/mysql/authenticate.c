#include "authenticate.h"
#include <stdio.h>
#include <string.h>

enum { AUTH_FULL_SEQUENCE=3,AUTH_DENIED=1045,AUTH_DB_DENIED=1044,AUTH_DB_UNKNOWN=1049,
       AUTH_CONNECTION_LIMIT=1040,AUTH_MEMORY=1037,AUTH_INTERNAL=1105 };
static turbodb_status_t auth_error(turbodb_error_t *error,turbodb_status_t status,const char *reason) {
  if(error && error->struct_size>=sizeof(*error)) {
    error->status=status; (void)snprintf(error->message,sizeof(error->message),"MySQL authentication: %s",reason);
  }
  return status;
}
static bool auth_name(vstr name,size_t limit) {
  return name.data && name.len && name.len<=limit && !memchr(name.data,0,name.len);
}
static bool auth_same(vstr first,vstr second) {
  return first.len==second.len && (!first.len || !memcmp(first.data,second.data,first.len));
}
turbodb_status_t tdsql_mysql_auth_policy_init(tdsql_mysql_auth_policy *out,const tdsql_mysql_auth_policy *config,
    turbodb_error_t *error) {
  if(!out || !config || !config->accounts || !config->databases || !config->account_count ||
      !config->database_count || !config->max_accounts || config->max_accounts>TDSQL_MYSQL_MAX_ACCOUNTS)
    return auth_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid policy spans or account bound");
  if(config->account_count>config->max_accounts || config->database_count>TDSQL_MYSQL_MAX_DATABASES)
    return auth_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"policy capacity exceeded");
  uint64_t mask=config->database_count==TDSQL_MYSQL_MAX_DATABASES?UINT64_MAX:
      (UINT64_C(1)<<config->database_count)-1;
  for(size_t i=0;i<config->database_count;++i) {
    const tdsql_mysql_database_binding *database=&config->databases[i];
    if(!database->database || !auth_name(database->name,TDSQL_MYSQL_DATABASE_BYTES))
      return auth_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid database binding");
    for(size_t j=0;j<i;++j) if(auth_same(database->name,config->databases[j].name))
      return auth_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"duplicate logical database");
  }
  for(size_t i=0;i<config->account_count;++i) {
    const tdsql_mysql_account *account=&config->accounts[i];
    if(!auth_name(account->username,TDSQL_MYSQL_USERNAME_BYTES) || !account->databases ||
        (account->databases & ~mask) || account->default_database>=config->database_count ||
        !(account->databases & (UINT64_C(1)<<account->default_database)) ||
        account->password.iterations<TDSQL_MYSQL_PASSWORD_MIN_ITERATIONS ||
        account->password.iterations>TDSQL_MYSQL_PASSWORD_MAX_ITERATIONS ||
        account->password.iterations!=config->accounts[0].password.iterations)
      return auth_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid account grants or verifier");
    for(size_t j=0;j<i;++j) if(auth_same(account->username,config->accounts[j].username))
      return auth_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"duplicate account");
  }
  *out=*config; return TURBODB_STATUS_OK;
}
turbodb_status_t tdsql_mysql_auth_begin(tdsql_mysql_auth *out,const tdsql_mysql_auth_policy *policy,
    const tdsql_mysql_negotiation *gate,const tdsql_mysql_login *login,turbodb_error_t *error) {
  if(!out || !policy || !gate || !login || !policy->accounts || !policy->databases ||
      !policy->account_count || !policy->database_count)
    return auth_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid authentication input");
  if(out->phase || out->connection) return auth_error(error,TURBODB_STATUS_BUSY,"authentication owner already active");
  if(gate->phase!=TDSQL_MYSQL_WAIT_CREDENTIALS || login->header.capabilities!=gate->header.capabilities ||
      login->header.max_packet_size!=gate->header.max_packet_size || login->header.character_set!=gate->header.character_set)
    return auth_error(error,TURBODB_STATUS_INVALID_STATE,"TLS login negotiation required");
  vstr user={(const char *)login->username.data,login->username.length};
  vstr name={(const char *)login->database.data,login->database.length};
  if(!auth_name(user,TDSQL_MYSQL_USERNAME_BYTES) || (name.len && !auth_name(name,TDSQL_MYSQL_DATABASE_BYTES)))
    return auth_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid login names");
  tdsql_mysql_auth auth={.policy=policy,.account=SIZE_MAX,.database=SIZE_MAX,
      .sequence=AUTH_FULL_SEQUENCE,.phase=TDSQL_MYSQL_AUTH_SEND_FULL};
  for(size_t i=0;i<policy->account_count;++i)
    if(auth_same(user,policy->accounts[i].username)) { auth.account=i; break; }
  if(!name.len) {
    if(auth.account!=SIZE_MAX) auth.database=policy->accounts[auth.account].default_database;
  } else {
    for(size_t i=0;i<policy->database_count;++i)
      if(auth_same(name,policy->databases[i].name)) { auth.database=i; break; }
  }
  *out=auth; return TURBODB_STATUS_OK;
}
static void auth_plan_error(tdsql_mysql_auth *auth,turbodb_status_t status,uint16_t code) {
  auth->failure=status; auth->error_code=code; auth->phase=TDSQL_MYSQL_AUTH_SEND_RESULT;
}
static uint16_t auth_sdk_code(turbodb_status_t status) {
  return status==TURBODB_STATUS_LIMIT_EXCEEDED?AUTH_CONNECTION_LIMIT:
      status==TURBODB_STATUS_OUT_OF_MEMORY?AUTH_MEMORY:AUTH_INTERNAL;
}
static turbodb_status_t auth_open(tdsql_mysql_auth *auth,turbodb_error_t *error) {
  turbodb_status_t status=tdsql_database_connect(auth->policy->databases[auth->database].database,&auth->connection,error);
  tdsql_session_state state=tdsql_session_state_default();
  if(status==TURBODB_STATUS_OK) status=tdsql_connection_state(auth->connection,&state,error);
  if(status==TURBODB_STATUS_OK && (state.failure!=TURBODB_STATUS_OK || state.busy || state.in_transaction))
    status=auth_error(error,TURBODB_STATUS_INVALID_STATE,"new SQL session is not idle");
  if(status!=TURBODB_STATUS_OK) { auth_plan_error(auth,status,auth_sdk_code(status)); return TURBODB_STATUS_OK; }
  auth->ok=(mysql_wire_ok_packet_t){.status_flags=state.autocommit?TDSQL_MYSQL_SERVER_AUTOCOMMIT:0,
      .warnings=(uint16_t)(state.warning_count>UINT16_MAX?UINT16_MAX:state.warning_count)};
  auth->phase=TDSQL_MYSQL_AUTH_SEND_RESULT; return TURBODB_STATUS_OK;
}
turbodb_status_t tdsql_mysql_auth_accept(tdsql_mysql_auth *auth,const uint8_t *payload,size_t size,uint8_t sequence,
    bool tls,turbodb_error_t *error) {
  if(!auth || !auth->policy) return auth_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"missing authentication owner");
  if(auth->phase!=TDSQL_MYSQL_AUTH_WAIT_PASSWORD || !tls || sequence!=auth->sequence) {
    auth->phase=TDSQL_MYSQL_AUTH_FAILED;
    return auth_error(error,TURBODB_STATUS_INVALID_STATE,"unexpected full-auth sequence or TLS state");
  }
  bool verified=false;
  /* Work-only comparison for unknown users never grants account zero. */
  const tdsql_mysql_account *account=&auth->policy->accounts[auth->account==SIZE_MAX?0:auth->account];
  turbodb_status_t status=tdsql_mysql_password_verify(&account->password,payload,size,true,&verified,error);
  auth->sequence=(uint8_t)(sequence+1);
  if(status!=TURBODB_STATUS_OK) {
    const bool shape_error=status==TURBODB_STATUS_INVALID_ARGUMENT || status==TURBODB_STATUS_LIMIT_EXCEEDED;
    auth_plan_error(auth,status,shape_error?AUTH_DENIED:auth_sdk_code(status));
    return TURBODB_STATUS_OK;
  }
  if(!verified || auth->account==SIZE_MAX) {
    auth_plan_error(auth,TURBODB_STATUS_CONNECTION_ERROR,AUTH_DENIED); return TURBODB_STATUS_OK;
  }
  if(auth->database==SIZE_MAX) {
    auth_plan_error(auth,TURBODB_STATUS_CONNECTION_ERROR,AUTH_DB_UNKNOWN); return TURBODB_STATUS_OK;
  }
  if(!(account->databases & (UINT64_C(1)<<auth->database))) {
    auth_plan_error(auth,TURBODB_STATUS_CONNECTION_ERROR,AUTH_DB_DENIED); return TURBODB_STATUS_OK;
  }
  return auth_open(auth,error);
}
static const char *auth_sqlstate(uint16_t code) {
  return code==AUTH_DENIED?"28000":(code==AUTH_DB_DENIED || code==AUTH_DB_UNKNOWN)?"42000":
      code==AUTH_CONNECTION_LIMIT?"08004":code==AUTH_MEMORY?"HY001":"HY000";
}
static const char *auth_reason(uint16_t code) {
  switch(code) {
    case AUTH_DENIED: return "Access denied";
    case AUTH_DB_DENIED: return "Database access denied";
    case AUTH_DB_UNKNOWN: return "Unknown logical database";
    case AUTH_CONNECTION_LIMIT: return "SQL session capacity exceeded";
    case AUTH_MEMORY: return "Authentication resources exhausted";
    default: return "Authentication initialization failed";
  }
}
turbodb_status_t tdsql_mysql_auth_emit(tdsql_mysql_auth *auth,tdsql_mysql_output *out,turbodb_error_t *error) {
  if(!auth || !out || !out->data) return auth_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"owned reply output required");
  if(auth->emitted) return auth_error(error,TURBODB_STATUS_BUSY,"authentication reply awaits handoff");
  if(auth->phase!=TDSQL_MYSQL_AUTH_SEND_FULL && auth->phase!=TDSQL_MYSQL_AUTH_SEND_RESULT)
    return auth_error(error,TURBODB_STATUS_INVALID_STATE,"no authentication reply available");
  uint8_t bytes[TDSQL_MYSQL_AUTH_PAYLOAD_BYTES]; tdsql_mysql_output payload={bytes,sizeof(bytes),0};
  tdsql_mysql_status status;
  if(auth->phase==TDSQL_MYSQL_AUTH_SEND_FULL) status=tdsql_mysql_full_auth_encode(&payload);
  else if(auth->error_code) {
    const char *reason=auth_reason(auth->error_code); mysql_wire_bytes_t message={(const uint8_t *)reason,strlen(reason)};
    status=tdsql_mysql_err_encode(auth->error_code,auth_sqlstate(auth->error_code),message,&payload);
  } else status=tdsql_mysql_ok_encode(&auth->ok,false,&payload);
  uint8_t next=auth->sequence;
  if(status==TDSQL_MYSQL_OK) status=tdsql_mysql_frame_encode(bytes,payload.size,auth->sequence,out,&next);
  if(status!=TDSQL_MYSQL_OK) return auth_error(error,status==TDSQL_MYSQL_LIMIT?TURBODB_STATUS_LIMIT_EXCEEDED:
      TURBODB_STATUS_INTERNAL_ERROR,"authentication reply encoding failed");
  auth->next_sequence=next; auth->emitted=true; return TURBODB_STATUS_OK;
}
turbodb_status_t tdsql_mysql_auth_acknowledge(tdsql_mysql_auth *auth,turbodb_error_t *error) {
  if(!auth || !auth->emitted || (auth->phase!=TDSQL_MYSQL_AUTH_SEND_FULL && auth->phase!=TDSQL_MYSQL_AUTH_SEND_RESULT))
    return auth_error(error,TURBODB_STATUS_INVALID_STATE,"no authentication handoff pending");
  auth->sequence=auth->next_sequence; auth->emitted=false;
  auth->phase=auth->phase==TDSQL_MYSQL_AUTH_SEND_FULL?TDSQL_MYSQL_AUTH_WAIT_PASSWORD:
      auth->error_code?TDSQL_MYSQL_AUTH_FAILED:TDSQL_MYSQL_AUTH_READY;
  return TURBODB_STATUS_OK;
}
turbodb_status_t tdsql_mysql_auth_take(tdsql_mysql_auth *auth,tdsql_connection **out,turbodb_error_t *error) {
  if(!auth || !out) return auth_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"connection output required");
  if(auth->phase!=TDSQL_MYSQL_AUTH_READY || !auth->connection)
    return auth_error(error,TURBODB_STATUS_INVALID_STATE,"successful authentication handoff required");
  *out=auth->connection; auth->connection=NULL; auth->phase=TDSQL_MYSQL_AUTH_TRANSFERRED; return TURBODB_STATUS_OK;
}
turbodb_status_t tdsql_mysql_auth_reject(tdsql_mysql_auth *auth,turbodb_status_t status,turbodb_error_t *error) {
  if(!auth || status==TURBODB_STATUS_OK) return auth_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"failure status required");
  if(auth->phase!=TDSQL_MYSQL_AUTH_SEND_RESULT || auth->emitted || auth->error_code)
    return auth_error(error,TURBODB_STATUS_INVALID_STATE,"unpublished authentication success required");
  auth_plan_error(auth,status,auth_sdk_code(status)); return TURBODB_STATUS_OK;
}
turbodb_status_t tdsql_mysql_auth_dispose(tdsql_mysql_auth *auth,turbodb_error_t *error) {
  if(!auth) return TURBODB_STATUS_OK;
  if(auth->cleanup_failure!=TURBODB_STATUS_OK)
    return auth_error(error,auth->cleanup_failure,"authentication cleanup is quarantined");
  auth->phase=TDSQL_MYSQL_AUTH_FAILED;
  turbodb_status_t status=tdsql_connection_close(auth->connection,error);
  if(status!=TURBODB_STATUS_OK) {
    if(status!=TURBODB_STATUS_BUSY) auth->cleanup_failure=status;
    return status;
  }
  *auth=(tdsql_mysql_auth){0}; return TURBODB_STATUS_OK;
}

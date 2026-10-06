#include "registry.h"
#include <stdio.h>
#include <string.h>

typedef struct registry_entry {
  tdsql_statement *statement;
  vec_t types,values;
  uint32_t id;
  bool types_valid;
} registry_entry;

static turbodb_status_t registry_error(turbodb_error_t *error,turbodb_status_t status,const char *reason) {
  if(error && error->struct_size>=sizeof(*error)) {
    error->status=status; (void)snprintf(error->message,sizeof(error->message),"MySQL registry: %s",reason);
  }
  return status;
}
static turbodb_status_t registry_stl(stl_status status,turbodb_error_t *error) {
  if(status==STL_OK) return TURBODB_STATUS_OK;
  return registry_error(error,status==STL_OUT_OF_MEMORY?TURBODB_STATUS_OUT_OF_MEMORY:
      status==STL_CAPACITY_EXCEEDED?TURBODB_STATUS_LIMIT_EXCEEDED:TURBODB_STATUS_INTERNAL_ERROR,"fixed vector allocation failed");
}
static turbodb_status_t registry_wire(tdsql_mysql_status status,turbodb_error_t *error) {
  if(status==TDSQL_MYSQL_OK) return TURBODB_STATUS_OK;
  return registry_error(error,status==TDSQL_MYSQL_LIMIT?TURBODB_STATUS_LIMIT_EXCEEDED:
      status==TDSQL_MYSQL_UNSUPPORTED?TURBODB_STATUS_UNSUPPORTED:TURBODB_STATUS_INVALID_ARGUMENT,"invalid or unsupported command payload");
}
static bool registry_allocation(size_t count,size_t size,size_t alignment,size_t *bytes) {
  if(!count) { *bytes=0; return true; }
  const size_t metadata=sizeof(void *)+alignment-1;
  if(count>(SIZE_MAX-metadata)/size) return false;
  *bytes=count*size+metadata; return true;
}
static turbodb_status_t registry_vector(vec_t *out,size_t count,size_t size,size_t alignment,turbodb_error_t *error) {
  if(!count) return TURBODB_STATUS_OK;
  stl_status status=vec_init_bytes(out,size,alignment,count);
  if(status==STL_OK) status=vec_reserve(out,count);
  if(status==STL_OK) status=vec_resize(out,count);
  return registry_stl(status,error);
}
static turbodb_status_t registry_ready(tdsql_mysql_registry *r,turbodb_error_t *error) {
  if(!r || !r->connection || !r->slots.initialized) return registry_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"registry is not initialized");
  if(r->failure!=TURBODB_STATUS_OK) return registry_error(error,r->failure,"cleanup failed; close connection");
  if(r->closing) return registry_error(error,TURBODB_STATUS_INVALID_STATE,"registry is closing");
  return TURBODB_STATUS_OK;
}
static registry_entry *registry_find(tdsql_mysql_registry *r,uint32_t id) {
  if(!id) return NULL;
  for(size_t i=0;i<vec_size(&r->slots);++i) {
    registry_entry *entry=vec_at(&r->slots,i);
    if(entry->statement && entry->id==id) return entry;
  }
  return NULL;
}
static turbodb_status_t registry_lookup(tdsql_mysql_registry *r,uint32_t id,registry_entry **out,turbodb_error_t *error) {
  turbodb_status_t status=registry_ready(r,error);
  if(status!=TURBODB_STATUS_OK) return status;
  *out=registry_find(r,id);
  return *out?TURBODB_STATUS_OK:registry_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"unknown statement ID");
}
static bool registry_bytes(const tdsql_mysql_registry *r,size_t *out) {
  size_t slots=0;
  if(!registry_allocation(vec_capacity(&r->slots),sizeof(registry_entry),_Alignof(registry_entry),&slots) || slots>SIZE_MAX-sizeof(*r)) return false;
  size_t total=sizeof(*r)+slots;
  for(size_t i=0;i<vec_size(&r->slots);++i) {
    const registry_entry *entry=vec_at_const(&r->slots,i); size_t types=0,values=0;
    if(!registry_allocation(vec_capacity(&entry->types),sizeof(uint16_t),_Alignof(uint16_t),&types) ||
        !registry_allocation(vec_capacity(&entry->values),sizeof(turbodb_value_t),_Alignof(turbodb_value_t),&values) ||
        types>SIZE_MAX-total || values>SIZE_MAX-total-types) return false;
    total+=types+values;
  }
  *out=total; return true;
}
static turbodb_status_t registry_entry_close(tdsql_mysql_registry *r,registry_entry *entry,turbodb_error_t *error) {
  const turbodb_status_t status=tdsql_statement_close(entry->statement,error);
  if(status==TURBODB_STATUS_BUSY) return status;
  vec_destroy(&entry->types); vec_destroy(&entry->values); *entry=(registry_entry){0};
  if(status!=TURBODB_STATUS_OK && r->failure==TURBODB_STATUS_OK) r->failure=status;
  return status;
}
static turbodb_status_t registry_command(tdsql_mysql_registry *r,const tdsql_mysql_command *command,
    uint8_t kind,tdsql_mysql_command *verified,turbodb_error_t *error) {
  turbodb_status_t status=registry_ready(r,error);
  if(status!=TURBODB_STATUS_OK) return status;
  if(!command || command->kind!=kind) return registry_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"unexpected command kind");
  status=registry_wire(tdsql_mysql_command_decode(command->payload.data,command->payload.length,r->config.max_command_bytes,verified),error);
  if(status==TURBODB_STATUS_OK && (verified->kind!=kind || verified->statement_id!=command->statement_id))
    return registry_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"command differs from its payload");
  return status;
}
turbodb_status_t tdsql_mysql_registry_init(tdsql_mysql_registry *out,tdsql_connection *connection,
    const tdsql_mysql_registry_config *config,turbodb_error_t *error) {
  if(!out || out->connection || out->slots.initialized || !connection || !config || !config->max_statements ||
      !config->max_wire_bytes || !config->max_command_bytes || !config->limits.max_parameters || !config->limits.max_columns ||
      !config->limits.max_query_bytes || !config->limits.max_parameter_bytes || !config->limits.max_result_rows || !config->limits.max_result_bytes)
    return registry_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid configuration or occupied output");
  size_t slots=0;
  if(config->max_statements>UINT32_MAX || config->limits.max_parameters>UINT16_MAX || config->limits.max_columns>UINT16_MAX ||
      !registry_allocation(config->max_statements,sizeof(registry_entry),_Alignof(registry_entry),&slots) ||
      slots>SIZE_MAX-sizeof(*out) || sizeof(*out)+slots>config->max_wire_bytes)
    return registry_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"registry capacity exceeded");
  tdsql_mysql_registry r={.connection=connection,.config=*config};
  const turbodb_status_t status=registry_vector(&r.slots,config->max_statements,sizeof(registry_entry),_Alignof(registry_entry),error);
  if(status!=TURBODB_STATUS_OK) { vec_destroy(&r.slots); return status; }
  *out=r; return TURBODB_STATUS_OK;
}
turbodb_status_t tdsql_mysql_registry_prepare(tdsql_mysql_registry *r,const tdsql_mysql_command *command,
    mysql_stmt_prepare_ok_t *out,turbodb_error_t *error) {
  if(!out) return registry_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"missing prepare response");
  tdsql_mysql_command verified={0}; turbodb_status_t status=registry_command(r,command,MYSQL_COM_STMT_PREPARE,&verified,error);
  if(status!=TURBODB_STATUS_OK) return status;
  if(verified.sql.length>r->config.limits.max_query_bytes || r->last_id==UINT32_MAX)
    return registry_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"SQL bytes or statement ID exhausted");
  registry_entry *entry=NULL;
  for(size_t i=0;i<vec_size(&r->slots);++i) {
    registry_entry *candidate=vec_at(&r->slots,i);
    if(!candidate->statement) { entry=candidate; break; }
  }
  if(!entry) return registry_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"statement slots full");
  tdsql_request request=tdsql_request_default((vstr){(const char *)verified.sql.data,verified.sql.length});
  request.limits=r->config.limits;
  status=tdsql_connection_statement_prepare(r->connection,&request,&entry->statement,error);
  if(status!=TURBODB_STATUS_OK) return status;
  const size_t count=tdsql_statement_parameters(entry->statement),columns=tdsql_statement_columns(entry->statement);
  size_t used=0,types=0,values=0;
  if(count>UINT16_MAX || columns>UINT16_MAX || !registry_bytes(r,&used) ||
      !registry_allocation(count,sizeof(uint16_t),_Alignof(uint16_t),&types) ||
      !registry_allocation(count,sizeof(turbodb_value_t),_Alignof(turbodb_value_t),&values) ||
      used>r->config.max_wire_bytes || types>r->config.max_wire_bytes-used || values>r->config.max_wire_bytes-used-types)
    status=registry_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"prepared wire cache quota exceeded");
  if(status==TURBODB_STATUS_OK) status=registry_vector(&entry->types,count,sizeof(uint16_t),_Alignof(uint16_t),error);
  if(status==TURBODB_STATUS_OK) status=registry_vector(&entry->values,count,sizeof(turbodb_value_t),_Alignof(turbodb_value_t),error);
  if(status!=TURBODB_STATUS_OK) {
    const turbodb_status_t closed=registry_entry_close(r,entry,NULL);
    if(closed!=TURBODB_STATUS_OK) {
      r->failure=closed==TURBODB_STATUS_BUSY?TURBODB_STATUS_CLEANUP_FAILED:closed;
      return registry_error(error,r->failure,"prepare ownership cleanup failed");
    }
    return status;
  }
  entry->id=++r->last_id;
  *out=(mysql_stmt_prepare_ok_t){entry->id,(uint16_t)columns,(uint16_t)count,0}; return TURBODB_STATUS_OK;
}
turbodb_status_t tdsql_mysql_registry_execute(tdsql_mysql_registry *r,const tdsql_mysql_command *command,
    tdsql_response *out,turbodb_error_t *error) {
  if(!out) return registry_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"missing execute response");
  if(out->struct_size<sizeof(*out) || out->abi_version!=TDSQL_ABI_VERSION)
    return registry_error(error,TURBODB_STATUS_ABI_MISMATCH,"invalid execute response ABI");
  if(out->result) return registry_error(error,TURBODB_STATUS_BUSY,"destroy response result before execute");
  tdsql_mysql_command verified={0}; turbodb_status_t status=registry_command(r,command,MYSQL_COM_STMT_EXECUTE,&verified,error);
  if(status!=TURBODB_STATUS_OK) return status;
  registry_entry *entry=NULL; status=registry_lookup(r,verified.statement_id,&entry,error);
  if(status!=TURBODB_STATUS_OK) return status;
  tdsql_session_state state=tdsql_session_state_default(); status=tdsql_connection_state(r->connection,&state,error);
  if(status!=TURBODB_STATUS_OK) return status;
  if(state.failure!=TURBODB_STATUS_OK) return registry_error(error,state.failure,"connection requires close");
  if(state.busy) return registry_error(error,TURBODB_STATUS_BUSY,"connection has an active result or transaction handle");
  const size_t count=tdsql_statement_parameters(entry->statement);
  status=registry_wire(tdsql_mysql_execute_decode(&verified,count,vec_data(&entry->types),vec_size(&entry->types),
      &entry->types_valid,vec_data(&entry->values),vec_size(&entry->values),r->config.limits.max_parameter_bytes),error);
  if(status==TURBODB_STATUS_OK) {
    const tdsql_bindings bindings=tdsql_bindings_default(vec_data_const(&entry->values),count);
    status=tdsql_statement_execute(entry->statement,&bindings,out,error);
  }
  if(count) memset(vec_data(&entry->values),0,count*sizeof(turbodb_value_t));
  return status;
}
turbodb_status_t tdsql_mysql_registry_parameter(tdsql_mysql_registry *r,uint32_t id,size_t index,tdsql_column *out,turbodb_error_t *error) {
  registry_entry *entry=NULL; const turbodb_status_t status=registry_lookup(r,id,&entry,error);
  return status==TURBODB_STATUS_OK?tdsql_statement_parameter(entry->statement,index,out,error):status;
}
turbodb_status_t tdsql_mysql_registry_column(tdsql_mysql_registry *r,uint32_t id,size_t index,tdsql_column *out,turbodb_error_t *error) {
  registry_entry *entry=NULL; const turbodb_status_t status=registry_lookup(r,id,&entry,error);
  return status==TURBODB_STATUS_OK?tdsql_statement_column(entry->statement,index,out,error):status;
}
turbodb_status_t tdsql_mysql_registry_reset(tdsql_mysql_registry *r,uint32_t id,turbodb_error_t *error) {
  registry_entry *entry=NULL; const turbodb_status_t status=registry_lookup(r,id,&entry,error);
  return status==TURBODB_STATUS_OK?tdsql_statement_reset(entry->statement,error):status;
}
turbodb_status_t tdsql_mysql_registry_close(tdsql_mysql_registry *r,uint32_t id,turbodb_error_t *error) {
  if(!id || !r || !r->connection) return registry_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid close ID or registry");
  registry_entry *entry=registry_find(r,id);
  return entry?registry_entry_close(r,entry,error):TURBODB_STATUS_OK;
}
size_t tdsql_mysql_registry_count(const tdsql_mysql_registry *r) {
  if(!r) return 0;
  size_t count=0;
  for(size_t i=0;i<vec_size(&r->slots);++i) if(((const registry_entry *)vec_at_const(&r->slots,i))->statement) ++count;
  return count;
}
bool tdsql_mysql_registry_contains(const tdsql_mysql_registry *r,uint32_t id) {
  if(!r || !id) return false;
  for(size_t i=0;i<vec_size(&r->slots);++i) {
    const registry_entry *entry=vec_at_const(&r->slots,i);
    if(entry->statement && entry->id==id) return true;
  }
  return false;
}
turbodb_status_t tdsql_mysql_registry_dispose(tdsql_mysql_registry *r,turbodb_error_t *error) {
  if(!r || !r->connection) return TURBODB_STATUS_OK;
  r->closing=true; turbodb_status_t status=r->failure; bool busy=false;
  for(size_t i=0;i<vec_size(&r->slots);++i) {
    registry_entry *entry=vec_at(&r->slots,i);
    if(!entry->statement) continue;
    const turbodb_status_t closed=registry_entry_close(r,entry,status==TURBODB_STATUS_OK?error:NULL);
    if(closed==TURBODB_STATUS_BUSY) busy=true;
    else if(status==TURBODB_STATUS_OK) status=closed;
  }
  if(busy) return registry_error(error,TURBODB_STATUS_BUSY,"destroy results before registry disposal");
  vec_destroy(&r->slots); *r=(tdsql_mysql_registry){0};
  return status==TURBODB_STATUS_OK?status:registry_error(error,status,"registry ownership cleanup failed");
}

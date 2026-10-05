#include "dispatch.h"
#include <stdio.h>
#include <string.h>

enum { DISPATCH_IDLE,DISPATCH_PREPARE,DISPATCH_PARAMETERS,DISPATCH_PARAMETER_END,
       DISPATCH_PREPARE_COLUMNS,DISPATCH_PREPARE_END,DISPATCH_COUNT,DISPATCH_COLUMNS,
       DISPATCH_METADATA_END,DISPATCH_ROWS,DISPATCH_END,DISPATCH_OK,DISPATCH_ERROR,DISPATCH_TERMINAL };
enum { DISPATCH_LONG_DATA=0x18,DISPATCH_GENERAL_ERROR=1105,DISPATCH_WRONG_ARGUMENTS=1210,
       DISPATCH_UNKNOWN_COMMAND=1047,DISPATCH_UNSUPPORTED=1235,DISPATCH_UNKNOWN_STATEMENT=1243,
       DISPATCH_BINARY_CHARSET=63,DISPATCH_NOT_NULL=1,DISPATCH_BINARY_FLAG=128,
       DISPATCH_INTEGER_DIGITS=20,DISPATCH_DOUBLE_DIGITS=24,DISPATCH_DOUBLE_DECIMALS=31,
       DISPATCH_EOF_MARKER=0xfe };
static turbodb_status_t dispatch_error(turbodb_error_t *error,turbodb_status_t status,const char *reason) {
  if(error && error->struct_size>=sizeof(*error)) {
    error->status=status; (void)snprintf(error->message,sizeof(error->message),"MySQL dispatch: %s",reason);
  }
  return status;
}
static turbodb_status_t dispatch_codec(tdsql_mysql_status status,turbodb_error_t *error) {
  if(status==TDSQL_MYSQL_OK) return TURBODB_STATUS_OK;
  return dispatch_error(error,status==TDSQL_MYSQL_LIMIT?TURBODB_STATUS_LIMIT_EXCEEDED:
      status==TDSQL_MYSQL_UNSUPPORTED?TURBODB_STATUS_UNSUPPORTED:TURBODB_STATUS_INVALID_ARGUMENT,"codec rejected payload");
}
static turbodb_status_t dispatch_state(tdsql_mysql_dispatch *d,mysql_wire_ok_packet_t *out) {
  tdsql_session_state state=tdsql_session_state_default();
  turbodb_status_t status=tdsql_connection_state(d->registry.connection,&state,&d->diagnostic);
  if(status!=TURBODB_STATUS_OK) return status;
  if(state.failure!=TURBODB_STATUS_OK) return dispatch_error(&d->diagnostic,state.failure,"SQL session requires close");
  out->status_flags=(state.in_transaction?TDSQL_MYSQL_SERVER_IN_TRANS:0)|
      (state.autocommit?TDSQL_MYSQL_SERVER_AUTOCOMMIT:0)|
      (state.transaction_read_only?TDSQL_MYSQL_SERVER_IN_TRANS_READONLY:0);
  /* MySQL carries a uint16 warning count; detailed engine count remains SDK-owned. */
  out->warnings=(uint16_t)(state.warning_count>UINT16_MAX?UINT16_MAX:state.warning_count);
  return TURBODB_STATUS_OK;
}
static void dispatch_fail(tdsql_mysql_dispatch *d,turbodb_status_t status) {
  if(d->response.result) {
    turbodb_error_t cleanup; turbodb_error_init(&cleanup);
    const turbodb_status_t closed=tdsql_result_destroy_checked(d->response.result,&cleanup); d->response.result=NULL;
    if(closed!=TURBODB_STATUS_OK) { d->diagnostic=cleanup; status=closed; d->error_code=0; d->close_after_reply=true; }
  }
  if(d->phase>=DISPATCH_PREPARE && d->phase<=DISPATCH_PREPARE_END && d->prepared.statement_id) {
    turbodb_error_t cleanup; turbodb_error_init(&cleanup);
    const turbodb_status_t closed=tdsql_mysql_registry_close(&d->registry,d->prepared.statement_id,&cleanup);
    if(closed!=TURBODB_STATUS_OK) { d->diagnostic=cleanup; status=closed; d->error_code=0; d->close_after_reply=true; }
    d->prepared=(mysql_stmt_prepare_ok_t){0};
  }
  tdsql_session_state state=tdsql_session_state_default();
  const turbodb_status_t observed=tdsql_connection_state(d->registry.connection,&state,NULL);
  if(observed!=TURBODB_STATUS_OK || state.failure!=TURBODB_STATUS_OK ||
      status==TURBODB_STATUS_COMMIT_UNKNOWN || status==TURBODB_STATUS_CLEANUP_FAILED) d->close_after_reply=true;
  if(!d->error_code) d->error_code=status==TURBODB_STATUS_INVALID_ARGUMENT?DISPATCH_WRONG_ARGUMENTS:
      status==TURBODB_STATUS_UNSUPPORTED?DISPATCH_UNSUPPORTED:DISPATCH_GENERAL_ERROR;
  d->phase=DISPATCH_ERROR; d->pending=false; d->emitted=false;
}
static turbodb_status_t dispatch_column(tdsql_mysql_dispatch *d,const tdsql_column *source,mysql_column_definition_t *out) {
  if(!source || (!source->name.data && source->name.len)) return dispatch_error(&d->diagnostic,TURBODB_STATUS_INTERNAL_ERROR,"invalid SDK metadata");
  *out=(mysql_column_definition_t){.catalog={(const uint8_t *)"def",3,false},
      .name={(const uint8_t *)source->name.data,source->name.len,false},
      .character_set=DISPATCH_BINARY_CHARSET,.flags=source->nullable?0:DISPATCH_NOT_NULL};
  switch(source->kind) {
    case TURBODB_VALUE_NULL: out->type=MYSQL_TYPE_NULL; break;
    case TURBODB_VALUE_INT64: out->type=MYSQL_TYPE_LONGLONG; out->column_length=DISPATCH_INTEGER_DIGITS; break;
    case TURBODB_VALUE_UINT64: out->type=MYSQL_TYPE_LONGLONG; out->column_length=DISPATCH_INTEGER_DIGITS; out->flags|=MYSQL_COLUMN_FLAG_UNSIGNED; break;
    case TURBODB_VALUE_DOUBLE: out->type=MYSQL_TYPE_DOUBLE; out->column_length=DISPATCH_DOUBLE_DIGITS; out->decimals=DISPATCH_DOUBLE_DECIMALS; break;
    case TURBODB_VALUE_BOOLEAN: out->type=MYSQL_TYPE_TINY; out->column_length=1; break;
    case TURBODB_VALUE_TEXT: out->type=MYSQL_TYPE_VAR_STRING; out->column_length=(uint32_t)d->capacity; break;
    case TURBODB_VALUE_BLOB: out->type=MYSQL_TYPE_BLOB; out->column_length=(uint32_t)d->capacity; out->flags|=DISPATCH_BINARY_FLAG; break;
    default: return dispatch_error(&d->diagnostic,TURBODB_STATUS_UNSUPPORTED,"SDK type has no wire representation");
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t dispatch_metadata(tdsql_mysql_dispatch *d,size_t index,bool parameter,mysql_column_definition_t *out) {
  tdsql_column source={0}; turbodb_status_t status;
  if(d->response.result) status=tdsql_result_column(d->response.result,index,&source,&d->diagnostic);
  else if(parameter) status=tdsql_mysql_registry_parameter(&d->registry,d->prepared.statement_id,index,&source,&d->diagnostic);
  else status=tdsql_mysql_registry_column(&d->registry,d->prepared.statement_id,index,&source,&d->diagnostic);
  return status==TURBODB_STATUS_OK?dispatch_column(d,&source,out):status;
}
static turbodb_status_t dispatch_preflight(tdsql_mysql_dispatch *d) {
  const size_t groups=d->response.result?1:2;
  for(size_t group=0;group<groups;++group) {
    const bool parameter=!d->response.result && group==0;
    const size_t count=d->response.result?tdsql_result_columns(d->response.result):
        parameter?d->prepared.parameter_count:d->prepared.column_count;
    for(size_t index=0;index<count;++index) {
      mysql_column_definition_t column={0}; turbodb_status_t status=dispatch_metadata(d,index,parameter,&column);
      tdsql_mysql_output measure={.capacity=d->capacity};
      if(status==TURBODB_STATUS_OK) status=dispatch_codec(tdsql_mysql_column_encode(&column,&measure),&d->diagnostic);
      if(status!=TURBODB_STATUS_OK) return status;
    }
  }
  return TURBODB_STATUS_OK;
}
turbodb_status_t tdsql_mysql_dispatch_init(tdsql_mysql_dispatch *out,tdsql_connection *connection,
    const tdsql_mysql_registry_config *config,bool deprecated,uint8_t *scratch,size_t capacity,turbodb_error_t *error) {
  if(!out || out->scratch || out->registry.connection || !scratch || capacity<TDSQL_MYSQL_MIN_REPLY_BYTES)
    return dispatch_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid owner or reply scratch");
  if(capacity>UINT32_MAX) return dispatch_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"wire reply width exceeded");
  tdsql_mysql_dispatch d={.scratch=scratch,.capacity=capacity,.deprecate_eof=deprecated,.response=tdsql_response_default()};
  turbodb_error_init(&d.diagnostic);
  const turbodb_status_t status=tdsql_mysql_registry_init(&d.registry,connection,config,error);
  if(status==TURBODB_STATUS_OK) *out=d;
  return status;
}
turbodb_status_t tdsql_mysql_dispatch_accept(tdsql_mysql_dispatch *d,const uint8_t *payload,size_t size,
    uint8_t sequence,turbodb_error_t *error) {
  if(!d || !d->scratch || !d->registry.connection || (!payload && size)) return dispatch_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid request owner");
  if(d->phase==DISPATCH_TERMINAL) return dispatch_error(error,TURBODB_STATUS_INVALID_STATE,"connection is terminal");
  if(d->phase!=DISPATCH_IDLE || d->pending) return dispatch_error(error,TURBODB_STATUS_BUSY,"acknowledge reply before another command");
  d->sequence=sequence; d->index=0; d->error_code=0; d->close_after_reply=false; d->binary=false;
  d->prepared=(mysql_stmt_prepare_ok_t){0}; d->response=tdsql_response_default(); turbodb_error_init(&d->diagnostic);
  tdsql_mysql_command command={0}; turbodb_status_t status=dispatch_codec(tdsql_mysql_command_decode(payload,size,d->registry.config.max_command_bytes,&command),&d->diagnostic);
  if(status!=TURBODB_STATUS_OK) {
    if(size && (payload[0]==DISPATCH_LONG_DATA || payload[0]==MYSQL_COM_STMT_CLOSE || payload[0]==TDSQL_MYSQL_QUIT)) {
      d->phase=DISPATCH_TERMINAL;
      return dispatch_error(error,status,"no-response command rejected; disconnect");
    }
    if(size && status==TURBODB_STATUS_UNSUPPORTED && payload[0]!=MYSQL_COM_STMT_EXECUTE) d->error_code=DISPATCH_UNKNOWN_COMMAND;
    dispatch_fail(d,status); return TURBODB_STATUS_OK;
  }
  switch(command.kind) {
    case TDSQL_MYSQL_QUIT: d->phase=DISPATCH_TERMINAL; return TURBODB_STATUS_OK;
    case MYSQL_COM_STMT_CLOSE:
      status=tdsql_mysql_registry_close(&d->registry,command.statement_id,&d->diagnostic);
      if(status!=TURBODB_STATUS_OK) { d->phase=DISPATCH_TERMINAL; return dispatch_error(error,status,"statement close failed; disconnect"); }
      return TURBODB_STATUS_OK;
    case TDSQL_MYSQL_PING: {
      mysql_wire_ok_packet_t snapshot={0}; status=dispatch_state(d,&snapshot); d->phase=DISPATCH_OK; break;
    }
    case MYSQL_COM_STMT_PREPARE:
      status=tdsql_mysql_registry_prepare(&d->registry,&command,&d->prepared,&d->diagnostic);
      if(status==TURBODB_STATUS_OK) { d->phase=DISPATCH_PREPARE; status=dispatch_preflight(d); }
      break;
    case MYSQL_COM_STMT_RESET:
    case MYSQL_COM_STMT_EXECUTE:
      if(!tdsql_mysql_registry_contains(&d->registry,command.statement_id)) {
        d->error_code=DISPATCH_UNKNOWN_STATEMENT; status=dispatch_error(&d->diagnostic,TURBODB_STATUS_INVALID_ARGUMENT,"unknown prepared statement ID"); break;
      }
      if(command.kind==MYSQL_COM_STMT_RESET) { status=tdsql_mysql_registry_reset(&d->registry,command.statement_id,&d->diagnostic); d->phase=DISPATCH_OK; break; }
      d->binary=true; status=tdsql_mysql_registry_execute(&d->registry,&command,&d->response,&d->diagnostic); break;
    case TDSQL_MYSQL_QUERY: {
      tdsql_request request=tdsql_request_default((vstr){(const char *)command.sql.data,command.sql.length});
      request.limits=d->registry.config.limits; status=tdsql_connection_run(d->registry.connection,&request,&d->response,&d->diagnostic); break;
    }
  }
  if(status==TURBODB_STATUS_OK && d->response.kind==TDSQL_ROWS) {
    const size_t columns=tdsql_result_columns(d->response.result);
    if(!columns || columns>UINT16_MAX) status=dispatch_error(&d->diagnostic,TURBODB_STATUS_LIMIT_EXCEEDED,"wire result columns exceeded");
    else { d->phase=DISPATCH_COUNT; status=dispatch_preflight(d); }
  } else if(status==TURBODB_STATUS_OK && d->response.kind==TDSQL_COMMAND) d->phase=DISPATCH_OK;
  if(status!=TURBODB_STATUS_OK) dispatch_fail(d,status);
  return TURBODB_STATUS_OK;
}
static turbodb_status_t dispatch_terminator(tdsql_mysql_dispatch *d,tdsql_mysql_output *out,bool final) {
  mysql_wire_ok_packet_t ok={.header=final && d->deprecate_eof?DISPATCH_EOF_MARKER:0};
  turbodb_status_t status=dispatch_state(d,&ok);
  if(status!=TURBODB_STATUS_OK) return status;
  if(d->phase==DISPATCH_OK) { ok.affected_rows=d->response.affected_rows; ok.last_insert_id=d->response.last_insert_id; }
  if(d->phase==DISPATCH_OK || (final && d->deprecate_eof)) return dispatch_codec(tdsql_mysql_ok_encode(&ok,d->deprecate_eof,out),&d->diagnostic);
  const mysql_wire_eof_packet_t eof={ok.warnings,ok.status_flags};
  return dispatch_codec(tdsql_mysql_eof_encode(&eof,out),&d->diagnostic);
}
static turbodb_status_t dispatch_row(tdsql_mysql_dispatch *d,tdsql_mysql_output *out) {
  tdsql_row row={0}; turbodb_status_t status=tdsql_result_next(d->response.result,&row,&d->diagnostic);
  if(status!=TURBODB_STATUS_OK) return status;
  if(row.state==TDSQL_DONE) {
    status=tdsql_result_destroy_checked(d->response.result,&d->diagnostic); d->response.result=NULL;
    if(status!=TURBODB_STATUS_OK) return status;
    d->phase=DISPATCH_END; return dispatch_terminator(d,out,true);
  }
  if(row.state!=TDSQL_ROW || row.count!=tdsql_result_columns(d->response.result) || !row.values)
    return dispatch_error(&d->diagnostic,TURBODB_STATUS_INTERNAL_ERROR,"invalid SDK row state or width");
  for(size_t i=0;i<row.count;++i) {
    tdsql_column metadata={0}; status=tdsql_result_column(d->response.result,i,&metadata,&d->diagnostic);
    if(status!=TURBODB_STATUS_OK) return status;
    if(row.values[i].kind!=TURBODB_VALUE_NULL && row.values[i].kind!=metadata.kind)
      return dispatch_error(&d->diagnostic,TURBODB_STATUS_INTERNAL_ERROR,"SDK row type differs from metadata");
  }
  return dispatch_codec(tdsql_mysql_row_encode(row.values,row.count,d->binary,out),&d->diagnostic);
}
static turbodb_status_t dispatch_payload(tdsql_mysql_dispatch *d,tdsql_mysql_output *out) {
  switch(d->phase) {
    case DISPATCH_PREPARE: return dispatch_codec(tdsql_mysql_prepare_encode(&d->prepared,out),&d->diagnostic);
    case DISPATCH_PARAMETERS: case DISPATCH_PREPARE_COLUMNS: case DISPATCH_COLUMNS: {
      mysql_column_definition_t column={0}; turbodb_status_t status=dispatch_metadata(d,d->index,d->phase==DISPATCH_PARAMETERS,&column);
      return status==TURBODB_STATUS_OK?dispatch_codec(tdsql_mysql_column_encode(&column,out),&d->diagnostic):status;
    }
    case DISPATCH_COUNT: return dispatch_codec(tdsql_mysql_count_encode(tdsql_result_columns(d->response.result),out),&d->diagnostic);
    case DISPATCH_ROWS: return dispatch_row(d,out);
    case DISPATCH_PARAMETER_END: case DISPATCH_PREPARE_END: case DISPATCH_METADATA_END: return dispatch_terminator(d,out,false);
    case DISPATCH_END: return dispatch_terminator(d,out,true);
    case DISPATCH_OK: return dispatch_terminator(d,out,false);
    case DISPATCH_ERROR: {
      const char *end=memchr(d->diagnostic.message,0,sizeof(d->diagnostic.message));
      if(!end) return dispatch_error(&d->diagnostic,TURBODB_STATUS_INTERNAL_ERROR,"unterminated SDK diagnostic");
      const char *state=d->error_code==DISPATCH_UNSUPPORTED?"42000":d->error_code==DISPATCH_UNKNOWN_COMMAND?"08S01":"HY000";
      const mysql_wire_bytes_t message={(const uint8_t *)d->diagnostic.message,(size_t)(end-d->diagnostic.message),false};
      return dispatch_codec(tdsql_mysql_err_encode(d->error_code,state,message,out),NULL);
    }
    default: return dispatch_error(&d->diagnostic,TURBODB_STATUS_INTERNAL_ERROR,"unexpected response phase");
  }
}
turbodb_status_t tdsql_mysql_dispatch_emit(tdsql_mysql_dispatch *d,tdsql_mysql_output *out,int *state,turbodb_error_t *error) {
  if(!d || !d->scratch || !out || !out->data || !state) return dispatch_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid packet output");
  if(d->emitted) return dispatch_error(error,TURBODB_STATUS_BUSY,"acknowledge emitted packet before reuse");
  if(d->phase==DISPATCH_IDLE || d->phase==DISPATCH_TERMINAL) { *state=d->phase==DISPATCH_IDLE?TDSQL_MYSQL_REPLY_COMPLETE:TDSQL_MYSQL_REPLY_DISCONNECT; return TURBODB_STATUS_OK; }
  if(!d->pending) {
    tdsql_mysql_output payload={d->scratch,d->capacity,0}; turbodb_status_t status=dispatch_payload(d,&payload);
    if(status!=TURBODB_STATUS_OK) {
      if(d->phase==DISPATCH_ERROR) { d->phase=DISPATCH_TERMINAL; return dispatch_error(error,status,"ERR encoding failed; disconnect"); }
      dispatch_fail(d,status); payload.size=0; status=dispatch_payload(d,&payload);
      if(status!=TURBODB_STATUS_OK) { d->phase=DISPATCH_TERMINAL; return dispatch_error(error,status,"ERR encoding failed; disconnect"); }
    }
    d->payload_bytes=payload.size; d->pending=true;
  }
  uint8_t next=0;
  const turbodb_status_t status=dispatch_codec(tdsql_mysql_frame_encode(d->scratch,d->payload_bytes,d->sequence,out,&next),error);
  if(status==TURBODB_STATUS_OK) { d->next_sequence=next; d->emitted=true; *state=TDSQL_MYSQL_REPLY_PACKET; }
  return status;
}
static uint32_t dispatch_prepare_columns(const tdsql_mysql_dispatch *d) { return d->prepared.column_count?DISPATCH_PREPARE_COLUMNS:DISPATCH_IDLE; }
turbodb_status_t tdsql_mysql_dispatch_acknowledge(tdsql_mysql_dispatch *d,turbodb_error_t *error) {
  if(!d || !d->emitted || !d->pending) return dispatch_error(error,TURBODB_STATUS_INVALID_STATE,"no emitted packet to acknowledge");
  d->sequence=d->next_sequence; d->pending=false; d->emitted=false;
  switch(d->phase) {
    case DISPATCH_PREPARE: d->phase=d->prepared.parameter_count?DISPATCH_PARAMETERS:dispatch_prepare_columns(d); d->index=0; break;
    case DISPATCH_PARAMETERS:
      if(++d->index==d->prepared.parameter_count) { d->phase=d->deprecate_eof?dispatch_prepare_columns(d):DISPATCH_PARAMETER_END; d->index=0; } break;
    case DISPATCH_PARAMETER_END: d->phase=dispatch_prepare_columns(d); break;
    case DISPATCH_PREPARE_COLUMNS:
      if(++d->index==d->prepared.column_count) d->phase=d->deprecate_eof?DISPATCH_IDLE:DISPATCH_PREPARE_END;
      break;
    case DISPATCH_PREPARE_END: d->phase=DISPATCH_IDLE; break;
    case DISPATCH_COUNT: d->phase=DISPATCH_COLUMNS; d->index=0; break;
    case DISPATCH_COLUMNS:
      if(++d->index==tdsql_result_columns(d->response.result)) d->phase=d->deprecate_eof?DISPATCH_ROWS:DISPATCH_METADATA_END;
      break;
    case DISPATCH_METADATA_END: d->phase=DISPATCH_ROWS; break;
    case DISPATCH_ROWS: break;
    case DISPATCH_END: case DISPATCH_OK: d->phase=DISPATCH_IDLE; break;
    case DISPATCH_ERROR: d->phase=d->close_after_reply?DISPATCH_TERMINAL:DISPATCH_IDLE; break;
    default: d->phase=DISPATCH_TERMINAL; return dispatch_error(error,TURBODB_STATUS_INTERNAL_ERROR,"unexpected acknowledged phase");
  }
  return TURBODB_STATUS_OK;
}
turbodb_status_t tdsql_mysql_dispatch_dispose(tdsql_mysql_dispatch *d,turbodb_error_t *error) {
  if(!d || !d->scratch) return TURBODB_STATUS_OK;
  d->phase=DISPATCH_TERMINAL;
  turbodb_status_t status=tdsql_result_destroy_checked(d->response.result,error); d->response.result=NULL;
  const turbodb_status_t disposed=tdsql_mysql_registry_dispose(&d->registry,status==TURBODB_STATUS_OK?error:NULL);
  if(disposed==TURBODB_STATUS_BUSY) return dispatch_error(error,disposed,"registry still has external result ownership");
  if(status==TURBODB_STATUS_OK) status=disposed;
  *d=(tdsql_mysql_dispatch){0}; return status;
}

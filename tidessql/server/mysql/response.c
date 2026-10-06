#include "wire.h"
#include <inttypes.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { RESPONSE_INTEGER_BYTES=8, RESPONSE_LENENC_BYTES=9, RESPONSE_NUMBER_BYTES=64,
       RESPONSE_SQLSTATE_BYTES=5, RESPONSE_COLUMN_FIXED=12, RESPONSE_BINARY_OFFSET=2,
       RESPONSE_EOF_MARKER=0xfe, RESPONSE_ERR_MARKER=0xff, RESPONSE_NULL_MARKER=0xfb };
typedef struct writer { uint8_t *data; size_t size,capacity; tdsql_mysql_status status; } writer;
static void emit(writer *w,const void *data,size_t size) {
  if(w->status!=TDSQL_MYSQL_OK) return;
  if((!data && size) || w->size>w->capacity) { w->status=TDSQL_MYSQL_INVALID; return; }
  if(size>w->capacity-w->size) { w->status=TDSQL_MYSQL_LIMIT; return; }
  if(w->data && size) memcpy(w->data+w->size,data,size);
  w->size+=size;
}
static void integer(writer *w,uint64_t value,size_t width) {
  uint8_t bytes[RESPONSE_INTEGER_BYTES]={0}; size_t offset=0;
  mysql_wire_status_t status=MYSQL_WIRE_STATUS_OK;
  switch(width) {
    case 1: bytes[0]=(uint8_t)value; break;
    case 2: status=mysql_wire_write_u16_le(bytes,sizeof(bytes),&offset,(uint16_t)value); break;
    case 4: status=mysql_wire_write_u32_le(bytes,sizeof(bytes),&offset,(uint32_t)value); break;
    case 8: status=mysql_wire_write_u64_le(bytes,sizeof(bytes),&offset,value); break;
    default: status=MYSQL_WIRE_STATUS_INVALID; break;
  }
  if(status!=MYSQL_WIRE_STATUS_OK) { if(w->status==TDSQL_MYSQL_OK) w->status=status; return; }
  emit(w,bytes,width);
}
static void lenenc(writer *w,uint64_t value) {
  uint8_t bytes[RESPONSE_LENENC_BYTES]; size_t size=0;
  const mysql_wire_status_t status=mysql_wire_write_lenenc_uint(bytes,sizeof(bytes),&size,value);
  if(status!=MYSQL_WIRE_STATUS_OK) { if(w->status==TDSQL_MYSQL_OK) w->status=status; return; }
  emit(w,bytes,size);
}
static void bytes(writer *w,mysql_wire_bytes_t value) {
  if(value.is_null || (!value.data && value.length)) { w->status=TDSQL_MYSQL_INVALID; return; }
  lenenc(w,value.length); emit(w,value.data,value.length);
}
static tdsql_mysql_status status_flags(uint16_t flags) {
  const uint16_t supported=TDSQL_MYSQL_SERVER_IN_TRANS|TDSQL_MYSQL_SERVER_AUTOCOMMIT|TDSQL_MYSQL_SERVER_IN_TRANS_READONLY;
  if(flags & (uint16_t)~supported) return TDSQL_MYSQL_UNSUPPORTED;
  if((flags&TDSQL_MYSQL_SERVER_IN_TRANS_READONLY) && !(flags&TDSQL_MYSQL_SERVER_IN_TRANS)) return TDSQL_MYSQL_INVALID;
  return TDSQL_MYSQL_OK;
}
static void ok_write(writer *w,const mysql_wire_ok_packet_t *packet,bool deprecated) {
  if(!packet || (packet->header!=0 && packet->header!=RESPONSE_EOF_MARKER)) { w->status=TDSQL_MYSQL_INVALID; return; }
  if((packet->header==RESPONSE_EOF_MARKER && !deprecated) || packet->session_state.length) {
    w->status=TDSQL_MYSQL_UNSUPPORTED; return;
  }
  w->status=status_flags(packet->status_flags);
  integer(w,packet->header,1); lenenc(w,packet->affected_rows); lenenc(w,packet->last_insert_id);
  integer(w,packet->status_flags,2); integer(w,packet->warnings,2);
  if(packet->info.is_null || (!packet->info.data && packet->info.length)) { w->status=TDSQL_MYSQL_INVALID; return; }
  emit(w,packet->info.data,packet->info.length);
}
static void column_write(writer *w,const mysql_column_definition_t *column) {
  if(!column) { w->status=TDSQL_MYSQL_INVALID; return; }
  switch(column->type) {
    case MYSQL_TYPE_TINY: case MYSQL_TYPE_DOUBLE: case MYSQL_TYPE_NULL:
    case MYSQL_TYPE_LONGLONG: case MYSQL_TYPE_BLOB: case MYSQL_TYPE_VAR_STRING: break;
    default: w->status=TDSQL_MYSQL_UNSUPPORTED; return;
  }
  const mysql_wire_bytes_t fields[]={column->catalog,column->schema,column->table,column->org_table,column->name,column->org_name};
  for(size_t i=0;i<sizeof(fields)/sizeof(fields[0]);++i) bytes(w,fields[i]);
  integer(w,RESPONSE_COLUMN_FIXED,1); integer(w,column->character_set,2); integer(w,column->column_length,4);
  integer(w,column->type,1); integer(w,column->flags,2); integer(w,column->decimals,1); integer(w,0,2);
}
static tdsql_mysql_status value_check(const turbodb_value_t *value) {
  if(value->reserved) return TDSQL_MYSQL_INVALID;
  switch(value->kind) {
    case TURBODB_VALUE_NULL: case TURBODB_VALUE_INT64: case TURBODB_VALUE_UINT64: return TDSQL_MYSQL_OK;
    case TURBODB_VALUE_DOUBLE: return isfinite(value->data.double_value)?TDSQL_MYSQL_OK:TDSQL_MYSQL_INVALID;
    case TURBODB_VALUE_BOOLEAN: return value->data.boolean_value<=1?TDSQL_MYSQL_OK:TDSQL_MYSQL_INVALID;
    case TURBODB_VALUE_TEXT: return value->data.text_value.data || !value->data.text_value.len?TDSQL_MYSQL_OK:TDSQL_MYSQL_INVALID;
    case TURBODB_VALUE_BLOB: return value->data.blob_value.data || !value->data.blob_value.size?TDSQL_MYSQL_OK:TDSQL_MYSQL_INVALID;
    default: return TDSQL_MYSQL_UNSUPPORTED;
  }
}
static tdsql_mysql_status text_value(const turbodb_value_t *value,char buffer[RESPONSE_NUMBER_BYTES],mysql_wire_bytes_t *out) {
  *out=(mysql_wire_bytes_t){0};
  if(value->kind==TURBODB_VALUE_TEXT) { *out=(mysql_wire_bytes_t){(const uint8_t *)value->data.text_value.data,value->data.text_value.len,false}; return TDSQL_MYSQL_OK; }
  if(value->kind==TURBODB_VALUE_BLOB) { *out=(mysql_wire_bytes_t){value->data.blob_value.data,value->data.blob_value.size,false}; return TDSQL_MYSQL_OK; }
  int count=0;
  switch(value->kind) {
    case TURBODB_VALUE_INT64: count=snprintf(buffer,RESPONSE_NUMBER_BYTES,"%" PRId64,value->data.int64_value); break;
    case TURBODB_VALUE_UINT64: count=snprintf(buffer,RESPONSE_NUMBER_BYTES,"%" PRIu64,value->data.uint64_value); break;
    case TURBODB_VALUE_BOOLEAN: buffer[0]=value->data.boolean_value?'1':'0'; count=1; break;
    case TURBODB_VALUE_DOUBLE:
      if(strcmp(localeconv()->decimal_point,".")) return TDSQL_MYSQL_UNSUPPORTED;
      count=snprintf(buffer,RESPONSE_NUMBER_BYTES,"%.17g",value->data.double_value); break;
    default: return TDSQL_MYSQL_INVALID;
  }
  if(count<0 || count>=RESPONSE_NUMBER_BYTES) return TDSQL_MYSQL_LIMIT;
  *out=(mysql_wire_bytes_t){(const uint8_t *)buffer,(size_t)count,false}; return TDSQL_MYSQL_OK;
}
static void row_write(writer *w,const turbodb_value_t *values,size_t count,bool binary) {
  if(!values || !count) { w->status=TDSQL_MYSQL_INVALID; return; }
  if(count>UINT16_MAX) { w->status=TDSQL_MYSQL_LIMIT; return; }
  const size_t bitmap=(count+TDSQL_MYSQL_BYTE_BITS-1+RESPONSE_BINARY_OFFSET)/TDSQL_MYSQL_BYTE_BITS;
  size_t bitmap_offset=0;
  if(binary) {
    integer(w,0,1); bitmap_offset=w->size;
    /* Emit a bounded zero bitmap without allocating scratch per column. */
    for(size_t i=0;i<bitmap;++i) integer(w,0,1);
  }
  for(size_t i=0;i<count && w->status==TDSQL_MYSQL_OK;++i) {
    w->status=value_check(&values[i]);
    if(w->status!=TDSQL_MYSQL_OK) break;
    if(values[i].kind==TURBODB_VALUE_NULL) {
      if(!binary) integer(w,RESPONSE_NULL_MARKER,1);
      else if(w->data) w->data[bitmap_offset+(i+RESPONSE_BINARY_OFFSET)/TDSQL_MYSQL_BYTE_BITS]|=(uint8_t)(1u<<((i+RESPONSE_BINARY_OFFSET)%TDSQL_MYSQL_BYTE_BITS));
    } else if(!binary) {
      char number[RESPONSE_NUMBER_BYTES]; mysql_wire_bytes_t text={0};
      w->status=text_value(&values[i],number,&text); if(w->status==TDSQL_MYSQL_OK) bytes(w,text);
    } else switch(values[i].kind) {
      case TURBODB_VALUE_INT64: integer(w,(uint64_t)values[i].data.int64_value,8); break;
      case TURBODB_VALUE_UINT64: integer(w,values[i].data.uint64_value,8); break;
      case TURBODB_VALUE_DOUBLE: { uint64_t bits=0; memcpy(&bits,&values[i].data.double_value,sizeof(bits)); integer(w,bits,8); break; }
      case TURBODB_VALUE_BOOLEAN: integer(w,values[i].data.boolean_value,1); break;
      case TURBODB_VALUE_TEXT: bytes(w,(mysql_wire_bytes_t){(const uint8_t *)values[i].data.text_value.data,values[i].data.text_value.len,false}); break;
      case TURBODB_VALUE_BLOB: bytes(w,(mysql_wire_bytes_t){values[i].data.blob_value.data,values[i].data.blob_value.size,false}); break;
      default: w->status=TDSQL_MYSQL_UNSUPPORTED; break;
    }
  }
}
typedef enum response_kind { RESPONSE_OK,RESPONSE_ERR,RESPONSE_EOF,RESPONSE_PREPARE,RESPONSE_COLUMN,RESPONSE_COUNT,RESPONSE_ROW } response_kind;
typedef struct response {
  response_kind kind;
  union {
    const mysql_wire_ok_packet_t *ok;
    const mysql_wire_eof_packet_t *eof;
    const mysql_stmt_prepare_ok_t *prepare;
    const mysql_column_definition_t *column;
    const turbodb_value_t *values;
  } value;
  mysql_wire_bytes_t message;
  const char *sqlstate;
  uint16_t code;
  size_t count;
  bool flag;
} response;
static void response_write(const response *r,writer *w) {
  switch(r->kind) {
    case RESPONSE_OK: ok_write(w,r->value.ok,r->flag); break;
    case RESPONSE_ERR:
      if(!r->code || !r->sqlstate || r->message.is_null || (!r->message.data && r->message.length)) { w->status=TDSQL_MYSQL_INVALID; break; }
      if(r->message.length>TDSQL_MYSQL_ERRMSG_MAX) { w->status=TDSQL_MYSQL_LIMIT; break; }
      for(size_t i=0;i<RESPONSE_SQLSTATE_BYTES;++i)
        if(!((r->sqlstate[i]>='A' && r->sqlstate[i]<='Z') || (r->sqlstate[i]>='0' && r->sqlstate[i]<='9'))) { w->status=TDSQL_MYSQL_INVALID; return; }
      integer(w,RESPONSE_ERR_MARKER,1); integer(w,r->code,2); integer(w,'#',1);
      emit(w,r->sqlstate,RESPONSE_SQLSTATE_BYTES); emit(w,r->message.data,r->message.length); break;
    case RESPONSE_EOF:
      if(!r->value.eof) { w->status=TDSQL_MYSQL_INVALID; break; }
      w->status=status_flags(r->value.eof->status_flags);
      integer(w,RESPONSE_EOF_MARKER,1); integer(w,r->value.eof->warnings,2); integer(w,r->value.eof->status_flags,2); break;
    case RESPONSE_PREPARE: {
      const mysql_stmt_prepare_ok_t *p=r->value.prepare;
      if(!p || !p->statement_id) { w->status=TDSQL_MYSQL_INVALID; break; }
      integer(w,0,1); integer(w,p->statement_id,4); integer(w,p->column_count,2);
      integer(w,p->parameter_count,2); integer(w,0,1); integer(w,p->warning_count,2); break;
    }
    case RESPONSE_COLUMN: column_write(w,r->value.column); break;
    case RESPONSE_COUNT:
      if(!r->count) { w->status=TDSQL_MYSQL_INVALID; break; }
      if(r->count>UINT16_MAX) { w->status=TDSQL_MYSQL_LIMIT; break; }
      lenenc(w,r->count); break;
    case RESPONSE_ROW: row_write(w,r->value.values,r->count,r->flag); break;
  }
}
static tdsql_mysql_status response_encode(const response *r,tdsql_mysql_output *out) {
  if(!out) return TDSQL_MYSQL_INVALID;
  writer measure={.capacity=out->capacity}; response_write(r,&measure);
  if(measure.status!=TDSQL_MYSQL_OK) return measure.status;
  if(out->data) {
    writer output={.data=out->data,.capacity=out->capacity}; response_write(r,&output);
    if(output.status!=TDSQL_MYSQL_OK) return output.status;
  }
  out->size=measure.size; return TDSQL_MYSQL_OK;
}
tdsql_mysql_status tdsql_mysql_ok_encode(const mysql_wire_ok_packet_t *value,bool deprecated,tdsql_mysql_output *out) {
  const response r={.kind=RESPONSE_OK,.value.ok=value,.flag=deprecated}; return response_encode(&r,out);
}
tdsql_mysql_status tdsql_mysql_err_encode(uint16_t code,const char sqlstate[5],mysql_wire_bytes_t message,tdsql_mysql_output *out) {
  const response r={.kind=RESPONSE_ERR,.code=code,.sqlstate=sqlstate,.message=message}; return response_encode(&r,out);
}
tdsql_mysql_status tdsql_mysql_eof_encode(const mysql_wire_eof_packet_t *value,tdsql_mysql_output *out) {
  const response r={.kind=RESPONSE_EOF,.value.eof=value}; return response_encode(&r,out);
}
tdsql_mysql_status tdsql_mysql_prepare_encode(const mysql_stmt_prepare_ok_t *value,tdsql_mysql_output *out) {
  const response r={.kind=RESPONSE_PREPARE,.value.prepare=value}; return response_encode(&r,out);
}
tdsql_mysql_status tdsql_mysql_column_encode(const mysql_column_definition_t *value,tdsql_mysql_output *out) {
  const response r={.kind=RESPONSE_COLUMN,.value.column=value}; return response_encode(&r,out);
}
tdsql_mysql_status tdsql_mysql_count_encode(size_t value,tdsql_mysql_output *out) {
  const response r={.kind=RESPONSE_COUNT,.count=value}; return response_encode(&r,out);
}
tdsql_mysql_status tdsql_mysql_row_encode(const turbodb_value_t *values,size_t count,bool binary,tdsql_mysql_output *out) {
  const response r={.kind=RESPONSE_ROW,.value.values=values,.count=count,.flag=binary}; return response_encode(&r,out);
}

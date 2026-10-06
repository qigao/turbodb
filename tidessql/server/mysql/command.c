#include "wire.h"
#include <math.h>
#include <float.h>
#include <string.h>

_Static_assert(sizeof(double)==sizeof(uint64_t) && DBL_MANT_DIG==53 && DBL_MAX_EXP==1024,"MySQL DOUBLE requires binary64");
_Static_assert(sizeof(float)==sizeof(uint32_t) && FLT_MANT_DIG==24 && FLT_MAX_EXP==128,"MySQL FLOAT requires binary32");
static tdsql_mysql_status complete(mysql_wire_status_t status) {
  return status==MYSQL_WIRE_STATUS_NEED_MORE?TDSQL_MYSQL_INVALID:status;
}
tdsql_mysql_status tdsql_mysql_command_decode(const uint8_t *data,size_t size,size_t max_bytes,tdsql_mysql_command *out) {
  if(!data || !size || !max_bytes || !out) return TDSQL_MYSQL_INVALID;
  if(size>max_bytes) return TDSQL_MYSQL_LIMIT;
  tdsql_mysql_command command={.kind=data[0],.payload={data,size,false}};
  size_t offset=1;
  switch(command.kind) {
    case TDSQL_MYSQL_QUERY:
    case MYSQL_COM_STMT_PREPARE:
      if(size==1 || memchr(data+1,0,size-1)) return TDSQL_MYSQL_INVALID;
      command.sql=(mysql_wire_bytes_t){data+1,size-1,false}; break;
    case TDSQL_MYSQL_PING:
    case TDSQL_MYSQL_QUIT:
      if(size!=1) return TDSQL_MYSQL_INVALID;
      break;
    case MYSQL_COM_STMT_EXECUTE:
    case MYSQL_COM_STMT_CLOSE:
    case MYSQL_COM_STMT_RESET: {
      if(size<(command.kind==MYSQL_COM_STMT_EXECUTE?TDSQL_MYSQL_EXECUTE_HEADER:TDSQL_MYSQL_STATEMENT_COMMAND))
        return TDSQL_MYSQL_INVALID;
      tdsql_mysql_status status=complete(mysql_wire_read_u32_le(data,size,&offset,&command.statement_id));
      if(status!=TDSQL_MYSQL_OK || !command.statement_id) return TDSQL_MYSQL_INVALID;
      if(command.kind==MYSQL_COM_STMT_EXECUTE) {
        if(data[offset++]) return TDSQL_MYSQL_UNSUPPORTED;
        uint32_t iterations=0; status=complete(mysql_wire_read_u32_le(data,size,&offset,&iterations));
        if(status!=TDSQL_MYSQL_OK || iterations!=1) return TDSQL_MYSQL_INVALID;
      } else if(size!=TDSQL_MYSQL_STATEMENT_COMMAND) return TDSQL_MYSQL_INVALID;
      break;
    }
    default: return TDSQL_MYSQL_UNSUPPORTED;
  }
  *out=command; return TDSQL_MYSQL_OK;
}
static tdsql_mysql_status parameter_type(uint16_t type) {
  const uint8_t flags=(uint8_t)(type>>TDSQL_MYSQL_BYTE_BITS),kind=(uint8_t)type;
  if(flags && flags!=MYSQL_TYPE_UNSIGNED_FLAG) return TDSQL_MYSQL_INVALID;
  switch(kind) {
    case MYSQL_TYPE_LONGLONG: case MYSQL_FIELD_TYPE_LONG: case MYSQL_FIELD_TYPE_INT24:
    case MYSQL_FIELD_TYPE_SHORT: case MYSQL_TYPE_TINY: return TDSQL_MYSQL_OK;
    case MYSQL_TYPE_DOUBLE: case MYSQL_FIELD_TYPE_FLOAT: case MYSQL_TYPE_NULL:
    case MYSQL_TYPE_VAR_STRING: case MYSQL_FIELD_TYPE_STRING: case MYSQL_FIELD_TYPE_VARCHAR:
    case MYSQL_TYPE_BLOB: case MYSQL_FIELD_TYPE_TINY_BLOB:
    case MYSQL_FIELD_TYPE_MEDIUM_BLOB: case MYSQL_FIELD_TYPE_LONG_BLOB:
      return flags?TDSQL_MYSQL_INVALID:TDSQL_MYSQL_OK;
    default: return TDSQL_MYSQL_UNSUPPORTED;
  }
}
static tdsql_mysql_status parameter_value(const uint8_t *data,size_t size,size_t *offset,
    uint16_t type,size_t *retained,size_t max_bytes,turbodb_value_t *out) {
  const uint8_t kind=(uint8_t)type; const bool unsigned_value=(type>>TDSQL_MYSQL_BYTE_BITS)!=0;
  uint64_t word=0; uint32_t small=0; uint16_t short_word=0;
  tdsql_mysql_status status=TDSQL_MYSQL_OK;
  switch(kind) {
    case MYSQL_TYPE_NULL: *out=turbodb_null(); return status;
    case MYSQL_TYPE_LONGLONG: status=complete(mysql_wire_read_u64_le(data,size,offset,&word)); break;
    case MYSQL_FIELD_TYPE_LONG: case MYSQL_FIELD_TYPE_INT24:
      status=complete(mysql_wire_read_u32_le(data,size,offset,&small));
      if(status==TDSQL_MYSQL_OK) { int32_t signed_word=0; memcpy(&signed_word,&small,sizeof(small));
        *out=unsigned_value?turbodb_u64(small):turbodb_i64(signed_word); }
      return status;
    case MYSQL_FIELD_TYPE_SHORT:
      status=complete(mysql_wire_read_u16_le(data,size,offset,&short_word));
      if(status==TDSQL_MYSQL_OK) { int16_t signed_word=0; memcpy(&signed_word,&short_word,sizeof(short_word));
        *out=unsigned_value?turbodb_u64(short_word):turbodb_i64(signed_word); }
      return status;
    case MYSQL_TYPE_TINY: {
      if(*offset>=size) return TDSQL_MYSQL_INVALID;
      const uint8_t byte=data[(*offset)++]; int8_t signed_byte=0; memcpy(&signed_byte,&byte,sizeof(byte));
      *out=byte<=1 && !unsigned_value?turbodb_bool(byte):unsigned_value?turbodb_u64(byte):turbodb_i64(signed_byte);
      return status;
    }
    case MYSQL_TYPE_DOUBLE:
      status=complete(mysql_wire_read_u64_le(data,size,offset,&word));
      if(status==TDSQL_MYSQL_OK) {
        double value=0; memcpy(&value,&word,sizeof(value));
        if(!isfinite(value)) return TDSQL_MYSQL_INVALID;
        *out=turbodb_f64(value);
      }
      return status;
    case MYSQL_FIELD_TYPE_FLOAT:
      status=complete(mysql_wire_read_u32_le(data,size,offset,&small));
      if(status==TDSQL_MYSQL_OK) {
        float value=0; memcpy(&value,&small,sizeof(value));
        if(!isfinite(value)) return TDSQL_MYSQL_INVALID;
        *out=turbodb_f64(value);
      }
      return status;
    default: {
      mysql_wire_bytes_t bytes={0}; status=complete(mysql_wire_read_lenenc_bytes(data,size,offset,&bytes));
      if(status!=TDSQL_MYSQL_OK) return status;
      if(bytes.is_null) return TDSQL_MYSQL_INVALID;
      if(*retained>max_bytes || bytes.length>max_bytes-*retained) return TDSQL_MYSQL_LIMIT;
      *retained+=bytes.length;
      const bool text=kind==MYSQL_TYPE_VAR_STRING || kind==MYSQL_FIELD_TYPE_STRING || kind==MYSQL_FIELD_TYPE_VARCHAR;
      *out=text?turbodb_text_v((vstr){(const char *)bytes.data,bytes.length}):turbodb_blob(bytes.data,bytes.length); return status;
    }
  }
  if(status==TDSQL_MYSQL_OK) { int64_t signed_word=0; memcpy(&signed_word,&word,sizeof(word));
    *out=unsigned_value?turbodb_u64(word):turbodb_i64(signed_word); }
  return status;
}
typedef struct execute_layout { size_t bitmap,types,values; bool new_types; } execute_layout;
static tdsql_mysql_status execute_layout_open(const tdsql_mysql_command *command,size_t count,bool valid,
    execute_layout *out) {
  const uint8_t *data=command->payload.data; const size_t size=command->payload.length;
  size_t offset=TDSQL_MYSQL_EXECUTE_HEADER;
  if(!count) return size==offset?TDSQL_MYSQL_OK:TDSQL_MYSQL_INVALID;
  const size_t bitmap=(count+TDSQL_MYSQL_BYTE_BITS-1)/TDSQL_MYSQL_BYTE_BITS;
  if(size<offset || bitmap>=size-offset) return TDSQL_MYSQL_INVALID;
  if(count%TDSQL_MYSQL_BYTE_BITS && (data[offset+bitmap-1]>>(count%TDSQL_MYSQL_BYTE_BITS))) return TDSQL_MYSQL_INVALID;
  offset+=bitmap;
  const uint8_t new_types=data[offset++];
  if(new_types>1) return TDSQL_MYSQL_INVALID;
  if(!new_types && !valid) return TDSQL_MYSQL_INVALID;
  *out=(execute_layout){.bitmap=TDSQL_MYSQL_EXECUTE_HEADER,.types=offset,.new_types=new_types!=0};
  if(new_types) {
    if(count>(size-offset)/TDSQL_MYSQL_TYPE_BYTES) return TDSQL_MYSQL_INVALID;
    offset+=count*TDSQL_MYSQL_TYPE_BYTES;
  }
  out->values=offset; return TDSQL_MYSQL_OK;
}
static uint16_t execute_type(const tdsql_mysql_command *command,const execute_layout *layout,
    const uint16_t *types,size_t index) {
  if(!layout->new_types) return types[index];
  const uint8_t *bytes=command->payload.data+layout->types+index*TDSQL_MYSQL_TYPE_BYTES;
  return (uint16_t)((uint16_t)bytes[0]|((uint16_t)bytes[1]<<TDSQL_MYSQL_BYTE_BITS));
}
static tdsql_mysql_status execute_values(const tdsql_mysql_command *command,size_t count,
    const uint16_t *types,const execute_layout *layout,turbodb_value_t *values,size_t max_bytes) {
  size_t offset=layout->values,retained=0;
  const uint8_t *data=command->payload.data; const size_t size=command->payload.length;
  for(size_t i=0;i<count;++i) {
    const uint16_t type=execute_type(command,layout,types,i);
    tdsql_mysql_status status=parameter_type(type);
    if(status!=TDSQL_MYSQL_OK) return status;
    turbodb_value_t value=turbodb_null();
    if(!(data[layout->bitmap+i/TDSQL_MYSQL_BYTE_BITS]&(uint8_t)(1u<<(i%TDSQL_MYSQL_BYTE_BITS))))
      status=parameter_value(data,size,&offset,type,&retained,max_bytes,&value);
    if(status!=TDSQL_MYSQL_OK) return status;
    if(values) values[i]=value;
  }
  return offset==size?TDSQL_MYSQL_OK:TDSQL_MYSQL_INVALID;
}
tdsql_mysql_status tdsql_mysql_execute_decode(const tdsql_mysql_command *command,size_t count,
    uint16_t *types,size_t type_capacity,bool *valid,turbodb_value_t *values,size_t value_capacity,size_t max_bytes) {
  if(!command || !valid || !max_bytes || (!types && count) || (!values && count)) return TDSQL_MYSQL_INVALID;
  if(count>UINT16_MAX || count>type_capacity || count>value_capacity) return TDSQL_MYSQL_LIMIT;
  tdsql_mysql_command verified={0};
  tdsql_mysql_status status=tdsql_mysql_command_decode(command->payload.data,command->payload.length,command->payload.length,&verified);
  if(status!=TDSQL_MYSQL_OK) return status;
  if(verified.kind!=MYSQL_COM_STMT_EXECUTE || verified.statement_id!=command->statement_id) return TDSQL_MYSQL_INVALID;
  execute_layout layout={0}; status=execute_layout_open(command,count,*valid,&layout);
  if(status!=TDSQL_MYSQL_OK || !count) return status;
  status=execute_values(command,count,types,&layout,NULL,max_bytes);
  if(status!=TDSQL_MYSQL_OK) return status;
  status=execute_values(command,count,types,&layout,values,max_bytes);
  if(status!=TDSQL_MYSQL_OK) return status;
  if(layout.new_types) for(size_t i=0;i<count;++i) types[i]=execute_type(command,&layout,types,i);
  *valid=true; return TDSQL_MYSQL_OK;
}

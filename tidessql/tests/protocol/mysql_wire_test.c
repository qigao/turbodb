#include "wire.h"
#include <tinytest.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { TEST_BUFFER=512, TEST_VALUES=7, TEST_SENTINEL=0xa5, TEST_STATEMENT=42,
       TEST_SMALL_CAPACITY=64, TEST_TYPES_OFFSET=12, TEST_LONG_DATA=0x18,TEST_FETCH=0x1c };
static uint8_t output[TEST_BUFFER],storage[TEST_BUFFER];
static tdsql_mysql_output destination;
static tdsql_mysql_input receiver;
static void reset_output(size_t capacity) {
  memset(output,TEST_SENTINEL,sizeof(output)); destination=(tdsql_mysql_output){output,capacity,TEST_SENTINEL};
}
static mysql_wire_bytes_t text(const char *value) { return (mysql_wire_bytes_t){(const uint8_t *)value,strlen(value),false}; }
static mysql_column_definition_t column(const char *name,uint8_t type,uint16_t flags) {
  return (mysql_column_definition_t){.catalog=text("def"),.name=text(name),.character_set=63,
      .column_length=TEST_BUFFER,.type=type,.flags=flags,.decimals=0};
}
static tdsql_mysql_command execute_command(const uint8_t *payload,size_t size) {
  tdsql_mysql_command command={0};
  check_equal(tdsql_mysql_command_decode(payload,size,TEST_BUFFER,&command),TDSQL_MYSQL_OK); return command;
}
static void unchanged(void) {
  uint8_t expected[TEST_BUFFER]; memset(expected,TEST_SENTINEL,sizeof(expected));
  check_equal(output,expected,sizeof(expected)); check_equal(destination.size,(size_t)TEST_SENTINEL);
}
static void value_is(const turbodb_value_t *value,const turbodb_value_t *expected) {
  check_equal(value->kind,expected->kind); check_equal(value->reserved,0u);
  switch(value->kind) {
    case TURBODB_VALUE_INT64: check_equal(value->data.int64_value,expected->data.int64_value); break;
    case TURBODB_VALUE_UINT64: check_equal(value->data.uint64_value,expected->data.uint64_value); break;
    case TURBODB_VALUE_DOUBLE: check_equal(value->data.double_value,expected->data.double_value); break;
    case TURBODB_VALUE_BOOLEAN: check_equal(value->data.boolean_value,expected->data.boolean_value); break;
    case TURBODB_VALUE_TEXT:
      check_equal(value->data.text_value.len,expected->data.text_value.len);
      check_equal(value->data.text_value.data,expected->data.text_value.data,expected->data.text_value.len); break;
    case TURBODB_VALUE_BLOB:
      check_equal(value->data.blob_value.size,expected->data.blob_value.size);
      check_equal(value->data.blob_value.data,expected->data.blob_value.data,expected->data.blob_value.size); break;
  }
}
spec("TidesSQL MySQL server wire codec") {
  before_each() {
    reset_output(sizeof(output)); memset(storage,TEST_SENTINEL,sizeof(storage));
    check_equal(tdsql_mysql_input_init(&receiver,storage,sizeof(storage),0),TDSQL_MYSQL_OK);
  }
  group("bounded framing") {
    it("publishes only a complete request across every byte boundary") {
      const uint8_t packet[]={3,0,0,0,TDSQL_MYSQL_QUERY,'s','q'};
      for(size_t i=0;i<sizeof(packet);++i) {
        size_t consumed=TEST_SENTINEL;
        check_equal(tdsql_mysql_input_feed(&receiver,packet+i,1,&consumed),i+1==sizeof(packet)?TDSQL_MYSQL_OK:TDSQL_MYSQL_NEED_MORE);
        check_equal(consumed,1u); mysql_wire_bytes_t message={0};
        check_equal(tdsql_mysql_input_message(&receiver,&message),i+1==sizeof(packet)?TDSQL_MYSQL_OK:TDSQL_MYSQL_NEED_MORE);
      }
      mysql_wire_bytes_t message={0}; check_equal(tdsql_mysql_input_message(&receiver,&message),TDSQL_MYSQL_OK);
      check_equal(message.length,3u); check_equal(message.data,packet+4,3);
      size_t consumed=TEST_SENTINEL; check_equal(tdsql_mysql_input_feed(&receiver,packet,sizeof(packet),&consumed),TDSQL_MYSQL_BUSY); check_equal(consumed,0u);
      check_equal(tdsql_mysql_input_release(&receiver,0),TDSQL_MYSQL_OK);
      check_equal(tdsql_mysql_input_release(&receiver,0),TDSQL_MYSQL_BUSY);
    }
    it("stops at one message and exposes unconsumed pipelined bytes") {
      const uint8_t packets[]={1,0,0,0,TDSQL_MYSQL_PING,1,0,0,0,TDSQL_MYSQL_QUIT}; size_t consumed=0;
      check_equal(tdsql_mysql_input_feed(&receiver,packets,sizeof(packets),&consumed),TDSQL_MYSQL_OK); check_equal(consumed,5u);
      mysql_wire_bytes_t message={0}; check_equal(tdsql_mysql_input_message(&receiver,&message),TDSQL_MYSQL_OK); check_equal(message.data[0],TDSQL_MYSQL_PING);
      check_equal(tdsql_mysql_input_release(&receiver,0),TDSQL_MYSQL_OK);
      check_equal(tdsql_mysql_input_feed(&receiver,packets+consumed,sizeof(packets)-consumed,&consumed),TDSQL_MYSQL_OK);
      check_equal(consumed,5u); check_equal(tdsql_mysql_input_message(&receiver,&message),TDSQL_MYSQL_OK); check_equal(message.data[0],TDSQL_MYSQL_QUIT);
    }
    it("rejects announced overflow and wrong sequence without resynchronizing") {
      const uint8_t overflow[]={0,3,0,0},wrong[]={0,0,0,1}; size_t consumed=0;
      check_equal(tdsql_mysql_input_feed(&receiver,overflow,sizeof(overflow),&consumed),TDSQL_MYSQL_LIMIT); check_equal(consumed,4u);
      check_equal(receiver.used,0u); mysql_wire_bytes_t message={0};
      check_equal(tdsql_mysql_input_message(&receiver,&message),TDSQL_MYSQL_LIMIT);
      check_equal(tdsql_mysql_input_release(&receiver,0),TDSQL_MYSQL_LIMIT);
      check_equal(tdsql_mysql_input_feed(&receiver,wrong,sizeof(wrong),&consumed),TDSQL_MYSQL_LIMIT); check_equal(consumed,0u);
      check_equal(tdsql_mysql_input_init(&receiver,storage,sizeof(storage),0),TDSQL_MYSQL_OK);
      check_equal(tdsql_mysql_input_feed(&receiver,wrong,sizeof(wrong),&consumed),TDSQL_MYSQL_SEQUENCE);
      check_equal(tdsql_mysql_input_feed(&receiver,NULL,0,&consumed),TDSQL_MYSQL_SEQUENCE); check_equal(consumed,0u);
    }
    it("encodes independent golden headers and preserves insufficient output") {
      const uint8_t payload[]={TDSQL_MYSQL_PING}; const uint8_t golden[]={1,0,0,9,TDSQL_MYSQL_PING}; uint8_t next=0;
      check_equal(tdsql_mysql_frame_encode(payload,sizeof(payload),9,&destination,&next),TDSQL_MYSQL_OK);
      check_equal(destination.size,sizeof(golden)); check_equal(output,golden,sizeof(golden)); check_equal(next,10u);
      reset_output(sizeof(golden)-1); next=TEST_SENTINEL;
      check_equal(tdsql_mysql_frame_encode(payload,sizeof(payload),9,&destination,&next),TDSQL_MYSQL_LIMIT); unchanged(); check_equal(next,TEST_SENTINEL);
      destination=(tdsql_mysql_output){NULL,sizeof(golden),0};
      check_equal(tdsql_mysql_frame_encode(payload,sizeof(payload),9,&destination,&next),TDSQL_MYSQL_OK); check_equal(destination.size,sizeof(golden));
      reset_output(sizeof(output)); check_equal(tdsql_mysql_frame_encode(NULL,0,0,&destination,&next),TDSQL_MYSQL_OK);
      const uint8_t zero[]={0,0,0,0}; check_equal(output,zero,sizeof(zero)); check_equal(destination.size,4u);
    }
    it("requires the empty terminator for exact maximum payload and wraps sequence") {
      const size_t count=MYSQL_WIRE_PACKET_MAX_PAYLOAD,wire_size=count+2*MYSQL_WIRE_PACKET_HEADER_SIZE;
      uint8_t *payload=malloc(count),*wire=malloc(wire_size),*buffer=malloc(count);
      check_not_null(payload); check_not_null(wire); check_not_null(buffer); memset(payload,'x',count);
      tdsql_mysql_output encoded={wire,wire_size,0}; uint8_t next=0;
      check_equal(tdsql_mysql_frame_encode(payload,count,UINT8_MAX,&encoded,&next),TDSQL_MYSQL_OK); check_equal(next,1u);
      const uint8_t start[]={255,255,255,255},last[]={0,0,0,0};
      check_equal(wire,start,sizeof(start)); check_equal(wire+count+4,last,sizeof(last));
      tdsql_mysql_input input={0}; check_equal(tdsql_mysql_input_init(&input,buffer,count,UINT8_MAX),TDSQL_MYSQL_OK);
      size_t consumed=0; check_equal(tdsql_mysql_input_feed(&input,wire,count+4,&consumed),TDSQL_MYSQL_NEED_MORE); check_equal(consumed,count+4);
      mysql_wire_bytes_t message={0}; check_equal(tdsql_mysql_input_message(&input,&message),TDSQL_MYSQL_NEED_MORE);
      check_equal(tdsql_mysql_input_feed(&input,wire+count+4,2,&consumed),TDSQL_MYSQL_NEED_MORE);
      check_equal(tdsql_mysql_input_feed(&input,wire+count+6,2,&consumed),TDSQL_MYSQL_OK);
      check_equal(tdsql_mysql_input_message(&input,&message),TDSQL_MYSQL_OK); check_equal(message.length,count); check_equal(message.data,payload,count);
      check_equal(tdsql_mysql_input_init(&input,buffer,count-1,UINT8_MAX),TDSQL_MYSQL_OK);
      check_equal(tdsql_mysql_input_feed(&input,wire,4,&consumed),TDSQL_MYSQL_LIMIT);
      free(buffer); free(wire); free(payload);
    }
    it("rejects arithmetic overflow before inspecting payload or writing output") {
      const uint8_t byte=0; uint8_t next=TEST_SENTINEL;
      check_equal(tdsql_mysql_frame_encode(&byte,SIZE_MAX,0,&destination,&next),TDSQL_MYSQL_LIMIT);
      unchanged(); check_equal(next,TEST_SENTINEL);
      const turbodb_value_t value=turbodb_blob(&byte,SIZE_MAX);
      check_equal(tdsql_mysql_row_encode(&value,1,false,&destination),TDSQL_MYSQL_LIMIT); unchanged();
      check_equal(tdsql_mysql_row_encode(&value,1,true,&destination),TDSQL_MYSQL_LIMIT); unchanged();
    }
    it("assembles a real maximum-size continuation followed by a short packet") {
      const size_t count=MYSQL_WIRE_PACKET_MAX_PAYLOAD+3,wire_size=count+2*MYSQL_WIRE_PACKET_HEADER_SIZE;
      uint8_t *payload=malloc(count),*wire=malloc(wire_size),*buffer=malloc(count);
      check_not_null(payload); check_not_null(wire); check_not_null(buffer); memset(payload,'x',count);
      payload[0]=TDSQL_MYSQL_QUERY; tdsql_mysql_output encoded={wire,wire_size,0}; uint8_t next=0;
      check_equal(tdsql_mysql_frame_encode(payload,count,0,&encoded,&next),TDSQL_MYSQL_OK); check_equal(next,2u);
      const uint8_t tail_header[]={3,0,0,1}; check_equal(wire+MYSQL_WIRE_PACKET_MAX_PAYLOAD+4,tail_header,sizeof(tail_header));
      tdsql_mysql_input input={0}; check_equal(tdsql_mysql_input_init(&input,buffer,count,0),TDSQL_MYSQL_OK);
      size_t consumed=0; check_equal(tdsql_mysql_input_feed(&input,wire,encoded.size,&consumed),TDSQL_MYSQL_OK); check_equal(consumed,encoded.size);
      mysql_wire_bytes_t message={0}; check_equal(tdsql_mysql_input_message(&input,&message),TDSQL_MYSQL_OK); check_equal(message.length,count); check_equal(message.data,payload,count);
      tdsql_mysql_command command={0}; check_equal(tdsql_mysql_command_decode(message.data,message.length,count,&command),TDSQL_MYSQL_OK); check_equal(command.sql.length,count-1);
      check_equal(tdsql_mysql_input_init(&input,buffer,count-1,0),TDSQL_MYSQL_OK);
      check_equal(tdsql_mysql_input_feed(&input,wire,MYSQL_WIRE_PACKET_MAX_PAYLOAD+4,&consumed),TDSQL_MYSQL_NEED_MORE);
      check_equal(tdsql_mysql_input_feed(&input,wire+MYSQL_WIRE_PACKET_MAX_PAYLOAD+4,4,&consumed),TDSQL_MYSQL_LIMIT);
      free(buffer); free(wire); free(payload);
    }
  }
  group("commands and native parameters") {
    it("decodes existing client prepare close reset and zero-parameter execute") {
      size_t size=0; tdsql_mysql_command command={0};
      check_equal(mysql_wire_build_stmt_prepare((const uint8_t *)"SELECT 1",strlen("SELECT 1"),output,sizeof(output),&size),MYSQL_WIRE_STATUS_OK);
      check_equal(tdsql_mysql_command_decode(output,size,sizeof(output),&command),TDSQL_MYSQL_OK);
      check_equal(command.kind,MYSQL_COM_STMT_PREPARE); check_equal(command.sql.data,"SELECT 1",command.sql.length);
      check_equal(mysql_wire_build_stmt_close(TEST_STATEMENT,output,sizeof(output),&size),MYSQL_WIRE_STATUS_OK);
      check_equal(tdsql_mysql_command_decode(output,size,sizeof(output),&command),TDSQL_MYSQL_OK); check_equal(command.statement_id,TEST_STATEMENT);
      check_equal(mysql_wire_build_stmt_reset(TEST_STATEMENT,output,sizeof(output),&size),MYSQL_WIRE_STATUS_OK);
      check_equal(tdsql_mysql_command_decode(output,size,sizeof(output),&command),TDSQL_MYSQL_OK); check_equal(command.kind,MYSQL_COM_STMT_RESET);
      check_equal(mysql_wire_build_stmt_execute(TEST_STATEMENT,NULL,0,output,sizeof(output),&size),MYSQL_WIRE_STATUS_OK);
      command=execute_command(output,size); bool valid=false;
      check_equal(tdsql_mysql_execute_decode(&command,0,NULL,0,&valid,NULL,0,TEST_BUFFER),TDSQL_MYSQL_OK); check_false(valid);
    }
    it("decodes seven driver values without rewriting SQL or retaining bytes") {
      const uint8_t blob[]={0,255,1};
      const mysql_stmt_value_t input[]={
        {.kind=MYSQL_STMT_VALUE_NULL}, {.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=INT64_MIN},
        {.kind=MYSQL_STMT_VALUE_UINT64,.data.uint64_value=UINT64_MAX}, {.kind=MYSQL_STMT_VALUE_DOUBLE,.data.double_value=-1.5},
        {.kind=MYSQL_STMT_VALUE_BOOL,.data.bool_value=1}, {.kind=MYSQL_STMT_VALUE_TEXT,.data.bytes={(const uint8_t *)"'quoted'",8}},
        {.kind=MYSQL_STMT_VALUE_BLOB,.data.bytes={blob,sizeof(blob)}}
      };
      size_t size=0; check_equal(mysql_wire_build_stmt_execute(TEST_STATEMENT,input,TEST_VALUES,output,sizeof(output),&size),MYSQL_WIRE_STATUS_OK);
      tdsql_mysql_command command=execute_command(output,size); uint16_t types[TEST_VALUES]={0}; bool valid=false; turbodb_value_t values[TEST_VALUES]={0};
      check_equal(tdsql_mysql_execute_decode(&command,TEST_VALUES,types,TEST_VALUES,&valid,values,TEST_VALUES,TEST_BUFFER),TDSQL_MYSQL_OK); check_true(valid);
      const turbodb_value_t expected[]={turbodb_null(),turbodb_i64(INT64_MIN),turbodb_u64(UINT64_MAX),turbodb_f64(-1.5),turbodb_bool(1),turbodb_text("'quoted'"),turbodb_blob(blob,sizeof(blob))};
      for(size_t i=0;i<TEST_VALUES;++i) value_is(&values[i],&expected[i]);
      check_true((const uint8_t *)values[5].data.text_value.data>=output && (const uint8_t *)values[5].data.text_value.data<output+size);
      check_true((const uint8_t *)values[6].data.blob_value.data>=output && (const uint8_t *)values[6].data.blob_value.data<output+size);
    }
    it("decodes independent golden integer widths unsigned flags and binary32") {
      const struct { uint8_t kind,flags,width,bytes[8]; turbodb_value_t expected; } cases[]={
        {MYSQL_TYPE_TINY,0,1,{0x80},turbodb_i64(INT8_MIN)},
        {MYSQL_TYPE_TINY,MYSQL_TYPE_UNSIGNED_FLAG,1,{0xff},turbodb_u64(UINT8_MAX)},
        {MYSQL_TYPE_TINY,0,1,{0},turbodb_bool(0)},
        {MYSQL_FIELD_TYPE_SHORT,0,2,{0,0x80},turbodb_i64(INT16_MIN)},
        {MYSQL_FIELD_TYPE_SHORT,MYSQL_TYPE_UNSIGNED_FLAG,2,{0xff,0xff},turbodb_u64(UINT16_MAX)},
        {MYSQL_FIELD_TYPE_LONG,0,4,{0,0,0,0x80},turbodb_i64(INT32_MIN)},
        {MYSQL_FIELD_TYPE_LONG,MYSQL_TYPE_UNSIGNED_FLAG,4,{0xff,0xff,0xff,0xff},turbodb_u64(UINT32_MAX)},
        {MYSQL_FIELD_TYPE_INT24,0,4,{0,0,0x80,0xff},turbodb_i64(-8388608)},
        {MYSQL_FIELD_TYPE_INT24,MYSQL_TYPE_UNSIGNED_FLAG,4,{0xff,0xff,0xff,0},turbodb_u64(16777215)},
        {MYSQL_FIELD_TYPE_FLOAT,0,4,{0,0,0xc0,0x3f},turbodb_f64(1.5)},
        {MYSQL_FIELD_TYPE_FLOAT,0,4,{0,0,0,0x80},turbodb_f64(-0.0)}
      };
      const uint8_t prefix[]={MYSQL_COM_STMT_EXECUTE,TEST_STATEMENT,0,0,0,0,1,0,0,0,0,1};
      for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        memcpy(output,prefix,sizeof(prefix)); output[TEST_TYPES_OFFSET]=cases[i].kind; output[TEST_TYPES_OFFSET+1]=cases[i].flags;
        memcpy(output+sizeof(prefix)+TDSQL_MYSQL_TYPE_BYTES,cases[i].bytes,cases[i].width);
        const size_t size=sizeof(prefix)+TDSQL_MYSQL_TYPE_BYTES+cases[i].width;
        tdsql_mysql_command command=execute_command(output,size); uint16_t type=0; bool valid=false; turbodb_value_t value=turbodb_null();
        check_equal(tdsql_mysql_execute_decode(&command,1,&type,1,&valid,&value,1,TEST_BUFFER),TDSQL_MYSQL_OK);
        check_true(valid); value_is(&value,&cases[i].expected);
        check_equal(type,(uint16_t)(cases[i].kind|((uint16_t)cases[i].flags<<TDSQL_MYSQL_BYTE_BITS)));
        if(cases[i].kind==MYSQL_FIELD_TYPE_FLOAT && cases[i].expected.data.double_value==0) check_true(signbit(value.data.double_value));
        command=execute_command(output,size-1); value=turbodb_i64(TEST_SENTINEL);
        check_equal(tdsql_mysql_execute_decode(&command,1,&type,1,&valid,&value,1,TEST_BUFFER),TDSQL_MYSQL_INVALID);
        check_equal(value.data.int64_value,TEST_SENTINEL);
      }
    }
    it("distinguishes byte type aliases and rejects a NULL length prefix outside the bitmap") {
      const uint8_t types[]={MYSQL_TYPE_VAR_STRING,MYSQL_FIELD_TYPE_STRING,MYSQL_FIELD_TYPE_VARCHAR,
          MYSQL_TYPE_BLOB,MYSQL_FIELD_TYPE_TINY_BLOB,MYSQL_FIELD_TYPE_MEDIUM_BLOB,MYSQL_FIELD_TYPE_LONG_BLOB};
      const uint8_t golden[]={MYSQL_COM_STMT_EXECUTE,TEST_STATEMENT,0,0,0,0,1,0,0,0,0,1,MYSQL_TYPE_BLOB,0,3,'a',0,'b'};
      for(size_t i=0;i<sizeof(types);++i) {
        memcpy(output,golden,sizeof(golden)); output[TEST_TYPES_OFFSET]=types[i];
        tdsql_mysql_command command=execute_command(output,sizeof(golden)); uint16_t type=0; bool valid=false; turbodb_value_t value=turbodb_null();
        check_equal(tdsql_mysql_execute_decode(&command,1,&type,1,&valid,&value,1,TEST_BUFFER),TDSQL_MYSQL_OK);
        const turbodb_value_t expected=i<3?turbodb_text_v((vstr){"a\0b",3}):turbodb_blob("a\0b",3);
        value_is(&value,&expected);
        output[14]=0xfb; command=execute_command(output,15); value=turbodb_i64(TEST_SENTINEL);
        check_equal(tdsql_mysql_execute_decode(&command,1,&type,1,&valid,&value,1,TEST_BUFFER),TDSQL_MYSQL_INVALID);
        check_equal(value.data.int64_value,TEST_SENTINEL);
      }
    }
    it("reuses only previously validated types and does not publish a failed replacement") {
      const mysql_stmt_value_t value={.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=7}; size_t size=0;
      check_equal(mysql_wire_build_stmt_execute(TEST_STATEMENT,&value,1,output,sizeof(output),&size),MYSQL_WIRE_STATUS_OK);
      tdsql_mysql_command command=execute_command(output,size); uint16_t type=0; bool valid=false; turbodb_value_t decoded=turbodb_null();
      check_equal(tdsql_mysql_execute_decode(&command,1,&type,1,&valid,&decoded,1,TEST_BUFFER),TDSQL_MYSQL_OK); check_equal(type,MYSQL_TYPE_LONGLONG);
      output[11]=0; memmove(output+12,output+14,size-14); size-=2; command=execute_command(output,size);
      valid=false; decoded=turbodb_i64(TEST_SENTINEL);
      check_equal(tdsql_mysql_execute_decode(&command,1,&type,1,&valid,&decoded,1,TEST_BUFFER),TDSQL_MYSQL_INVALID);
      check_false(valid); check_equal(decoded.data.int64_value,TEST_SENTINEL);
      valid=true; check_equal(tdsql_mysql_execute_decode(&command,1,&type,1,&valid,&decoded,1,TEST_BUFFER),TDSQL_MYSQL_OK); check_equal(decoded.data.int64_value,7);
      check_equal(mysql_wire_build_stmt_execute(TEST_STATEMENT,&value,1,output,sizeof(output),&size),MYSQL_WIRE_STATUS_OK);
      output[TEST_TYPES_OFFSET]=MYSQL_TYPE_DOUBLE; size--; command=execute_command(output,size); decoded=turbodb_i64(TEST_SENTINEL);
      check_equal(tdsql_mysql_execute_decode(&command,1,&type,1,&valid,&decoded,1,TEST_BUFFER),TDSQL_MYSQL_INVALID);
      check_true(valid); check_equal(type,MYSQL_TYPE_LONGLONG); check_equal(decoded.data.int64_value,TEST_SENTINEL);
    }
    it("rejects every truncated execute and trailing bytes before publishing values") {
      const mysql_stmt_value_t input[]={ {.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=7}, {.kind=MYSQL_STMT_VALUE_TEXT,.data.bytes={(const uint8_t *)"abc",3}} };
      size_t size=0; check_equal(mysql_wire_build_stmt_execute(TEST_STATEMENT,input,2,output,sizeof(output),&size),MYSQL_WIRE_STATUS_OK);
      for(size_t prefix=0;prefix<size;++prefix) {
        tdsql_mysql_command command={0}; const tdsql_mysql_status status=tdsql_mysql_command_decode(output,prefix,sizeof(output),&command);
        if(status==TDSQL_MYSQL_OK) {
          uint16_t types[]={MYSQL_TYPE_NULL,MYSQL_TYPE_NULL}; bool valid=false; turbodb_value_t values[]={turbodb_i64(TEST_SENTINEL),turbodb_i64(TEST_SENTINEL)};
          check_equal(tdsql_mysql_execute_decode(&command,2,types,2,&valid,values,2,TEST_BUFFER),TDSQL_MYSQL_INVALID);
          check_false(valid); check_equal(types[0],MYSQL_TYPE_NULL); check_equal(values[0].data.int64_value,TEST_SENTINEL);
        } else check_equal(status,TDSQL_MYSQL_INVALID);
      }
      output[size++]=0; tdsql_mysql_command command=execute_command(output,size);
      uint16_t types[2]={0}; bool valid=false; turbodb_value_t values[2]={0};
      check_equal(tdsql_mysql_execute_decode(&command,2,types,2,&valid,values,2,TEST_BUFFER),TDSQL_MYSQL_INVALID); check_false(valid);
    }
    it("rejects unsupported commands flags and types including NULL hidden unknown types") {
      const uint8_t rejected[]={TEST_LONG_DATA,TEST_FETCH,0xff}; tdsql_mysql_command command={.statement_id=TEST_SENTINEL};
      for(size_t i=0;i<sizeof(rejected);++i) check_equal(tdsql_mysql_command_decode(rejected+i,1,TEST_BUFFER,&command),TDSQL_MYSQL_UNSUPPORTED);
      check_equal(command.statement_id,TEST_SENTINEL);
      const mysql_stmt_value_t value={.kind=MYSQL_STMT_VALUE_NULL}; size_t size=0;
      check_equal(mysql_wire_build_stmt_execute(TEST_STATEMENT,&value,1,output,sizeof(output),&size),MYSQL_WIRE_STATUS_OK);
      output[5]=1; check_equal(tdsql_mysql_command_decode(output,size,sizeof(output),&command),TDSQL_MYSQL_UNSUPPORTED);
      output[5]=0; command=execute_command(output,size); uint16_t type=0; bool valid=false; turbodb_value_t decoded=turbodb_i64(TEST_SENTINEL);
      output[TEST_TYPES_OFFSET]=MYSQL_FIELD_TYPE_NEWDECIMAL;
      check_equal(tdsql_mysql_execute_decode(&command,1,&type,1,&valid,&decoded,1,TEST_BUFFER),TDSQL_MYSQL_UNSUPPORTED);
      check_false(valid); check_equal(decoded.data.int64_value,TEST_SENTINEL);
      output[TEST_TYPES_OFFSET]=MYSQL_TYPE_NULL; output[TEST_TYPES_OFFSET+1]=1;
      check_equal(tdsql_mysql_execute_decode(&command,1,&type,1,&valid,&decoded,1,TEST_BUFFER),TDSQL_MYSQL_INVALID);
    }
    it("bounds aggregate parameter payload and preserves outputs on capacity failure") {
      const mysql_stmt_value_t input[]={ {.kind=MYSQL_STMT_VALUE_TEXT,.data.bytes={(const uint8_t *)"abc",3}}, {.kind=MYSQL_STMT_VALUE_BLOB,.data.bytes={(const uint8_t *)"xyz",3}} };
      size_t size=0; check_equal(mysql_wire_build_stmt_execute(TEST_STATEMENT,input,2,output,sizeof(output),&size),MYSQL_WIRE_STATUS_OK);
      tdsql_mysql_command command=execute_command(output,size); uint16_t types[2]={0}; bool valid=false; turbodb_value_t values[2]={0};
      check_equal(tdsql_mysql_execute_decode(&command,2,types,2,&valid,values,2,5),TDSQL_MYSQL_LIMIT); check_false(valid); check_equal(types[0],0u);
      check_equal(tdsql_mysql_execute_decode(&command,2,types,1,&valid,values,2,TEST_BUFFER),TDSQL_MYSQL_LIMIT);
      check_equal(tdsql_mysql_execute_decode(&command,2,types,2,&valid,values,1,TEST_BUFFER),TDSQL_MYSQL_LIMIT);
      check_equal(tdsql_mysql_execute_decode(&command,2,types,2,&valid,values,2,6),TDSQL_MYSQL_OK); check_true(valid);
    }
    it("rejects malformed flags bitmaps iterations nonfinite numbers and SQL admission") {
      size_t size=0; mysql_stmt_value_t value={.kind=MYSQL_STMT_VALUE_DOUBLE,.data.double_value=INFINITY};
      check_equal(mysql_wire_build_stmt_execute(TEST_STATEMENT,&value,1,output,sizeof(output),&size),MYSQL_WIRE_STATUS_OK);
      tdsql_mysql_command command=execute_command(output,size); uint16_t type=MYSQL_TYPE_NULL; bool valid=false; turbodb_value_t decoded=turbodb_i64(TEST_SENTINEL);
      check_equal(tdsql_mysql_execute_decode(&command,1,&type,1,&valid,&decoded,1,TEST_BUFFER),TDSQL_MYSQL_INVALID); check_false(valid); check_equal(decoded.data.int64_value,TEST_SENTINEL);
      value.kind=MYSQL_STMT_VALUE_SINT64; value.data.sint64_value=7;
      check_equal(mysql_wire_build_stmt_execute(TEST_STATEMENT,&value,1,output,sizeof(output),&size),MYSQL_WIRE_STATUS_OK); command=execute_command(output,size);
      output[10]=0x80; check_equal(tdsql_mysql_execute_decode(&command,1,&type,1,&valid,&decoded,1,TEST_BUFFER),TDSQL_MYSQL_INVALID);
      output[10]=0; output[11]=2; check_equal(tdsql_mysql_execute_decode(&command,1,&type,1,&valid,&decoded,1,TEST_BUFFER),TDSQL_MYSQL_INVALID);
      output[11]=1; output[6]=2; check_equal(tdsql_mysql_command_decode(output,size,TEST_BUFFER,&command),TDSQL_MYSQL_INVALID);
      const uint8_t no_sql[]={TDSQL_MYSQL_QUERY},zero_sql[]={TDSQL_MYSQL_QUERY,'x',0,'y'},unknown_id[]={MYSQL_COM_STMT_CLOSE,0,0,0,0};
      check_equal(tdsql_mysql_command_decode(no_sql,sizeof(no_sql),TEST_BUFFER,&command),TDSQL_MYSQL_INVALID);
      check_equal(tdsql_mysql_command_decode(zero_sql,sizeof(zero_sql),TEST_BUFFER,&command),TDSQL_MYSQL_INVALID);
      check_equal(tdsql_mysql_command_decode(unknown_id,sizeof(unknown_id),TEST_BUFFER,&command),TDSQL_MYSQL_INVALID);
      const uint8_t sql[]={TDSQL_MYSQL_QUERY,'x'}; check_equal(tdsql_mysql_command_decode(sql,sizeof(sql),1,&command),TDSQL_MYSQL_LIMIT);
      const uint8_t extra_ping[]={TDSQL_MYSQL_PING,0}; check_equal(tdsql_mysql_command_decode(extra_ping,sizeof(extra_ping),TEST_BUFFER,&command),TDSQL_MYSQL_INVALID);
    }
  }
  group("server responses") {
    it("encodes independent OK ERR and EOF golden payloads readable by the client") {
      const uint8_t ok_golden[]={0,0,0,2,0,0,0};
      mysql_wire_ok_packet_t packet={.status_flags=TDSQL_MYSQL_SERVER_AUTOCOMMIT};
      check_equal(tdsql_mysql_ok_encode(&packet,false,&destination),TDSQL_MYSQL_OK); check_equal(output,ok_golden,sizeof(ok_golden));
      mysql_wire_ok_packet_t decoded={0}; check_equal(mysql_wire_decode_ok_packet(output,destination.size,MYSQL_WIRE_CLIENT_PROTOCOL_41,&decoded),MYSQL_WIRE_STATUS_OK);
      check_equal(decoded.status_flags,TDSQL_MYSQL_SERVER_AUTOCOMMIT);
      check_equal(tdsql_mysql_err_encode(1064,"42000",text("syntax"),&destination),TDSQL_MYSQL_OK);
      const uint8_t err_golden[]={255,0x28,4,'#','4','2','0','0','0','s','y','n','t','a','x'};
      check_equal(output,err_golden,sizeof(err_golden)); mysql_wire_err_packet_t err={0};
      check_equal(mysql_wire_decode_err_packet(output,destination.size,MYSQL_WIRE_CLIENT_PROTOCOL_41,&err),MYSQL_WIRE_STATUS_OK);
      check_equal(err.error_code,1064u); check_equal(err.sql_state,"42000",5); check_equal(err.message.data,"syntax",6);
      const mysql_wire_eof_packet_t eof={.warnings=1,.status_flags=TDSQL_MYSQL_SERVER_IN_TRANS};
      check_equal(tdsql_mysql_eof_encode(&eof,&destination),TDSQL_MYSQL_OK); const uint8_t eof_golden[]={254,1,0,1,0}; check_equal(output,eof_golden,sizeof(eof_golden));
      mysql_wire_eof_packet_t parsed={0}; check_equal(mysql_wire_decode_eof_packet(output,destination.size,MYSQL_WIRE_CLIENT_PROTOCOL_41,&parsed),MYSQL_WIRE_STATUS_OK);
      check_equal(parsed.warnings,1u); check_equal(parsed.status_flags,TDSQL_MYSQL_SERVER_IN_TRANS);
    }
    it("encodes prepare response and complete column metadata for all value kinds") {
      const mysql_stmt_prepare_ok_t packet={.statement_id=TEST_STATEMENT,.column_count=TEST_VALUES,.parameter_count=TEST_VALUES,.warning_count=1};
      check_equal(tdsql_mysql_prepare_encode(&packet,&destination),TDSQL_MYSQL_OK);
      const uint8_t golden[]={0,TEST_STATEMENT,0,0,0,TEST_VALUES,0,TEST_VALUES,0,0,1,0}; check_equal(output,golden,sizeof(golden));
      mysql_stmt_prepare_ok_t decoded={0}; check_equal(mysql_wire_decode_stmt_prepare_ok(output,destination.size,&decoded),MYSQL_WIRE_STATUS_OK);
      check_equal(decoded.statement_id,TEST_STATEMENT); check_equal(decoded.column_count,TEST_VALUES);
      const uint8_t kinds[]={MYSQL_TYPE_NULL,MYSQL_TYPE_LONGLONG,MYSQL_TYPE_LONGLONG,MYSQL_TYPE_DOUBLE,MYSQL_TYPE_TINY,MYSQL_TYPE_VAR_STRING,MYSQL_TYPE_BLOB};
      for(size_t i=0;i<TEST_VALUES;++i) {
        const mysql_column_definition_t info=column("value",kinds[i],i==2?MYSQL_COLUMN_FLAG_UNSIGNED:0);
        check_equal(tdsql_mysql_column_encode(&info,&destination),TDSQL_MYSQL_OK);
        mysql_column_definition_t parsed={0}; check_equal(mysql_wire_decode_column_definition41(output,destination.size,&parsed),MYSQL_WIRE_STATUS_OK);
        check_equal(parsed.type,kinds[i]); check_equal(parsed.flags,info.flags); check_equal(parsed.name.data,"value",5); check_equal(parsed.catalog.data,"def",3);
      }
      check_equal(tdsql_mysql_count_encode(TEST_VALUES,&destination),TDSQL_MYSQL_OK); check_equal(output[0],TEST_VALUES); check_equal(destination.size,1u);
    }
    it("encodes text rows with raw bytes NULL and exact numeric boundaries") {
      const uint8_t blob[]={0,255}; const turbodb_value_t values[]={turbodb_null(),turbodb_i64(INT64_MIN),turbodb_u64(UINT64_MAX),turbodb_f64(-1.5),turbodb_bool(1),turbodb_text("'quoted'"),turbodb_blob(blob,sizeof(blob))};
      check_equal(tdsql_mysql_row_encode(values,TEST_VALUES,false,&destination),TDSQL_MYSQL_OK);
      mysql_wire_bytes_t parsed[TEST_VALUES]={0}; check_equal(mysql_wire_decode_text_row(output,destination.size,TEST_VALUES,parsed,TEST_VALUES),MYSQL_WIRE_STATUS_OK);
      check_true(parsed[0].is_null); check_equal(parsed[1].data,"-9223372036854775808",20); check_equal(parsed[2].data,"18446744073709551615",20);
      check_equal(parsed[3].data,"-1.5",4); check_equal(parsed[4].data,"1",1); check_equal(parsed[5].data,"'quoted'",8); check_equal(parsed[6].data,blob,sizeof(blob));
    }
    it("encodes binary rows using metadata and the result bitmap offset of two") {
      const uint8_t blob[]={0,255}; const turbodb_value_t values[]={turbodb_null(),turbodb_i64(INT64_MIN),turbodb_u64(UINT64_MAX),turbodb_f64(-1.5),turbodb_bool(1),turbodb_text("abc"),turbodb_blob(blob,sizeof(blob))};
      const uint8_t types[]={MYSQL_TYPE_NULL,MYSQL_TYPE_LONGLONG,MYSQL_TYPE_LONGLONG,MYSQL_TYPE_DOUBLE,MYSQL_TYPE_TINY,MYSQL_TYPE_VAR_STRING,MYSQL_TYPE_BLOB};
      mysql_column_definition_t columns[TEST_VALUES]; for(size_t i=0;i<TEST_VALUES;++i) columns[i]=column("value",types[i],i==2?MYSQL_COLUMN_FLAG_UNSIGNED:0);
      check_equal(tdsql_mysql_row_encode(values,TEST_VALUES,true,&destination),TDSQL_MYSQL_OK); check_equal(output[0],0u); check_equal(output[1],4u); check_equal(output[2],0u);
      mysql_binary_value_t parsed[TEST_VALUES]={0};
      check_equal(mysql_wire_decode_binary_row(output,destination.size,columns,TEST_VALUES,parsed,TEST_VALUES),MYSQL_WIRE_STATUS_OK);
      check_equal(parsed[0].kind,MYSQL_BINARY_VALUE_NULL); check_equal(parsed[1].data.sint64_value,INT64_MIN); check_equal(parsed[2].data.uint64_value,UINT64_MAX);
      check_equal(parsed[3].data.double_value,-1.5); check_equal(parsed[4].data.sint64_value,1); check_equal(parsed[5].data.bytes.data,"abc",3); check_equal(parsed[6].data.bytes.data,blob,sizeof(blob));
    }
    it("preserves every output byte on invalid or insufficient row encoding") {
      turbodb_value_t values[]={turbodb_i64(7),turbodb_text("abc"),turbodb_f64(1.5)};
      for(size_t mode=0;mode<2;++mode) {
        reset_output(sizeof(output)); check_equal(tdsql_mysql_row_encode(values,3,mode!=0,&destination),TDSQL_MYSQL_OK); const size_t required=destination.size;
        for(size_t capacity=0;capacity<required;++capacity) {
          reset_output(capacity); check_equal(tdsql_mysql_row_encode(values,3,mode!=0,&destination),TDSQL_MYSQL_LIMIT); unchanged();
        }
        reset_output(sizeof(output)); values[2]=turbodb_f64(INFINITY);
        check_equal(tdsql_mysql_row_encode(values,3,mode!=0,&destination),TDSQL_MYSQL_INVALID); unchanged(); values[2]=turbodb_f64(1.5);
        values[1].reserved=1; check_equal(tdsql_mysql_row_encode(values,3,mode!=0,&destination),TDSQL_MYSQL_INVALID); unchanged(); values[1].reserved=0;
      }
    }
    it("rejects unsupported state and enforces negotiated EOF markers") {
      mysql_wire_ok_packet_t packet={.header=0xfe,.status_flags=TDSQL_MYSQL_SERVER_AUTOCOMMIT};
      check_equal(tdsql_mysql_ok_encode(&packet,false,&destination),TDSQL_MYSQL_UNSUPPORTED); unchanged();
      check_equal(tdsql_mysql_ok_encode(&packet,true,&destination),TDSQL_MYSQL_OK);
      mysql_wire_ok_packet_t parsed={0};
      check_equal(mysql_wire_decode_ok_packet(output,destination.size,MYSQL_WIRE_CLIENT_PROTOCOL_41|MYSQL_WIRE_CLIENT_DEPRECATE_EOF,&parsed),MYSQL_WIRE_STATUS_OK);
      reset_output(sizeof(output)); packet.status_flags=MYSQL_WIRE_SERVER_MORE_RESULTS_EXISTS;
      check_equal(tdsql_mysql_ok_encode(&packet,true,&destination),TDSQL_MYSQL_UNSUPPORTED); unchanged();
      packet.status_flags=MYSQL_WIRE_SERVER_SESSION_STATE_CHANGED;
      check_equal(tdsql_mysql_ok_encode(&packet,true,&destination),TDSQL_MYSQL_UNSUPPORTED); unchanged();
      check_equal(tdsql_mysql_err_encode(1064,"bad!!",text("failure"),&destination),TDSQL_MYSQL_INVALID); unchanged();
    }
    it("preserves all packet encoders on one-byte-short output and invalid metadata") {
      const mysql_wire_ok_packet_t ok={.affected_rows=UINT64_MAX,.last_insert_id=UINT64_MAX};
      const mysql_wire_eof_packet_t eof={0}; const mysql_stmt_prepare_ok_t prepared={.statement_id=TEST_STATEMENT};
      mysql_column_definition_t definition=column("value",MYSQL_TYPE_VAR_STRING,0);
      check_equal(tdsql_mysql_ok_encode(&ok,false,&destination),TDSQL_MYSQL_OK); size_t required=destination.size;
      reset_output(required-1); check_equal(tdsql_mysql_ok_encode(&ok,false,&destination),TDSQL_MYSQL_LIMIT); unchanged();
      reset_output(4); check_equal(tdsql_mysql_eof_encode(&eof,&destination),TDSQL_MYSQL_LIMIT); unchanged();
      reset_output(11); check_equal(tdsql_mysql_prepare_encode(&prepared,&destination),TDSQL_MYSQL_LIMIT); unchanged();
      reset_output(sizeof(output)); check_equal(tdsql_mysql_column_encode(&definition,&destination),TDSQL_MYSQL_OK); required=destination.size;
      reset_output(required-1); check_equal(tdsql_mysql_column_encode(&definition,&destination),TDSQL_MYSQL_LIMIT); unchanged();
      reset_output(1); check_equal(tdsql_mysql_count_encode(251,&destination),TDSQL_MYSQL_LIMIT); unchanged();
      reset_output(sizeof(output)); definition.type=MYSQL_FIELD_TYPE_NEWDECIMAL;
      check_equal(tdsql_mysql_column_encode(&definition,&destination),TDSQL_MYSQL_UNSUPPORTED); unchanged();
      char oversized[TDSQL_MYSQL_ERRMSG_MAX+1]; memset(oversized,'x',sizeof(oversized));
      const mysql_wire_bytes_t oversized_message={(const uint8_t *)oversized,sizeof(oversized),false};
      check_equal(tdsql_mysql_err_encode(1064,"HY000",oversized_message,&destination),TDSQL_MYSQL_LIMIT); unchanged();
      const mysql_wire_eof_packet_t impossible={.status_flags=TDSQL_MYSQL_SERVER_IN_TRANS_READONLY};
      check_equal(tdsql_mysql_eof_encode(&impossible,&destination),TDSQL_MYSQL_INVALID); unchanged();
    }
    it("encodes bitmap rollover and independent length-encoding boundaries") {
      turbodb_value_t values[9]; for(size_t i=0;i<9;++i) values[i]=turbodb_null();
      check_equal(tdsql_mysql_row_encode(values,9,true,&destination),TDSQL_MYSQL_OK); const uint8_t bitmap[]={0,0xfc,0x07}; check_equal(output,bitmap,sizeof(bitmap)); check_equal(destination.size,sizeof(bitmap));
      check_equal(tdsql_mysql_count_encode(250,&destination),TDSQL_MYSQL_OK); check_equal(output[0],250u); check_equal(destination.size,1u);
      check_equal(tdsql_mysql_count_encode(251,&destination),TDSQL_MYSQL_OK); const uint8_t first[]={0xfc,0xfb,0}; check_equal(output,first,sizeof(first));
      check_equal(tdsql_mysql_count_encode(UINT16_MAX,&destination),TDSQL_MYSQL_OK); const uint8_t last[]={0xfc,0xff,0xff}; check_equal(output,last,sizeof(last));
      reset_output(sizeof(output)); check_equal(tdsql_mysql_count_encode((size_t)UINT16_MAX+1,&destination),TDSQL_MYSQL_LIMIT); unchanged();
      destination=(tdsql_mysql_output){NULL,TEST_BUFFER,0};
      const turbodb_value_t value=turbodb_text("abc"); check_equal(tdsql_mysql_row_encode(&value,1,false,&destination),TDSQL_MYSQL_OK); check_equal(destination.size,4u);
    }
  }
}

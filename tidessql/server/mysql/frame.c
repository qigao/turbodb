#include "wire.h"
#include <string.h>

tdsql_mysql_status tdsql_mysql_input_init(tdsql_mysql_input *out,uint8_t *storage,size_t capacity,uint8_t sequence) {
  if(!out || !storage || !capacity) return TDSQL_MYSQL_INVALID;
  tdsql_mysql_input input={.storage=storage,.capacity=capacity};
  const mysql_wire_status_t status=mysql_wire_packet_stream_init(&input.stream,sequence,MYSQL_WIRE_PACKET_MAX_PAYLOAD);
  if(status==MYSQL_WIRE_STATUS_OK) *out=input;
  return status;
}
tdsql_mysql_status tdsql_mysql_input_feed(tdsql_mysql_input *input,const uint8_t *data,size_t size,size_t *consumed) {
  if(consumed) *consumed=0;
  if(!input || !input->storage || !input->capacity || !consumed || (!data && size)) return TDSQL_MYSQL_INVALID;
  if(input->failure) return input->failure;
  if(input->ready) return TDSQL_MYSQL_BUSY;
  size_t cursor=0;
  while(cursor<size) {
    mysql_wire_packet_event_t event={0}; size_t taken=0;
    const mysql_wire_status_t status=mysql_wire_packet_stream_feed(&input->stream,data+cursor,size-cursor,&taken,&event);
    cursor+=taken; *consumed=cursor;
    if(status!=MYSQL_WIRE_STATUS_OK && status!=MYSQL_WIRE_STATUS_NEED_MORE) {
      input->failure=status; return status;
    }
    /* Reject the announced remaining payload before buffering a large body. */
    const size_t remaining=event.packet_payload_length?event.packet_payload_length-event.offset:
        input->stream.in_payload?input->stream.packet_payload_length-input->stream.payload_seen:0;
    if(input->used>input->capacity || remaining>input->capacity-input->used || event.length>input->capacity-input->used) {
      input->failure=TDSQL_MYSQL_LIMIT; return input->failure;
    }
    if(event.length) { memcpy(input->storage+input->used,event.data,event.length); input->used+=event.length; }
    if(event.message_end) { input->ready=true; return TDSQL_MYSQL_OK; }
    if(status==MYSQL_WIRE_STATUS_NEED_MORE) return TDSQL_MYSQL_NEED_MORE;
    if(!taken) { input->failure=TDSQL_MYSQL_INVALID; return input->failure; }
  }
  return TDSQL_MYSQL_NEED_MORE;
}
tdsql_mysql_status tdsql_mysql_input_message(const tdsql_mysql_input *input,mysql_wire_bytes_t *out) {
  if(!input || !input->storage || !out) return TDSQL_MYSQL_INVALID;
  if(input->failure) return input->failure;
  if(!input->ready) return TDSQL_MYSQL_NEED_MORE;
  *out=(mysql_wire_bytes_t){input->storage,input->used,false}; return TDSQL_MYSQL_OK;
}
tdsql_mysql_status tdsql_mysql_input_release(tdsql_mysql_input *input,uint8_t sequence) {
  if(!input || !input->storage) return TDSQL_MYSQL_INVALID;
  if(input->failure) return input->failure;
  if(!input->ready) return TDSQL_MYSQL_BUSY;
  input->used=0; input->ready=false; mysql_wire_packet_stream_reset(&input->stream,sequence); return TDSQL_MYSQL_OK;
}
tdsql_mysql_status tdsql_mysql_frame_encode(const uint8_t *payload,size_t size,uint8_t sequence,
    tdsql_mysql_output *out,uint8_t *next) {
  if(!out || !next || (!payload && size)) return TDSQL_MYSQL_INVALID;
  const size_t packets=size/MYSQL_WIRE_PACKET_MAX_PAYLOAD+1;
  if(packets>(SIZE_MAX-size)/MYSQL_WIRE_PACKET_HEADER_SIZE) return TDSQL_MYSQL_LIMIT;
  const size_t total=size+packets*MYSQL_WIRE_PACKET_HEADER_SIZE;
  if(total>out->capacity) return TDSQL_MYSQL_LIMIT;
  if(out->data) {
    size_t position=0,offset=0;
    for(size_t i=0;i<packets;++i) {
      const size_t remaining=size-offset;
      const uint32_t bytes=(uint32_t)(remaining>MYSQL_WIRE_PACKET_MAX_PAYLOAD?MYSQL_WIRE_PACKET_MAX_PAYLOAD:remaining);
      mysql_wire_status_t status=mysql_wire_write_u24_le(out->data,total,&position,bytes);
      if(status!=MYSQL_WIRE_STATUS_OK) return status;
      out->data[position++]=sequence++;
      if(bytes) memcpy(out->data+position,payload+offset,bytes);
      position+=bytes; offset+=bytes;
    }
  } else sequence=(uint8_t)(sequence+(uint8_t)packets);
  out->size=total; *next=sequence; return TDSQL_MYSQL_OK;
}

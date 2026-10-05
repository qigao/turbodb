#ifndef TDSQL_MYSQL_SERVER_WIRE_H
#define TDSQL_MYSQL_SERVER_WIRE_H
#include <turbodb/types.h>
#include <wire/packet.h>
#include <wire/statement.h>
#include <wire/row.h>
#include <wire/result.h>

/* Private serialized-owner codec, no allocation or SQL/network side effects. */
typedef int32_t tdsql_mysql_status;
enum {
  TDSQL_MYSQL_OK=MYSQL_WIRE_STATUS_OK, TDSQL_MYSQL_NEED_MORE=MYSQL_WIRE_STATUS_NEED_MORE,
  TDSQL_MYSQL_INVALID=MYSQL_WIRE_STATUS_INVALID, TDSQL_MYSQL_LIMIT=MYSQL_WIRE_STATUS_LIMIT,
  TDSQL_MYSQL_SEQUENCE=MYSQL_WIRE_STATUS_SEQUENCE, TDSQL_MYSQL_UNSUPPORTED=5, TDSQL_MYSQL_BUSY=6
};
enum { TDSQL_MYSQL_QUIT=0x01, TDSQL_MYSQL_QUERY=0x03, TDSQL_MYSQL_PING=0x0e,
       TDSQL_MYSQL_EXECUTE_HEADER=10, TDSQL_MYSQL_STATEMENT_COMMAND=5,
       TDSQL_MYSQL_SERVER_IN_TRANS=0x0001, TDSQL_MYSQL_SERVER_AUTOCOMMIT=0x0002,
       TDSQL_MYSQL_SERVER_IN_TRANS_READONLY=0x2000, TDSQL_MYSQL_ERRMSG_MAX=512,
       TDSQL_MYSQL_BYTE_BITS=8, TDSQL_MYSQL_TYPE_BYTES=2 };

typedef struct tdsql_mysql_input {
  mysql_wire_packet_stream_t stream;
  uint8_t *storage;
  size_t capacity, used;
  tdsql_mysql_status failure;
  bool ready;
} tdsql_mysql_input;
/* Capacity is the total logical message limit. storage must remain stable,
 * separate from feed input, until disposal. Success replaces out. */
tdsql_mysql_status tdsql_mysql_input_init(tdsql_mysql_input *,uint8_t *storage,size_t capacity,uint8_t sequence);
/* Consumes through at most one complete message. NEED_MORE means none published;
 * ready/fatal failure reject more feed. consumed is always initialized. */
tdsql_mysql_status tdsql_mysql_input_feed(tdsql_mysql_input *,const uint8_t *,size_t,size_t *consumed);
/* Read-only view until release/disposal; no mutation or partial request. */
tdsql_mysql_status tdsql_mysql_input_message(const tdsql_mysql_input *,mysql_wire_bytes_t *);
/* Only ready messages release. Fatal framing errors require disposing input,
 * not retry/resync. The owner releases after all borrowed views expire. */
tdsql_mysql_status tdsql_mysql_input_release(tdsql_mysql_input *,uint8_t next_sequence);

typedef struct tdsql_mysql_command {
  uint8_t kind;
  uint32_t statement_id;
  mysql_wire_bytes_t sql, payload;
} tdsql_mysql_command;
/* Complete logical payload only; max_bytes bounds admission. Unknown commands,
 * long data/cursors/attributes explicitly UNSUPPORTED; malformed payload INVALID.
 * SQL/execute bytes borrow payload. Failure preserves out. */
tdsql_mysql_status tdsql_mysql_command_decode(const uint8_t *,size_t,size_t max_bytes,tdsql_mysql_command *);
/* Type words use low byte enum_field_type, high byte 0/0x80 unsigned. Caller
 * owns count slots; count is PREPARE's count (<= UINT16_MAX), never from wire.
 * Cache valid flag and arrays publish only after entire payload validation;
 * NULL bitmap does not bypass unsupported type checks. Values borrow payload
 * through execution; capacities and aggregate TEXT/BLOB bytes are hard limits. No aliases
 * between payload, types, values or valid flag. Failure preserves all outputs. */
tdsql_mysql_status tdsql_mysql_execute_decode(const tdsql_mysql_command *,size_t count,
    uint16_t *types,size_t type_capacity,bool *types_valid,
    turbodb_value_t *values,size_t value_capacity,size_t max_parameter_bytes);

typedef struct tdsql_mysql_output { uint8_t *data; size_t capacity,size; } tdsql_mysql_output;
/* Encoders preserve bytes/size on failure. NULL data measures required size
 * within capacity. Inputs must not overlap output or mutate during the call.
 * No caller view may outlive the owning input/result. O(bytes+values) time,
 * constant extra work, no allocations. Payload encode excludes packet headers. */
tdsql_mysql_status tdsql_mysql_frame_encode(const uint8_t *,size_t,uint8_t sequence,
    tdsql_mysql_output *,uint8_t *next_sequence);
tdsql_mysql_status tdsql_mysql_ok_encode(const mysql_wire_ok_packet_t *,bool deprecate_eof,tdsql_mysql_output *);
tdsql_mysql_status tdsql_mysql_err_encode(uint16_t code,const char sqlstate[5],mysql_wire_bytes_t message,tdsql_mysql_output *);
tdsql_mysql_status tdsql_mysql_eof_encode(const mysql_wire_eof_packet_t *,tdsql_mysql_output *);
tdsql_mysql_status tdsql_mysql_prepare_encode(const mysql_stmt_prepare_ok_t *,tdsql_mysql_output *);
tdsql_mysql_status tdsql_mysql_column_encode(const mysql_column_definition_t *,tdsql_mysql_output *);
tdsql_mysql_status tdsql_mysql_count_encode(size_t count,tdsql_mysql_output *);
tdsql_mysql_status tdsql_mysql_row_encode(const turbodb_value_t *,size_t count,bool binary,tdsql_mysql_output *);
#endif

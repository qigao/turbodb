#ifndef TDSQL_MYSQL_SERVER_DISPATCH_H
#define TDSQL_MYSQL_SERVER_DISPATCH_H
#include "registry.h"

enum { TDSQL_MYSQL_REPLY_PACKET=1,TDSQL_MYSQL_REPLY_COMPLETE=2,TDSQL_MYSQL_REPLY_DISCONNECT=3,
       TDSQL_MYSQL_MIN_REPLY_BYTES=TDSQL_MYSQL_ERRMSG_MAX+9 };
/* Private command-phase owner, initialized after authentication/negotiation.
 * connection exclusively borrows until disposal; caller makes no other SDK
 * requests on it. Fixed scratch borrows likewise, no copies/reentrancy.
 * No input is retained after accept. Only dispatcher consumes its SDK result. */
typedef struct tdsql_mysql_dispatch {
  tdsql_mysql_registry registry;
  tdsql_response response;
  mysql_stmt_prepare_ok_t prepared;
  turbodb_error_t diagnostic;
  uint8_t *scratch;
  size_t capacity,payload_bytes,index;
  uint32_t phase;
  uint16_t error_code;
  uint8_t sequence,next_sequence;
  bool deprecate_eof,binary,pending,emitted,close_after_reply;
} tdsql_mysql_dispatch;

/* Zero output, stable nonaliasing scratch at least MIN_REPLY_BYTES and at most
 * UINT32_MAX bytes. config bounds registry/requests/SDK execution. EOF choice
 * is the actual negotiated capability, no authentication bypass here. */
turbodb_status_t tdsql_mysql_dispatch_init(tdsql_mysql_dispatch *,tdsql_connection *,
    const tdsql_mysql_registry_config *,bool deprecate_eof,uint8_t *,size_t,turbodb_error_t *);
/* Complete admitted logical request only, next_sequence from frame assembler.
 * OK means a command is accepted (SQL failure plans ERR). Active reply -> BUSY.
 * No-response unsupported/malformed commands are terminal failures. CLOSE/QUIT
 * never produce packets. input borrows this call, not the outgoing stream. */
turbodb_status_t tdsql_mysql_dispatch_accept(tdsql_mysql_dispatch *,const uint8_t *,size_t,
    uint8_t next_sequence,turbodb_error_t *);
/* Output does not alias scratch. Success returns PACKET, COMPLETE or DISCONNECT;
 * only PACKET replaces output bytes/size. Failed frame capacity preserves out
 * and cached payload (including consumed row), may retry the same packet with
 * enough capacity. Successful emit blocks further emit until acknowledge.
 * Fixed scratch overflow plans an ERR and checked-closes the result. */
turbodb_status_t tdsql_mysql_dispatch_emit(tdsql_mysql_dispatch *,tdsql_mysql_output *,int *state,turbodb_error_t *);
/* Exactly once after complete output transfers to owned transport or is written.
 * Not peer acknowledgement. Caller expires views before scratch/output reuse;
 * only ACK advances phase/index/sequence. No ACK after failed/measurement emit. */
turbodb_status_t tdsql_mysql_dispatch_acknowledge(tdsql_mysql_dispatch *,turbodb_error_t *);
/* Stop network admission first. Checked-destroys result, disposes registry;
 * does not own connection. Non-BUSY consumes even on cleanup failure; caller
 * closes SQL connection next for disconnect rollback, then shared database. */
turbodb_status_t tdsql_mysql_dispatch_dispose(tdsql_mysql_dispatch *,turbodb_error_t *);
#endif

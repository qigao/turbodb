#ifndef TURBODB_MYSQL_WIRE_PACKET_H
#define TURBODB_MYSQL_WIRE_PACKET_H

#include "codec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYSQL_WIRE_PACKET_HEADER_SIZE 4u
#define MYSQL_WIRE_PACKET_MAX_PAYLOAD UINT32_C(0x00ffffff)

typedef struct mysql_wire_packet_event_t {
  const uint8_t *data;
  size_t length;
  uint32_t offset;
  uint32_t packet_payload_length;
  uint8_t sequence_id;
  bool packet_end;
  bool message_end;
} mysql_wire_packet_event_t;

typedef struct mysql_wire_packet_stream_t {
  uint8_t header[MYSQL_WIRE_PACKET_HEADER_SIZE];
  size_t header_used;
  uint32_t packet_payload_length;
  uint32_t payload_seen;
  uint32_t max_payload;
  uint8_t sequence_id;
  uint8_t expected_sequence;
  bool in_payload;
} mysql_wire_packet_stream_t;

mysql_wire_status_t mysql_wire_packet_header_decode(
    const uint8_t *header, size_t size, uint8_t expected_sequence,
    uint32_t max_payload, uint32_t *payload_length, uint8_t *sequence_id);

mysql_wire_status_t mysql_wire_packet_stream_init(
    mysql_wire_packet_stream_t *stream, uint8_t expected_sequence,
    uint32_t max_payload);
void mysql_wire_packet_stream_reset(
    mysql_wire_packet_stream_t *stream, uint8_t expected_sequence);

mysql_wire_status_t mysql_wire_packet_stream_feed(
    mysql_wire_packet_stream_t *stream, const uint8_t *input,
    size_t input_size, size_t *consumed, mysql_wire_packet_event_t *event);

#ifdef __cplusplus
}
#endif

#endif

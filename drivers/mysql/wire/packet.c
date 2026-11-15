#include "packet.h"

#include <string.h>

mysql_wire_status_t mysql_wire_packet_header_decode(
    const uint8_t *header, size_t size, uint8_t expected_sequence,
    uint32_t max_payload, uint32_t *payload_length, uint8_t *sequence_id) {
  uint32_t length;

  if (header == NULL || payload_length == NULL || sequence_id == NULL ||
      max_payload > MYSQL_WIRE_PACKET_MAX_PAYLOAD)
    return MYSQL_WIRE_STATUS_INVALID;
  if (size < MYSQL_WIRE_PACKET_HEADER_SIZE)
    return MYSQL_WIRE_STATUS_NEED_MORE;

  length = (uint32_t)header[0] |
           ((uint32_t)header[1] << 8u) |
           ((uint32_t)header[2] << 16u);
  if (length > max_payload)
    return MYSQL_WIRE_STATUS_LIMIT;
  if (header[3] != expected_sequence)
    return MYSQL_WIRE_STATUS_SEQUENCE;

  *payload_length = length;
  *sequence_id = header[3];
  return MYSQL_WIRE_STATUS_OK;
}

mysql_wire_status_t mysql_wire_packet_stream_init(
    mysql_wire_packet_stream_t *stream, uint8_t expected_sequence,
    uint32_t max_payload) {
  if (stream == NULL || max_payload > MYSQL_WIRE_PACKET_MAX_PAYLOAD)
    return MYSQL_WIRE_STATUS_INVALID;
  memset(stream, 0, sizeof(*stream));
  stream->expected_sequence = expected_sequence;
  stream->max_payload = max_payload;
  return MYSQL_WIRE_STATUS_OK;
}

void mysql_wire_packet_stream_reset(
    mysql_wire_packet_stream_t *stream, uint8_t expected_sequence) {
  uint32_t max_payload;
  if (stream == NULL)
    return;
  max_payload = stream->max_payload;
  memset(stream, 0, sizeof(*stream));
  stream->expected_sequence = expected_sequence;
  stream->max_payload = max_payload;
}

mysql_wire_status_t mysql_wire_packet_stream_feed(
    mysql_wire_packet_stream_t *stream, const uint8_t *input,
    size_t input_size, size_t *consumed, mysql_wire_packet_event_t *event) {
  size_t cursor = 0u;
  mysql_wire_status_t status;

  if (stream == NULL || consumed == NULL || event == NULL ||
      (input == NULL && input_size != 0u))
    return MYSQL_WIRE_STATUS_INVALID;

  *consumed = 0u;
  memset(event, 0, sizeof(*event));

  if (!stream->in_payload) {
    while (stream->header_used < MYSQL_WIRE_PACKET_HEADER_SIZE &&
           cursor < input_size) {
      stream->header[stream->header_used++] = input[cursor++];
    }
    *consumed = cursor;
    if (stream->header_used < MYSQL_WIRE_PACKET_HEADER_SIZE)
      return MYSQL_WIRE_STATUS_NEED_MORE;

    status = mysql_wire_packet_header_decode(
        stream->header, sizeof(stream->header), stream->expected_sequence,
        stream->max_payload, &stream->packet_payload_length,
        &stream->sequence_id);
    if (status != MYSQL_WIRE_STATUS_OK)
      return status;

    stream->payload_seen = 0u;
    stream->in_payload = true;

    if (stream->packet_payload_length == 0u) {
      event->packet_payload_length = 0u;
      event->sequence_id = stream->sequence_id;
      event->packet_end = true;
      event->message_end = true;
      stream->expected_sequence = (uint8_t)(stream->expected_sequence + 1u);
      stream->header_used = 0u;
      stream->in_payload = false;
      return MYSQL_WIRE_STATUS_OK;
    }
  }

  if (cursor == input_size) {
    *consumed = cursor;
    return MYSQL_WIRE_STATUS_NEED_MORE;
  }

  {
    uint32_t remaining =
        stream->packet_payload_length - stream->payload_seen;
    size_t available = input_size - cursor;
    size_t chunk = available;
    if ((uint64_t)chunk > (uint64_t)remaining)
      chunk = (size_t)remaining;

    event->data = input + cursor;
    event->length = chunk;
    event->offset = stream->payload_seen;
    event->packet_payload_length = stream->packet_payload_length;
    event->sequence_id = stream->sequence_id;

    stream->payload_seen += (uint32_t)chunk;
    cursor += chunk;
    *consumed = cursor;

    if (stream->payload_seen == stream->packet_payload_length) {
      event->packet_end = true;
      event->message_end =
          stream->packet_payload_length < MYSQL_WIRE_PACKET_MAX_PAYLOAD;
      stream->expected_sequence = (uint8_t)(stream->expected_sequence + 1u);
      stream->header_used = 0u;
      stream->packet_payload_length = 0u;
      stream->payload_seen = 0u;
      stream->in_payload = false;
    }
  }

  return MYSQL_WIRE_STATUS_OK;
}

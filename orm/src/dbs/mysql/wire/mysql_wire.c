#include "mysql_wire.h"

#include <string.h>

uint16_t mysql_wire_load_le16(const uint8_t *data) {
  return (uint16_t)((uint16_t)data[0] |
                    ((uint16_t)data[1] << 8u));
}

uint32_t mysql_wire_load_le24(const uint8_t *data) {
  return (uint32_t)data[0] |
         ((uint32_t)data[1] << 8u) |
         ((uint32_t)data[2] << 16u);
}

uint32_t mysql_wire_load_le32(const uint8_t *data) {
  return (uint32_t)data[0] |
         ((uint32_t)data[1] << 8u) |
         ((uint32_t)data[2] << 16u) |
         ((uint32_t)data[3] << 24u);
}

uint64_t mysql_wire_load_le64(const uint8_t *data) {
  return (uint64_t)mysql_wire_load_le32(data) |
         ((uint64_t)mysql_wire_load_le32(data + 4u) << 32u);
}

void mysql_wire_store_le16(uint8_t *data, uint16_t value) {
  data[0] = (uint8_t)(value & UINT16_C(0xff));
  data[1] = (uint8_t)((value >> 8u) & UINT16_C(0xff));
}

void mysql_wire_store_le24(uint8_t *data, uint32_t value) {
  data[0] = (uint8_t)(value & UINT32_C(0xff));
  data[1] = (uint8_t)((value >> 8u) & UINT32_C(0xff));
  data[2] = (uint8_t)((value >> 16u) & UINT32_C(0xff));
}

void mysql_wire_store_le32(uint8_t *data, uint32_t value) {
  data[0] = (uint8_t)(value & UINT32_C(0xff));
  data[1] = (uint8_t)((value >> 8u) & UINT32_C(0xff));
  data[2] = (uint8_t)((value >> 16u) & UINT32_C(0xff));
  data[3] = (uint8_t)((value >> 24u) & UINT32_C(0xff));
}

void mysql_wire_store_le64(uint8_t *data, uint64_t value) {
  mysql_wire_store_le32(data, (uint32_t)(value & UINT64_C(0xffffffff)));
  mysql_wire_store_le32(data + 4u, (uint32_t)(value >> 32u));
}

void mysql_wire_reader_init(mysql_wire_reader *reader,
                            const void *data,
                            size_t size) {
  if (reader == NULL)
    return;
  reader->data = (const uint8_t *)data;
  reader->size = data != NULL ? size : 0u;
  reader->offset = 0u;
}

size_t mysql_wire_reader_remaining(const mysql_wire_reader *reader) {
  if (reader == NULL || reader->offset > reader->size)
    return 0u;
  return reader->size - reader->offset;
}

static int mysql_wire_reader_require(const mysql_wire_reader *reader,
                                     size_t required) {
  if (reader == NULL || reader->data == NULL)
    return MYSQL_WIRE_INVALID_ARGUMENT;
  return required <= mysql_wire_reader_remaining(reader)
             ? MYSQL_WIRE_OK
             : MYSQL_WIRE_NEED_MORE;
}

int mysql_wire_read_u8(mysql_wire_reader *reader, uint8_t *out) {
  const int status = mysql_wire_reader_require(reader, 1u);
  if (out == NULL)
    return MYSQL_WIRE_INVALID_ARGUMENT;
  if (status != MYSQL_WIRE_OK)
    return status;
  *out = reader->data[reader->offset++];
  return MYSQL_WIRE_OK;
}

int mysql_wire_read_le16(mysql_wire_reader *reader, uint16_t *out) {
  const int status = mysql_wire_reader_require(reader, 2u);
  if (out == NULL)
    return MYSQL_WIRE_INVALID_ARGUMENT;
  if (status != MYSQL_WIRE_OK)
    return status;
  *out = mysql_wire_load_le16(reader->data + reader->offset);
  reader->offset += 2u;
  return MYSQL_WIRE_OK;
}

int mysql_wire_read_le24(mysql_wire_reader *reader, uint32_t *out) {
  const int status = mysql_wire_reader_require(reader, 3u);
  if (out == NULL)
    return MYSQL_WIRE_INVALID_ARGUMENT;
  if (status != MYSQL_WIRE_OK)
    return status;
  *out = mysql_wire_load_le24(reader->data + reader->offset);
  reader->offset += 3u;
  return MYSQL_WIRE_OK;
}

int mysql_wire_read_le32(mysql_wire_reader *reader, uint32_t *out) {
  const int status = mysql_wire_reader_require(reader, 4u);
  if (out == NULL)
    return MYSQL_WIRE_INVALID_ARGUMENT;
  if (status != MYSQL_WIRE_OK)
    return status;
  *out = mysql_wire_load_le32(reader->data + reader->offset);
  reader->offset += 4u;
  return MYSQL_WIRE_OK;
}

int mysql_wire_read_le64(mysql_wire_reader *reader, uint64_t *out) {
  const int status = mysql_wire_reader_require(reader, 8u);
  if (out == NULL)
    return MYSQL_WIRE_INVALID_ARGUMENT;
  if (status != MYSQL_WIRE_OK)
    return status;
  *out = mysql_wire_load_le64(reader->data + reader->offset);
  reader->offset += 8u;
  return MYSQL_WIRE_OK;
}

int mysql_wire_read_bytes(mysql_wire_reader *reader, size_t size,
                          mysql_wire_bytes *out) {
  const int status = mysql_wire_reader_require(reader, size);
  if (out == NULL)
    return MYSQL_WIRE_INVALID_ARGUMENT;
  if (status != MYSQL_WIRE_OK)
    return status;
  out->data = reader->data + reader->offset;
  out->size = size;
  reader->offset += size;
  return MYSQL_WIRE_OK;
}

int mysql_wire_read_lenenc(mysql_wire_reader *reader,
                           mysql_wire_lenenc *out) {
  const size_t start = reader != NULL ? reader->offset : 0u;
  uint8_t marker = 0u;
  uint16_t value16 = 0u;
  uint32_t value24 = 0u;
  uint64_t value64 = 0u;
  int status;

  if (reader == NULL || out == NULL)
    return MYSQL_WIRE_INVALID_ARGUMENT;

  out->value = 0u;
  out->is_null = 0;
  status = mysql_wire_read_u8(reader, &marker);
  if (status != MYSQL_WIRE_OK)
    return status;

  if (marker < UINT8_C(0xfb)) {
    out->value = marker;
    return MYSQL_WIRE_OK;
  }
  if (marker == UINT8_C(0xfb)) {
    out->is_null = 1;
    return MYSQL_WIRE_OK;
  }

  switch (marker) {
    case UINT8_C(0xfc):
      status = mysql_wire_read_le16(reader, &value16);
      if (status == MYSQL_WIRE_OK)
        out->value = value16;
      break;
    case UINT8_C(0xfd):
      status = mysql_wire_read_le24(reader, &value24);
      if (status == MYSQL_WIRE_OK)
        out->value = value24;
      break;
    case UINT8_C(0xfe):
      status = mysql_wire_read_le64(reader, &value64);
      if (status == MYSQL_WIRE_OK)
        out->value = value64;
      break;
    default:
      status = MYSQL_WIRE_PROTOCOL_ERROR;
      break;
  }

  if (status != MYSQL_WIRE_OK)
    reader->offset = start;
  return status;
}

int mysql_wire_decoder_init(mysql_wire_decoder *decoder,
                            void *payload_buffer,
                            size_t payload_capacity,
                            uint32_t max_payload_size,
                            uint8_t expected_sequence,
                            int enforce_sequence) {
  if (decoder == NULL || payload_buffer == NULL ||
      payload_capacity == 0u || max_payload_size == 0u ||
      max_payload_size > MYSQL_WIRE_MAX_PACKET_PAYLOAD ||
      (size_t)max_payload_size > payload_capacity)
    return MYSQL_WIRE_INVALID_ARGUMENT;

  memset(decoder, 0, sizeof(*decoder));
  decoder->payload = (uint8_t *)payload_buffer;
  decoder->payload_capacity = payload_capacity;
  decoder->max_payload_size = max_payload_size;
  decoder->expected_sequence = expected_sequence;
  decoder->enforce_sequence = enforce_sequence != 0;
  return MYSQL_WIRE_OK;
}

void mysql_wire_decoder_reset(mysql_wire_decoder *decoder,
                              uint8_t expected_sequence,
                              int enforce_sequence) {
  if (decoder == NULL)
    return;
  decoder->header_used = 0u;
  decoder->payload_size = 0u;
  decoder->payload_used = 0u;
  decoder->sequence = 0u;
  decoder->expected_sequence = expected_sequence;
  decoder->enforce_sequence = enforce_sequence != 0;
}

static int mysql_wire_decoder_publish(mysql_wire_decoder *decoder,
                                      mysql_wire_packet_fn on_packet,
                                      void *user) {
  mysql_wire_packet packet;

  if (decoder == NULL || on_packet == NULL)
    return MYSQL_WIRE_INVALID_ARGUMENT;

  packet.sequence = decoder->sequence;
  packet.payload = decoder->payload;
  packet.payload_size = decoder->payload_size;
  if (on_packet(user, &packet) != 0)
    return MYSQL_WIRE_CALLBACK_ERROR;

  if (decoder->enforce_sequence)
    decoder->expected_sequence = (uint8_t)(decoder->expected_sequence + 1u);
  decoder->header_used = 0u;
  decoder->payload_size = 0u;
  decoder->payload_used = 0u;
  decoder->sequence = 0u;
  return MYSQL_WIRE_OK;
}

int mysql_wire_decoder_feed(mysql_wire_decoder *decoder,
                            const void *data,
                            size_t size,
                            size_t *out_consumed,
                            mysql_wire_packet_fn on_packet,
                            void *user) {
  const uint8_t *input = (const uint8_t *)data;
  size_t offset = 0u;
  int published = 0;

  if (out_consumed != NULL)
    *out_consumed = 0u;
  if (decoder == NULL || on_packet == NULL ||
      (data == NULL && size != 0u))
    return MYSQL_WIRE_INVALID_ARGUMENT;

  while (offset < size) {
    if (decoder->header_used < MYSQL_WIRE_PACKET_HEADER_BYTES) {
      const size_t need = MYSQL_WIRE_PACKET_HEADER_BYTES - decoder->header_used;
      const size_t available = size - offset;
      const size_t take = available < need ? available : need;
      memcpy(decoder->header + decoder->header_used, input + offset, take);
      decoder->header_used += take;
      offset += take;

      if (decoder->header_used < MYSQL_WIRE_PACKET_HEADER_BYTES)
        break;

      decoder->payload_size = mysql_wire_load_le24(decoder->header);
      decoder->sequence = decoder->header[3];
      decoder->payload_used = 0u;

      if (decoder->payload_size > decoder->max_payload_size ||
          (size_t)decoder->payload_size > decoder->payload_capacity) {
        if (out_consumed != NULL)
          *out_consumed = offset;
        return MYSQL_WIRE_LIMIT_EXCEEDED;
      }
      if (decoder->enforce_sequence &&
          decoder->sequence != decoder->expected_sequence) {
        if (out_consumed != NULL)
          *out_consumed = offset;
        return MYSQL_WIRE_SEQUENCE_ERROR;
      }
      if (decoder->payload_size == 0u) {
        const int status = mysql_wire_decoder_publish(decoder, on_packet, user);
        if (out_consumed != NULL)
          *out_consumed = offset;
        if (status != MYSQL_WIRE_OK)
          return status;
        published = 1;
        continue;
      }
    }

    if (decoder->payload_used < decoder->payload_size) {
      const size_t need = (size_t)decoder->payload_size - decoder->payload_used;
      const size_t available = size - offset;
      const size_t take = available < need ? available : need;
      memcpy(decoder->payload + decoder->payload_used, input + offset, take);
      decoder->payload_used += take;
      offset += take;

      if (decoder->payload_used == decoder->payload_size) {
        const int status = mysql_wire_decoder_publish(decoder, on_packet, user);
        if (out_consumed != NULL)
          *out_consumed = offset;
        if (status != MYSQL_WIRE_OK)
          return status;
        published = 1;
      }
    }
  }

  if (out_consumed != NULL)
    *out_consumed = offset;
  if (decoder->header_used != 0u || decoder->payload_used != 0u)
    return MYSQL_WIRE_NEED_MORE;
  return published ? MYSQL_WIRE_OK : MYSQL_WIRE_NEED_MORE;
}

int mysql_wire_decode_ok(const void *payload, size_t size,
                         mysql_wire_ok_packet *out) {
  mysql_wire_reader reader;
  mysql_wire_lenenc affected;
  mysql_wire_lenenc insert_id;
  uint8_t marker = 0u;
  int status;

  if (payload == NULL || out == NULL)
    return MYSQL_WIRE_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  mysql_wire_reader_init(&reader, payload, size);

  status = mysql_wire_read_u8(&reader, &marker);
  if (status != MYSQL_WIRE_OK)
    return status;
  if (marker != UINT8_C(0x00) && marker != UINT8_C(0xfe))
    return MYSQL_WIRE_PROTOCOL_ERROR;
  /*
   * 0xfe is also the legacy EOF marker. MySQL disambiguates it by payload
   * length: EOF is strictly shorter than nine bytes. Only the non-EOF form
   * may be interpreted as an OK packet when CLIENT_DEPRECATE_EOF is active.
   */
  if (marker == UINT8_C(0xfe) && mysql_wire_is_eof_packet(payload, size))
    return MYSQL_WIRE_PROTOCOL_ERROR;

  status = mysql_wire_read_lenenc(&reader, &affected);
  if (status != MYSQL_WIRE_OK)
    return status;
  status = mysql_wire_read_lenenc(&reader, &insert_id);
  if (status != MYSQL_WIRE_OK)
    return status;
  if (affected.is_null || insert_id.is_null)
    return MYSQL_WIRE_PROTOCOL_ERROR;

  status = mysql_wire_read_le16(&reader, &out->status_flags);
  if (status != MYSQL_WIRE_OK)
    return status;
  status = mysql_wire_read_le16(&reader, &out->warning_count);
  if (status != MYSQL_WIRE_OK)
    return status;

  out->affected_rows = affected.value;
  out->last_insert_id = insert_id.value;
  out->info.data = reader.data + reader.offset;
  out->info.size = mysql_wire_reader_remaining(&reader);
  return MYSQL_WIRE_OK;
}

int mysql_wire_decode_error(const void *payload, size_t size,
                            mysql_wire_error_packet *out) {
  mysql_wire_reader reader;
  mysql_wire_bytes state = {0};
  uint8_t marker = 0u;
  uint8_t state_marker = 0u;
  int status;

  if (payload == NULL || out == NULL)
    return MYSQL_WIRE_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  mysql_wire_reader_init(&reader, payload, size);

  status = mysql_wire_read_u8(&reader, &marker);
  if (status != MYSQL_WIRE_OK)
    return status;
  if (marker != UINT8_C(0xff))
    return MYSQL_WIRE_PROTOCOL_ERROR;
  status = mysql_wire_read_le16(&reader, &out->error_code);
  if (status != MYSQL_WIRE_OK)
    return status;

  if (mysql_wire_reader_remaining(&reader) != 0u &&
      reader.data[reader.offset] == (uint8_t)'#') {
    status = mysql_wire_read_u8(&reader, &state_marker);
    if (status != MYSQL_WIRE_OK)
      return status;
    status = mysql_wire_read_bytes(&reader, 5u, &state);
    if (status != MYSQL_WIRE_OK)
      return status;
    (void)state_marker;
    memcpy(out->sql_state, state.data, state.size);
    out->sql_state[5] = '\0';
    out->has_sql_state = 1;
  }

  out->message.data = reader.data + reader.offset;
  out->message.size = mysql_wire_reader_remaining(&reader);
  return MYSQL_WIRE_OK;
}

int mysql_wire_is_eof_packet(const void *payload, size_t size) {
  const uint8_t *bytes = (const uint8_t *)payload;
  return bytes != NULL && size >= 1u && size < 9u &&
         bytes[0] == UINT8_C(0xfe);
}

int mysql_wire_decode_eof(const void *payload, size_t size,
                          mysql_wire_eof_packet *out) {
  mysql_wire_reader reader;
  uint8_t marker = 0u;
  int status;

  if (out == NULL)
    return MYSQL_WIRE_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  if (!mysql_wire_is_eof_packet(payload, size))
    return MYSQL_WIRE_PROTOCOL_ERROR;

  mysql_wire_reader_init(&reader, payload, size);
  status = mysql_wire_read_u8(&reader, &marker);
  if (status != MYSQL_WIRE_OK)
    return status;
  status = mysql_wire_read_le16(&reader, &out->warning_count);
  if (status != MYSQL_WIRE_OK)
    return status;
  status = mysql_wire_read_le16(&reader, &out->status_flags);
  if (status != MYSQL_WIRE_OK)
    return status;
  return mysql_wire_reader_remaining(&reader) == 0u
             ? MYSQL_WIRE_OK
             : MYSQL_WIRE_PROTOCOL_ERROR;
}

#include "mysql_wire.h"

#include <tinytest.h>

#include <stdint.h>
#include <string.h>

typedef struct packet_capture {
  unsigned int count;
  uint8_t sequence[4];
  size_t size[4];
  uint8_t payload[4][8];
} packet_capture;

static int capture_packet(void *user, const mysql_wire_packet *packet) {
  packet_capture *capture = (packet_capture *)user;
  unsigned int index;

  if (capture == NULL || packet == NULL || capture->count >= 4u ||
      packet->payload_size > sizeof(capture->payload[0]))
    return -1;
  index = capture->count++;
  capture->sequence[index] = packet->sequence;
  capture->size[index] = packet->payload_size;
  if (packet->payload_size != 0u)
    memcpy(capture->payload[index], packet->payload, packet->payload_size);
  return 0;
}

static mysql_wire_reader reader_of(const uint8_t *bytes, size_t size) {
  mysql_wire_reader reader;
  mysql_wire_reader_init(&reader, bytes, size);
  return reader;
}

spec("MySQL wire foundation") {
  (void)ttest_config__;

  it("loads and stores little-endian scalars without alignment assumptions") {
    uint8_t bytes[8] = {0};

    mysql_wire_store_le16(bytes, UINT16_C(0xabcd));
    check_equal(bytes[0], UINT8_C(0xcd));
    check_equal(bytes[1], UINT8_C(0xab));
    check_equal(mysql_wire_load_le16(bytes), UINT16_C(0xabcd));

    memset(bytes, 0, sizeof(bytes));
    mysql_wire_store_le24(bytes, UINT32_C(0x00abcdef));
    check_equal(mysql_wire_load_le24(bytes), UINT32_C(0x00abcdef));

    memset(bytes, 0, sizeof(bytes));
    mysql_wire_store_le32(bytes, UINT32_C(0x89abcdef));
    check_equal(mysql_wire_load_le32(bytes), UINT32_C(0x89abcdef));

    memset(bytes, 0, sizeof(bytes));
    mysql_wire_store_le64(bytes, UINT64_C(0x0123456789abcdef));
    check_equal(mysql_wire_load_le64(bytes), UINT64_C(0x0123456789abcdef));
  }

  it("decodes every length-encoded integer form and preserves truncation") {
    const uint8_t small[] = {250u};
    const uint8_t null_value[] = {0xfbu};
    const uint8_t two[] = {0xfcu, 0x34u, 0x12u};
    const uint8_t three[] = {0xfdu, 0x56u, 0x34u, 0x12u};
    const uint8_t eight[] = {0xfeu, 0xefu, 0xcdu, 0xabu, 0x89u,
                             0x67u, 0x45u, 0x23u, 0x01u};
    const uint8_t truncated[] = {0xfcu, 0x34u};
    const uint8_t invalid[] = {0xffu};
    mysql_wire_reader reader;
    mysql_wire_lenenc value;

    reader = reader_of(small, sizeof(small));
    check_equal(mysql_wire_read_lenenc(&reader, &value), MYSQL_WIRE_OK);
    check_equal(value.value, UINT64_C(250));
    check_equal(value.is_null, 0);

    reader = reader_of(null_value, sizeof(null_value));
    check_equal(mysql_wire_read_lenenc(&reader, &value), MYSQL_WIRE_OK);
    check_equal(value.is_null, 1);

    reader = reader_of(two, sizeof(two));
    check_equal(mysql_wire_read_lenenc(&reader, &value), MYSQL_WIRE_OK);
    check_equal(value.value, UINT64_C(0x1234));

    reader = reader_of(three, sizeof(three));
    check_equal(mysql_wire_read_lenenc(&reader, &value), MYSQL_WIRE_OK);
    check_equal(value.value, UINT64_C(0x123456));

    reader = reader_of(eight, sizeof(eight));
    check_equal(mysql_wire_read_lenenc(&reader, &value), MYSQL_WIRE_OK);
    check_equal(value.value, UINT64_C(0x0123456789abcdef));

    reader = reader_of(truncated, sizeof(truncated));
    check_equal(mysql_wire_read_lenenc(&reader, &value), MYSQL_WIRE_NEED_MORE);
    check_equal(reader.offset, (size_t)0u);

    reader = reader_of(invalid, sizeof(invalid));
    check_equal(mysql_wire_read_lenenc(&reader, &value),
                MYSQL_WIRE_PROTOCOL_ERROR);
    check_equal(reader.offset, (size_t)0u);
  }

  it("frames a packet across split header and payload receives") {
    const uint8_t wire[] = {3u, 0u, 0u, 7u, 'a', 'b', 'c'};
    uint8_t payload[8] = {0};
    mysql_wire_decoder decoder;
    packet_capture capture = {0};
    size_t consumed = 0u;

    check_equal(mysql_wire_decoder_init(&decoder, payload, sizeof(payload),
                                        (uint32_t)sizeof(payload), 7u, 1),
                MYSQL_WIRE_OK);
    check_equal(mysql_wire_decoder_feed(&decoder, wire, 2u, &consumed,
                                        capture_packet, &capture),
                MYSQL_WIRE_NEED_MORE);
    check_equal(consumed, (size_t)2u);
    check_equal(capture.count, 0u);

    check_equal(mysql_wire_decoder_feed(&decoder, wire + 2u,
                                        sizeof(wire) - 2u, &consumed,
                                        capture_packet, &capture),
                MYSQL_WIRE_OK);
    check_equal(consumed, sizeof(wire) - 2u);
    check_equal(capture.count, 1u);
    check_equal(capture.sequence[0], UINT8_C(7));
    check_equal(capture.size[0], (size_t)3u);
    check_equal(memcmp(capture.payload[0], "abc", 3u), 0);
    check_equal(decoder.expected_sequence, UINT8_C(8));
  }

  it("rejects oversized and out-of-sequence packets before payload copy") {
    const uint8_t oversized[] = {3u, 0u, 0u, 0u, 'a', 'b', 'c'};
    const uint8_t wrong_sequence[] = {1u, 0u, 0u, 9u, 'x'};
    uint8_t payload[2] = {0};
    mysql_wire_decoder decoder;
    packet_capture capture = {0};
    size_t consumed = 0u;

    check_equal(mysql_wire_decoder_init(&decoder, payload, sizeof(payload), 2u,
                                        0u, 0),
                MYSQL_WIRE_OK);
    check_equal(mysql_wire_decoder_feed(&decoder, oversized,
                                        sizeof(oversized), &consumed,
                                        capture_packet, &capture),
                MYSQL_WIRE_LIMIT_EXCEEDED);
    check_equal(consumed, (size_t)4u);
    check_equal(capture.count, 0u);

    mysql_wire_decoder_reset(&decoder, 8u, 1);
    check_equal(mysql_wire_decoder_feed(&decoder, wrong_sequence,
                                        sizeof(wrong_sequence), &consumed,
                                        capture_packet, &capture),
                MYSQL_WIRE_SEQUENCE_ERROR);
    check_equal(consumed, (size_t)4u);
    check_equal(capture.count, 0u);
  }

  it("publishes multiple ordered packets from one receive slice") {
    const uint8_t wire[] = {
        1u, 0u, 0u, 0u, 'x',
        2u, 0u, 0u, 1u, 'y', 'z'};
    uint8_t payload[8] = {0};
    mysql_wire_decoder decoder;
    packet_capture capture = {0};
    size_t consumed = 0u;

    check_equal(mysql_wire_decoder_init(&decoder, payload, sizeof(payload),
                                        (uint32_t)sizeof(payload), 0u, 1),
                MYSQL_WIRE_OK);
    check_equal(mysql_wire_decoder_feed(&decoder, wire, sizeof(wire),
                                        &consumed, capture_packet, &capture),
                MYSQL_WIRE_OK);
    check_equal(consumed, sizeof(wire));
    check_equal(capture.count, 2u);
    check_equal(capture.sequence[0], UINT8_C(0));
    check_equal(capture.sequence[1], UINT8_C(1));
    check_equal(capture.size[0], (size_t)1u);
    check_equal(capture.size[1], (size_t)2u);
    check_equal(capture.payload[0][0], (uint8_t)'x');
    check_equal(memcmp(capture.payload[1], "yz", 2u), 0);
  }

  it("decodes protocol-41 OK ERR and legacy EOF payloads") {
    const uint8_t ok[] = {0x00u, 0x02u, 0x09u, 0x02u, 0x00u,
                          0x01u, 0x00u, 'o', 'k'};
    const uint8_t error[] = {0xffu, 0x15u, 0x04u, '#', '2', '3',
                             '0', '0', '0', 'b', 'a', 'd'};
    const uint8_t eof[] = {0xfeu, 0x01u, 0x00u, 0x02u, 0x00u};
    const uint8_t not_eof[] = {0xfeu, 0u, 0u, 0u, 0u,
                               0u, 0u, 0u, 0u};
    mysql_wire_ok_packet ok_packet;
    mysql_wire_error_packet error_packet;
    mysql_wire_eof_packet eof_packet;

    check_equal(mysql_wire_decode_ok(ok, sizeof(ok), &ok_packet),
                MYSQL_WIRE_OK);
    check_equal(ok_packet.affected_rows, UINT64_C(2));
    check_equal(ok_packet.last_insert_id, UINT64_C(9));
    check_equal(ok_packet.status_flags, UINT16_C(2));
    check_equal(ok_packet.warning_count, UINT16_C(1));
    check_equal(ok_packet.info.size, (size_t)2u);
    check_equal(memcmp(ok_packet.info.data, "ok", 2u), 0);

    check_equal(mysql_wire_decode_error(error, sizeof(error), &error_packet),
                MYSQL_WIRE_OK);
    check_equal(error_packet.error_code, UINT16_C(0x0415));
    check_equal(error_packet.has_sql_state, 1);
    check_equal(memcmp(error_packet.sql_state, "23000", 5u), 0);
    check_equal(error_packet.message.size, (size_t)3u);
    check_equal(memcmp(error_packet.message.data, "bad", 3u), 0);

    check_true(mysql_wire_is_eof_packet(eof, sizeof(eof)));
    check_true(!mysql_wire_is_eof_packet(not_eof, sizeof(not_eof)));
    check_equal(mysql_wire_decode_ok(eof, sizeof(eof), &ok_packet),
                MYSQL_WIRE_PROTOCOL_ERROR);
    check_equal(mysql_wire_decode_eof(eof, sizeof(eof), &eof_packet),
                MYSQL_WIRE_OK);
    check_equal(eof_packet.warning_count, UINT16_C(1));
    check_equal(eof_packet.status_flags, UINT16_C(2));
  }
}

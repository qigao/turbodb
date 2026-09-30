#include "codec.h"
#include "packet.h"
#include "result.h"

#include <tinytest.h>

#include <stdint.h>
#include <string.h>

spec("mysql wire codec") {
  (void)ttest_config__;

  it("loads little-endian integers without alignment assumptions") {
    const uint8_t bytes[] = {
        0xaa, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x90};
    size_t offset = 1u;
    uint16_t u16 = 0u;
    uint32_t u24 = 0u;
    uint32_t u32 = 0u;

    check_equal(mysql_wire_read_u16_le(
                    bytes, sizeof(bytes), &offset, &u16),
                MYSQL_WIRE_STATUS_OK);
    check_equal(u16, UINT16_C(0x1234));
    check_equal(offset, (size_t)3u);

    offset = 3u;
    check_equal(mysql_wire_read_u24_le(
                    bytes, sizeof(bytes), &offset, &u24),
                MYSQL_WIRE_STATUS_OK);
    check_equal(u24, UINT32_C(0x345678));

    offset = 7u;
    check_equal(mysql_wire_read_u32_le(
                    bytes, sizeof(bytes), &offset, &u32),
                MYSQL_WIRE_STATUS_OK);
    check_equal(u32, UINT32_C(0x90abcdef));
  }

  it("round-trips every length-encoded integer boundary") {
    static const uint64_t values[] = {
        UINT64_C(0), UINT64_C(250), UINT64_C(251),
        UINT64_C(65535), UINT64_C(65536), UINT64_C(16777215),
        UINT64_C(16777216), UINT64_MAX};
    size_t i;

    for (i = 0u; i < sizeof(values) / sizeof(values[0]); ++i) {
      uint8_t encoded[9] = {0};
      size_t written = 0u;
      size_t read = 0u;
      uint64_t decoded = 0u;
      bool is_null = true;

      check_equal(mysql_wire_write_lenenc_uint(
                      encoded, sizeof(encoded), &written, values[i]),
                  MYSQL_WIRE_STATUS_OK);
      check_equal(mysql_wire_read_lenenc_uint(
                      encoded, written, &read, &decoded, &is_null),
                  MYSQL_WIRE_STATUS_OK);
      check_equal(is_null, false);
      check_equal(decoded, values[i]);
      check_equal(read, written);
    }
  }

  it("distinguishes length-encoded NULL from the scalar 251") {
    uint8_t encoded[9] = {0};
    size_t offset = 0u;
    uint64_t value = UINT64_MAX;
    bool is_null = false;

    check_equal(mysql_wire_write_lenenc_null(
                    encoded, sizeof(encoded), &offset),
                MYSQL_WIRE_STATUS_OK);
    check_equal(offset, (size_t)1u);
    check_equal(encoded[0], UINT8_C(0xfb));

    offset = 0u;
    check_equal(mysql_wire_read_lenenc_uint(
                    encoded, 1u, &offset, &value, &is_null),
                MYSQL_WIRE_STATUS_OK);
    check_equal(is_null, true);
    check_equal(value, UINT64_C(0));

    offset = 0u;
    check_equal(mysql_wire_write_lenenc_uint(
                    encoded, sizeof(encoded), &offset, UINT64_C(251)),
                MYSQL_WIRE_STATUS_OK);
    check_equal(encoded[0], UINT8_C(0xfc));
  }

  it("leaves the cursor unchanged for truncated length-encoded integers") {
    static const uint8_t truncated[] = {0xfe, 0x01, 0x02, 0x03};
    size_t offset = 0u;
    uint64_t value = 0u;
    bool is_null = false;

    check_equal(mysql_wire_read_lenenc_uint(
                    truncated, sizeof(truncated), &offset, &value, &is_null),
                MYSQL_WIRE_STATUS_NEED_MORE);
    check_equal(offset, (size_t)0u);
  }
}

spec("mysql wire packet stream") {
  (void)ttest_config__;

  it("accepts split headers and split payloads without copying payload bytes") {
    const uint8_t first[] = {0x03, 0x00};
    const uint8_t second[] = {0x00, 0x00, 'a', 'b'};
    const uint8_t third[] = {'c'};
    mysql_wire_packet_stream_t stream;
    mysql_wire_packet_event_t event;
    size_t consumed = 0u;

    check_equal(mysql_wire_packet_stream_init(
                    &stream, 0u, MYSQL_WIRE_PACKET_MAX_PAYLOAD),
                MYSQL_WIRE_STATUS_OK);
    check_equal(mysql_wire_packet_stream_feed(
                    &stream, first, sizeof(first), &consumed, &event),
                MYSQL_WIRE_STATUS_NEED_MORE);
    check_equal(consumed, sizeof(first));

    check_equal(mysql_wire_packet_stream_feed(
                    &stream, second, sizeof(second), &consumed, &event),
                MYSQL_WIRE_STATUS_OK);
    check_equal(consumed, sizeof(second));
    check_equal(event.length, (size_t)2u);
    check_equal(event.offset, UINT32_C(0));
    check_equal(event.packet_end, false);
    check_equal(memcmp(event.data, "ab", 2u), 0);

    check_equal(mysql_wire_packet_stream_feed(
                    &stream, third, sizeof(third), &consumed, &event),
                MYSQL_WIRE_STATUS_OK);
    check_equal(event.length, (size_t)1u);
    check_equal(event.offset, UINT32_C(2));
    check_equal(event.packet_end, true);
    check_equal(event.message_end, true);
    check_equal(event.sequence_id, UINT8_C(0));
    check_equal(event.data[0], (uint8_t)'c');
  }

  it("rejects configured oversize packets before consuming payload") {
    const uint8_t header[] = {0x03, 0x00, 0x00, 0x00};
    mysql_wire_packet_stream_t stream;
    mysql_wire_packet_event_t event;
    size_t consumed = 0u;

    check_equal(mysql_wire_packet_stream_init(&stream, 0u, 2u),
                MYSQL_WIRE_STATUS_OK);
    check_equal(mysql_wire_packet_stream_feed(
                    &stream, header, sizeof(header), &consumed, &event),
                MYSQL_WIRE_STATUS_LIMIT);
    check_equal(consumed, sizeof(header));
  }

  it("fails closed on sequence mismatch") {
    const uint8_t packet[] = {0x00, 0x00, 0x00, 0x05};
    mysql_wire_packet_stream_t stream;
    mysql_wire_packet_event_t event;
    size_t consumed = 0u;

    check_equal(mysql_wire_packet_stream_init(
                    &stream, 4u, MYSQL_WIRE_PACKET_MAX_PAYLOAD),
                MYSQL_WIRE_STATUS_OK);
    check_equal(mysql_wire_packet_stream_feed(
                    &stream, packet, sizeof(packet), &consumed, &event),
                MYSQL_WIRE_STATUS_SEQUENCE);
  }

  it("wraps sequence ids after 255") {
    const uint8_t packet_255[] = {0x00, 0x00, 0x00, 0xff};
    const uint8_t packet_0[] = {0x00, 0x00, 0x00, 0x00};
    mysql_wire_packet_stream_t stream;
    mysql_wire_packet_event_t event;
    size_t consumed = 0u;

    check_equal(mysql_wire_packet_stream_init(
                    &stream, UINT8_MAX, MYSQL_WIRE_PACKET_MAX_PAYLOAD),
                MYSQL_WIRE_STATUS_OK);
    check_equal(mysql_wire_packet_stream_feed(
                    &stream, packet_255, sizeof(packet_255),
                    &consumed, &event),
                MYSQL_WIRE_STATUS_OK);
    check_equal(event.sequence_id, UINT8_MAX);
    check_equal(event.packet_end, true);

    check_equal(mysql_wire_packet_stream_feed(
                    &stream, packet_0, sizeof(packet_0),
                    &consumed, &event),
                MYSQL_WIRE_STATUS_OK);
    check_equal(event.sequence_id, UINT8_C(0));
  }

  it("accepts the protocol continuation sentinel at the hard packet ceiling") {
    const uint8_t header[] = {0xff, 0xff, 0xff, 0x07};
    uint32_t length = 0u;
    uint8_t sequence = 0u;

    check_equal(mysql_wire_packet_header_decode(
                    header, sizeof(header), 7u,
                    MYSQL_WIRE_PACKET_MAX_PAYLOAD, &length, &sequence),
                MYSQL_WIRE_STATUS_OK);
    check_equal(length, MYSQL_WIRE_PACKET_MAX_PAYLOAD);
    check_equal(sequence, UINT8_C(7));
  }
}

spec("mysql wire multi-packet continuation") {
  (void)ttest_config__;

  it("requires a zero-length trailer after an exact 0xffffff payload") {
    const uint8_t header[] = {0xff, 0xff, 0xff, 0x2a};
    const uint8_t trailer[] = {0x00, 0x00, 0x00, 0x2b};
    uint8_t chunk[65536] = {0};
    mysql_wire_packet_stream_t stream;
    mysql_wire_packet_event_t event;
    size_t consumed = 0u;
    uint32_t remaining = MYSQL_WIRE_PACKET_MAX_PAYLOAD;

    check_equal(mysql_wire_packet_stream_init(
                    &stream, UINT8_C(0x2a), MYSQL_WIRE_PACKET_MAX_PAYLOAD),
                MYSQL_WIRE_STATUS_OK);
    check_equal(mysql_wire_packet_stream_feed(
                    &stream, header, sizeof(header), &consumed, &event),
                MYSQL_WIRE_STATUS_NEED_MORE);
    check_equal(consumed, sizeof(header));

    while (remaining != 0u) {
      size_t offered = sizeof(chunk);
      if ((uint32_t)offered > remaining)
        offered = (size_t)remaining;
      check_equal(mysql_wire_packet_stream_feed(
                      &stream, chunk, offered, &consumed, &event),
                  MYSQL_WIRE_STATUS_OK);
      check_equal(consumed, offered);
      remaining -= (uint32_t)offered;
      if (remaining == 0u) {
        check_equal(event.packet_end, true);
        check_equal(event.message_end, false);
        check_equal(event.sequence_id, UINT8_C(0x2a));
      } else {
        check_equal(event.packet_end, false);
      }
    }

    check_equal(mysql_wire_packet_stream_feed(
                    &stream, trailer, sizeof(trailer), &consumed, &event),
                MYSQL_WIRE_STATUS_OK);
    check_equal(consumed, sizeof(trailer));
    check_equal(event.packet_end, true);
    check_equal(event.message_end, true);
    check_equal(event.packet_payload_length, UINT32_C(0));
    check_equal(event.sequence_id, UINT8_C(0x2b));
  }
}

spec("mysql wire result packets") {
  (void)ttest_config__;

  it("decodes protocol-41 OK packets") {
    const uint8_t payload[] = {
        0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 'o', 'k'};
    mysql_wire_ok_packet_t ok;

    check_equal(mysql_wire_decode_ok_packet(
                    payload, sizeof(payload), MYSQL_WIRE_CLIENT_PROTOCOL_41,
                    &ok),
                MYSQL_WIRE_STATUS_OK);
    check_equal(ok.affected_rows, UINT64_C(1));
    check_equal(ok.last_insert_id, UINT64_C(0));
    check_equal(ok.status_flags, UINT16_C(2));
    check_equal(ok.warnings, UINT16_C(0));
    check_equal(ok.info.length, (size_t)2u);
    check_equal(memcmp(ok.info.data, "ok", 2u), 0);
  }

  it("decodes session-track OK payloads with exact bounded strings") {
    const uint8_t payload[] = {
        0x00, 0x00, 0x00, 0x02, 0x40, 0x00, 0x00,
        0x02, 'o', 'k', 0x03, 'a', 'b', 'c'};
    mysql_wire_ok_packet_t ok;
    const uint32_t caps =
        MYSQL_WIRE_CLIENT_PROTOCOL_41 | MYSQL_WIRE_CLIENT_SESSION_TRACK;

    check_equal(mysql_wire_decode_ok_packet(
                    payload, sizeof(payload), caps, &ok),
                MYSQL_WIRE_STATUS_OK);
    check_equal(ok.status_flags,
                (uint16_t)(UINT16_C(0x4000) | UINT16_C(0x0002)));
    check_equal(ok.info.length, (size_t)2u);
    check_equal(ok.session_state.length, (size_t)3u);
    check_equal(memcmp(ok.session_state.data, "abc", 3u), 0);
  }

  it("decodes protocol-41 ERR packets without owning message storage") {
    const uint8_t payload[] = {
        0xff, 0x15, 0x04, '#', 'H', 'Y', '0', '0', '0',
        'd', 'e', 'n', 'i', 'e', 'd'};
    mysql_wire_err_packet_t error;

    check_equal(mysql_wire_decode_err_packet(
                    payload, sizeof(payload), MYSQL_WIRE_CLIENT_PROTOCOL_41,
                    &error),
                MYSQL_WIRE_STATUS_OK);
    check_equal(error.error_code, UINT16_C(1045));
    check_equal(error.has_sql_state, true);
    check_equal(memcmp(error.sql_state, "HY000", 6u), 0);
    check_equal(error.message.length, (size_t)6u);
    check_equal(memcmp(error.message.data, "denied", 6u), 0);
  }

  it("decodes legacy EOF only when the payload cannot be lenenc 0xfe") {
    const uint8_t payload[] = {0xfe, 0x00, 0x00, 0x02, 0x00};
    mysql_wire_eof_packet_t eof;

    check_equal(mysql_wire_decode_eof_packet(
                    payload, sizeof(payload), MYSQL_WIRE_CLIENT_PROTOCOL_41,
                    &eof),
                MYSQL_WIRE_STATUS_OK);
    check_equal(eof.warnings, UINT16_C(0));
    check_equal(eof.status_flags, UINT16_C(2));
  }

  it("accepts 0xfe as OK terminator only with CLIENT_DEPRECATE_EOF") {
    const uint8_t payload[] = {
        0xfe, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00};
    mysql_wire_ok_packet_t ok;

    check_equal(mysql_wire_decode_ok_packet(
                    payload, sizeof(payload), MYSQL_WIRE_CLIENT_PROTOCOL_41,
                    &ok),
                MYSQL_WIRE_STATUS_INVALID);
    check_equal(mysql_wire_decode_ok_packet(
                    payload, sizeof(payload),
                    MYSQL_WIRE_CLIENT_PROTOCOL_41 |
                        MYSQL_WIRE_CLIENT_DEPRECATE_EOF,
                    &ok),
                MYSQL_WIRE_STATUS_OK);
  }

  it("decodes COM_QUERY text rows with NULL and embedded NUL") {
    const uint8_t payload[] = {
        0x01, 'a',
        0xfb,
        0x03, 'x', 0x00, 'y'};
    mysql_wire_bytes_t columns[3];

    check_equal(mysql_wire_decode_text_row(
                    payload, sizeof(payload), 3u,
                    columns, 3u),
                MYSQL_WIRE_STATUS_OK);
    check_equal(columns[0].is_null, false);
    check_equal(columns[0].length, (size_t)1u);
    check_equal(memcmp(columns[0].data, "a", 1u), 0);
    check_equal(columns[1].is_null, true);
    check_null(columns[1].data);
    check_equal(columns[1].length, (size_t)0u);
    check_equal(columns[2].is_null, false);
    check_equal(columns[2].length, (size_t)3u);
    check_equal(columns[2].data[0], (uint8_t)'x');
    check_equal(columns[2].data[1], UINT8_C(0));
    check_equal(columns[2].data[2], (uint8_t)'y');
  }

  it("rejects truncated, trailing, and undersized text-row outputs") {
    const uint8_t truncated[] = {0x03, 'a'};
    const uint8_t trailing[] = {0x01, 'a', 0x00};
    mysql_wire_bytes_t columns[2];

    check_equal(mysql_wire_decode_text_row(
                    truncated, sizeof(truncated), 1u,
                    columns, 2u),
                MYSQL_WIRE_STATUS_INVALID);
    check_equal(mysql_wire_decode_text_row(
                    trailing, sizeof(trailing), 1u,
                    columns, 2u),
                MYSQL_WIRE_STATUS_INVALID);
    check_equal(mysql_wire_decode_text_row(
                    trailing, sizeof(trailing), 2u,
                    columns, 1u),
                MYSQL_WIRE_STATUS_LIMIT);
  }

  it("rejects truncated complete result packets") {
    const uint8_t bad_ok[] = {0x00, 0xfc, 0x01};
    const uint8_t bad_err[] = {0xff, 0x15, 0x04, '#', 'H'};
    mysql_wire_ok_packet_t ok;
    mysql_wire_err_packet_t error;

    check_equal(mysql_wire_decode_ok_packet(
                    bad_ok, sizeof(bad_ok), MYSQL_WIRE_CLIENT_PROTOCOL_41,
                    &ok),
                MYSQL_WIRE_STATUS_INVALID);
    check_equal(mysql_wire_decode_err_packet(
                    bad_err, sizeof(bad_err), MYSQL_WIRE_CLIENT_PROTOCOL_41,
                    &error),
                MYSQL_WIRE_STATUS_INVALID);
  }
}

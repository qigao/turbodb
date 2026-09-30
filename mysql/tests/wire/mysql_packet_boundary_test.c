#include <wire/packet.h>
#include <tinytest.h>
#include <string.h>

spec("MySQL packet stream boundaries") {
  (void)ttest_config__;

  it("preserves payload bytes at every two-fragment split point") {
    static const uint8_t packet[] = {4, 0, 0, 9, 'a', 0, 'b', 'c'};
    for (size_t split = 0u; split <= sizeof(packet); ++split) {
      mysql_wire_packet_stream_t stream;
      const size_t sizes[] = {split, sizeof(packet) - split};
      uint8_t payload[4] = {0};
      size_t input_offset = 0u;
      size_t payload_size = 0u;
      unsigned endings = 0u;
      check_equal(mysql_wire_packet_stream_init(&stream, 9u, sizeof(payload)),
                  MYSQL_WIRE_STATUS_OK);
      for (size_t part = 0u; part < 2u; ++part) {
        mysql_wire_packet_event_t event;
        size_t consumed = 0u;
        const mysql_wire_status_t status = mysql_wire_packet_stream_feed(
            &stream, packet + input_offset, sizes[part], &consumed, &event);
        check_equal(consumed, sizes[part]);
        check_true(status == MYSQL_WIRE_STATUS_OK ||
                   status == MYSQL_WIRE_STATUS_NEED_MORE);
        if (event.length != 0u) {
          check_equal(event.offset, (uint32_t)payload_size);
          check_true(event.data == packet + MYSQL_WIRE_PACKET_HEADER_SIZE + payload_size);
          if (event.length <= sizeof(payload) - payload_size) {
            memcpy(payload + payload_size, event.data, event.length);
            payload_size += event.length;
          }
        }
        endings += event.message_end ? 1u : 0u;
        input_offset += consumed;
      }
      check_equal(payload_size, sizeof(payload));
      check_equal(memcmp(payload, packet + MYSQL_WIRE_PACKET_HEADER_SIZE,
                         sizeof(payload)), 0);
      check_equal(endings, 1u);
    }
  }

  it("leaves coalesced following packets for the next feed") {
    static const uint8_t packets[] = {1, 0, 0, 3, 'a', 1, 0, 0, 4, 'b'};
    const size_t first_packet_size = 5u;
    mysql_wire_packet_stream_t stream;
    mysql_wire_packet_event_t event;
    size_t consumed = 0u;
    check_equal(mysql_wire_packet_stream_init(&stream, 3u, 1u), MYSQL_WIRE_STATUS_OK);
    check_equal(mysql_wire_packet_stream_feed(&stream, packets, sizeof(packets),
                                              &consumed, &event), MYSQL_WIRE_STATUS_OK);
    check_equal(consumed, first_packet_size);
    check_equal(event.data[0], (uint8_t)'a');
    check_equal(mysql_wire_packet_stream_feed(
                    &stream, packets + first_packet_size, sizeof(packets) - first_packet_size,
                    &consumed, &event), MYSQL_WIRE_STATUS_OK);
    check_equal(event.sequence_id, (uint8_t)4u);
    check_equal(event.data[0], (uint8_t)'b');
  }

  it("reset discards a partial header while preserving the payload ceiling") {
    static const uint8_t partial[] = {3, 0};
    static const uint8_t oversized[] = {4, 0, 0, 7};
    static const uint8_t valid[] = {1, 0, 0, 7, 'v'};
    mysql_wire_packet_stream_t stream;
    mysql_wire_packet_event_t event;
    size_t consumed = 0u;
    check_equal(mysql_wire_packet_stream_init(&stream, 0u, 3u), MYSQL_WIRE_STATUS_OK);
    check_equal(mysql_wire_packet_stream_feed(&stream, partial, sizeof(partial),
                                              &consumed, &event), MYSQL_WIRE_STATUS_NEED_MORE);
    mysql_wire_packet_stream_reset(&stream, 7u);
    check_equal(mysql_wire_packet_stream_feed(&stream, oversized, sizeof(oversized),
                                              &consumed, &event), MYSQL_WIRE_STATUS_LIMIT);
    mysql_wire_packet_stream_reset(&stream, 7u);
    check_equal(mysql_wire_packet_stream_feed(&stream, valid, sizeof(valid),
                                              &consumed, &event), MYSQL_WIRE_STATUS_OK);
    check_equal(event.data[0], (uint8_t)'v');
  }

  it("accepts an empty packet with a zero payload budget") {
    static const uint8_t packet[] = {0, 0, 0, 0};
    mysql_wire_packet_stream_t stream;
    mysql_wire_packet_event_t event;
    size_t consumed = 0u;
    check_equal(mysql_wire_packet_stream_init(&stream, 0u, 0u), MYSQL_WIRE_STATUS_OK);
    check_equal(mysql_wire_packet_stream_feed(&stream, packet, sizeof(packet),
                                              &consumed, &event), MYSQL_WIRE_STATUS_OK);
    check_equal(consumed, sizeof(packet));
    check_equal(event.length, (size_t)0u);
    check_null(event.data);
    check_true(event.packet_end);
    check_true(event.message_end);
  }

  it("an empty feed after a header preserves the pending payload") {
    static const uint8_t header[] = {1, 0, 0, 0};
    static const uint8_t payload[] = {'z'};
    mysql_wire_packet_stream_t stream;
    mysql_wire_packet_event_t event;
    size_t consumed = 0u;
    check_equal(mysql_wire_packet_stream_init(&stream, 0u, 1u), MYSQL_WIRE_STATUS_OK);
    check_equal(mysql_wire_packet_stream_feed(&stream, header, sizeof(header),
                                              &consumed, &event), MYSQL_WIRE_STATUS_NEED_MORE);
    check_equal(mysql_wire_packet_stream_feed(&stream, NULL, 0u, &consumed, &event),
                MYSQL_WIRE_STATUS_NEED_MORE);
    check_equal(consumed, (size_t)0u);
    check_equal(mysql_wire_packet_stream_feed(&stream, payload, sizeof(payload),
                                              &consumed, &event), MYSQL_WIRE_STATUS_OK);
    check_equal(event.data[0], (uint8_t)'z');
  }
}

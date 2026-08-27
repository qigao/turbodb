#include "../redis_internal.h"

#include "tinytest.h"
#include "turbo_error.h"

#include <string.h>

suite("redis bounded RESP buffer") {
  it("parses an array incrementally across network-sized fragments") {
    static const char first[] = "*2\r\n:7\r\n$3\r\nf";
    static const char second[] = "oo\r\n";
    redis_resp_buffer buffer = {0};
    redis_resp_array_reader reader;
    redis_reply_t *item = NULL;

    check_equal(redis_resp_buffer_init(&buffer, 4u), 0);
    redis_resp_array_reader_init(&reader, 2u, 1024u);
    check_equal(redis_resp_buffer_append_bounded(
                    &buffer, first, sizeof(first) - 1u, 64u),
                0);
    check_equal(redis_resp_array_reader_next_buffer(&buffer, &reader, &item),
                REDIS_RESP_ARRAY_ITEM);
    check_not_null(item);
    check_equal(item->type, REDIS_REPLY_INTEGER);
    check_equal(item->integer, 7);
    redis_reply_free(item);
    item = NULL;

    check_equal(redis_resp_array_reader_next_buffer(&buffer, &reader, &item),
                REDIS_RESP_ARRAY_NEED_MORE);
    check_null(item);
    check_equal(redis_resp_buffer_append_bounded(
                    &buffer, second, sizeof(second) - 1u, 64u),
                0);
    check_equal(redis_resp_array_reader_next_buffer(&buffer, &reader, &item),
                REDIS_RESP_ARRAY_ITEM);
    check_not_null(item);
    check_equal(item->type, REDIS_REPLY_BULK_STRING);
    check_equal(item->len, 3u);
    check_equal(item->str, "foo", 3u);
    redis_reply_free(item);
    item = NULL;
    check_equal(redis_resp_array_reader_next_buffer(&buffer, &reader, &item),
                REDIS_RESP_ARRAY_DONE);
    check_equal(buffer.used, 0u);
    check_true(buffer.capacity <= 64u);
    redis_resp_buffer_destroy(&buffer);
  }

  it("rejects input beyond its hard bound without changing retained data") {
    redis_resp_buffer buffer = {0};
    check_equal(redis_resp_buffer_init(&buffer, 4u), 0);
    check_equal(redis_resp_buffer_append_bounded(&buffer, "1234", 4u, 4u), 0);
    check_equal(redis_resp_buffer_append_bounded(&buffer, "5", 1u, 4u), -2);
    check_equal(buffer.used, 4u);
    check_equal(buffer.capacity, 4u);
    check_equal(buffer.data, "1234", 4u);
    redis_resp_buffer_destroy(&buffer);
  }

  it("preserves top-level array header progress across one-byte fragments") {
    static const char encoded[] = "*00000000000000000001\r\n:7\r\n";
    redis_resp_buffer buffer = {0};
    redis_resp_array_reader reader;
    redis_reply_t *item = NULL;
    size_t index;
    size_t previous_scan = 0u;
    redis_resp_array_step step = REDIS_RESP_ARRAY_NEED_MORE;

    check_equal(redis_resp_buffer_init(&buffer, 1u), 0);
    redis_resp_array_reader_init(&reader, 1u, 1024u);
    for (index = 0u; index < sizeof(encoded) - 1u; ++index) {
      check_equal(redis_resp_buffer_append_bounded(
                      &buffer, &encoded[index], 1u, sizeof(encoded) - 1u),
                  0);
      step = redis_resp_array_reader_next_buffer(&buffer, &reader, &item);
      if (!reader.header_read) {
        check_true(reader.header_scan >= previous_scan);
        previous_scan = reader.header_scan;
      }
      if (index + 1u < sizeof(encoded) - 1u) {
        check_equal(step, REDIS_RESP_ARRAY_NEED_MORE);
        check_null(item);
      }
    }
    check_equal(step, REDIS_RESP_ARRAY_ITEM);
    check_not_null(item);
    check_equal(item->type, REDIS_REPLY_INTEGER);
    check_equal(item->integer, 7);
    redis_reply_free(item);
    check_equal(redis_resp_array_reader_next_buffer(&buffer, &reader, &item),
                REDIS_RESP_ARRAY_DONE);
    redis_resp_array_reader_destroy(&reader);
    redis_resp_buffer_destroy(&buffer);
  }

  it("bounds binary-safe command encoding at the exact RESP size") {
    static const char binary[] = {'\0', '\r', '\n'};
    static const char expected[] = {
        '*', '1', '\r', '\n', '$', '3', '\r', '\n',
        '\0', '\r', '\n', '\r', '\n'};
    const char *arguments[] = {binary};
    const size_t lengths[] = {sizeof(binary)};
    tstr command = NULL;

    check_equal(redis_resp_command_build_bounded(
                    1, arguments, lengths, sizeof(expected) - 1u, &command),
                TURBO_ENOBUFS);
    check_null(command);
    check_equal(redis_resp_command_build_bounded(
                    1, arguments, lengths, sizeof(expected), &command),
                TURBO_OK);
    check_equal(tstr_len(command), sizeof(expected));
    check_equal(command, expected, sizeof(expected));
    tstr_free(command);
  }

  it("preserves nested parser progress across one-byte network fragments") {
    static const char encoded[] = "*2\r\n*1\r\n$3\r\nfoo\r\n:7\r\n";
    redis_resp_buffer buffer = {0};
    redis_resp_reply_reader reader;
    redis_reply_t *reply = NULL;
    size_t index;
    int parsed = 0;

    check_equal(redis_resp_buffer_init(&buffer, 1u), 0);
    redis_resp_reply_reader_init(&reader, 1024u);
    for (index = 0u; index < sizeof(encoded) - 1u; ++index) {
      check_equal(redis_resp_buffer_append_bounded(
                      &buffer, &encoded[index], 1u, sizeof(encoded) - 1u),
                  0);
      parsed = redis_resp_reply_reader_next_buffer(&buffer, &reader, &reply);
      if (index + 1u < sizeof(encoded) - 1u) {
        check_equal(parsed, 0);
        check_null(reply);
      }
    }
    check_equal(parsed, (int)(sizeof(encoded) - 1u));
    check_not_null(reply);
    check_equal(reply->type, REDIS_REPLY_ARRAY);
    check_equal(reply->element_count, 2u);
    check_equal(reply->elements[0]->type, REDIS_REPLY_ARRAY);
    check_equal(reply->elements[0]->element_count, 1u);
    check_equal(reply->elements[0]->elements[0]->type,
                REDIS_REPLY_BULK_STRING);
    check_equal(reply->elements[0]->elements[0]->str, "foo", 3u);
    check_equal(reply->elements[1]->type, REDIS_REPLY_INTEGER);
    check_equal(reply->elements[1]->integer, 7);
    redis_reply_free(reply);
    redis_resp_reply_reader_destroy(&reader);
    redis_resp_buffer_destroy(&buffer);
  }
}

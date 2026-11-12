#include "../redis_internal.h"

#include "tinytest.h"
#include "core/error.h"

#include <string.h>

suite("redis bounded RESP buffer") {
  static redis_resp_buffer scanned_buffer;
  static redis_resp_reply_reader scanned_reader;
  static redis_resp_array_reader scanned_array_reader;
  static redis_reply_t *scanned_reply;
  static tstr encoded_command;

  after_each() {
    redis_reply_free(scanned_reply);
    scanned_reply = NULL;
    redis_resp_reply_reader_destroy(&scanned_reader);
    redis_resp_array_reader_destroy(&scanned_array_reader);
    redis_resp_buffer_destroy(&scanned_buffer);
    tstr_freep(&encoded_command);
  }
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
                SALTS_ENOBUFS);
    check_null(command);
    check_equal(redis_resp_command_build_bounded(
                    1, arguments, lengths, sizeof(expected), &command),
                SALTS_OK);
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

  it("finds CRLF across every vector boundary and keeps owned binary replies") {
    for (size_t length = 0u; length < 320u; ++length) {
      char encoded[324], expected[320];
      encoded[0] = '+';
      for (size_t i = 0u; i < length; ++i)
        expected[i] = encoded[i + 1u] =
            (i % 3u == 0u ? '\0' : (char)(0x80u + i % 64u));
      encoded[length + 1u] = '\r';
      encoded[length + 2u] = '\n';
      check_equal(redis_resp_buffer_init(&scanned_buffer, 1u), 0);
      redis_resp_reply_reader_init(&scanned_reader, 4096u);
      check_equal(redis_resp_buffer_append_bounded(&scanned_buffer, encoded,
                      length + 2u, sizeof(encoded)), 0);
      check_equal(redis_resp_reply_reader_next_buffer(
                      &scanned_buffer, &scanned_reader, &scanned_reply), 0);
      check_null(scanned_reply);
      check_equal(redis_resp_buffer_append_bounded(&scanned_buffer, "\n", 1u,
                      sizeof(encoded)), 0);
      check_equal(redis_resp_reply_reader_next_buffer(
                      &scanned_buffer, &scanned_reader, &scanned_reply),
                  (int)(length + 3u));
      check_not_null(scanned_reply);
      check_equal(scanned_reply->len, length);
      memset(scanned_buffer.data, 'z', scanned_buffer.used);
      check_equal(scanned_reply->str, expected, length);
      check_equal(scanned_reply->str[length], '\0');
      redis_reply_free(scanned_reply);
      scanned_reply = NULL;
      redis_resp_reply_reader_destroy(&scanned_reader);
      redis_resp_buffer_destroy(&scanned_buffer);
    }
  }

  it("keeps a long array header's trailing CR until its next fragment") {
    char header[300];
    memset(header, '0', sizeof(header));
    header[0] = '*';
    header[sizeof(header) - 2u] = '1';
    header[sizeof(header) - 1u] = '\r';
    check_equal(redis_resp_buffer_init(&scanned_buffer, sizeof(header)), 0);
    redis_resp_array_reader_init(&scanned_array_reader, 1u, 4096u);
    check_equal(redis_resp_buffer_append_bounded(&scanned_buffer, header,
                    sizeof(header), 512u), 0);
    check_equal(redis_resp_array_reader_next_buffer(
                    &scanned_buffer, &scanned_array_reader, &scanned_reply),
                REDIS_RESP_ARRAY_NEED_MORE);
    check_equal(scanned_array_reader.header_scan, sizeof(header));
    check_equal(redis_resp_buffer_append_bounded(&scanned_buffer,
                    "\n:7\r\n", 5u, 512u), 0);
    check_equal(redis_resp_array_reader_next_buffer(
                    &scanned_buffer, &scanned_array_reader, &scanned_reply),
                REDIS_RESP_ARRAY_ITEM);
    check_not_null(scanned_reply);
    check_equal(scanned_reply->integer, 7);
    redis_reply_free(scanned_reply);
    scanned_reply = NULL;
    check_equal(redis_resp_array_reader_next_buffer(
                    &scanned_buffer, &scanned_array_reader, &scanned_reply),
                REDIS_RESP_ARRAY_DONE);
  }

  it("encodes empty and C-string arguments with exact framing and termination") {
    static const char expected[] =
        "*10\r\n$0\r\n\r\n$1\r\n1\r\n$1\r\n2\r\n$1\r\n3\r\n$1\r\n4\r\n"
        "$1\r\n5\r\n$1\r\n6\r\n$1\r\n7\r\n$1\r\n8\r\n$2\r\n99\r\n";
    const char *arguments[] = {"", "1", "2", "3", "4", "5", "6", "7", "8", "99"};
    check_equal(redis_resp_command_build_bounded(10, arguments, NULL,
                    sizeof(expected) - 2u, &encoded_command), SALTS_ENOBUFS);
    check_null(encoded_command);
    check_equal(redis_resp_command_build_bounded(10, arguments, NULL,
                    sizeof(expected) - 1u, &encoded_command), SALTS_OK);
    check_equal(tstr_len(encoded_command), sizeof(expected) - 1u);
    check_equal(encoded_command, expected, sizeof(expected));
  }

  it("rejects invalid commands without changing an existing output owner") {
    const char *arguments[] = {"GET", NULL};
    check_equal(redis_resp_command_build_bounded(0, arguments, NULL,
                    1024u, &encoded_command), SALTS_EINVAL);
    check_equal(redis_resp_command_build_bounded(2, arguments, NULL,
                    1024u, &encoded_command), SALTS_EINVAL);
    check_null(encoded_command);
    encoded_command = tstr_dup("owned");
    check_not_null(encoded_command);
    check_equal(redis_resp_command_build_bounded(1, arguments, NULL,
                    1024u, &encoded_command), SALTS_EINVAL);
    check_equal(encoded_command, "owned");
  }
}

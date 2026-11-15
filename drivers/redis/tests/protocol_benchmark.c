#include "../redis_internal.h"
#include <tinytest.h>
#include <string.h>

enum { SAMPLES = 200, BATCH = 1024, MAX_PAYLOAD = 4096, ARGUMENTS = 16 };

spec("Redis protocol benchmarks") {
  static tstr line;
  static tstr command;
  static redis_reply_t *reply;
  static redis_resp_reply_reader reader;
  static redis_resp_buffer buffer;

  after_each() {
    redis_reply_free(reply);
    reply = NULL;
    redis_resp_reply_reader_destroy(&reader);
    tstr_freep(&line);
    tstr_freep(&command);
  }

  for (size_t length = 8u; length <= MAX_PAYLOAD; length *= 8u) {
    bench("parse and own a %zu-byte simple reply", length) {
      line = tstr_new_len(NULL, length + 3u);
      check_not_null(line);
      line[0] = '+';
      memset(line + 1u, 'x', length);
      memcpy(line + 1u + length, "\r\n", 2u);
      buffer.data = line;
      buffer.capacity = buffer.used = tstr_len(line);
      redis_resp_reply_reader_init(&reader, MAX_PAYLOAD * 4u);
      check_equal(redis_resp_reply_reader_next_buffer(&buffer, &reader, &reply),
                  (int)buffer.used);
      check_equal(reply->len, length);
      check_equal(reply->str, line + 1u, length);
      redis_reply_free(reply);
      reply = NULL;
      size_t failures = 0u;
      benchmark_io("RESP reply parse/own/free", SAMPLES, BATCH,
                   BATCH * buffer.used) {
        for (size_t i = 0u; i < BATCH; ++i) {
          if (redis_resp_reply_reader_next_buffer(&buffer, &reader, &reply) !=
              (int)buffer.used) ++failures;
          redis_reply_free(reply);
          reply = NULL;
        }
      }
      check_equal(failures, 0u);
    }
  }

  for (size_t length = 8u; length <= MAX_PAYLOAD; length *= 8u) {
    bench("encode %d binary arguments of %zu bytes", ARGUMENTS, length) {
      const char *arguments[ARGUMENTS];
      size_t lengths[ARGUMENTS];
      line = tstr_new_len(NULL, length);
      check_not_null(line);
      memset(line, 'x', length);
      for (size_t i = 0u; i < ARGUMENTS; ++i) {
        arguments[i] = line;
        lengths[i] = length;
      }
      check_equal(redis_resp_command_build_bounded(ARGUMENTS, arguments, lengths,
                      MAX_PAYLOAD * ARGUMENTS * 2u, &command), 0);
      const size_t bytes = tstr_len(command);
      check_greater(bytes, ARGUMENTS * length);
      tstr_freep(&command);
      size_t failures = 0u;
      benchmark_io("RESP command build/free", SAMPLES, BATCH, BATCH * bytes) {
        for (size_t i = 0u; i < BATCH; ++i) {
          if (redis_resp_command_build_bounded(ARGUMENTS, arguments, lengths,
                  MAX_PAYLOAD * ARGUMENTS * 2u, &command) != 0) ++failures;
          tstr_freep(&command);
        }
      }
      check_equal(failures, 0u);
    }
  }
}

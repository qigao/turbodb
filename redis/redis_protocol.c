#include "redis_internal.h"

#include <fmt.h>
#include "salts_error.h"

#include <stdint.h>
#include <string.h>

static size_t redis_resp_decimal_digits(size_t value) {
  size_t digits = 1u;
  while (value >= 10u) {
    value /= 10u;
    ++digits;
  }
  return digits;
}

int redis_resp_command_build_bounded(int argc, const char **argv,
                                     const size_t *argvlen,
                                     size_t max_command_bytes,
                                     tstr *out_command) {
  tstr command = NULL;
  tstr next;
  size_t total;
  int index;
  if (argc <= 0 || !argv || max_command_bytes == 0u || !out_command ||
      *out_command)
    return SALTS_EINVAL;
  total = 1u + redis_resp_decimal_digits((size_t)argc) + 2u;
  if (total > max_command_bytes) return SALTS_ENOBUFS;
  for (index = 0; index < argc; ++index) {
    size_t length;
    size_t framing;
    if (!argv[index]) return SALTS_EINVAL;
    length = argvlen ? argvlen[index] : strlen(argv[index]);
    framing = 1u + redis_resp_decimal_digits(length) + 2u + 2u;
    if (length > max_command_bytes || framing > max_command_bytes - length ||
        total > max_command_bytes - framing - length)
      return SALTS_ENOBUFS;
    total += framing + length;
  }
  command = tstr_new();
  if (!command) return SALTS_ENOMEM;
  next = tstr_reserve(command, total);
  if (!next) {
    tstr_free(command);
    return SALTS_ENOMEM;
  }
  command = next;
  next = tstr_append_format(command, "*{}\r\n", argc);
  if (!next) {
    tstr_free(command);
    return SALTS_ENOMEM;
  }
  command = next;
  for (index = 0; index < argc; ++index) {
    size_t length = argvlen ? argvlen[index] : strlen(argv[index]);
    next = tstr_append_format(command, "${}\r\n", length);
    if (!next) {
      tstr_free(command);
      return SALTS_ENOMEM;
    }
    command = next;
    next = tstr_cat_v(command, vstr_from_buf(argv[index], length));
    if (!next) {
      tstr_free(command);
      return SALTS_ENOMEM;
    }
    command = next;
    next = tstr_cat_len(command, "\r\n", 2u);
    if (!next) {
      tstr_free(command);
      return SALTS_ENOMEM;
    }
    command = next;
  }
  *out_command = command;
  return SALTS_OK;
}

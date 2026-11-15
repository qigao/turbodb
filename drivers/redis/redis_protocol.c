#include "redis_internal.h"

#include <core/fmt.h>
#include "core/error.h"

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
  tstr reserved;
  size_t total;
  size_t used;
  int written;
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
  /* Reserve once, then render directly into the owner's capacity. Avoid both
   * temporary tstr appends and zero-initializing large binary payloads. */
  command = tstr_new();
  if (!command) return SALTS_ENOMEM;
  reserved = tstr_reserve(command, total);
  if (!reserved) {
    tstr_free(command);
    return SALTS_ENOMEM;
  }
  command = reserved;
  written = fmt(command, total + 1u, "*{}\r\n", argc);
  if (written <= 0) {
    tstr_free(command);
    return SALTS_ENOMEM;
  }
  used = (size_t)written;
  for (index = 0; index < argc; ++index) {
    const vstr argument = vstr_from_buf(
        argv[index], argvlen ? argvlen[index] : strlen(argv[index]));
    written = fmt(command + used, total - used + 1u, "${}\r\n", argument.len);
    if (written <= 0) {
      tstr_free(command);
      return SALTS_ENOMEM;
    }
    used += (size_t)written;
    memcpy(command + used, argument.data, argument.len);
    used += argument.len;
    memcpy(command + used, "\r\n", 2u);
    used += 2u;
  }
  if (!tstr_set_len_checked(command, used)) {
    tstr_free(command);
    return SALTS_ENOBUFS;
  }
  *out_command = command;
  return SALTS_OK;
}

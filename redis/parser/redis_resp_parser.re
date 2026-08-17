// re2c --lang c
#include "redis_resp_parser.h"

#include <limits.h>
#include <string.h>

static int redis_resp_scan_header(const char *data, size_t len, char *type) {
  const unsigned char *YYCURSOR;
  const unsigned char *YYLIMIT;
  const unsigned char *YYMARKER;

  if (!type || (!data && len != 0)) return -1;
  if (len == 0) return 0;
  YYCURSOR = (const unsigned char *)data;
  YYLIMIT = YYCURSOR + len;

  /*!re2c
    re2c:define:YYCTYPE = "unsigned char";
    re2c:define:YYCURSOR = YYCURSOR;
    re2c:define:YYLIMIT = YYLIMIT;
    re2c:yyfill:enable = 0;
    re2c:eof = 0;

    SIMPLE = [+-] [^\r\n]* "\r\n";
    NUMBER = [:$*] "-"? [0-9]+ "\r\n";

    SIMPLE | NUMBER { *type = data[0]; return 1; }
    $               { return 0; }
    *               { return -1; }
  */
}

static int redis_resp_parse_integer(const char *data, size_t len, int64_t *value) {
  uint64_t magnitude = 0;
  uint64_t limit;
  size_t pos = 0;
  int negative = 0;

  if (!data || !value || len == 0) return -1;
  if (data[pos] == '-') {
    negative = 1;
    if (++pos == len) return -1;
  }
  limit = negative ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
  for (; pos < len; ++pos) {
    unsigned digit;
    if (data[pos] < '0' || data[pos] > '9') return -1;
    digit = (unsigned)(data[pos] - '0');
    if (magnitude > (limit - digit) / 10u) return -1;
    magnitude = magnitude * 10u + digit;
  }
  if (negative) {
    *value = magnitude == (uint64_t)INT64_MAX + 1u
                 ? INT64_MIN
                 : -(int64_t)magnitude;
  } else {
    *value = (int64_t)magnitude;
  }
  return 0;
}

int redis_resp_scan_token(const char *data, size_t len, redis_resp_token_t *token) {
  const char *newline;
  size_t value_len;
  int header_rc;

  if (!token || (!data && len != 0)) return -1;
  memset(token, 0, sizeof(*token));
  if (len == 0) return 0;

  newline = memchr(data + 1, '\n', len - 1u);
  if (!newline) return 0;
  if (newline == data + 1 || newline[-1] != '\r') return -1;

  token->header_len = (size_t)(newline - data) + 1u;
  header_rc = redis_resp_scan_header(data, token->header_len, &token->type);
  if (header_rc != 1) return -1;
  value_len = (size_t)(newline - data) - 2u;
  token->value = tstr_v_from_buf(data + 1, value_len);

  if (token->type == ':' || token->type == '$' || token->type == '*') {
    if (redis_resp_parse_integer(token->value.data, token->value.len,
                                 &token->int_value) != 0)
      return -1;
  }
  return 1;
}

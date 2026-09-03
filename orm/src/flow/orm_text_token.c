#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "orm_text_token.h"

#include <salts/thread.h>

#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { ORM_TEXT_TOKEN_FLOAT_CAPACITY = 768u };

static salts_once_t orm_text_token_locale_once = SALTS_ONCE_INIT;
#if defined(_WIN32)
static _locale_t orm_text_token_c_locale;
#else
static locale_t orm_text_token_c_locale;
#endif

static void orm_text_token_locale_init(void) {
  /* Process-lifetime locale keeps the hot token path allocation-free. */
#if defined(_WIN32)
  orm_text_token_c_locale = _create_locale(LC_NUMERIC, "C");
#else
  orm_text_token_c_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
#endif
}

static double orm_text_token_strtod(const char *text, char **end) {
  salts_once(&orm_text_token_locale_once, orm_text_token_locale_init);
  if (orm_text_token_c_locale == NULL) {
    *end = (char *)text;
    return 0.0;
  }
#if defined(_WIN32)
  return _strtod_l(text, end, orm_text_token_c_locale);
#else
  return strtod_l(text, end, orm_text_token_c_locale);
#endif
}

static int orm_text_token_decimal_valid(const unsigned char *data,
                                        size_t size) {
  size_t index = 0u;
  size_t digits = 0u;
  if (size == 0u)
    return 0;
  if (data[index] == (unsigned char)'-') {
    ++index;
    if (index == size)
      return 0;
  }
  while (index < size && data[index] >= (unsigned char)'0' &&
         data[index] <= (unsigned char)'9') {
    ++index;
    ++digits;
  }
  if (digits == 0u)
    return 0;
  if (index < size && data[index] == (unsigned char)'.') {
    size_t fraction_digits = 0u;
    ++index;
    while (index < size && data[index] >= (unsigned char)'0' &&
           data[index] <= (unsigned char)'9') {
      ++index;
      ++fraction_digits;
    }
    if (fraction_digits == 0u)
      return 0;
  }
  if (index < size &&
      (data[index] == (unsigned char)'e' ||
       data[index] == (unsigned char)'E')) {
    size_t exponent_digits = 0u;
    ++index;
    if (index < size &&
        (data[index] == (unsigned char)'+' ||
         data[index] == (unsigned char)'-'))
      ++index;
    while (index < size && data[index] >= (unsigned char)'0' &&
           data[index] <= (unsigned char)'9') {
      ++index;
      ++exponent_digits;
    }
    if (exponent_digits == 0u)
      return 0;
  }
  return index == size;
}

static int orm_text_token_postgres_nonfinite(const unsigned char *data,
                                               size_t size) {
  static const char nan_text[] = "NaN";
  static const char infinity_text[] = "Infinity";
  static const char negative_infinity_text[] = "-Infinity";
  return (size == sizeof(nan_text) - 1u &&
          memcmp(data, nan_text, sizeof(nan_text) - 1u) == 0) ||
         (size == sizeof(infinity_text) - 1u &&
          memcmp(data, infinity_text, sizeof(infinity_text) - 1u) == 0) ||
         (size == sizeof(negative_infinity_text) - 1u &&
          memcmp(data, negative_infinity_text,
                 sizeof(negative_infinity_text) - 1u) == 0);
}

cserde_status orm_text_token_sint(const unsigned char *data, size_t size,
                                  cserde_token *out) {
  const uint64_t negative_limit = (uint64_t)INT64_MAX + UINT64_C(1);
  uint64_t limit;
  uint64_t value = 0u;
  size_t index = 0u;
  int negative = 0;

  if (out == NULL || data == NULL || size == 0u)
    return CSERDE_SOURCE_ERROR;
  if (data[0] == (unsigned char)'-') {
    negative = 1;
    index = 1u;
    if (index == size)
      return CSERDE_SOURCE_ERROR;
  }
  limit = negative ? negative_limit : (uint64_t)INT64_MAX;
  for (; index < size; ++index) {
    const unsigned char digit = data[index];
    if (digit < (unsigned char)'0' || digit > (unsigned char)'9')
      return CSERDE_SOURCE_ERROR;
    if (value > (limit - (uint64_t)(digit - (unsigned char)'0')) /
                    UINT64_C(10))
      return CSERDE_SOURCE_ERROR;
    value = value * UINT64_C(10) +
            (uint64_t)(digit - (unsigned char)'0');
  }

  out->kind = CSERDE_SINT;
  out->value.sint = negative
                        ? value == negative_limit
                              ? INT64_MIN
                              : -(int64_t)value
                        : (int64_t)value;
  return CSERDE_OK;
}

cserde_status orm_text_token_uint(const unsigned char *data, size_t size,
                                  cserde_token *out) {
  uint64_t value = 0u;
  size_t index;

  if (out == NULL || data == NULL || size == 0u)
    return CSERDE_SOURCE_ERROR;
  for (index = 0u; index < size; ++index) {
    const unsigned char digit = data[index];
    if (digit < (unsigned char)'0' || digit > (unsigned char)'9')
      return CSERDE_SOURCE_ERROR;
    if (value > (UINT64_MAX -
                 (uint64_t)(digit - (unsigned char)'0')) /
                    UINT64_C(10))
      return CSERDE_SOURCE_ERROR;
    value = value * UINT64_C(10) +
            (uint64_t)(digit - (unsigned char)'0');
  }

  out->kind = CSERDE_UINT;
  out->value.uint = value;
  return CSERDE_OK;
}

cserde_status orm_text_token_float(const unsigned char *data, size_t size,
                                   int finite_only, cserde_token *out) {
  char text[ORM_TEXT_TOKEN_FLOAT_CAPACITY + 1u];
  char *end = NULL;
  double value;

  if (out == NULL || data == NULL || size == 0u ||
      size > ORM_TEXT_TOKEN_FLOAT_CAPACITY)
    return CSERDE_SOURCE_ERROR;
  if (!orm_text_token_decimal_valid(data, size) &&
      (finite_only || !orm_text_token_postgres_nonfinite(data, size)))
    return CSERDE_SOURCE_ERROR;
  memcpy(text, data, size);
  text[size] = '\0';
  errno = 0;
  value = orm_text_token_strtod(text, &end);
  if (end != text + size ||
      (errno == ERANGE && (value == 0.0 || !isfinite(value))) ||
      (finite_only && !isfinite(value)))
    return CSERDE_SOURCE_ERROR;

  out->kind = CSERDE_FLOAT;
  out->value.floating = value;
  return CSERDE_OK;
}

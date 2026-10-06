#ifndef ORM_TIDESDB_SQL_WIRE_H
#define ORM_TIDESDB_SQL_WIRE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
enum { ORM_SQL_WIRE_U32 = 4, ORM_SQL_WIRE_U64 = 8, ORM_SQL_WIRE_BITS = 8 };
/* Private fixed-width little endian fields; caller has checked the span and
 * supplies U32 or U64. Independent of host alignment, ABI and enum values. */
static inline uint64_t orm_sql_wire_read(const uint8_t *data, size_t width) {
  uint64_t value = 0;
  for (size_t i = 0; i < width; ++i) value |= (uint64_t)data[i] << (i * ORM_SQL_WIRE_BITS);
  return value;
}
static inline void orm_sql_wire_write(uint8_t *data, size_t width, uint64_t value) {
  for (size_t i = 0; i < width; ++i) { data[i] = (uint8_t)value; value >>= ORM_SQL_WIRE_BITS; }
}
/* Private order-preserving integer payload shared by primary and index keys.
 * Unsigned magnitude is big endian; signed order shifts the sign boundary.
 * Caller validates the eight-byte span. No allocation or budget ownership. */
static inline void orm_sql_wire_order_write(uint8_t data[ORM_SQL_WIRE_U64], uint64_t value) {
  for (size_t i = ORM_SQL_WIRE_U64; i; --i) { data[i - 1] = (uint8_t)value; value >>= ORM_SQL_WIRE_BITS; }
}
static inline uint64_t orm_sql_wire_order_read(const uint8_t data[ORM_SQL_WIRE_U64]) {
  uint64_t value = 0;
  for (size_t i = 0; i < ORM_SQL_WIRE_U64; ++i) value = (value << ORM_SQL_WIRE_BITS) | data[i];
  return value;
}
static inline uint64_t orm_sql_wire_signed_order(int64_t value) {
  return (uint64_t)value ^ (UINT64_C(1) << (ORM_SQL_WIRE_U64 * ORM_SQL_WIRE_BITS - 1));
}
static inline int64_t orm_sql_wire_order_signed(uint64_t value) {
  const uint64_t sign = UINT64_C(1) << (ORM_SQL_WIRE_U64 * ORM_SQL_WIRE_BITS - 1);
  /* Both casts are representable; never cast a high unsigned value to signed. */
  return value & sign ? (int64_t)(value ^ sign) : INT64_MIN + (int64_t)value;
}
/* Finite binary64 ordered domain shared by tuple encoding and range bounds.
 * The key codec validates the platform and rejects nonfinite/noncanonical
 * stored values. SQL equality requires both zero signs to use positive zero. */
static inline uint64_t orm_sql_wire_double_order(double value) {
  const uint64_t sign = UINT64_C(1) << (ORM_SQL_WIRE_U64 * ORM_SQL_WIRE_BITS - 1);
  const double canonical = value == 0.0 ? 0.0 : value;
  uint64_t bits; memcpy(&bits, &canonical, sizeof(bits));
  return bits & sign ? ~bits : bits ^ sign;
}
static inline double orm_sql_wire_order_double(uint64_t word) {
  const uint64_t sign = UINT64_C(1) << (ORM_SQL_WIRE_U64 * ORM_SQL_WIRE_BITS - 1);
  const uint64_t bits = word & sign ? word ^ sign : ~word;
  double value; memcpy(&value, &bits, sizeof(value)); return value;
}
#endif

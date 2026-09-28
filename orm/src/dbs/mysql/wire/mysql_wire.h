#ifndef ORM_MYSQL_WIRE_H
#define ORM_MYSQL_WIRE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  MYSQL_WIRE_OK = 0,
  MYSQL_WIRE_NEED_MORE = 1,
  MYSQL_WIRE_INVALID_ARGUMENT = 2,
  MYSQL_WIRE_LIMIT_EXCEEDED = 3,
  MYSQL_WIRE_SEQUENCE_ERROR = 4,
  MYSQL_WIRE_PROTOCOL_ERROR = 5,
  MYSQL_WIRE_CALLBACK_ERROR = 6
};

enum {
  MYSQL_WIRE_PACKET_HEADER_BYTES = 4u,
  MYSQL_WIRE_MAX_PACKET_PAYLOAD = 0x00ffffffu
};

typedef struct mysql_wire_bytes {
  const uint8_t *data;
  size_t size;
} mysql_wire_bytes;

typedef struct mysql_wire_reader {
  const uint8_t *data;
  size_t size;
  size_t offset;
} mysql_wire_reader;

typedef struct mysql_wire_lenenc {
  uint64_t value;
  int is_null;
} mysql_wire_lenenc;

typedef struct mysql_wire_packet {
  uint8_t sequence;
  const uint8_t *payload;
  size_t payload_size;
} mysql_wire_packet;

typedef int (*mysql_wire_packet_fn)(void *user,
                                    const mysql_wire_packet *packet);

typedef struct mysql_wire_decoder {
  uint8_t header[MYSQL_WIRE_PACKET_HEADER_BYTES];
  size_t header_used;
  uint32_t payload_size;
  size_t payload_used;
  uint8_t sequence;
  uint8_t expected_sequence;
  int enforce_sequence;
  uint8_t *payload;
  size_t payload_capacity;
  uint32_t max_payload_size;
} mysql_wire_decoder;

typedef struct mysql_wire_ok_packet {
  uint64_t affected_rows;
  uint64_t last_insert_id;
  uint16_t status_flags;
  uint16_t warning_count;
  mysql_wire_bytes info;
} mysql_wire_ok_packet;

typedef struct mysql_wire_error_packet {
  uint16_t error_code;
  int has_sql_state;
  char sql_state[6];
  mysql_wire_bytes message;
} mysql_wire_error_packet;

typedef struct mysql_wire_eof_packet {
  uint16_t warning_count;
  uint16_t status_flags;
} mysql_wire_eof_packet;

uint16_t mysql_wire_load_le16(const uint8_t *data);
uint32_t mysql_wire_load_le24(const uint8_t *data);
uint32_t mysql_wire_load_le32(const uint8_t *data);
uint64_t mysql_wire_load_le64(const uint8_t *data);

void mysql_wire_store_le16(uint8_t *data, uint16_t value);
void mysql_wire_store_le24(uint8_t *data, uint32_t value);
void mysql_wire_store_le32(uint8_t *data, uint32_t value);
void mysql_wire_store_le64(uint8_t *data, uint64_t value);

void mysql_wire_reader_init(mysql_wire_reader *reader,
                            const void *data,
                            size_t size);
int mysql_wire_read_u8(mysql_wire_reader *reader, uint8_t *out);
int mysql_wire_read_le16(mysql_wire_reader *reader, uint16_t *out);
int mysql_wire_read_le24(mysql_wire_reader *reader, uint32_t *out);
int mysql_wire_read_le32(mysql_wire_reader *reader, uint32_t *out);
int mysql_wire_read_le64(mysql_wire_reader *reader, uint64_t *out);
int mysql_wire_read_bytes(mysql_wire_reader *reader, size_t size,
                          mysql_wire_bytes *out);
int mysql_wire_read_lenenc(mysql_wire_reader *reader,
                           mysql_wire_lenenc *out);
size_t mysql_wire_reader_remaining(const mysql_wire_reader *reader);

int mysql_wire_decoder_init(mysql_wire_decoder *decoder,
                            void *payload_buffer,
                            size_t payload_capacity,
                            uint32_t max_payload_size,
                            uint8_t expected_sequence,
                            int enforce_sequence);
void mysql_wire_decoder_reset(mysql_wire_decoder *decoder,
                              uint8_t expected_sequence,
                              int enforce_sequence);
int mysql_wire_decoder_feed(mysql_wire_decoder *decoder,
                            const void *data,
                            size_t size,
                            size_t *out_consumed,
                            mysql_wire_packet_fn on_packet,
                            void *user);

int mysql_wire_decode_ok(const void *payload, size_t size,
                         mysql_wire_ok_packet *out);
int mysql_wire_decode_error(const void *payload, size_t size,
                            mysql_wire_error_packet *out);
int mysql_wire_is_eof_packet(const void *payload, size_t size);
int mysql_wire_decode_eof(const void *payload, size_t size,
                          mysql_wire_eof_packet *out);

#ifdef __cplusplus
}
#endif

#endif /* ORM_MYSQL_WIRE_H */

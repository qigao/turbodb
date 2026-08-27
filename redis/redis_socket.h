#ifndef REDIS_SOCKET_H
#define REDIS_SOCKET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { REDIS_SOCKET_ADDRESS_CAPACITY = 128u };

#define REDIS_SOCKET_INVALID UINTPTR_MAX

typedef enum redis_socket_family {
  REDIS_SOCKET_IPV4 = 1,
  REDIS_SOCKET_IPV6 = 2
} redis_socket_family;

typedef union redis_socket_address_storage {
  uintptr_t alignment;
  unsigned char bytes[REDIS_SOCKET_ADDRESS_CAPACITY];
} redis_socket_address_storage;

typedef struct redis_socket_address {
  redis_socket_address_storage storage;
  size_t length;
  redis_socket_family family;
} redis_socket_address;

int redis_socket_platform_init(void);
void redis_socket_platform_shutdown(void);
int redis_socket_resolve(const char *host, uint16_t port,
                         redis_socket_address *addresses, size_t capacity,
                         size_t *count);
int redis_socket_open(const redis_socket_address *address,
                      uintptr_t *out_socket);
int redis_socket_close(uintptr_t socket_value);

#ifdef __cplusplus
}
#endif

#endif

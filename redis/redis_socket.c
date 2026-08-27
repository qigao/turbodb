#include "redis_socket.h"

#include "turbo_error.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

static int redis_socket_last_error(void) {
#if defined(_WIN32)
  return -(int)WSAGetLastError();
#else
  return -errno;
#endif
}

int redis_socket_platform_init(void) {
#if defined(_WIN32)
  WSADATA data;
  int status = WSAStartup(MAKEWORD(2, 2), &data);
  return status == 0 ? TURBO_OK : -status;
#else
  return TURBO_OK;
#endif
}

void redis_socket_platform_shutdown(void) {
#if defined(_WIN32)
  (void)WSACleanup();
#endif
}

int redis_socket_resolve(const char *host, uint16_t port,
                         redis_socket_address *addresses, size_t capacity,
                         size_t *count) {
  struct addrinfo hints;
  struct addrinfo *results = NULL;
  struct addrinfo *current;
  char service[6];
  size_t used = 0u;
  int status;
  bool overflow = false;
  if (count == NULL) return TURBO_EINVAL;
  *count = 0u;
  if (host == NULL || host[0] == '\0' || addresses == NULL ||
      capacity == 0u || port == 0u)
    return TURBO_EINVAL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  (void)snprintf(service, sizeof(service), "%u", (unsigned int)port);
  status = getaddrinfo(host, service, &hints, &results);
  if (status != 0) return TURBO_EHOSTUNREACH;
  for (current = results; current != NULL; current = current->ai_next) {
    redis_socket_family family;
    if (current->ai_family == AF_INET)
      family = REDIS_SOCKET_IPV4;
    else if (current->ai_family == AF_INET6)
      family = REDIS_SOCKET_IPV6;
    else
      continue;
    if ((size_t)current->ai_addrlen > REDIS_SOCKET_ADDRESS_CAPACITY) {
      overflow = true;
      break;
    }
    if (used == capacity) {
      overflow = true;
      break;
    }
    memset(&addresses[used], 0, sizeof(addresses[used]));
    memcpy(addresses[used].storage.bytes, current->ai_addr,
           (size_t)current->ai_addrlen);
    addresses[used].length = (size_t)current->ai_addrlen;
    addresses[used].family = family;
    ++used;
  }
  freeaddrinfo(results);
  *count = used;
  if (overflow) return TURBO_ENOBUFS;
  return used != 0u ? TURBO_OK : TURBO_EHOSTUNREACH;
}

static int redis_socket_set_nonblocking(uintptr_t socket_value) {
#if defined(_WIN32)
  u_long enabled = 1u;
  return ioctlsocket((SOCKET)socket_value, FIONBIO, &enabled) == 0
             ? TURBO_OK
             : redis_socket_last_error();
#else
  int flags;
  do {
    flags = fcntl((int)socket_value, F_GETFL);
  } while (flags < 0 && errno == EINTR);
  if (flags < 0) return -errno;
  while (fcntl((int)socket_value, F_SETFL, flags | O_NONBLOCK) < 0) {
    if (errno != EINTR) return -errno;
  }
  return TURBO_OK;
#endif
}

int redis_socket_open(const redis_socket_address *address,
                      uintptr_t *out_socket) {
  int native_family;
  uintptr_t socket_value;
  int status;
  if (out_socket == NULL) return TURBO_EINVAL;
  *out_socket = REDIS_SOCKET_INVALID;
  if (address == NULL || address->length == 0u ||
      address->length > REDIS_SOCKET_ADDRESS_CAPACITY)
    return TURBO_EINVAL;
  if (address->family == REDIS_SOCKET_IPV4)
    native_family = AF_INET;
  else if (address->family == REDIS_SOCKET_IPV6)
    native_family = AF_INET6;
  else
    return TURBO_EINVAL;
#if defined(_WIN32)
  socket_value = (uintptr_t)WSASocketW(native_family, SOCK_STREAM, IPPROTO_TCP,
                                       NULL, 0u, WSA_FLAG_OVERLAPPED);
  if ((SOCKET)socket_value == INVALID_SOCKET)
    return redis_socket_last_error();
#else
  socket_value = (uintptr_t)socket(native_family, SOCK_STREAM, IPPROTO_TCP);
  if ((int)socket_value < 0) return redis_socket_last_error();
#endif
  status = redis_socket_set_nonblocking(socket_value);
  if (status != TURBO_OK) {
    (void)redis_socket_close(socket_value);
    return status;
  }
  *out_socket = socket_value;
  return TURBO_OK;
}

int redis_socket_close(uintptr_t socket_value) {
  if (socket_value == REDIS_SOCKET_INVALID) return TURBO_EINVAL;
#if defined(_WIN32)
  return closesocket((SOCKET)socket_value) == 0
             ? TURBO_OK
             : redis_socket_last_error();
#else
  while (close((int)socket_value) != 0) {
    if (errno != EINTR) return -errno;
  }
  return TURBO_OK;
#endif
}

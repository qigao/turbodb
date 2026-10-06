#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

/* Load native declarations before replacing only the block manager's syscall
 * call sites. Production libraries contain neither this probe nor its state. */
#include <block_manager.h>
#include "wal_fault.h"
#include "memory_fault.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_MSC_VER)
#define ORM_TDB_TEST_TLS __declspec(thread)
#else
#define ORM_TDB_TEST_TLS _Thread_local
#endif

static ORM_TDB_TEST_TLS orm_tdb_wal_fault armed_fault;
static ORM_TDB_TEST_TLS size_t fault_hits;
static ORM_TDB_TEST_TLS int sync_fd = -1;

static int sync_fault(orm_tdb_wal_fault fault) {
  return fault >= ORM_TDB_WAL_SYNC_FAIL_BEFORE && fault <= ORM_TDB_WAL_SYNC_EXIT_AFTER;
}

static void crash_exit(void) {
  (void)fputs("storage-crash-point\n", stderr);
  (void)fflush(stderr);
  _Exit(EXIT_SUCCESS);
}

void orm_tdb_wal_fault_arm(orm_tdb_wal_fault fault) {
  if (armed_fault != ORM_TDB_WAL_FAULT_NONE) abort();
  armed_fault = fault;
  fault_hits = 0;
  sync_fd = -1;
}

size_t orm_tdb_wal_fault_hits(void) { return fault_hits; }

static ssize_t write_frame(int fd, const struct iovec *iov, int count, off_t offset) {
  const ssize_t written = tdb_pwritev_safe(fd, iov, count, offset);
  size_t expected = 0;
  for (int i = 0; i < count; ++i) {
    if (iov[i].iov_len > SIZE_MAX - expected) abort();
    expected += iov[i].iov_len;
  }
  if (written >= 0 && (size_t)written == expected)
    orm_tdb_memory_fault_wal_written();
  return written;
}

static ssize_t fault_write(int fd, const struct iovec *iov, int count, off_t offset) {
  const orm_tdb_wal_fault fault = armed_fault;
  if (fault == ORM_TDB_WAL_FAULT_NONE)
    return write_frame(fd, iov, count, offset);
  if (sync_fault(fault)) {
    /* Arm only the barrier for a fully written WAL frame on this thread. */
    const ssize_t written = write_frame(fd, iov, count, offset);
    size_t expected = 0;
    for (int i = 0; i < count; ++i) expected += iov[i].iov_len;
    if (written < 0 || (size_t)written != expected) abort();
    sync_fd = fd;
    return written;
  }
  armed_fault = ORM_TDB_WAL_FAULT_NONE;
  ++fault_hits;
  if (fault == ORM_TDB_WAL_FAIL_BEFORE) {
    errno = EIO;
    return -1;
  }
  ssize_t written;
  if (fault == ORM_TDB_WAL_FAIL_PARTIAL || fault == ORM_TDB_WAL_EXIT_PARTIAL) {
    /* The native frame header is real; its payload/footer remain unwritten. */
    if (count < 1) abort();
    written = pwrite(fd, iov[0].iov_base, iov[0].iov_len, offset);
    if (written != (ssize_t)iov[0].iov_len) abort();
  } else {
    written = write_frame(fd, iov, count, offset);
    size_t expected = 0;
    for (int i = 0; i < count; ++i) expected += iov[i].iov_len;
    if (written < 0 || (size_t)written != expected) abort();
  }
  if (fault == ORM_TDB_WAL_EXIT_PARTIAL || fault == ORM_TDB_WAL_EXIT_AFTER) {
    crash_exit();
  }
  errno = EIO;
  return fault == ORM_TDB_WAL_FAIL_PARTIAL ? written : -1;
}

static int fault_sync(int fd) {
  const orm_tdb_wal_fault fault = armed_fault;
  if (!sync_fault(fault) || fd != sync_fd) return fdatasync(fd);
  armed_fault = ORM_TDB_WAL_FAULT_NONE;
  sync_fd = -1;
  ++fault_hits;
  if (fault == ORM_TDB_WAL_SYNC_FAIL_AFTER || fault == ORM_TDB_WAL_SYNC_EXIT_AFTER) {
    if (fdatasync(fd) != 0) abort();
  }
  if (fault == ORM_TDB_WAL_SYNC_EXIT_BEFORE || fault == ORM_TDB_WAL_SYNC_EXIT_AFTER)
    crash_exit();
  errno = EIO;
  return -1;
}

#define tdb_pwritev_safe fault_write
#define fdatasync fault_sync
#include "../../../../../tidesdb/src/block_manager.c"
#undef fdatasync
#undef tdb_pwritev_safe
#undef ORM_TDB_TEST_TLS

int orm_tdb_wal_uses_explicit_sync(void) { return !odsync_available(); }

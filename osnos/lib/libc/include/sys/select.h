#pragma once

#include <stdint.h>
#include <sys/time.h>
#include <sys/types.h>

/*
 * POSIX select(2). The fd_set bitmap is 1024 bits wide on Linux
 * x86_64; we match the same layout so the kernel can do
 * byte-for-byte memory accesses.
 */

#define FD_SETSIZE  1024

typedef struct {
    uint64_t fds_bits[FD_SETSIZE / 64];   /* 16 × 64 = 1024 bits */
} fd_set;

#define FD_ZERO(set)    do { \
    fd_set *_s = (set); \
    for (int _i = 0; _i < FD_SETSIZE / 64; _i++) _s->fds_bits[_i] = 0; \
} while (0)

#define FD_SET(fd, set)   ((set)->fds_bits[(fd) >> 6] |=  (1ULL << ((fd) & 63)))
#define FD_CLR(fd, set)   ((set)->fds_bits[(fd) >> 6] &= ~(1ULL << ((fd) & 63)))
#define FD_ISSET(fd, set) (((set)->fds_bits[(fd) >> 6] >>  ((fd) & 63)) & 1ULL)

int select(int nfds,
           fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout);

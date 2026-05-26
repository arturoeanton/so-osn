/*
 * lib/libc/poll.c — POSIX poll(2) wrapper.
 *
 * Thin shim over SYS_POLL (#7). Kernel does the heavy lifting:
 * blocks the task with timer + wake hooks, returns the count of
 * ready pollfd entries.
 */

#include <errno.h>
#include <poll.h>

#include "syscall.h"

int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms) {
    long r = osnos_syscall3(SYS_POLL,
                            (long)fds, (long)nfds, (long)timeout_ms);
    if (r < 0) { errno = (int)-r; return -1; }
    return (int)r;
}

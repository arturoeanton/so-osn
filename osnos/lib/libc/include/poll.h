#pragma once
/*
 * poll.h — POSIX poll(2) surface for osnos.
 *
 * Standard pollfd shape + event bits. Kernel implements sys_poll
 * (#7) with these semantics: blocks the caller up to `timeout` ms
 * (or until the next event), returns the number of ready entries.
 *
 * osnos extension: POLL_IPC_PENDING (0x40) lets a task pollwait on
 * its IPC inbox alongside fds. Useful for ring-3 servers (oxsrv) that
 * multiplex mouse, keyboard, and IPC events. The events bit can be
 * set on any pollfd entry — fd<0 is fine, the slot just carries the
 * IPC-readiness query.
 */

#include <stdint.h>

struct pollfd {
    int   fd;
    short events;
    short revents;
};
typedef unsigned long nfds_t;

#define POLLIN          0x0001
#define POLLPRI         0x0002
#define POLLOUT         0x0004
#define POLLERR         0x0008
#define POLLHUP         0x0010
#define POLLNVAL        0x0020
#define POLL_IPC_PENDING 0x0040     /* osnos: also wake on incoming IPC */

int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms);

#pragma once

/*
 * Ring-3 facing wrapper for the OSnOS IPC ABI. Re-exposes the
 * kernel/userland shared types so user ELFs (the ring-3 servers
 * under elfs/osn-server/) can build ipc_msg_t values and hand
 * them to SYS_IPC_SEND. The actual struct + opcode definitions
 * live in src/include/osnos_ipc_abi.h.
 *
 * The user build adds `-I src/include` so the ABI header resolves
 * cleanly here.
 */

#include "osnos_ipc_abi.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>     /* nanosleep, for the blocking ipc_recv loop */

#include "../syscall.h"

/* SYS_* numbers shared with the kernel (src/micro/syscall.h). */
#ifndef SYS_IPC_SEND
#define SYS_IPC_SEND          260
#define SYS_IPC_RECV          261
#define SYS_SERVICE_REGISTER  262
#define SYS_SERVICE_LOOKUP    263
#define SYS_TTY_INPUT         264
#endif

/*
 * Send `msg` over the kernel IPC queue. Returns 0 on success or
 * a negative errno (-EAGAIN if the queue is full, -ESRCH if the
 * `to` service is not registered). The kernel overwrites
 * `msg->from` with the caller's pid before enqueuing.
 */
static inline long ipc_send(const ipc_msg_t *msg) {
    long r = osnos_syscall1(SYS_IPC_SEND, (long)msg);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

/*
 * Non-blocking pop. Returns 0 on success (msg populated), -1
 * with errno=EAGAIN if no message is queued for this task.
 */
static inline long ipc_recv(ipc_msg_t *out) {
    long r = osnos_syscall2(SYS_IPC_RECV, (long)out, 0);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

/*
 * Blocking pop. Loops on EAGAIN with a short nanosleep so the
 * scheduler can dispatch other tasks. Mirrors how read() handles
 * blocking I/O.
 */
static inline long ipc_recv_block(ipc_msg_t *out) {
    for (;;) {
        long r = osnos_syscall2(SYS_IPC_RECV, (long)out, 1);
        if (r >= 0) return 0;
        if (-r != EAGAIN) { errno = (int)(-r); return -1; }
        struct timespec ts = { 0, 20 * 1000000 };
        nanosleep(&ts, 0);
    }
}

/*
 * Register the current task as the implementation of SERVER_*
 * sid. Future ipc_send() calls aimed at that sid will land on
 * this task's IPC queue.
 */
static inline long ipc_service_register(int sid) {
    long r = osnos_syscall1(SYS_SERVICE_REGISTER, sid);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

/*
 * Look up the pid currently registered for SERVER_* sid, or -1
 * with errno=ENOENT if none.
 */
static inline long ipc_service_lookup(int sid) {
    long r = osnos_syscall1(SYS_SERVICE_LOOKUP, sid);
    if (r < 0) { errno = (int)(-r); return -1; }
    return r;
}

/*
 * Feed a single byte to the kernel TTY line discipline. Restricted
 * to the task currently registered as SERVER_KEYBOARD (see
 * sys_tty_input in src/micro/syscall.c). Used by ring-3 kbdsrv to
 * forward keystrokes into canonical / ISIG processing.
 */
static inline long ipc_tty_input(int c) {
    long r = osnos_syscall1(SYS_TTY_INPUT, c);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

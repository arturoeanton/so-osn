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

/* SYS_* numbers shared with the kernel (src/micro/syscall.h).
 * Moved out of the 260-268 range to dodge Linux x86_64 syscalls
 * 262=newfstatat (used by musl `stat`), 263=unlinkat, etc. */
#ifndef SYS_IPC_SEND
#define SYS_IPC_SEND          510
#define SYS_IPC_RECV          511
#define SYS_SERVICE_REGISTER  512
#define SYS_SERVICE_LOOKUP    513
#define SYS_TTY_INPUT         514
#define SYS_TASKINFO          515
#define SYS_SPAWN             516
#define SYS_SET_FG            517
#define SYS_RESUME            518
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

/*
 * osn_spawn — fork-and-exec replacement for ring-3 tasks (FASE 10.4
 * pre-req). Creates a child task running `path` with `args` and the
 * packed `envp_flat` ("KEY=VAL\0KEY=VAL\0\0") or NULL. fd inheritance:
 * if `stdin_fd` / `stdout_fd` are >= 0 they MUST be open fds in the
 * caller's table; their slots are MOVED into the child's fds[0]/[1]
 * and zeroed in the caller. Returns child pid on success.
 */
static inline long osn_spawn(const char *path, const char *args,
                              const char *envp_flat,
                              int stdin_fd, int stdout_fd) {
    long r = osnos_syscall5(SYS_SPAWN,
                            (long)path, (long)args, (long)envp_flat,
                            (long)stdin_fd, (long)stdout_fd);
    if (r < 0) { errno = (int)(-r); return -1; }
    return r;
}

/* Publish the caller's current foreground child pid (or 0 to clear)
 * so the TTY layer routes Ctrl+C / Ctrl+Z signals to it instead of
 * the caller. Used by the ring-3 shell so the shell itself isn't
 * killed by stray ^C while waiting for a child. */
static inline long osn_set_fg(long pid) {
    long r = osnos_syscall1(SYS_SET_FG, pid);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

/* Resume a stopped task (Ctrl+Z'd via SIGTSTP) without killing it. */
static inline long osn_resume(long pid) {
    long r = osnos_syscall1(SYS_RESUME, pid);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../include/osnos_stat.h"

/*
 * Linux x86_64 syscall numbers. Values match exactly. Numbers MUST NOT
 * be renumbered; new syscalls go at the next free Linux slot or above
 * 200 if osnos-specific.
 */
#define SYS_READ      0
#define SYS_WRITE     1
#define SYS_OPEN      2
#define SYS_CLOSE     3
#define SYS_FSTAT     5
#define SYS_LSEEK     8
#define SYS_BRK      12
#define SYS_NANOSLEEP 35
#define SYS_GETPID   39
#define SYS_EXIT     60
#define SYS_KILL     62
#define SYS_RENAME   82
#define SYS_MKDIR    83
#define SYS_RMDIR    84
#define SYS_UNLINK   87
#define SYS_GETDENTS 217   /* getdents64 in Linux x86_64 */

/* osnos-specific (above 200 by convention). */
#define SYS_ISATTY  201

/*
 * Saved user GPR set, pushed by int80_entry / syscall_entry on every
 * kernel entry. Order matches the asm push sequence (low offset =
 * pushed last). 15 GPRs * 8 bytes = 120 bytes per frame.
 *
 * Having every GPR snapshotted in a known location is what lets
 * sys_nanosleep (and future syscalls that suspend) checkpoint the
 * full user-visible state without writing more asm.
 */
typedef struct {
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
} syscall_frame_t;

void syscall_init(void);

/* Frame-based dispatcher (the future ring3 entry point). */
uint64_t syscall_dispatch(syscall_frame_t *frame);

/*
 * Direct kernel-side handles for testing and for use by other kernel
 * subsystems before userland exists. Return convention is Linux-style:
 *   >= 0   -> success (fd / byte count / 0)
 *   <  0   -> -osnos_status_t (errno-style)
 */
int64_t sys_open  (const char *path, int flags, uint32_t mode);
int64_t sys_close (int fd);
int64_t sys_read  (int fd, void *buf, size_t count);
int64_t sys_write (int fd, const void *buf, size_t count);
int64_t sys_lseek (int fd, int64_t offset, int whence);
int64_t sys_fstat (int fd, osnos_stat_t *out);
int64_t sys_isatty(int fd);
int64_t sys_exit  (int code);

/*
 * Linux brk(2): grow or shrink the heap window of the current task.
 *   new_brk == 0          -> just query, returns current heap_brk
 *   new_brk in valid range -> map/unmap pages, returns new heap_brk
 *   new_brk invalid       -> returns current heap_brk unchanged
 *
 * libc inspects the return: if it equals what was requested, success;
 * otherwise the request was rejected (libc sets errno = ENOMEM).
 */
int64_t sys_brk   (uintptr_t new_brk);

/*
 * struct timespec — layout-compatible with Linux x86_64. Used by
 * sys_nanosleep. tv_sec and tv_nsec are both 8 bytes on x86_64.
 */
typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} osnos_timespec_t;

/*
 * Linux nanosleep(2): blocks the calling task for at least
 * req->tv_sec + req->tv_nsec/1e9 seconds. `rem` is currently
 * ignored (we never return early). Implementation today is a
 * cooperative `hlt`-loop — no preemption, every other task is
 * paused for the duration. Real per-task BLOCKED + wakeup lands in
 * FASE 9.3 with context switching.
 */
int64_t sys_nanosleep(const osnos_timespec_t *req, osnos_timespec_t *rem);

/*
 * Linux kill(2). Today the signal number is ignored — we only deliver
 * a single kind of "die soon" notification by setting the target
 * task's kill_pending flag. The next kernel return path for that task
 * (syscall, timer IRQ, fault) routes it through proc_exit_current_user
 * with exit code 130 (128 + SIGINT convention).
 *
 * Returns 0 on success, -ESRCH if no such pid (or pid is a kernel
 * task we shouldn't be killing).
 */
int64_t sys_kill  (uint64_t pid, int sig);

/*
 * Linux getpid(2). Returns the calling task's pid (always non-zero for
 * a running task). For kernel-mode callers (no task) we return 0; user
 * code never sees that path.
 */
int64_t sys_getpid(void);

int64_t sys_mkdir   (const char *path, uint32_t mode);
int64_t sys_rmdir   (const char *path);
int64_t sys_unlink  (const char *path);
int64_t sys_rename  (const char *oldpath, const char *newpath);
int64_t sys_getdents(int fd, void *buf, size_t buf_size);

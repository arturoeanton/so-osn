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
#define SYS_STAT      4
#define SYS_FSTAT     5
#define SYS_MMAP      9
#define SYS_MUNMAP   11
#define SYS_LSEEK     8
#define SYS_BRK      12
#define SYS_DUP      32
#define SYS_DUP2     33
#define SYS_NANOSLEEP 35
#define SYS_FCNTL    72
#define SYS_GETPID   39
#define SYS_EXIT     60
#define SYS_KILL     62
#define SYS_ACCESS   21
#define SYS_GETCWD   79
#define SYS_CHDIR    80
#define SYS_TIME    201   /* osnos uses Linux's old time(2) slot */
#define SYS_CLOCK_GETTIME 228
#define SYS_RENAME   82
#define SYS_MKDIR    83
#define SYS_RMDIR    84
#define SYS_UNLINK   87
#define SYS_GETDENTS 217   /* getdents64 in Linux x86_64 */
#define SYS_IOCTL      16
#define SYS_SELECT     23
#define SYS_SOCKET     41
#define SYS_CONNECT    42
#define SYS_ACCEPT     43
#define SYS_SENDTO     44
#define SYS_RECVFROM   45
#define SYS_BIND       49
#define SYS_LISTEN     50
#define SYS_SETSOCKOPT 54

/* osnos-specific (above 250 to dodge Linux's #201 = time, #228 = clock_gettime). */
#define SYS_ISATTY  250

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

/* POSIX-ish cwd. getcwd returns the byte count INCLUDING the NUL
 * (matches Linux's behaviour, which returns the buffer length used).
 * chdir refuses anything that doesn't resolve to a directory. */
int64_t sys_getcwd  (char *buf, size_t size);
int64_t sys_chdir   (const char *path);

/* sys_stat — like fstat but takes a path. EFAULT on bad pointer,
 * ENOENT if path doesn't exist. */
int64_t sys_stat    (const char *path, void *out);

/* sys_access — check that `path` exists. `mode` is the access bits
 * mask (R_OK / W_OK / X_OK / F_OK) — osnos doesn't enforce yet, so
 * the only failure modes are EFAULT and ENOENT. */
int64_t sys_access  (const char *path, int mode);

/* sys_time — seconds since boot (no RTC). Matches Linux's old
 * `time(t_t *t)` slot: writes the value to *t too if non-NULL,
 * and returns it. */
int64_t sys_time    (int64_t *t);

/* sys_clock_gettime — POSIX clock_gettime. Today only CLOCK_REALTIME
 * (0) and CLOCK_MONOTONIC (1) are recognised; both return ticks
 * since boot (no RTC). Writes a struct timespec to user memory. */
int64_t sys_clock_gettime(int clk_id, void *tp);

/*
 * mmap / munmap — POSIX. Today only the anonymous flavour is
 * supported (MAP_ANONYMOUS | MAP_PRIVATE/SHARED, fd == -1). File-
 * backed mmap returns -ENOSYS. MAP_FIXED is not supported either.
 *
 * `addr` is a hint only (or NULL for "pick one"); the kernel
 * places the mapping at the task's bump cursor (USER_MMAP_BASE).
 * Return value is the user virtual address, or -errno cast as a
 * pointer-sized signed int.
 */
int64_t sys_mmap   (void *addr, size_t length, int prot, int flags,
                     int fd, int64_t offset);
int64_t sys_munmap (void *addr, size_t length);

/*
 * dup / dup2. Both return the new fd on success, -1 on error. Today
 * the clone gets a copy of the source's struct — they share path
 * and flags, but offsets diverge after the dup (POSIX-strict dup
 * requires a shared "open file description" which we don't have).
 */
int64_t sys_dup     (int fd);
int64_t sys_dup2    (int oldfd, int newfd);

/*
 * Minimal fcntl(2). Supported cmds:
 *   F_DUPFD (0) — dup with min fd arg.
 *   F_GETFD (1) — close-on-exec flag (always 0; we don't have CLOEXEC).
 *   F_SETFD (2) — accepted but ignored.
 *   F_GETFL (3) — returns the fd's flags.
 *   F_SETFL (4) — updates the editable subset (O_APPEND, O_NONBLOCK).
 *                 Note: today neither flag changes runtime behaviour
 *                 of read/write — they are stored for future use.
 * Any other cmd returns -EINVAL.
 */
int64_t sys_fcntl   (int fd, int cmd, int64_t arg);

/*
 * Linux socket layer. Only AF_INET (2) + SOCK_DGRAM (2) lit up for now;
 * SOCK_STREAM (1) returns EAFNOSUPPORT until TCP lands in 8.5.5.
 *
 * sockaddr_in layout (16 bytes, Linux-compatible):
 *   0..1  sin_family   (LE,  AF_INET = 2)
 *   2..3  sin_port     (BE)
 *   4..7  sin_addr     (BE)
 *   8..15 sin_zero     (must be 0)
 */
int64_t sys_socket    (int domain, int type, int protocol);
int64_t sys_bind      (int fd, const void *addr, uint32_t addrlen);
int64_t sys_connect   (int fd, const void *addr, uint32_t addrlen);
int64_t sys_listen    (int fd, int backlog);
int64_t sys_accept    (int fd, void *addr, void *addrlen_ptr);
int64_t sys_sendto    (int fd, const void *buf, size_t len, int flags,
                        const void *dst_addr, uint32_t addrlen);
int64_t sys_recvfrom  (int fd, void *buf, size_t len, int flags,
                        void *src_addr, void *addrlen_ptr);
int64_t sys_setsockopt(int fd, int level, int optname,
                        const void *optval, uint32_t optlen);

/*
 * Linux select(2). fd_set is a 1024-bit bitmap (16 uint64_t). nfds is
 * highest fd to consider + 1. timeout points to a struct timeval
 * { sec, usec } or NULL for "block until something is ready".
 * timeval { 0, 0 } means "poll once".
 *
 * Bitmaps are modified in-place: only fds that are ready stay set.
 * Return value is the total count across all three sets.
 */
int64_t sys_select    (int nfds,
                        void *readfds, void *writefds, void *exceptfds,
                        const void *timeout);

/*
 * Linux ioctl(2). Today only the TTY ones land:
 *   TCGETS (0x5401)  -> arg = struct termios *out, kernel fills it
 *   TCSETS (0x5402)  -> arg = const struct termios *in, kernel adopts
 *   TCSETSW (0x5403) -> same as TCSETS for now (no output queue)
 *   TCSETSF (0x5404) -> TCSETS + drop pending input
 * Any other request returns -ENOTTY.
 */
int64_t sys_ioctl     (int fd, uint64_t request, void *arg);

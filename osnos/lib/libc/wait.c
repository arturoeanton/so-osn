/*
 * lib/libc/wait.c — POSIX wait(2) + waitpid(2) wrappers over
 * SYS_WAIT4 (#61, Linux x86_64).
 *
 * The kernel handles the blocking + reaping logic; this file is
 * just argument plumbing. EINTR is propagated as-is (errno=EINTR,
 * return -1) — let the caller decide whether to retry.
 */

#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

#include "syscall.h"

pid_t waitpid(pid_t pid, int *wstatus, int options) {
    long r = osnos_syscall4(SYS_WAIT4,
                             (long)pid,
                             (long)wstatus,
                             (long)options,
                             0 /* rusage — ignored */);
    if (r < 0) {
        errno = (int)(-r);
        return -1;
    }
    return (pid_t)r;
}

pid_t wait(int *wstatus) {
    return waitpid(-1, wstatus, 0);
}

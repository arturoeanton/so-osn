#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "syscall.h"

/*
 * Each wrapper drops a syscall straight to the kernel. The dispatcher
 * returns >= 0 on success, < 0 (== -errno) on failure. The caller
 * expects the libc convention: -1 + errno on failure, real value on
 * success.
 */

static long set_errno(long r) {
    if (r < 0) { errno = (int)(-r); return -1; }
    return r;
}

ssize_t read(int fd, void *buf, size_t n) {
    for (;;) {
        long r = osnos_syscall3(SYS_READ, fd, (long)buf, (long)n);
        if (r >= 0) return (ssize_t)r;
        if (-r != EAGAIN) { errno = (int)(-r); return -1; }
        /* Block: nanosleep yields to the scheduler so the keyboard
         * server can land bytes in the TTY, then we retry. 20 ms
         * matches what select/accept/recv use. */
        struct timespec ts = { 0, 20 * 1000000 };
        nanosleep(&ts, 0);
    }
}

ssize_t write(int fd, const void *buf, size_t n) {
    return (ssize_t)set_errno(
        osnos_syscall3(SYS_WRITE, fd, (long)buf, (long)n));
}

char *getcwd(char *buf, size_t size) {
    if (!buf || size == 0) { errno = EINVAL; return 0; }
    long r = osnos_syscall2(SYS_GETCWD, (long)buf, (long)size);
    if (r < 0) { errno = (int)(-r); return 0; }
    return buf;
}

int chdir(const char *path) {
    return (int)set_errno(osnos_syscall1(SYS_CHDIR, (long)path));
}

/*
 * Resolve a relative path against the per-task cwd into `out`. The
 * osnos kernel VFS only accepts absolute paths (returns EINVAL
 * otherwise), so every libc syscall that takes a path runs through
 * here first.
 *
 *   "/sd/foo"   -> "/sd/foo"               (unchanged)
 *   "foo"       -> "<cwd>/foo"             (relative)
 *
 * Falls back to $PWD when getcwd fails, and finally to "/" so the
 * original error path still surfaces a useful errno.
 */
static const char *resolve_path(const char *path,
                                  char *out, size_t out_size) {
    if (!path || path[0] == '/') return path;

    char cwd_buf[256];
    const char *base = 0;
    if (getcwd(cwd_buf, sizeof(cwd_buf))) {
        base = cwd_buf;
    } else {
        base = getenv("PWD");
    }
    if (!base || !*base) base = "/";

    size_t plen = 0; while (base[plen]) plen++;
    size_t flen = 0; while (path[flen]) flen++;
    int need_slash = (plen > 0 && base[plen - 1] != '/');
    if (plen + (need_slash ? 1 : 0) + flen + 1 > out_size) return path;
    size_t w = 0;
    for (size_t i = 0; i < plen; i++) out[w++] = base[i];
    if (need_slash) out[w++] = '/';
    for (size_t i = 0; i < flen; i++) out[w++] = path[i];
    out[w] = 0;
    return out;
}

int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    char abs_buf[256];
    const char *abs_path = resolve_path(path, abs_buf, sizeof(abs_buf));
    return (int)set_errno(
        osnos_syscall3(SYS_OPEN, (long)abs_path, flags, (long)mode));
}

int close(int fd) {
    return (int)set_errno(osnos_syscall1(SYS_CLOSE, fd));
}

off_t lseek(int fd, off_t off, int whence) {
    return (off_t)set_errno(
        osnos_syscall3(SYS_LSEEK, fd, (long)off, whence));
}

int isatty(int fd) {
    long r = osnos_syscall1(SYS_ISATTY, fd);
    if (r < 0) { errno = (int)(-r); return 0; }
    return (int)r;
}

int fstat(int fd, struct stat *out) {
    return (int)set_errno(
        osnos_syscall2(SYS_FSTAT, fd, (long)out));
}

int mkdir(const char *path, mode_t mode) {
    char abs_buf[256];
    const char *abs_path = resolve_path(path, abs_buf, sizeof(abs_buf));
    return (int)set_errno(
        osnos_syscall2(SYS_MKDIR, (long)abs_path, (long)mode));
}

int rmdir(const char *path) {
    char abs_buf[256];
    const char *abs_path = resolve_path(path, abs_buf, sizeof(abs_buf));
    return (int)set_errno(osnos_syscall1(SYS_RMDIR, (long)abs_path));
}

int unlink(const char *path) {
    char abs_buf[256];
    const char *abs_path = resolve_path(path, abs_buf, sizeof(abs_buf));
    return (int)set_errno(osnos_syscall1(SYS_UNLINK, (long)abs_path));
}

int rename(const char *oldpath, const char *newpath) {
    char old_buf[256], new_buf[256];
    const char *abs_old = resolve_path(oldpath, old_buf, sizeof(old_buf));
    const char *abs_new = resolve_path(newpath, new_buf, sizeof(new_buf));
    return (int)set_errno(
        osnos_syscall2(SYS_RENAME, (long)abs_old, (long)abs_new));
}

/*
 * brk / sbrk.
 *
 * brk(addr): asks the kernel to move the program break to `addr`.
 * The kernel returns the NEW break on success, the OLD break on
 * refusal. brk(0) is a query that returns the current break.
 *
 * sbrk(incr): convenience that wraps brk. Returns the OLD break on
 * success (so the caller has the start of the newly-allocated
 * region), or (void *)-1 on failure.
 */
int brk(void *addr) {
    long r = osnos_syscall1(SYS_BRK, (long)addr);
    if (r < 0) { errno = (int)(-r); return -1; }
    if ((unsigned long)r != (unsigned long)addr) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

void *sbrk(intptr_t increment) {
    long cur = osnos_syscall1(SYS_BRK, 0);
    if (cur < 0) { errno = (int)(-cur); return (void *)-1; }
    if (increment == 0) return (void *)cur;

    long want = cur + (long)increment;
    long got  = osnos_syscall1(SYS_BRK, want);
    if (got < 0) { errno = (int)(-got); return (void *)-1; }
    if (got != want) { errno = ENOMEM; return (void *)-1; }
    return (void *)cur;
}

__attribute__((noreturn))
void _exit(int code) {
    osnos_syscall1(SYS_EXIT, code);
    for (;;) __asm__ volatile ("hlt");   /* unreachable defensive */
}

/* ---------------------------------------------------------------- */
/* sleep / nanosleep                                                  */
/* ---------------------------------------------------------------- */

int nanosleep(const struct timespec *req, struct timespec *rem) {
    return (int)set_errno(
        osnos_syscall2(SYS_NANOSLEEP, (long)req, (long)rem));
}

unsigned int sleep(unsigned int seconds) {
    struct timespec req = { (time_t)seconds, 0 };
    if (nanosleep(&req, 0) < 0) return seconds;
    return 0;
}

int usleep(unsigned long usec) {
    struct timespec req = {
        (time_t)(usec / 1000000UL),
        (long)((usec % 1000000UL) * 1000UL)
    };
    return nanosleep(&req, 0);
}

int kill(pid_t pid, int sig) {
    return (int)set_errno(osnos_syscall2(SYS_KILL, (long)pid, (long)sig));
}

pid_t getpid(void) {
    return (pid_t)osnos_syscall0(SYS_GETPID);
}

/* ---- exec family ----
 *
 * Each variant ultimately routes to SYS_EXECVE. The kernel currently
 * returns -ENOSYS for that number (real "exec-in-place" needs a chunk
 * of work coupling with the shell's fg_pid tracking, postponed). The
 * libc surface still walks $PATH so user code is written against the
 * real POSIX contract from day one. */

#include <stdlib.h>   /* environ */

int execve(const char *path, char *const argv[], char *const envp[]) {
    long r = osnos_syscall3(SYS_EXECVE,
                              (long)path, (long)argv, (long)envp);
    errno = (int)(-r);
    return -1;
}

int execv(const char *path, char *const argv[]) {
    return execve(path, argv, environ);
}

int execvp(const char *file, char *const argv[]) {
    /* Slash in name → literal path, no PATH search (POSIX). */
    int has_slash = 0;
    for (const char *p = file; *p; p++) if (*p == '/') { has_slash = 1; break; }
    if (has_slash) return execve(file, argv, environ);

    const char *path = getenv("PATH");
    if (!path || !*path) path = "/bin";

    char attempt[256];
    int  last_errno = ENOENT;
    while (*path) {
        const char *colon = path;
        while (*colon && *colon != ':') colon++;
        size_t dlen = (size_t)(colon - path);

        if (dlen > 0 && dlen + 1 + 64 < sizeof(attempt)) {
            size_t w = 0;
            for (size_t i = 0; i < dlen; i++) attempt[w++] = path[i];
            if (w > 0 && attempt[w - 1] != '/') attempt[w++] = '/';
            for (size_t i = 0; file[i] && w + 1 < sizeof(attempt); i++) {
                attempt[w++] = file[i];
            }
            attempt[w] = 0;

            execve(attempt, argv, environ);
            if (errno != ENOENT) last_errno = errno;
        }
        path = (*colon == ':') ? colon + 1 : colon;
    }
    errno = last_errno;
    return -1;
}

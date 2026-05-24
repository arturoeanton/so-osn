#pragma once

#include <stddef.h>
#include <sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

ssize_t read (int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int     close(int fd);
off_t   lseek(int fd, off_t off, int whence);
int     isatty(int fd);

/* getopt(3) — POSIX command-line option parsing. */
extern char *optarg;
extern int   optind, opterr, optopt;
int getopt(int argc, char *const argv[], const char *optstring);

int     unlink(const char *path);
int     rmdir (const char *path);
int     mkdir (const char *path, mode_t mode);
int     rename(const char *oldpath, const char *newpath);

/* POSIX access(2) — checks the path resolves. `mode` is a bitmask of
 * the F_OK/R_OK/W_OK/X_OK constants below. osnos doesn't enforce
 * permission bits yet, so the only failure modes are ENOENT/EFAULT. */
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1
int     access(const char *path, int mode);

/*
 * dup / dup2 — return a new fd referring to the same open file.
 * Today the clone gets a copy of the source's flags/path but a
 * snapshot of offset (POSIX-strict shared offsets need an "open
 * file description" refcount that osnos doesn't have yet).
 */
int     dup (int fd);
int     dup2(int oldfd, int newfd);

/*
 * Linux pipe(2). Allocates a fresh kernel pipe + two task-local fds.
 * pipefd[0] = read end, pipefd[1] = write end. Returns 0 on success,
 * -1 on failure with errno set (EMFILE if the fd table is full,
 * ENFILE if the kernel pipe pool is empty, EFAULT for bad pointer).
 */
int     pipe(int pipefd[2]);

/*
 * Linux brk(addr): set the program break to addr; returns the new
 * break, or the OLD break if the request was refused. sbrk(incr)
 * is a libc convenience that adjusts brk by incr and returns the
 * OLD break (or (void*)-1 on failure).
 */
int    brk (void *addr);
void  *sbrk(intptr_t increment);

__attribute__((noreturn))
void   _exit(int code);

/*
 * sleep / usleep — wrap nanosleep. sleep returns the number of
 * seconds left unslept if interrupted (currently always 0 since
 * we never wake early). usleep returns 0 on success, -1 + errno on
 * error.
 */
unsigned int sleep (unsigned int seconds);
int          usleep(unsigned long usec);

/*
 * kill(pid, sig) — request the kernel to deliver a signal to `pid`.
 * Today osnos has no signal table: any non-zero `sig` simply marks
 * the target task with kill_pending; the kernel routes it through
 * exit(130) at the next return-to-user boundary. Returns 0 on
 * success or -1 + errno on failure (errno = ESRCH if pid doesn't
 * exist or is a kernel task).
 */
int          kill  (pid_t pid, int sig);

/* getpid — the calling task's pid. Always non-zero in user context. */
pid_t        getpid(void);

/*
 * fork(2) — clone the current process. Returns 0 in the new (child)
 * process, the child's pid in the parent, and -1 with errno set on
 * failure (e.g. ENOMEM, EMFILE). The child inherits memory image
 * (full copy, no COW yet), open fds (with pipe refcount bumps),
 * cwd, env, mmap regions, and stdin/stdout redirects.
 */
pid_t        fork(void);

/*
 * POSIX job control: process groups + sessions.
 *   getppid  → parent's pid (0 if orphan or kernel-spawned)
 *   getpgrp  → caller's process-group id
 *   getpgid  → pgid of `pid` (0 = self), or -1+ESRCH
 *   getsid   → sid  of `pid` (0 = self), or -1+ESRCH
 *   setpgid(pid, pgid) → 0 / -1+EPERM/ESRCH (pid=0 → self; pgid=0 → pid)
 *   setsid   → new sid (= caller's pid), or -1+EPERM if already a
 *              process-group leader
 *
 * Default after fork: child inherits parent's pgid + sid. Default
 * after a top-level spawn: pgid = sid = pid (own one-task group +
 * own session, like Linux for direct-spawned tasks).
 */
pid_t        getppid(void);
pid_t        getpgrp(void);
pid_t        getpgid(pid_t pid);
pid_t        getsid (pid_t pid);
int          setpgid(pid_t pid, pid_t pgid);
pid_t        setsid (void);

/*
 * POSIX cwd. getcwd writes the absolute path into `buf` (NUL
 * terminated) and returns `buf`, or NULL with errno on failure
 * (ERANGE if `size` too small). chdir adopts `path` as the new
 * cwd — must point at an existing directory, else ENOENT/ENOTDIR.
 * Per-task; children inherit the parent's cwd at exec via PWD.
 */
char *getcwd(char *buf, size_t size);
int   chdir (const char *path);

/*
 * exec family. POSIX semantics: replace the current process image with
 * the program at `path`. argv / envp are NULL-terminated arrays of
 * pointers. On success the call does not return.
 *
 *   execv (path, argv)            -> uses caller's environ
 *   execve(path, argv, envp)      -> explicit env
 *   execvp(file, argv)            -> walks $PATH if file has no '/',
 *                                    inherits environ
 *
 * Today the kernel side (SYS_EXECVE) is not yet wired: all three
 * propagate -ENOSYS until the syscall lands (real "exec-in-place"
 * needs careful coupling with the shell's fg_pid tracking). The libc
 * surface is here so user code can be written against the real POSIX
 * contract — it'll just fail at runtime for now.
 */
int execv (const char *path, char *const argv[]);
int execve(const char *path, char *const argv[], char *const envp[]);
int execvp(const char *file, char *const argv[]);

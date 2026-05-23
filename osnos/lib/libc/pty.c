/*
 * libc PTY surface — POSIX-style helpers for pseudo-terminals.
 *
 *   int  posix_openpt(int flags)    — open /dev/ptmx, get master fd
 *   int  grantpt    (int master_fd) — no-op (osnos doesn't enforce perms)
 *   int  unlockpt   (int master_fd) — sends TIOCSPTLCK ioctl (no-op kernel)
 *   int  ptsname_r  (int master_fd, char *buf, size_t buflen)
 *                                   — formats "/dev/pts/<N>" using TIOCGPTN
 *   char *ptsname   (int master_fd) — static-buffer variant (POSIX)
 *
 * Typical pattern (terminal multiplexer / ssh-like):
 *
 *   int m = posix_openpt(O_RDWR);
 *   grantpt(m); unlockpt(m);
 *   char name[64];
 *   ptsname_r(m, name, sizeof(name));
 *   if (fork() == 0) {
 *       int s = open(name, O_RDWR);
 *       dup2(s, 0); dup2(s, 1); dup2(s, 2);
 *       execlp("sh", "sh", NULL);
 *   }
 *   // parent: read/write master fd to talk to the child's terminal
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Linux ioctl numbers — must match TTY_TIOCGPTN / TTY_TIOCSPTLCK in
 * src/micro/syscall.c. */
#define TIOCGPTN   0x80045430u
#define TIOCSPTLCK 0x40045431u

int posix_openpt(int flags) {
    return open("/dev/ptmx", flags);
}

int grantpt(int fd) {
    /* osnos has no uid/gid model — nothing to grant. Return 0 so
     * upstream code that always calls grantpt() before opening the
     * slave doesn't choke. */
    (void)fd;
    return 0;
}

int unlockpt(int fd) {
    int unlock = 0;
    if (ioctl(fd, TIOCSPTLCK, &unlock) < 0) return -1;
    return 0;
}

int ptsname_r(int fd, char *buf, size_t buflen) {
    if (!buf || buflen < 12) { errno = ERANGE; return -1; }
    int n = -1;
    if (ioctl(fd, TIOCGPTN, &n) < 0) return -1;
    /* Format "/dev/pts/N" without depending on full snprintf
     * (some embedded environments stub it). 11-char max output for
     * N up to 99 — fits 12-byte buffer with NUL. */
    int wlen = snprintf(buf, buflen, "/dev/pts/%d", n);
    if (wlen < 0 || (size_t)wlen >= buflen) {
        errno = ERANGE;
        return -1;
    }
    return 0;
}

char *ptsname(int fd) {
    static char ptsname_buf[32];
    if (ptsname_r(fd, ptsname_buf, sizeof(ptsname_buf)) != 0) return 0;
    return ptsname_buf;
}

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>

#include "syscall.h"

/* ---------------------------------------------------------------- */
/* IPv4 string parsing                                               */
/* ---------------------------------------------------------------- */

/*
 * Parse one decimal octet (0..255). Reads up to 3 digits, fails if
 * the first byte isn't a digit. *cur_out advances past the octet on
 * success.
 */
static int parse_octet(const char *s, const char **cur_out, int *out) {
    if (*s < '0' || *s > '9') return 0;
    int v = 0, n = 0;
    while (*s >= '0' && *s <= '9' && n < 3) {
        v = v * 10 + (*s - '0');
        s++; n++;
    }
    if (v > 255) return 0;
    *out = v;
    *cur_out = s;
    return 1;
}

int inet_aton(const char *cp, struct in_addr *out) {
    if (!cp) return 0;
    int oct[4];
    const char *s = cp;
    for (int i = 0; i < 4; i++) {
        if (!parse_octet(s, &s, &oct[i])) return 0;
        if (i < 3) {
            if (*s != '.') return 0;
            s++;
        }
    }
    /* Reject trailing garbage. */
    if (*s != 0) return 0;

    /* Pack big-endian into the in_addr (which is in network order). */
    uint32_t v = ((uint32_t)oct[0] << 24) |
                 ((uint32_t)oct[1] << 16) |
                 ((uint32_t)oct[2] <<  8) |
                 ((uint32_t)oct[3]);
    if (out) out->s_addr = htonl(v);
    return 1;
}

in_addr_t inet_addr(const char *cp) {
    struct in_addr a;
    if (!inet_aton(cp, &a)) return INADDR_NONE;
    return a.s_addr;
}

char *inet_ntoa(struct in_addr a) {
    static char buf[INET_ADDRSTRLEN];
    uint32_t v = ntohl(a.s_addr);
    unsigned b0 = (v >> 24) & 0xff;
    unsigned b1 = (v >> 16) & 0xff;
    unsigned b2 = (v >>  8) & 0xff;
    unsigned b3 =  v        & 0xff;
    char *p = buf;
    /* manual itoa to avoid pulling stdio into the inet path */
    unsigned octs[4] = { b0, b1, b2, b3 };
    for (int i = 0; i < 4; i++) {
        unsigned o = octs[i];
        char tmp[4]; int n = 0;
        do { tmp[n++] = (char)('0' + o % 10); o /= 10; } while (o);
        while (n--) *p++ = tmp[n];
        if (i < 3) *p++ = '.';
    }
    *p = 0;
    return buf;
}

int inet_pton(int af, const char *src, void *dst) {
    if (af != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    return inet_aton(src, (struct in_addr *)dst);
}

const char *inet_ntop(int af, const void *src, char *dst,
                      unsigned int size) {
    if (af != AF_INET) {
        errno = EAFNOSUPPORT;
        return 0;
    }
    if (!src || !dst || size < INET_ADDRSTRLEN) {
        errno = ENOSPC;
        return 0;
    }
    struct in_addr a;
    memcpy(&a, src, sizeof(a));
    const char *s = inet_ntoa(a);
    size_t n = strlen(s);
    if (n + 1 > size) { errno = ENOSPC; return 0; }
    memcpy(dst, s, n + 1);
    return dst;
}

/* ---------------------------------------------------------------- */
/* Socket syscall stubs — return -1 with ENOSYS until FASE 8.5       */
/* ---------------------------------------------------------------- */

static int nosys_int(void)    { errno = ENOSYS; return -1; }
static ssize_t nosys_ssize(void) { errno = ENOSYS; return -1; }

int socket(int domain, int type, int protocol) {
    long r = osnos_syscall3(SYS_SOCKET, domain, type, protocol);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (int)r;
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t len) {
    long r = osnos_syscall3(SYS_BIND, sockfd, (long)addr, (long)len);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

int listen(int sockfd, int backlog) {
    long r = osnos_syscall2(SYS_LISTEN, sockfd, backlog);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

/* Sleep a short slice between non-blocking syscall retries. nanosleep
 * properly suspends the task (FASE 9.3 task_block + sched_resume_jump),
 * so other ready tasks — keyboard, shell, server ticks — get to run.
 * That's what makes Ctrl+C arrive while an app blocks on accept/recv. */
static void osnos_yield_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec  = (long)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000);
    nanosleep(&ts, NULL);
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *len) {
    for (;;) {
        long r = osnos_syscall3(SYS_ACCEPT, sockfd, (long)addr, (long)len);
        if (r >= 0) return (int)r;
        if (-r != EAGAIN) { errno = (int)(-r); return -1; }
        osnos_yield_ms(20);
    }
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t len) {
    for (;;) {
        long r = osnos_syscall3(SYS_CONNECT, sockfd, (long)addr, (long)len);
        if (r == 0) return 0;
        if (-r != EINPROGRESS) { errno = (int)(-r); return -1; }
        osnos_yield_ms(10);
    }
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    /* send rarely blocks (kernel send path is sync). No retry loop here. */
    long r = osnos_syscall6(SYS_SENDTO,
                              sockfd, (long)buf, (long)len, flags,
                              0L, 0L);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (ssize_t)r;
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    for (;;) {
        long r = osnos_syscall6(SYS_RECVFROM,
                                  sockfd, (long)buf, (long)len, flags,
                                  0L, 0L);
        if (r >= 0) return (ssize_t)r;
        if (-r != EAGAIN) { errno = (int)(-r); return -1; }
        osnos_yield_ms(20);
    }
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *to, socklen_t tolen) {
    long r = osnos_syscall6(SYS_SENDTO,
                              sockfd, (long)buf, (long)len, flags,
                              (long)to, (long)tolen);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (ssize_t)r;
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *from, socklen_t *fromlen) {
    for (;;) {
        long r = osnos_syscall6(SYS_RECVFROM,
                                  sockfd, (long)buf, (long)len, flags,
                                  (long)from, (long)fromlen);
        if (r >= 0) return (ssize_t)r;
        if (-r != EAGAIN) { errno = (int)(-r); return -1; }
        osnos_yield_ms(20);
    }
}

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags) {
    (void)sockfd; (void)msg; (void)flags;
    return nosys_ssize();
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags) {
    (void)sockfd; (void)msg; (void)flags;
    return nosys_ssize();
}

int shutdown(int sockfd, int how) {
    (void)sockfd; (void)how;
    return nosys_int();
}

int setsockopt(int sockfd, int level, int optname,
               const void *optval, socklen_t optlen) {
    long r = osnos_syscall5(SYS_SETSOCKOPT, sockfd, level, optname,
                              (long)optval, (long)optlen);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

int select(int nfds,
           fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout) {
    /*
     * The kernel select() is non-blocking — one pass, returns 0 if
     * nothing is ready. We loop in user space and call nanosleep()
     * between polls so the cooperative scheduler can dispatch other
     * tasks (servers, shell) in the gaps. Without this, a select that
     * blocks forever would also starve the keyboard server and the
     * user couldn't even Ctrl+C.
     */

    /* Snapshot input sets so we can reset before each kernel call —
     * the kernel mutates them in place. */
    fd_set r_in, w_in, e_in;
    if (readfds)   r_in = *readfds;
    if (writefds)  w_in = *writefds;
    if (exceptfds) e_in = *exceptfds;

    int      has_deadline = (timeout != NULL);
    uint64_t remaining_ms = 0;
    if (has_deadline) {
        remaining_ms = (uint64_t)timeout->tv_sec * 1000 +
                        (uint64_t)timeout->tv_usec / 1000;
    }
    const uint64_t step_ms = 20;

    for (;;) {
        if (readfds)   *readfds   = r_in;
        if (writefds)  *writefds  = w_in;
        if (exceptfds) *exceptfds = e_in;

        long r = osnos_syscall5(SYS_SELECT,
                                  nfds,
                                  (long)readfds, (long)writefds, (long)exceptfds,
                                  0);
        if (r < 0) { errno = (int)(-r); return -1; }
        if (r > 0) return (int)r;

        /* Nothing ready. Bail on deadline. */
        if (has_deadline && remaining_ms == 0) {
            if (readfds)   FD_ZERO(readfds);
            if (writefds)  FD_ZERO(writefds);
            if (exceptfds) FD_ZERO(exceptfds);
            return 0;
        }

        uint64_t sleep_ms = step_ms;
        if (has_deadline && remaining_ms < step_ms) sleep_ms = remaining_ms;

        struct timespec req;
        req.tv_sec  = (long)(sleep_ms / 1000);
        req.tv_nsec = (long)((sleep_ms % 1000) * 1000000);
        nanosleep(&req, NULL);

        if (has_deadline) remaining_ms -= sleep_ms;
    }
}

int getsockname(int sockfd, struct sockaddr *addr, socklen_t *len) {
    (void)sockfd; (void)addr; (void)len;
    return nosys_int();
}

int getpeername(int sockfd, struct sockaddr *addr, socklen_t *len) {
    (void)sockfd; (void)addr; (void)len;
    return nosys_int();
}

int getsockopt(int sockfd, int level, int optname, void *optval,
               socklen_t *optlen) {
    (void)sockfd; (void)level; (void)optname; (void)optval; (void)optlen;
    return nosys_int();
}

/* setsockopt is defined earlier with a real syscall wrapper. */

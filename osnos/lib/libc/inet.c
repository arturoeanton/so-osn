#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>

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
    (void)domain; (void)type; (void)protocol;
    return nosys_int();
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t len) {
    (void)sockfd; (void)addr; (void)len;
    return nosys_int();
}

int listen(int sockfd, int backlog) {
    (void)sockfd; (void)backlog;
    return nosys_int();
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *len) {
    (void)sockfd; (void)addr; (void)len;
    return nosys_int();
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t len) {
    (void)sockfd; (void)addr; (void)len;
    return nosys_int();
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    (void)sockfd; (void)buf; (void)len; (void)flags;
    return nosys_ssize();
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    (void)sockfd; (void)buf; (void)len; (void)flags;
    return nosys_ssize();
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *to, socklen_t tolen) {
    (void)sockfd; (void)buf; (void)len; (void)flags;
    (void)to; (void)tolen;
    return nosys_ssize();
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *from, socklen_t *fromlen) {
    (void)sockfd; (void)buf; (void)len; (void)flags;
    (void)from; (void)fromlen;
    return nosys_ssize();
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

int setsockopt(int sockfd, int level, int optname, const void *optval,
               socklen_t optlen) {
    (void)sockfd; (void)level; (void)optname; (void)optval; (void)optlen;
    return nosys_int();
}

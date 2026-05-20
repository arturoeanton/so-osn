#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/select.h>
#include <sys/types.h>

/*
 * sys/socket.h — POSIX socket interface skeleton.
 *
 * Networking is not implemented in the kernel today; every function
 * declared here returns -1 with errno = ENOSYS. The constants, types
 * and function signatures match Linux x86_64 exactly so that real
 * impls in FASE 8.5 (post-FAT) can be dropped in without touching
 * the callers.
 */

typedef unsigned int socklen_t;
typedef unsigned short int sa_family_t;
#ifndef __osnos_in_addr_defined
#define __osnos_in_addr_defined
typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;
#endif

/* Address families — Linux numeric values. */
#define AF_UNSPEC     0
#define AF_UNIX       1
#define AF_LOCAL      1
#define AF_INET       2
#define AF_INET6     10
#define AF_PACKET    17

#define PF_UNSPEC    AF_UNSPEC
#define PF_UNIX      AF_UNIX
#define PF_LOCAL     AF_LOCAL
#define PF_INET      AF_INET
#define PF_INET6     AF_INET6
#define PF_PACKET    AF_PACKET

/* Socket types */
#define SOCK_STREAM   1
#define SOCK_DGRAM    2
#define SOCK_RAW      3
#define SOCK_SEQPACKET 5
#define SOCK_CLOEXEC  02000000
#define SOCK_NONBLOCK 00004000

/* Protocols (entries used by AF_INET) */
#define IPPROTO_IP    0
#define IPPROTO_ICMP  1
#define IPPROTO_TCP   6
#define IPPROTO_UDP  17

/* Levels for getsockopt / setsockopt */
#define SOL_SOCKET   1

/* Socket options (Linux numbers) */
#define SO_DEBUG     1
#define SO_REUSEADDR 2
#define SO_TYPE      3
#define SO_ERROR     4
#define SO_DONTROUTE 5
#define SO_BROADCAST 6
#define SO_SNDBUF    7
#define SO_RCVBUF    8
#define SO_KEEPALIVE 9
#define SO_OOBINLINE 10
#define SO_LINGER    13
#define SO_REUSEPORT 15

/* Flags for send/recv */
#define MSG_OOB       0x00001
#define MSG_PEEK      0x00002
#define MSG_DONTROUTE 0x00004
#define MSG_DONTWAIT  0x00040
#define MSG_WAITALL   0x00100
#define MSG_NOSIGNAL  0x04000

/* shutdown(2) directions */
#define SHUT_RD     0
#define SHUT_WR     1
#define SHUT_RDWR   2

/*
 * struct sockaddr — generic socket address. AF-specific structs
 * (sockaddr_in, sockaddr_un, etc.) are layout-compatible by being
 * the same size or smaller; callers cast.
 */
struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

struct sockaddr_storage {
    sa_family_t ss_family;
    char        __ss_pad[126];
} __attribute__((aligned(8)));

/* iovec for sendmsg/recvmsg */
struct iovec {
    void  *iov_base;
    size_t iov_len;
};

struct msghdr {
    void          *msg_name;
    socklen_t      msg_namelen;
    struct iovec  *msg_iov;
    size_t         msg_iovlen;
    void          *msg_control;
    size_t         msg_controllen;
    int            msg_flags;
};

int     socket    (int domain, int type, int protocol);
int     bind      (int sockfd, const struct sockaddr *addr, socklen_t len);
int     listen    (int sockfd, int backlog);
int     accept    (int sockfd, struct sockaddr *addr, socklen_t *len);
int     connect   (int sockfd, const struct sockaddr *addr, socklen_t len);
ssize_t send      (int sockfd, const void *buf, size_t len, int flags);
ssize_t recv      (int sockfd, void *buf, size_t len, int flags);
ssize_t sendto    (int sockfd, const void *buf, size_t len, int flags,
                   const struct sockaddr *to, socklen_t tolen);
ssize_t recvfrom  (int sockfd, void *buf, size_t len, int flags,
                   struct sockaddr *from, socklen_t *fromlen);
ssize_t sendmsg   (int sockfd, const struct msghdr *msg, int flags);
ssize_t recvmsg   (int sockfd, struct msghdr *msg, int flags);
int     shutdown  (int sockfd, int how);
int     getsockname(int sockfd, struct sockaddr *addr, socklen_t *len);
int     getpeername(int sockfd, struct sockaddr *addr, socklen_t *len);
int     getsockopt (int sockfd, int level, int optname, void *optval,
                    socklen_t *optlen);
int     setsockopt (int sockfd, int level, int optname, const void *optval,
                    socklen_t optlen);

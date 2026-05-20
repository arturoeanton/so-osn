#pragma once

#include <stdint.h>
#include <sys/socket.h>

/*
 * netinet/in.h — IPv4 (and skeleton IPv6) address types.
 *
 * Layout matches Linux x86_64 exactly. sin_family is at the same
 * offset as sockaddr.sa_family, so casting between sockaddr and
 * sockaddr_in is well-defined.
 */

#ifndef __osnos_in_addr_defined
#define __osnos_in_addr_defined
typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;
#endif

/* All wire-order, network byte order (big-endian). */
#define INADDR_ANY        ((in_addr_t) 0x00000000)
#define INADDR_BROADCAST  ((in_addr_t) 0xffffffff)
#define INADDR_LOOPBACK   ((in_addr_t) 0x7f000001)   /* 127.0.0.1 */
#define INADDR_NONE       ((in_addr_t) 0xffffffff)

#define INET_ADDRSTRLEN   16
#define INET6_ADDRSTRLEN  46

struct in_addr {
    in_addr_t s_addr;          /* network byte order */
};

struct sockaddr_in {
    sa_family_t    sin_family;
    in_port_t      sin_port;   /* network byte order */
    struct in_addr sin_addr;
    unsigned char  sin_zero[8];
};

struct in6_addr {
    union {
        uint8_t  __u6_addr8[16];
        uint16_t __u6_addr16[8];
        uint32_t __u6_addr32[4];
    } __in6_u;
};
#define s6_addr     __in6_u.__u6_addr8
#define s6_addr16   __in6_u.__u6_addr16
#define s6_addr32   __in6_u.__u6_addr32

struct sockaddr_in6 {
    sa_family_t     sin6_family;
    in_port_t       sin6_port;
    uint32_t        sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t        sin6_scope_id;
};

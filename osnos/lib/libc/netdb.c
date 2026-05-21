#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "resolver.h"

/*
 * getaddrinfo / freeaddrinfo / gai_strerror.
 *
 * Scope of this implementation:
 *   - AF_UNSPEC and AF_INET resolve to one IPv4 result; AF_INET6
 *     is rejected (EAI_FAMILY).
 *   - node == NULL: build INADDR_ANY (with AI_PASSIVE) or
 *     INADDR_LOOPBACK (without).
 *   - node != NULL: parse as a dotted-quad via inet_aton. Hostnames
 *     return EAI_NONAME — we have no DNS resolver yet.
 *   - service: must be a decimal port (1..65535). /etc/services
 *     lookup not supported.
 *
 * Enough for Beej's selectserver / selectclient and any program that
 * uses getaddrinfo as a portable sockaddr builder.
 */

static int parse_port(const char *s, uint16_t *out) {
    if (!s) { *out = 0; return 0; }
    uint32_t v = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint32_t)(*s - '0');
        if (v > 65535) return -1;
        s++; digits++;
    }
    if (*s != 0)  return -1;
    if (digits == 0) return -1;
    *out = (uint16_t)v;
    return 0;
}

int getaddrinfo(const char *node, const char *service,
                 const struct addrinfo *hints, struct addrinfo **res) {
    if (!res) return EAI_NONAME;
    *res = NULL;

    int family   = (hints && hints->ai_family != 0)   ? hints->ai_family   : AF_INET;
    int socktype = (hints && hints->ai_socktype != 0) ? hints->ai_socktype : 0;
    int protocol = (hints) ? hints->ai_protocol : 0;
    int flags    = (hints) ? hints->ai_flags    : 0;

    /* Unspecified family → default to IPv4 since that's all we have. */
    if (family == 0 /* AF_UNSPEC */) family = AF_INET;
    if (family != AF_INET) return EAI_FAMILY;

    uint16_t port = 0;
    if (parse_port(service, &port) != 0) return EAI_SERVICE;

    uint32_t ip;
    if (node == NULL) {
        ip = (flags & AI_PASSIVE) ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);
    } else {
        struct in_addr a;
        if (inet_aton(node, &a)) {
            ip = a.s_addr;
        } else if (flags & AI_NUMERICHOST) {
            return EAI_NONAME;
        } else {
            /* Hostname → DNS query to slirp's emulated resolver. */
            if (dns_resolve_a(node, &ip) != 0) return EAI_NONAME;
        }
    }

    struct addrinfo    *ai = (struct addrinfo *)malloc(sizeof *ai);
    if (!ai) return EAI_MEMORY;
    struct sockaddr_in *sa = (struct sockaddr_in *)malloc(sizeof *sa);
    if (!sa) { free(ai); return EAI_MEMORY; }

    sa->sin_family = AF_INET;
    sa->sin_port   = htons(port);
    sa->sin_addr.s_addr = ip;
    for (int i = 0; i < 8; i++) ((char *)sa->sin_zero)[i] = 0;

    ai->ai_flags      = 0;
    ai->ai_family     = AF_INET;
    ai->ai_socktype   = socktype;
    ai->ai_protocol   = protocol;
    ai->ai_addrlen    = (socklen_t)sizeof(struct sockaddr_in);
    ai->ai_addr       = (struct sockaddr *)sa;
    ai->ai_canonname  = NULL;
    ai->ai_next       = NULL;

    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res) {
    while (res) {
        struct addrinfo *next = res->ai_next;
        if (res->ai_addr) free(res->ai_addr);
        free(res);
        res = next;
    }
}

const char *gai_strerror(int e) {
    switch (e) {
    case 0:           return "Success";
    case EAI_BADFLAGS:return "Invalid flags";
    case EAI_NONAME:  return "Name or service not known";
    case EAI_AGAIN:   return "Temporary failure in name resolution";
    case EAI_FAIL:    return "Non-recoverable failure";
    case EAI_FAMILY:  return "Address family not supported";
    case EAI_SOCKTYPE:return "Socket type not supported";
    case EAI_SERVICE: return "Service not supported";
    case EAI_MEMORY:  return "Out of memory";
    case EAI_SYSTEM:  return "System error";
    default:          return "Unknown getaddrinfo error";
    }
}

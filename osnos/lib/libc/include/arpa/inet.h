#pragma once

#include <netinet/in.h>
#include <endian.h>

/*
 * arpa/inet.h — string ↔ binary conversions for IPv4 addresses,
 * plus the classic htons/htonl/ntohs/ntohl byte-order helpers.
 *
 * All functions here are pure userland — no kernel involvement.
 * They work today even though socket(2) doesn't.
 */

/* Network byte order is big-endian. */
#define htons(x) htobe16(x)
#define htonl(x) htobe32(x)
#define ntohs(x) be16toh(x)
#define ntohl(x) be32toh(x)

/*
 * Legacy IPv4 string parsing.
 *
 *   inet_addr ("1.2.3.4")            -> 0x04030201 in net order
 *                                       or INADDR_NONE on parse failure
 *   inet_aton ("1.2.3.4", &out)      -> 1 on success, 0 on failure
 *   inet_ntoa (struct in_addr)       -> "1.2.3.4" in static buffer
 */
in_addr_t inet_addr(const char *cp);
int       inet_aton(const char *cp, struct in_addr *out);
char     *inet_ntoa(struct in_addr a);

/*
 * inet_pton / inet_ntop — POSIX, AF_INET (IPv4) and AF_INET6 (IPv6).
 * Today only AF_INET is implemented; AF_INET6 returns -1 / NULL with
 * EAFNOSUPPORT.
 *
 *   inet_pton(af, src, dst)  -> 1 on success, 0 on bad string,
 *                                -1 on bad af (errno = EAFNOSUPPORT)
 *   inet_ntop(af, src, dst, size) -> dst on success, NULL on error
 */
int   inet_pton(int af, const char *src, void *dst);
const char *inet_ntop(int af, const void *src, char *dst,
                      unsigned int size);

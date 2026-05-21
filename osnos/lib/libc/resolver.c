#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "resolver.h"

/*
 * Tiny DNS A-record resolver (RFC 1035).
 *
 * Just enough to back getaddrinfo. We always ask slirp's emulated
 * resolver at 10.0.2.3:53 — the host's DNS is recursive on the other
 * side, so we never need to walk the root servers ourselves. UDP only;
 * if the answer is larger than 512 bytes we give up (TC bit ignored).
 *
 * No /etc/resolv.conf, no /etc/hosts, no AAAA, no CNAME chasing (slirp
 * usually inlines the A record next to any CNAME). One query per call.
 */

#define DNS_SERVER_IP  ((10u << 24) | (2u << 8) | 3u)   /* 10.0.2.3 */
#define DNS_PORT       53
#define DNS_TIMEOUT_MS 3000

/* Encode "example.com" into DNS label form
 *   "\7example\3com\0"
 * Returns total bytes written including the trailing zero, or -1 on
 * malformed input (empty label, label >63, total >255). */
static int dns_encode_name(uint8_t *out, size_t out_size, const char *name) {
    size_t written = 0;
    const char *label = name;
    for (;;) {
        const char *p = label;
        while (*p && *p != '.') p++;
        size_t len = (size_t)(p - label);
        if (len == 0) {
            /* "" or ".." in the middle is malformed. Trailing dot OK. */
            if (*p == 0 && written > 0) break;
            return -1;
        }
        if (len > 63) return -1;
        if (written + 1 + len + 1 > out_size) return -1;
        out[written++] = (uint8_t)len;
        for (size_t i = 0; i < len; i++) out[written++] = (uint8_t)label[i];
        if (*p == 0) break;
        label = p + 1;
    }
    out[written++] = 0;
    return (int)written;
}

/* Skip a DNS name field starting at `off`. Handles compression
 * pointers (0xc0...) by collapsing them to a 2-byte advance. Returns
 * the new offset or -1 on malformed. */
static int dns_skip_name(const uint8_t *buf, int len, int off) {
    while (off < len) {
        uint8_t b = buf[off];
        if (b == 0) return off + 1;
        if ((b & 0xC0) == 0xC0) {
            if (off + 1 >= len) return -1;
            return off + 2;
        }
        if (b > 63) return -1;
        off += 1 + b;
    }
    return -1;
}

/*
 * Parse a DNS response packet. Returns 0 and fills *ip_be (network
 * byte order) on success with the first A record's IPv4. Returns -1
 * on any malformed structure, mismatched ID, error RCODE, or no A
 * record present.
 */
static int dns_parse_response(const uint8_t *buf, int len,
                                uint16_t expected_id, uint32_t *ip_be) {
    if (len < 12) return -1;

    uint16_t id     = (uint16_t)((buf[0] << 8) | buf[1]);
    uint16_t flags  = (uint16_t)((buf[2] << 8) | buf[3]);
    uint16_t qd     = (uint16_t)((buf[4] << 8) | buf[5]);
    uint16_t an     = (uint16_t)((buf[6] << 8) | buf[7]);

    if (id != expected_id)        return -1;
    if (!(flags & 0x8000))        return -1;     /* not a response */
    if ((flags & 0x000F) != 0)    return -1;     /* RCODE != 0 */

    int off = 12;
    for (int i = 0; i < qd; i++) {
        off = dns_skip_name(buf, len, off);
        if (off < 0 || off + 4 > len) return -1;
        off += 4;                                /* QTYPE + QCLASS */
    }

    for (int i = 0; i < an; i++) {
        off = dns_skip_name(buf, len, off);
        if (off < 0 || off + 10 > len) return -1;
        uint16_t type     = (uint16_t)((buf[off]   << 8) | buf[off + 1]);
        /* class at +2..+3, ttl at +4..+7 — ignored. */
        uint16_t rdlength = (uint16_t)((buf[off + 8] << 8) | buf[off + 9]);
        off += 10;
        if (off + rdlength > len) return -1;
        if (type == 1 && rdlength == 4) {
            /* Build network-byte-order word: first byte is the high
             * octet of the IPv4 address, so on x86 host order this
             * needs htonl(). */
            uint32_t host = ((uint32_t)buf[off]     << 24) |
                            ((uint32_t)buf[off + 1] << 16) |
                            ((uint32_t)buf[off + 2] <<  8) |
                            ((uint32_t)buf[off + 3]);
            *ip_be = htonl(host);
            return 0;
        }
        off += rdlength;
    }
    return -1;
}

static uint16_t dns_next_id(void) {
    /* Bumps per call; seed from getpid so different processes don't
     * collide on the wire even if they fire back-to-back. */
    static uint16_t counter;
    if (counter == 0) counter = (uint16_t)(getpid() | 1);
    return counter++;
}

int dns_resolve_a(const char *hostname, uint32_t *ip_be) {
    if (!hostname || !ip_be) return -1;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    /* Build query: 12-byte header + QNAME + QTYPE(A=1) + QCLASS(IN=1). */
    uint8_t query[300];
    uint16_t id = dns_next_id();
    query[0] = (uint8_t)(id >> 8);
    query[1] = (uint8_t)id;
    query[2] = 0x01;        /* flags: standard query, RD=1 */
    query[3] = 0x00;
    query[4] = 0; query[5] = 1;     /* QDCOUNT = 1 */
    query[6] = 0; query[7] = 0;
    query[8] = 0; query[9] = 0;
    query[10] = 0; query[11] = 0;

    int nlen = dns_encode_name(query + 12, sizeof(query) - 12 - 4, hostname);
    if (nlen < 0) { close(fd); return -1; }
    int qoff = 12 + nlen;
    query[qoff++] = 0; query[qoff++] = 1;   /* QTYPE = A   */
    query[qoff++] = 0; query[qoff++] = 1;   /* QCLASS = IN */

    struct sockaddr_in dns_addr;
    dns_addr.sin_family = AF_INET;
    dns_addr.sin_port   = htons(DNS_PORT);
    dns_addr.sin_addr.s_addr = htonl(DNS_SERVER_IP);
    for (int i = 0; i < 8; i++) ((char *)dns_addr.sin_zero)[i] = 0;

    if (sendto(fd, query, (size_t)qoff, 0,
                (struct sockaddr *)&dns_addr, sizeof(dns_addr)) < 0) {
        close(fd);
        return -1;
    }

    /* Wait for the reply with a 3-second timeout. recvfrom blocks on
     * its own forever; we use select to bound the wait. */
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv;
    tv.tv_sec  = DNS_TIMEOUT_MS / 1000;
    tv.tv_usec = (DNS_TIMEOUT_MS % 1000) * 1000;
    int r = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (r <= 0) { close(fd); return -1; }

    uint8_t resp[512];
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    ssize_t n = recvfrom(fd, resp, sizeof(resp), 0,
                          (struct sockaddr *)&from, &flen);
    close(fd);
    if (n < 12) return -1;

    return dns_parse_response(resp, (int)n, id, ip_be);
}

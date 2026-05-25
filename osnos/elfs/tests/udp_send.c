/*
 * udp_send — diagnose UDP outbound. Send a DNS-ish packet to 10.0.2.3:53
 * (the slirp DNS server) and wait for a reply.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

int main(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { printf("socket failed errno=%d\n", errno); return 1; }

    /* Bind a local port so kernel knows where to deliver replies. */
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port   = htons(0);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        printf("bind failed errno=%d\n", errno);
        return 1;
    }

    /* Build a minimal DNS query for "google.com" type A. */
    unsigned char query[] = {
        0x12, 0x34, /* id */
        0x01, 0x00, /* flags: standard query, RD=1 */
        0x00, 0x01, /* qdcount=1 */
        0x00, 0x00, /* ancount=0 */
        0x00, 0x00, /* nscount=0 */
        0x00, 0x00, /* arcount=0 */
        /* QNAME: google.com */
        6, 'g','o','o','g','l','e',
        3, 'c','o','m',
        0,
        /* QTYPE=A=1 */
        0x00, 0x01,
        /* QCLASS=IN=1 */
        0x00, 0x01
    };

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(53);
    dst.sin_addr.s_addr = htonl(0x0A000203);   /* 10.0.2.3 */

    printf("sending DNS query to 10.0.2.3:53 ...\n");
    ssize_t n = sendto(fd, query, sizeof(query), 0,
                      (struct sockaddr *)&dst, sizeof(dst));
    if (n < 0) { printf("sendto failed errno=%d\n", errno); return 1; }
    printf("sent %d bytes\n", (int)n);

    /* Wait up to 5 sec for reply via polling read. */
    char reply[512];
    struct sockaddr_in from;
    socklen_t fromlen;
    for (int i = 0; i < 50; i++) {
        fromlen = sizeof(from);
        n = recvfrom(fd, reply, sizeof(reply), 0,
                    (struct sockaddr *)&from, &fromlen);
        if (n > 0) {
            printf("got %d bytes from %d.%d.%d.%d:%d\n", (int)n,
                   (int)(from.sin_addr.s_addr & 0xff),
                   (int)((from.sin_addr.s_addr >> 8) & 0xff),
                   (int)((from.sin_addr.s_addr >> 16) & 0xff),
                   (int)((from.sin_addr.s_addr >> 24) & 0xff),
                   ntohs(from.sin_port));
            for (int k = 0; k < (int)n && k < 32; k++) {
                printf("%02x ", (unsigned char)reply[k]);
            }
            printf("\n");
            close(fd);
            return 0;
        }
        if (n < 0 && errno != EAGAIN) {
            printf("recvfrom errno=%d\n", errno);
            close(fd);
            return 1;
        }
        usleep(100000);
    }
    printf("TIMEOUT waiting for reply (5s)\n");
    close(fd);
    return 1;
}

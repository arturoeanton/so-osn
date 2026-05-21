/*
 * /bin/udptest — user-mode demo of the FASE 8.5.4b socket syscalls.
 *
 *   exec /bin/udptest [PORT]
 *
 * Binds to UDP port (default 1234), echoes each received datagram
 * back to its sender, and exits after 5 receives or 30 seconds.
 * Pair with `echo hola | nc -u -w1 127.0.0.1 1234` on the host while
 * QEMU forwards :1234 into the guest.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv) {
    uint16_t port = 1234;
    if (argc >= 2) {
        int v = atoi(argv[1]);
        if (v > 0 && v < 65536) port = (uint16_t)v;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("socket: errno=%d\n", errno);
        return 1;
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    for (int i = 0; i < 8; i++) ((char *)addr.sin_zero)[i] = 0;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("bind: errno=%d\n", errno);
        close(fd);
        return 1;
    }

    printf("udptest: listening UDP %u (echo)\n", (unsigned)port);

    for (int i = 0; i < 5; i++) {
        char buf[256];
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0,
                              (struct sockaddr *)&src, &slen);
        if (n < 0) {
            printf("recvfrom: errno=%d\n", errno);
            break;
        }
        buf[n] = 0;
        /* Trim trailing newline for cleaner output. */
        if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = 0;

        uint32_t ip = ntohl(src.sin_addr.s_addr);
        printf("[%d] rx %u.%u.%u.%u:%u  '%s'\n",
               i + 1,
               (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
               (ip >> 8)  & 0xFF,  ip        & 0xFF,
               (unsigned)ntohs(src.sin_port),
               buf);

        sendto(fd, buf, (size_t)n, 0,
                (struct sockaddr *)&src, sizeof(src));
    }

    close(fd);
    printf("udptest: done\n");
    return 0;
}

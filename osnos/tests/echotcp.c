/*
 * /bin/echotcp — ring-3 demo of FASE 8.5.5c listen/accept.
 *
 *   exec /bin/echotcp [PORT]
 *
 * Binds to TCP port (default 80; QEMU forwards host:8080 → guest:80),
 * accepts up to 5 connections in a row, reads one chunk per connection
 * and echoes it back. Each connection ends with an orderly close.
 *
 * Pair with `echo hola | nc -v -w2 127.0.0.1 8080` on the host.
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
    uint16_t port = 80;
    if (argc >= 2) {
        int v = atoi(argv[1]);
        if (v > 0 && v < 65536) port = (uint16_t)v;
    }

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { printf("socket: errno=%d\n", errno); return 1; }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    for (int i = 0; i < 8; i++) ((char *)addr.sin_zero)[i] = 0;

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("bind: errno=%d\n", errno);
        close(srv);
        return 1;
    }
    if (listen(srv, 4) < 0) {
        printf("listen: errno=%d\n", errno);
        close(srv);
        return 1;
    }

    printf("echotcp: listening TCP %u (accept loop x5)\n", (unsigned)port);

    for (int conn = 0; conn < 5; conn++) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int cli = accept(srv, (struct sockaddr *)&peer, &plen);
        if (cli < 0) {
            printf("[%d] accept: errno=%d\n", conn + 1, errno);
            break;
        }

        uint32_t ip = ntohl(peer.sin_addr.s_addr);
        printf("[%d] conn from %u.%u.%u.%u:%u\n",
               conn + 1,
               (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
               (ip >> 8)  & 0xFF,  ip        & 0xFF,
               (unsigned)ntohs(peer.sin_port));

        char buf[256];
        ssize_t n = recv(cli, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = 0;
            if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = 0;
            printf("    rx %zd: '%s'\n", n, buf);
            send(cli, buf, (size_t)n, 0);
        } else if (n == 0) {
            printf("    peer closed without data\n");
        } else {
            printf("    recv errno=%d\n", errno);
        }
        close(cli);
    }

    close(srv);
    printf("echotcp: done\n");
    return 0;
}

/*
 * /bin/selecttest — FASE 8.5.6 demo of select(2).
 *
 *   exec /bin/selecttest [PORT]
 *
 * Multiplexes between two file descriptors:
 *   - stdin   (any keystroke ends the loop)
 *   - listen  (TCP accept → recv one chunk → echo back → close)
 *
 * Demonstrates that the kernel select() correctly wakes on either
 * source becoming readable, without a busy-poll in user space.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define STDIN_FILENO 0

int main(int argc, char **argv) {
    uint16_t port = 80;
    if (argc >= 2) {
        int v = atoi(argv[1]);
        if (v > 0 && v < 65536) port = (uint16_t)v;
    }

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { printf("socket: errno=%d\n", errno); return 1; }

    /* Hello, Beej's selectserver.c. */
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    for (int i = 0; i < 8; i++) ((char *)addr.sin_zero)[i] = 0;

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("bind: errno=%d\n", errno);
        return 1;
    }
    if (listen(srv, 4) < 0) {
        printf("listen: errno=%d\n", errno);
        return 1;
    }

    printf("selecttest: TCP %u + stdin via select(); press any key to exit\n",
           (unsigned)port);

    int max_fd = srv > STDIN_FILENO ? srv : STDIN_FILENO;

    for (int i = 0; i < 20; i++) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv, &rfds);
        FD_SET(STDIN_FILENO, &rfds);

        struct timeval tv;
        tv.tv_sec  = 30;
        tv.tv_usec = 0;

        int n = select(max_fd + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) { printf("select: errno=%d\n", errno); break; }
        if (n == 0) { printf("idle 30s, exiting\n"); break; }

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            char buf[16];
            read(STDIN_FILENO, buf, sizeof(buf));
            printf("stdin event → exit\n");
            break;
        }

        if (FD_ISSET(srv, &rfds)) {
            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            int cli = accept(srv, (struct sockaddr *)&peer, &plen);
            if (cli < 0) continue;

            uint32_t ip = ntohl(peer.sin_addr.s_addr);
            char rxbuf[256];
            ssize_t r = recv(cli, rxbuf, sizeof(rxbuf) - 1, 0);
            if (r > 0) {
                rxbuf[r] = 0;
                if (r > 0 && rxbuf[r - 1] == '\n') rxbuf[r - 1] = 0;
                printf("[%d] %u.%u.%u.%u: '%s'\n", i,
                       (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                       (ip >> 8)  & 0xFF,  ip        & 0xFF,
                       rxbuf);
                send(cli, rxbuf, (size_t)r, 0);
            }
            close(cli);
        }
    }

    close(srv);
    printf("selecttest: done\n");
    return 0;
}

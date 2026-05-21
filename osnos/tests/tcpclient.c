/*
 * /bin/tcpclient — outbound TCP demo.
 *
 *   exec /bin/tcpclient HOST PORT
 *
 * HOST is a dotted-quad IPv4 address (no DNS yet; 8.5.9 will add that).
 * Opens a socket, connects, sends a one-line greeting, prints whatever
 * the peer responds with, and closes.
 *
 * Pair with `nc -l 9050` on the Mac:
 *
 *   Mac:     nc -l 9050
 *   OSnOS:   exec /bin/tcpclient 10.0.2.2 9050
 *
 * 10.0.2.2 is the slirp gateway, which routes back to the host.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        printf("usage: tcpclient HOST PORT\n");
        return 1;
    }

    /* Resolve host: IP literal OR hostname via DNS (slirp resolver). */
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int gr = getaddrinfo(argv[1], argv[2], &hints, &res);
    if (gr != 0 || res == NULL) {
        printf("getaddrinfo(%s): %s\n", argv[1], gai_strerror(gr));
        return 1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { printf("socket: errno=%d\n", errno); freeaddrinfo(res); return 1; }

    struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
    uint32_t ip = ntohl(sa->sin_addr.s_addr);
    printf("connecting to %u.%u.%u.%u:%u (resolved from %s) ...\n",
            (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
            (ip >> 8)  & 0xFF,  ip        & 0xFF,
            (unsigned)ntohs(sa->sin_port), argv[1]);

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        printf("connect: errno=%d\n", errno);
        close(fd);
        freeaddrinfo(res);
        return 1;
    }
    uint16_t port_h = ntohs(sa->sin_port);
    freeaddrinfo(res);
    printf("connected!\n");

    /* Talking to port 80? Send a real HTTP/1.0 GET so the server
     * actually replies. Otherwise stick to the friendly greeting. */
    char msg[256];
    int  mlen;
    if (port_h == 80) {
        mlen = snprintf(msg, sizeof(msg),
                         "GET / HTTP/1.0\r\nHost: %s\r\n"
                         "User-Agent: osnos/0.0\r\nConnection: close\r\n\r\n",
                         argv[1]);
    } else {
        mlen = snprintf(msg, sizeof(msg), "hello from osnos\n");
    }
    ssize_t n = send(fd, msg, (size_t)mlen, 0);
    printf("sent %zd bytes\n", n);

    /* Bounded recv: cap the wait at 5 seconds via select() so we don't
     * hang forever on a peer that has nothing to say. */
    fd_set rfds;
    int total = 0;
    char buf[1024];
    for (;;) {
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = { 5, 0 };
        int sr = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (sr <= 0) {
            printf("(no more data within 5s)\n");
            break;
        }
        n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            if (n == 0) printf("(peer closed, %d bytes total)\n", total);
            else        printf("recv: errno=%d\n", errno);
            break;
        }
        buf[n] = 0;
        printf("%s", buf);
        total += (int)n;
        if (total > 4096) {
            printf("\n(truncating after 4KB)\n");
            break;
        }
    }

    close(fd);
    return 0;
}

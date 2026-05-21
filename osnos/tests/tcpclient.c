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

    struct in_addr ip;
    if (!inet_aton(argv[1], &ip)) {
        printf("bad host: %s\n", argv[1]);
        return 1;
    }
    int port_i = atoi(argv[2]);
    if (port_i <= 0 || port_i > 65535) {
        printf("bad port: %s\n", argv[2]);
        return 1;
    }
    uint16_t port = (uint16_t)port_i;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { printf("socket: errno=%d\n", errno); return 1; }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr   = ip;
    for (int i = 0; i < 8; i++) ((char *)addr.sin_zero)[i] = 0;

    printf("connecting to %s:%u ...\n", argv[1], (unsigned)port);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("connect: errno=%d\n", errno);
        close(fd);
        return 1;
    }
    printf("connected!\n");

    const char *msg = "hello from osnos\n";
    ssize_t n = send(fd, msg, strlen(msg), 0);
    printf("sent %zd bytes\n", n);

    char buf[512];
    n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
        buf[n] = 0;
        printf("got %zd bytes: %s", n, buf);
        if (buf[n - 1] != '\n') printf("\n");
    } else if (n == 0) {
        printf("peer closed without data\n");
    } else {
        printf("recv: errno=%d\n", errno);
    }

    close(fd);
    return 0;
}

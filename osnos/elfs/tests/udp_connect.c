/*
 * udp_connect — repro de nslookup: socket+bind+connect+write+poll+read.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

int main(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    printf("socket fd=%d errno=%d\n", fd, errno);
    if (fd < 0) return 1;

    /* Bind to local ephemeral port. */
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port   = htons(0);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    int r = bind(fd, (struct sockaddr *)&local, sizeof(local));
    printf("bind r=%d errno=%d\n", r, errno);
    if (r < 0) return 1;

    /* Connect to DNS. */
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(53);
    dst.sin_addr.s_addr = htonl(0x0A000203);
    r = connect(fd, (struct sockaddr *)&dst, sizeof(dst));
    printf("connect r=%d errno=%d\n", r, errno);
    if (r < 0) return 1;

    /* Set non-blocking. */
    int fl = fcntl(fd, F_GETFL, 0);
    r = fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    printf("fcntl set NONBLOCK r=%d errno=%d (fl was %d)\n", r, errno, fl);

    /* write() — should go to connected peer. */
    unsigned char query[] = {
        0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        6,'g','o','o','g','l','e', 3,'c','o','m', 0,
        0x00, 0x01, 0x00, 0x01
    };
    ssize_t w = write(fd, query, sizeof(query));
    printf("write w=%d errno=%d\n", (int)w, errno);

    /* Poll for response. */
    for (int i = 0; i < 50; i++) {
        char reply[512];
        ssize_t n = read(fd, reply, sizeof(reply));
        if (n > 0) {
            printf("read got %d bytes\n", (int)n);
            close(fd);
            return 0;
        }
        if (n < 0 && errno != EAGAIN) {
            printf("read errno=%d\n", errno);
            close(fd);
            return 1;
        }
        usleep(100000);
    }
    printf("TIMEOUT\n");
    close(fd);
    return 1;
}

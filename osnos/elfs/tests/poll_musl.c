/* Test directo de poll() linkeado contra musl. */
#include <poll.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

int main(void) {
    struct pollfd pfd = { .fd = 0, .events = POLLIN };
    printf("calling poll(stdin, 1, 100)...\n");
    int r = poll(&pfd, 1, 100);
    printf("poll returned %d errno=%d (revents=0x%x)\n",
           r, errno, pfd.revents);
    return 0;
}

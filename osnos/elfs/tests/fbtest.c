/*
 * tests/fbtest.c — write directly to /dev/fb0 (FASE 10.0.c).
 *
 * Bypasses the console_server IPC path; the bytes hit the framebuffer
 * driver via the devfs backend. Used to prove that a ring-3 task
 * (eventually the migrated console_server) can drive the display
 * through an fd instead of needing kernel internals.
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int fd = open("/dev/fb0", O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "fbtest: cannot open /dev/fb0\n");
        return 1;
    }

    const char *msg = "fbtest: hola desde /dev/fb0 \xe2\x9c\x93\n";
    long n = write(fd, msg, strlen(msg));
    if (n < 0) {
        fprintf(stderr, "fbtest: write failed\n");
        close(fd);
        return 1;
    }

    close(fd);
    printf("fbtest: wrote %ld bytes to /dev/fb0\n", n);
    return 0;
}

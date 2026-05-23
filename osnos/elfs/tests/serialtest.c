/*
 * /bin/serialtest — FASE 10.7 smoke test for /dev/ttyS0 + /dev/tty.
 *
 * Checks (5):
 *   1. open("/dev/ttyS0", O_WRONLY) succeeds.
 *   2. write of N bytes returns N (UART TX always accepts via spin).
 *   3. close of the ttyS0 fd succeeds.
 *   4. open("/dev/tty", O_RDWR) succeeds and returns is_special OFD.
 *   5. tcgetattr(tty_fd) returns 0 — proves the fd routes through
 *      the kernel TTY ioctl path (not the devfs char dispatch).
 *
 * We don't try to verify the receive side without a loopback —
 * QEMU's `-serial mon:stdio` is unidirectional w.r.t. host stdin
 * unless someone is typing. Read tests live in manual showcase
 * (`build_and_run.sh headless` + type into the prompt).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static int fails = 0;

#define CHECK(cond, msg)                                           \
    do {                                                            \
        if (cond) {                                                 \
            printf("  PASS %s\n", msg);                             \
        } else {                                                    \
            printf("  FAIL %s (errno=%d)\n", msg, errno);           \
            fails++;                                                \
        }                                                           \
    } while (0)

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("== serialtest: /dev/ttyS0 + /dev/tty smoke ==\n");

    /* 1+2+3. /dev/ttyS0 raw write. */
    int s = open("/dev/ttyS0", O_WRONLY);
    CHECK(s >= 0, "open /dev/ttyS0 O_WRONLY");
    if (s >= 0) {
        const char *msg = "[serialtest] hello via /dev/ttyS0\n";
        ssize_t w = write(s, msg, strlen(msg));
        CHECK(w == (ssize_t)strlen(msg), "write to /dev/ttyS0");
        CHECK(close(s) == 0, "close /dev/ttyS0");
    }

    /* 4+5. /dev/tty is the controlling terminal, with full ioctl
     * dispatch (same path as fd 0 default). */
    int t = open("/dev/tty", O_RDWR);
    CHECK(t >= 0, "open /dev/tty O_RDWR");
    if (t >= 0) {
        struct termios tio;
        int r = tcgetattr(t, &tio);
        CHECK(r == 0, "tcgetattr(/dev/tty)");
        /* Don't close — sys_close refuses on fds <3 with is_special,
         * but our fd here is >=3 so close should work. */
        close(t);
    }

    printf("serialtest: %d fail(s)\n", fails);
    return fails ? 1 : 0;
}

/*
 * ptytest — pseudo-terminal pairs sanity test.
 *
 * Coverage:
 *   1. posix_openpt → master fd >= 3.
 *   2. ptsname returns "/dev/pts/N".
 *   3. open(/dev/pts/N) → slave fd >= 3.
 *   4. master write → slave read (canonical mode: needs newline).
 *   5. slave write → master read (raw, byte-by-byte).
 *   6. tcsetattr to RAW mode (ICANON off) → slave reads each byte
 *      without waiting for newline.
 *   7. Multiple pairs allocated simultaneously have distinct indices.
 *   8. Close master → slave eventually sees EOF on subsequent read.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int total = 0;
static int fails = 0;
#define CHECK(c,n) do { total++; if (c) printf("PASS %s\n", n); else { printf("FAIL %s\n", n); fails++; } } while (0)

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("ptytest: PTY pairs (canonical + raw)\n");

    /* 1. Open master. */
    int m = posix_openpt(O_RDWR);
    CHECK(m >= 3, "posix_openpt master fd");

    /* 2. ptsname → "/dev/pts/N". */
    char name[32];
    int r = ptsname_r(m, name, sizeof(name));
    CHECK(r == 0, "ptsname_r success");

    /* Verify path starts with "/dev/pts/" and ends with a digit. */
    int ok = (strncmp(name, "/dev/pts/", 9) == 0) &&
             name[9] >= '0' && name[9] <= '9';
    CHECK(ok, "ptsname format /dev/pts/N");

    /* 3. unlockpt + open slave. */
    CHECK(unlockpt(m) == 0, "unlockpt");
    int s = open(name, O_RDWR);
    CHECK(s >= 3, "open slave fd");

    /* 3b. Disable ECHO right away so master writes don't accumulate
     *     in the slave→master ring buffer. This test isn't trying
     *     to verify echo (a separate test would); it just wants
     *     master↔slave data transfer in both directions. */
    struct termios t0;
    tcgetattr(s, &t0);
    t0.c_lflag &= ~ECHO;
    tcsetattr(s, TCSANOW, &t0);

    /* 4. Canonical mode (still): master writes "hola\n", slave
     *    reads "hola\n". The '\n' triggers line commit. */
    const char *msg = "hola\n";
    int w = write(m, msg, 5);
    CHECK(w == 5, "master write 5 bytes");

    /* Give scheduler a tick for the line to land. */
    struct timespec ts = { 0, 5 * 1000000 };
    nanosleep(&ts, 0);

    char buf[16] = {0};
    int n = read(s, buf, sizeof(buf) - 1);
    CHECK(n == 5, "slave read 5 bytes");
    CHECK(memcmp(buf, "hola\n", 5) == 0, "slave got correct bytes");

    /* 5. Slave writes "hi", master reads "hi". */
    w = write(s, "hi", 2);
    CHECK(w == 2, "slave write 2 bytes");
    nanosleep(&ts, 0);

    memset(buf, 0, sizeof(buf));
    n = read(m, buf, sizeof(buf) - 1);
    CHECK(n == 2 && memcmp(buf, "hi", 2) == 0,
          "master read slave's bytes");

    /* 6. Switch to RAW mode (ICANON off, ECHO off). Slave reads each
     *    byte without waiting for newline. */
    struct termios t;
    tcgetattr(s, &t);
    t.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(s, TCSANOW, &t);

    write(m, "X", 1);
    nanosleep(&ts, 0);
    memset(buf, 0, sizeof(buf));
    n = read(s, buf, sizeof(buf) - 1);
    CHECK(n == 1 && buf[0] == 'X',
          "raw mode: slave read 1 byte without newline");

    /* 7. Open second pair — distinct index. */
    int m2 = posix_openpt(O_RDWR);
    CHECK(m2 >= 3 && m2 != m, "second posix_openpt distinct fd");
    char name2[32];
    ptsname_r(m2, name2, sizeof(name2));
    CHECK(strcmp(name, name2) != 0, "second pts has distinct path");

    /* 8. Close master, slave should see EOF. */
    close(m);
    /* The pty layer marks master_refs=0; a slave_read on an empty
     * m2s_buf with no master returns 0 (EOF). */
    nanosleep(&ts, 0);
    n = read(s, buf, sizeof(buf));
    CHECK(n == 0, "slave reads EOF after master close");

    /* Cleanup */
    close(s);
    close(m2);

    printf("\nptytest: total=%d pass=%d fail=%d\n",
           total, total - fails, fails);
    return fails == 0 ? 0 : 1;
}

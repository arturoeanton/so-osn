/*
 * tests/pipetest.c — exercise the sys_pipe(2) syscall + per-task fd
 * tables (FASE 10.0.b). Verifies:
 *   - pipe() returns 0 and populates fd[0]/fd[1] with usable fds.
 *   - write to fd[1] is visible to read from fd[0].
 *   - closing the write end produces EOF (read returns 0).
 *   - reading from the write end / writing to the read end errors.
 *
 * No fork() yet, so this is a single-task self-loopback — enough to
 * prove the fd-based pipe plumbing works end-to-end.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fails = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("PASS %s\n", name); } \
    else      { printf("FAIL %s\n", name); fails++; } \
} while (0)

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int p[2];
    int r = pipe(p);
    CHECK(r == 0, "pipe(): returns 0");
    CHECK(p[0] >= 3 && p[1] >= 3 && p[0] != p[1],
          "pipe(): allocates two distinct fds >= 3");

    /* Round-trip. */
    const char *msg = "ping";
    long n_w = write(p[1], msg, 4);
    CHECK(n_w == 4, "write(p[1], 4): all bytes accepted");

    char buf[8];
    memset(buf, 0, sizeof(buf));
    long n_r = read(p[0], buf, 4);
    CHECK(n_r == 4, "read(p[0], 4): all bytes returned");
    CHECK(memcmp(buf, "ping", 4) == 0, "read(): payload matches");

    /* Writer-side close → reader sees EOF. */
    CHECK(close(p[1]) == 0, "close(p[1]): writer closed");
    long n_eof = read(p[0], buf, 4);
    CHECK(n_eof == 0, "read(p[0]) after writer close: EOF");

    /* Read end is read-only; write end is write-only. */
    long bad_r = read(p[1], buf, 4);
    CHECK(bad_r < 0 && errno == EBADF,
          "read(p[1]): EBADF (write end)");

    /* Re-open a fresh pipe for the inverse direction test. */
    int q[2];
    CHECK(pipe(q) == 0, "pipe(): second alloc OK");
    long bad_w = write(q[0], msg, 4);
    CHECK(bad_w < 0 && errno == EBADF,
          "write(q[0]): EBADF (read end)");
    close(q[0]);
    close(q[1]);

    close(p[0]);

    if (fails == 0) {
        printf("\npipetest: ALL PASS\n");
        return 0;
    }
    printf("\npipetest: %d FAIL\n", fails);
    return 1;
}

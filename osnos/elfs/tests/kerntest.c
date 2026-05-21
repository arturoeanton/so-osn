/*
 * tests/kerntest.c — userland kernel-ABI test ELF (FASE 10.0.e).
 *
 * Replaces the parts of the in-shell `cmd_test` that can be expressed
 * via ordinary syscalls. The shell's kernel-internal checks (kheap
 * counters, fd_get pointer inspection, sock_table internals) stay in
 * shell_server.c under cmd_test for now — they need direct kernel
 * access that ring-3 won't have once the shell moves to ring 3.
 *
 * Goal: exercise the ABI surface that ring-3 servers will rely on,
 * so any regression caused by FASE 10.1+ (servers to ring 3) shows
 * up here first. Format mirrors libctest: PASS / FAIL lines + totals.
 *
 * Run via `kerntest` from the shell (and is also invoked by the
 * shell's `test` command, which still runs cmd_test afterwards).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "osnos_taskinfo.h"

static int total = 0;
static int fails = 0;

#define CHECK(cond, name) do {                              \
    total++;                                                \
    if (cond) printf("PASS %s\n", name);                    \
    else     { printf("FAIL %s\n", name); fails++; }        \
} while (0)

/* Direct syscall wrapper for SYS_TASKINFO (no libc shim exists yet,
 * matches the pattern of other 26x syscalls — added in 10.0.e). */
static long sys_taskinfo_raw(size_t idx, osnos_taskinfo_t *out) {
    long ret;
    register long r10 __asm__("r10") = 0;
    register long r8  __asm__("r8")  = 0;
    register long r9  __asm__("r9")  = 0;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(265 /* SYS_TASKINFO */),
          "D"(idx), "S"((long)out),
          "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static void test_taskinfo(void) {
    int saw_shell    = 0;
    int saw_keyboard = 0;
    int saw_console  = 0;
    int saw_fs       = 0;
    int saw_self     = 0;
    int n_user       = 0;

    long my_pid = (long)getpid();

    for (size_t i = 0; i < 16; i++) {
        osnos_taskinfo_t info;
        long r = sys_taskinfo_raw(i, &info);
        if (r < 0) continue;             /* unused slot or out of range */

        if (strcmp(info.name, "shell")    == 0) saw_shell++;
        if (strcmp(info.name, "keyboard") == 0) saw_keyboard++;
        if (strcmp(info.name, "console")  == 0) saw_console++;
        if (strcmp(info.name, "fs")       == 0) saw_fs++;
        if ((long)info.pid == my_pid)           saw_self++;
        if (info.is_user)                       n_user++;
    }

    CHECK(saw_shell    == 1, "taskinfo: shell server present");
    CHECK(saw_keyboard == 1, "taskinfo: keyboard server present");
    CHECK(saw_console  == 1, "taskinfo: console server present");
    CHECK(saw_fs       == 1, "taskinfo: fs server present");
    CHECK(saw_self     == 1, "taskinfo: own task visible");
    CHECK(n_user       >= 1, "taskinfo: at least one ring-3 task");

    /* Walking past the slot table returns -ENOENT cleanly. */
    osnos_taskinfo_t dummy;
    long oob = sys_taskinfo_raw(100, &dummy);
    CHECK(oob < 0, "taskinfo: out-of-range slot returns error");

    /* Bad pointer surfaces as EFAULT — we use a low address that
     * isn't mapped in any user pml4. */
    long bad = sys_taskinfo_raw(0, (osnos_taskinfo_t *)1);
    CHECK(bad < 0, "taskinfo: bad pointer returns error");
}

static void test_dev_nodes(void) {
    struct stat st;
    CHECK(stat("/dev",       &st) == 0, "stat /dev OK");
    CHECK(stat("/dev/fb0",   &st) == 0, "stat /dev/fb0 OK");
    CHECK(stat("/dev/input0",&st) == 0, "stat /dev/input0 OK");
    CHECK(stat("/dev/null",  &st) == 0, "stat /dev/null OK");
    CHECK(stat("/dev/zero",  &st) == 0, "stat /dev/zero OK");

    /* /dev/fb0 must be writable. Empty write should succeed. */
    int fb = open("/dev/fb0", O_WRONLY);
    CHECK(fb >= 0, "/dev/fb0 opens write-only");
    if (fb >= 0) close(fb);

    /* /dev/input0 must be readable. */
    int in = open("/dev/input0", O_RDONLY);
    CHECK(in >= 0, "/dev/input0 opens read-only");
    if (in >= 0) close(in);
}

static void test_pipe_roundtrip(void) {
    int p[2];
    int r = pipe(p);
    CHECK(r == 0, "pipe(): returns 0");
    CHECK(p[0] >= 3 && p[1] >= 3 && p[0] != p[1],
          "pipe(): two distinct fds >= 3");

    long n = write(p[1], "hello", 5);
    CHECK(n == 5, "pipe write 5 bytes");

    char buf[8] = {0};
    n = read(p[0], buf, 5);
    CHECK(n == 5 && memcmp(buf, "hello", 5) == 0,
          "pipe roundtrip data matches");

    /* Reading the write end / writing the read end → EBADF. */
    long bad_r = read(p[1], buf, 4);
    CHECK(bad_r < 0 && errno == EBADF, "pipe: read from write end EBADF");
    long bad_w = write(p[0], "x", 1);
    CHECK(bad_w < 0 && errno == EBADF, "pipe: write to read end EBADF");

    close(p[1]);
    n = read(p[0], buf, 5);
    CHECK(n == 0, "pipe: read after writer close = EOF");

    close(p[0]);
}

static void test_dup_chain(void) {
    int p[2];
    if (pipe(p) != 0) {
        CHECK(0, "dup-chain: setup pipe");
        return;
    }
    int d = dup(p[1]);
    CHECK(d >= 3 && d != p[1], "dup(write end) yields new fd");

    /* Writing on the dup should be visible on the original reader. */
    write(d, "abc", 3);
    char buf[4] = {0};
    long n = read(p[0], buf, 3);
    CHECK(n == 3 && memcmp(buf, "abc", 3) == 0,
          "dup'd fd shares pipe with original");

    /* Close one of the two writer fds — pipe must NOT EOF yet. */
    close(d);
    write(p[1], "x", 1);
    n = read(p[0], buf, 1);
    CHECK(n == 1 && buf[0] == 'x',
          "pipe stays open while one writer fd remains");

    close(p[0]);
    close(p[1]);
}

static void test_sys_meminfo(void) {
    /* /sys/meminfo exists, is non-empty, and contains the canonical
     * "kheap used" field the kernel emits. Lets ring-3 tools
     * (top, ps, kerntest) read kernel counters without a dedicated
     * syscall. */
    int fd = open("/sys/meminfo", O_RDONLY);
    CHECK(fd >= 0, "/sys/meminfo opens");
    if (fd < 0) return;

    char buf[1024] = {0};
    long n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    CHECK(n > 0, "/sys/meminfo non-empty");
    CHECK(strstr(buf, "kheap used") != 0,
          "/sys/meminfo contains kheap used");
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("kerntest: FASE 10 ABI userland tests\n");

    test_taskinfo();
    test_dev_nodes();
    test_pipe_roundtrip();
    test_dup_chain();
    test_sys_meminfo();

    printf("\nkerntest: total=%d pass=%d fail=%d\n",
           total, total - fails, fails);
    return fails == 0 ? 0 : 1;
}

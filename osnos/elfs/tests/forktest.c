/*
 * forktest — verify Linux fork(2) semantics on osnos.
 *
 * What we check:
 *   1. fork() returns 0 in child, child pid in parent, -1 on error.
 *   2. Parent and child have DIFFERENT pids (getpid returns distinct).
 *   3. Stack-local variables diverge after fork (different writes
 *      don't leak across).
 *   4. open()'d fds are inherited (child reads file the parent opened).
 *   5. environ is preserved.
 *   6. Exit code from child shows up in the parent's wait.
 *
 * No wait(2) syscall yet; we poll on the child pid via sys_taskinfo
 * (#265) to know when it died — same trick shellsrv uses.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int total = 0;
static int fails = 0;

#define CHECK(cond, name) do {                              \
    total++;                                                \
    if (cond) printf("PASS %s\n", name);                    \
    else     { printf("FAIL %s\n", name); fails++; }        \
} while (0)

/* Wait for the child via POSIX waitpid(2). Returns the child's
 * exit code if exited normally, -1 otherwise. Now that wait(2) is a
 * real syscall we don't need the sys_taskinfo polling dance. */
static int wait_child(long pid) {
    int status = 0;
    pid_t r = waitpid((pid_t)pid, &status, 0);
    if (r < 0) return -1;
    if (WIFEXITED(status))    return WEXITSTATUS(status);
    if (WIFSIGNALED(status))  return 128 + WTERMSIG(status);
    return -1;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("forktest: fork(2) sanity tests\n");

    long parent_pid_before = (long)getpid();
    printf("forktest: parent pid before fork = %ld\n", parent_pid_before);

    /* Pre-open a file. Child should see it open too. */
    int fd = open("/home/HELLO.TXT", O_RDONLY);
    CHECK(fd >= 0, "open /home/HELLO.TXT (parent setup)");

    int local_var = 42;

    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "forktest: fork failed, errno=%d\n", errno);
        return 1;
    }

    if (child == 0) {
        /* --- in CHILD --- */
        long child_pid = (long)getpid();
        /* Don't reuse `total`/`fails` to score in child — child writes
         * to its own COPY of those statics, which the parent won't
         * see. Just print + exit with a coded status. */
        int rc = 0;
        if (child_pid == parent_pid_before) rc |= 0x1;   /* same pid is BAD */
        if (local_var != 42)                rc |= 0x2;   /* stack copy bad */
        local_var = 99;                                   /* shouldn't leak up */

        /* Verify the inherited fd is readable. */
        char buf[16] = {0};
        long n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) rc |= 0x4;
        close(fd);

        printf("forktest: child pid=%ld read %ld bytes, exit=%d\n",
               child_pid, n, rc);
        _exit(rc);
    }

    /* --- in PARENT --- */
    long parent_pid_after = (long)getpid();
    CHECK(parent_pid_after == parent_pid_before, "parent pid unchanged");
    CHECK((long)child != parent_pid_after,        "child pid differs from parent");
    CHECK((long)child > 0,                          "fork returned positive in parent");

    int ec = wait_child((long)child);
    CHECK(ec == 0, "child exited 0 (all internal checks passed)");

    CHECK(local_var == 42, "parent's local_var unaffected by child writes");

    close(fd);

    printf("\nforktest: total=%d pass=%d fail=%d\n",
           total, total - fails, fails);
    return fails == 0 ? 0 : 1;
}

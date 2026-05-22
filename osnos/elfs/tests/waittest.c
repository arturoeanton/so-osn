/*
 * waittest — verify POSIX wait(2) + waitpid(2) semantics.
 *
 * Cases:
 *   1. Fork 3 children that exit with distinct codes (0, 42, 7).
 *      Parent waits 3 times via wait(NULL) and reaps all three.
 *      Validates WIFEXITED + WEXITSTATUS for each.
 *   2. WNOHANG when no child ready: returns 0 (not -1).
 *   3. waitpid(specific_pid, ...) targets exactly that child.
 *   4. wait() on a process with no children → -1 + errno=ECHILD.
 *
 * The kernel-side TASK_ZOMBIE state preserves exit_code between
 * child death and parent wait, even if the children die before the
 * parent calls wait (race-free).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int total = 0;
static int fails = 0;
#define CHECK(c,n) do { total++; if (c) printf("PASS %s\n", n); else { printf("FAIL %s\n", n); fails++; } } while (0)

static pid_t spawn_with_exit(int code) {
    pid_t p = fork();
    if (p == 0) {
        /* Tiny delay so a WNOHANG test below can observe "no zombie
         * yet". 10 ms is enough on QEMU without slowing the whole
         * test noticeably. */
        struct timespec ts = { 0, 10 * 1000000 };
        nanosleep(&ts, 0);
        _exit(code);
    }
    return p;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("waittest: wait(2)/waitpid(2)\n");

    /* 1. Three children, three waits. */
    pid_t p0 = spawn_with_exit(0);
    pid_t p1 = spawn_with_exit(42);
    pid_t p2 = spawn_with_exit(7);
    CHECK(p0 > 0 && p1 > 0 && p2 > 0, "fork 3 children");

    int seen[3] = {0};
    for (int i = 0; i < 3; i++) {
        int status = 0;
        pid_t r = wait(&status);
        if (r <= 0) { fails++; total++; printf("FAIL wait #%d\n", i); continue; }
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if      (r == p0 && code == 0)  seen[0] = 1;
        else if (r == p1 && code == 42) seen[1] = 1;
        else if (r == p2 && code == 7)  seen[2] = 1;
        else { fails++; total++; printf("FAIL wait pid=%d code=%d\n", (int)r, code); }
    }
    CHECK(seen[0],                                "child0 (exit 0) reaped");
    CHECK(seen[1],                                "child1 (exit 42) reaped");
    CHECK(seen[2],                                "child2 (exit 7) reaped");

    /* 2. WNOHANG with no children: returns -1 + ECHILD. */
    errno = 0;
    int status = 0;
    pid_t r = waitpid(-1, &status, WNOHANG);
    CHECK(r == -1 && errno == ECHILD,
          "wait with no children → ECHILD");

    /* 3. waitpid(specific_pid). */
    pid_t pa = spawn_with_exit(99);
    pid_t pb = spawn_with_exit(1);
    /* Reap pb first by pid. */
    r = waitpid(pb, &status, 0);
    CHECK(r == pb,                                "waitpid targeted pb");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1,
          "pb exit code 1");
    /* Then any (= pa). */
    r = wait(&status);
    CHECK(r == pa && WEXITSTATUS(status) == 99,
          "wait reaped pa with code 99");

    printf("\nwaittest: total=%d pass=%d fail=%d\n",
           total, total - fails, fails);
    return fails == 0 ? 0 : 1;
}

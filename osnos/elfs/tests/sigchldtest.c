/*
 * sigchldtest — verify SIGCHLD automatic delivery on child exit.
 *
 * POSIX: a parent receives SIGCHLD whenever a child changes state
 * (exited, killed, stopped, continued). Default disposition is to
 * ignore, so it's a no-op for programs that don't install a handler.
 *
 * Tests:
 *   1. With a handler installed, raise SIGCHLD via fork+exit and
 *      verify the handler ran at least once.
 *   2. Default disposition (no handler / SIG_DFL) does NOT terminate
 *      the parent — fork+exit completes cleanly.
 *   3. SIG_IGN swallows similarly (no death, no handler invocation).
 *
 * Note: sig_pending is a bitmap, so N children dying in quick
 * succession may deliver only ONE SIGCHLD to the handler (POSIX
 * non-realtime signals merge). We assert >=1, not ==N.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int total = 0;
static int fails = 0;
#define CHECK(c,n) do { total++; if (c) printf("PASS %s\n", n); else { printf("FAIL %s\n", n); fails++; } } while (0)

static volatile int chld_count;

static void on_sigchld(int sig) {
    (void)sig;
    chld_count++;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("sigchldtest: SIGCHLD automatic delivery\n");

    /* 1. Install handler, fork+exit, verify handler fired. */
    struct sigaction act = { 0 };
    act.sa_handler = on_sigchld;
    int r = sigaction(SIGCHLD, &act, 0);
    CHECK(r == 0, "sigaction SIGCHLD installed");

    chld_count = 0;

    pid_t p = fork();
    if (p == 0) {
        /* Brief delay so the parent reaches waitpid before we exit —
         * gives the kernel's SIGCHLD delivery a clean wake-path
         * (parent BLOCKED in wait4, child exit triggers wake + signal). */
        struct timespec ts = { 0, 5 * 1000000 };
        nanosleep(&ts, 0);
        _exit(42);
    }
    if (p < 0) {
        fprintf(stderr, "fork failed: errno=%d\n", errno);
        return 1;
    }

    int status = 0;
    pid_t r2 = waitpid(p, &status, 0);
    CHECK(r2 == p,                          "waitpid reaped child");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 42,
                                            "child exit code 42");
    CHECK(chld_count >= 1,                  "SIGCHLD handler ran (>=1)");

    /* 2. Reset to SIG_DFL (default = ignore). Fork+exit should NOT
     *    kill the parent. */
    act.sa_handler = SIG_DFL;
    sigaction(SIGCHLD, &act, 0);
    chld_count = 0;
    p = fork();
    if (p == 0) _exit(7);
    waitpid(p, &status, 0);
    CHECK(chld_count == 0,                  "SIG_DFL no handler invocation");

    /* 3. SIG_IGN explicit — same outcome as SIG_DFL for SIGCHLD. */
    act.sa_handler = SIG_IGN;
    sigaction(SIGCHLD, &act, 0);
    chld_count = 0;
    p = fork();
    if (p == 0) _exit(0);
    waitpid(p, &status, 0);
    CHECK(chld_count == 0,                  "SIG_IGN no handler invocation");

    printf("\nsigchldtest: total=%d pass=%d fail=%d\n",
           total, total - fails, fails);
    return fails == 0 ? 0 : 1;
}

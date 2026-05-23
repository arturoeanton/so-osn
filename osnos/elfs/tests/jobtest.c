/*
 * jobtest — POSIX job-control verification.
 *
 * Coverage:
 *   1. fork child that sleeps. kill(child, SIGSTOP). waitpid(..., WUNTRACED)
 *      returns the child pid with WIFSTOPPED true + WSTOPSIG == SIGSTOP.
 *   2. kill(child, SIGCONT). waitpid(..., WCONTINUED) returns child pid
 *      with WIFCONTINUED true.
 *   3. kill(child, SIGTERM) terminates it. waitpid → WIFSIGNALED +
 *      WTERMSIG == SIGTERM.
 *   4. Process-group fan-out via kill(-pgid, sig): fork 3 children in
 *      a common pgid, kill the group, every member dies via SIGTERM.
 *      (Already covered in pgrouptest, repeated here for completeness.)
 *
 * Note: Ctrl+Z fan-out from the kernel TTY is NOT exercised here —
 * that needs interactive keyboard input. Logic shared with tty_signal,
 * tested manually via shellsrv.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int total = 0;
static int fails = 0;
#define CHECK(c,n) do { total++; if (c) printf("PASS %s\n", n); else { printf("FAIL %s\n", n); fails++; } } while (0)

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("jobtest: WUNTRACED / WCONTINUED / pgid fan-out\n");

    /* ----- 1. SIGSTOP + WUNTRACED ----- */
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed\n");
        return 1;
    }
    if (pid == 0) {
        /* Child: loop forever (small sleep each iter to be polite). */
        while (1) {
            struct timespec ts = { 0, 50 * 1000000 };
            nanosleep(&ts, 0);
        }
        _exit(99);
    }

    /* Brief sleep so the child reaches its loop before we kill. */
    {
        struct timespec ts = { 0, 30 * 1000000 };
        nanosleep(&ts, 0);
    }

    int r = kill(pid, SIGSTOP);
    CHECK(r == 0, "1.kill SIGSTOP");

    int status = 0;
    pid_t w = waitpid(pid, &status, WUNTRACED);
    CHECK(w == pid,             "1.waitpid WUNTRACED returns child pid");
    CHECK(WIFSTOPPED(status),   "1.WIFSTOPPED true");
    CHECK(WSTOPSIG(status) == SIGSTOP,
                                "1.WSTOPSIG == SIGSTOP");

    /* ----- 2. SIGCONT + WCONTINUED ----- */
    r = kill(pid, SIGCONT);
    CHECK(r == 0, "2.kill SIGCONT");

    /* Give a tick for the transition. */
    {
        struct timespec ts = { 0, 30 * 1000000 };
        nanosleep(&ts, 0);
    }

    status = 0;
    w = waitpid(pid, &status, WCONTINUED);
    CHECK(w == pid,             "2.waitpid WCONTINUED returns child pid");
    CHECK(WIFCONTINUED(status), "2.WIFCONTINUED true");

    /* ----- 3. SIGTERM kills it ----- */
    r = kill(pid, SIGTERM);
    CHECK(r == 0, "3.kill SIGTERM");

    status = 0;
    w = waitpid(pid, &status, 0);
    CHECK(w == pid,             "3.waitpid normal returns child pid");
    CHECK(WIFSIGNALED(status),  "3.WIFSIGNALED true");
    CHECK(WTERMSIG(status) == SIGTERM,
                                "3.WTERMSIG == SIGTERM");

    /* ----- 4. kill(-pgid, sig) fan-out ----- */
    pid_t kids[3];
    pid_t new_pgid = 0;
    for (int i = 0; i < 3; i++) {
        pid_t p = fork();
        if (p == 0) {
            while (1) {
                struct timespec ts = { 0, 50 * 1000000 };
                nanosleep(&ts, 0);
            }
            _exit(99);
        }
        kids[i] = p;
        if (i == 0) new_pgid = p;
        setpgid(p, new_pgid);
    }
    {
        struct timespec ts = { 0, 30 * 1000000 };
        nanosleep(&ts, 0);
    }
    r = kill(-new_pgid, SIGTERM);
    CHECK(r == 0, "4.kill(-pgid, SIGTERM)");

    int got = 0;
    for (int i = 0; i < 3; i++) {
        int s = 0;
        pid_t rr = waitpid(kids[i], &s, 0);
        if (rr == kids[i] && WIFSIGNALED(s) && WTERMSIG(s) == SIGTERM) {
            got++;
        }
    }
    CHECK(got == 3, "4.all 3 children killed via group SIGTERM");

    printf("\njobtest: total=%d pass=%d fail=%d\n",
           total, total - fails, fails);
    return fails == 0 ? 0 : 1;
}

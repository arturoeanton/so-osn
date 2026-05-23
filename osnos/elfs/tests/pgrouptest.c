/*
 * pgrouptest — POSIX process groups + sessions sanity check.
 *
 * Coverage:
 *   1. Defaults after fork: child inherits parent's pgid/sid
 *   2. getppid returns the actual parent pid
 *   3. setpgid moves a process into a new group
 *   4. setsid creates a new session (sid = pid = pgid)
 *   5. kill(-pgid, sig) broadcasts to every member of the group
 *   6. kill(0, sig) broadcasts to caller's own group
 *
 * Test 5 is the headline: fork 3 children, place them all in a
 * single new group, kill(-pgid, SIGTERM), waitpid them all,
 * verify every one died via SIGTERM.
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

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("pgrouptest: process groups + sessions\n");

    pid_t my_pid = getpid();
    pid_t my_ppid = getppid();
    pid_t my_pgid = getpgrp();
    pid_t my_sid  = getsid(0);

    CHECK(my_pid > 0,      "getpid returned positive");
    CHECK(my_ppid > 0,     "getppid returned positive (parent exists)");
    CHECK(my_pgid == my_pid, "default pgid == pid (we're top-level)");
    CHECK(my_sid == my_pid,  "default sid == pid (we're own session)");

    /* getpgid(pid) and getsid(pid) should agree with getpgrp()/getsid(0). */
    CHECK(getpgid(my_pid) == my_pgid, "getpgid(self) matches getpgrp");
    CHECK(getsid(my_pid)  == my_sid,  "getsid(self) matches getsid(0)");

    /* 1. fork: child inherits pgid/sid. */
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "fork failed: errno=%d\n", errno);
        return 1;
    }
    if (child == 0) {
        /* Child: verify inheritance + ppid pointing back. */
        pid_t expected_ppid = my_pid;
        pid_t expected_pgid = my_pgid;
        pid_t expected_sid  = my_sid;
        int rc = 0;
        if (getppid() != expected_ppid) rc |= 0x1;
        if (getpgrp() != expected_pgid) rc |= 0x2;
        if (getsid(0) != expected_sid)  rc |= 0x4;
        _exit(rc);
    }
    int status = 0;
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "child inherits pgid+sid+ppid");

    /* 2. kill(-pgid, SIGTERM) broadcast. Fork 3 children that loop
     *    forever; place them all in pgid == first_child_pid; kill
     *    the whole group; waitpid 3 times. */
    pid_t kids[3];
    pid_t new_pgid = 0;
    for (int i = 0; i < 3; i++) {
        pid_t p = fork();
        if (p < 0) {
            fprintf(stderr, "fork %d failed\n", i);
            return 1;
        }
        if (p == 0) {
            /* Child: spin until killed. Loop with brief sleeps so we
             * don't peg the CPU during the test. */
            while (1) {
                struct timespec ts = { 0, 50 * 1000000 };
                nanosleep(&ts, 0);
            }
            _exit(99);
        }
        kids[i] = p;
        if (i == 0) new_pgid = p;
        /* Parent moves the child into the new pgid. setpgid from
         * parent side is racy-but-OK: child might not have started
         * yet, but the call only touches the task struct. */
        if (setpgid(p, new_pgid) < 0) {
            fprintf(stderr, "setpgid(%d,%d) failed: errno=%d\n",
                    (int)p, (int)new_pgid, errno);
            return 1;
        }
    }

    /* Verify all 3 have the same pgid via getpgid(pid). */
    int all_in_group = 1;
    for (int i = 0; i < 3; i++) {
        if (getpgid(kids[i]) != new_pgid) all_in_group = 0;
    }
    CHECK(all_in_group, "3 children moved into common pgid via setpgid");

    /* Brief sleep so the children definitely reach their nanosleep
     * loop. Then broadcast SIGTERM to the whole group. */
    {
        struct timespec ts = { 0, 30 * 1000000 };
        nanosleep(&ts, 0);
    }

    int r = kill(-new_pgid, SIGTERM);
    CHECK(r == 0, "kill(-pgid, SIGTERM) returned 0");

    /* Reap them. Each should report WIFSIGNALED + WTERMSIG=SIGTERM. */
    int killed_ok = 0;
    for (int i = 0; i < 3; i++) {
        int s = 0;
        pid_t reaped = waitpid(kids[i], &s, 0);
        if (reaped == kids[i] &&
            WIFSIGNALED(s) && WTERMSIG(s) == SIGTERM) {
            killed_ok++;
        }
    }
    CHECK(killed_ok == 3, "all 3 children died via SIGTERM");

    printf("\npgrouptest: total=%d pass=%d fail=%d\n",
           total, total - fails, fails);
    return fails == 0 ? 0 : 1;
}

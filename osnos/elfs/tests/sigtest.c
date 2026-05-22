/*
 * sigtest — verify POSIX sigaction(2) + kill(2) plumbing on osnos.
 *
 * Checks:
 *   1. sigaction installs a user handler; raise(SIGUSR1) runs it
 *      exactly once and sets a flag.
 *   2. SIG_IGN swallows the signal — handler-side flag stays at 0.
 *   3. SIG_DFL with SIGTERM kills the task (verified via a child:
 *      parent forks, child does kill(self, SIGTERM), parent
 *      waitpid's and checks WIFSIGNALED + WTERMSIG == SIGTERM).
 *   4. signal() (BSD wrapper) round-trips through sigaction.
 *   5. SIGKILL cannot be caught — sigaction must return -1 + EINVAL.
 *
 * Coverage is shallow but exercises every code path of the signal
 * delivery machinery: bit setting in sys_kill, sigframe construction
 * in user_task_resume, __sigtramp epilogue, rt_sigreturn restore.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int total = 0;
static int fails = 0;
#define CHECK(c,n) do { total++; if (c) printf("PASS %s\n", n); else { printf("FAIL %s\n", n); fails++; } } while (0)

static volatile int sig_count;
static volatile int last_sig_received;

static void user_handler(int sig) {
    sig_count++;
    last_sig_received = sig;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("sigtest: sigaction + signal delivery\n");

    /* 1. Install handler for SIGUSR1, raise it, verify handler ran. */
    struct sigaction act = { 0 };
    act.sa_handler = user_handler;
    int r = sigaction(SIGUSR1, &act, 0);
    CHECK(r == 0, "sigaction SIGUSR1 installed");

    sig_count = 0;
    raise(SIGUSR1);
    /* The signal is delivered on the next return-to-user from the
     * raise()'s underlying kill(2) syscall. By the time we see this
     * printf, the handler has run. */
    CHECK(sig_count == 1,                "handler ran exactly once");
    CHECK(last_sig_received == SIGUSR1,  "handler got correct signum");

    /* 2. SIG_IGN swallows. */
    act.sa_handler = SIG_IGN;
    sigaction(SIGUSR1, &act, 0);
    sig_count = 0;
    raise(SIGUSR1);
    CHECK(sig_count == 0, "SIG_IGN suppresses delivery");

    /* 3. SIGKILL un-catchable. */
    act.sa_handler = user_handler;
    errno = 0;
    r = sigaction(SIGKILL, &act, 0);
    CHECK(r == -1 && errno == EINVAL, "sigaction SIGKILL → EINVAL");

    /* 4. signal() round-trip. */
    sighandler_t prev = signal(SIGUSR1, user_handler);
    CHECK(prev == SIG_IGN, "signal() returns previous SIG_IGN");

    /* 5. Default disposition: fork a child that nukes itself with
     *    SIGTERM, parent waitpid's and validates WIFSIGNALED. */
    pid_t pid = fork();
    if (pid == 0) {
        /* Child — reset to default first (parent left handler set). */
        signal(SIGTERM, SIG_DFL);
        raise(SIGTERM);
        /* If we get here, signal delivery failed. */
        _exit(99);
    } else {
        int status = 0;
        pid_t r2 = waitpid(pid, &status, 0);
        CHECK(r2 == pid,           "waitpid returned forked pid");
        CHECK(WIFSIGNALED(status), "WIFSIGNALED true after SIG_DFL SIGTERM");
        CHECK(WTERMSIG(status) == SIGTERM,
              "WTERMSIG == SIGTERM");
    }

    printf("\nsigtest: total=%d pass=%d fail=%d\n",
           total, total - fails, fails);
    return fails == 0 ? 0 : 1;
}

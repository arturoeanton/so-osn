#include <errno.h>
#include <signal.h>
#include <unistd.h>

/*
 * signal()/raise() — stub layer.
 *
 * osnos doesn't have user-installed signal handlers yet. The kernel
 * delivers SIGINT via the kill_pending bit, which always terminates
 * the task with exit(130); SIG_DFL is the only behaviour available.
 * We expose the libc surface so TCC and other programs compile,
 * with conservative semantics:
 *
 *   - signal(sig, SIG_DFL) returns SIG_DFL (nothing to change).
 *   - signal(sig, anything_else) returns SIG_ERR + errno=ENOSYS.
 *   - raise(sig) shells out to kill(getpid(), sig). Whether that
 *     terminates depends on the kernel signal delivery — today
 *     only SIGINT and friends route through kill_pending.
 */

sighandler_t signal(int sig, sighandler_t handler) {
    (void)sig;
    if (handler == SIG_DFL) return SIG_DFL;
    errno = ENOSYS;
    return SIG_ERR;
}

int raise(int sig) {
    return kill(getpid(), sig);
}

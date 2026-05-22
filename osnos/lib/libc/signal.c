#include <errno.h>
#include <signal.h>
#include <unistd.h>

#include "syscall.h"

/*
 * signal.c — user-mode signal API wrappers over SYS_RT_SIGACTION (#13)
 * and SYS_RT_SIGPROCMASK (#14).
 *
 * Model: sa_handler-only. struct sigaction.sa_handler holds one of:
 *   SIG_DFL (0)  — kernel default disposition
 *   SIG_IGN (1)  — silently discard
 *   fn ptr       — user-mode handler called with `int signum`
 *
 * The default `sa_restorer` is the libc-provided __sigtramp (see
 * sigtramp.S). The kernel uses it to know where to redirect after
 * the handler returns — there the trampoline calls SYS_RT_SIGRETURN
 * which restores the pre-signal context.
 */

extern void __sigtramp(void);

int sigaction(int signum,
              const struct sigaction *act,
              struct sigaction *oldact) {
    struct sigaction patched;
    const struct sigaction *user_act = act;
    if (act) {
        patched = *act;
        if (!patched.sa_restorer) {
            patched.sa_restorer = __sigtramp;
        }
        user_act = &patched;
    }
    long r = osnos_syscall3(SYS_RT_SIGACTION,
                             (long)signum,
                             (long)user_act,
                             (long)oldact);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    long r = osnos_syscall3(SYS_RT_SIGPROCMASK,
                             (long)how, (long)set, (long)oldset);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

/* sigset_t helpers (single-uint32 mask, bit (s-1) for signal s). */
int sigemptyset(sigset_t *set) { if (set) *set = 0;          return 0; }
int sigfillset (sigset_t *set) { if (set) *set = 0xfffffffeu; return 0; } /* bits 1..31 */
int sigaddset  (sigset_t *set, int sig) {
    if (!set || sig < 1 || sig > 31) { errno = EINVAL; return -1; }
    *set |= 1u << (sig - 1);
    return 0;
}
int sigdelset  (sigset_t *set, int sig) {
    if (!set || sig < 1 || sig > 31) { errno = EINVAL; return -1; }
    *set &= ~(1u << (sig - 1));
    return 0;
}
int sigismember(const sigset_t *set, int sig) {
    if (!set || sig < 1 || sig > 31) { errno = EINVAL; return -1; }
    return (*set & (1u << (sig - 1))) ? 1 : 0;
}

/* Legacy BSD signal(2) — re-implemented on top of sigaction(2). */
sighandler_t signal(int sig, sighandler_t handler) {
    struct sigaction act = { 0 };
    struct sigaction old = { 0 };
    act.sa_handler = handler;
    act.sa_flags   = 0;
    if (sigaction(sig, &act, &old) != 0) return SIG_ERR;
    return old.sa_handler;
}

int raise(int sig) {
    return kill(getpid(), sig);
}

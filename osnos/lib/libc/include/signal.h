#pragma once

#include <sys/types.h>

/* ISO C requires sig_atomic_t — an integer type that can be read
 * and written atomically wrt async signal delivery. On x86_64
 * native `int` qualifies. Lua's lstate.h uses this even when no
 * real signal handlers are wired up. */
typedef volatile int sig_atomic_t;

/*
 * Minimal signal.h — TCC and most simple C programs include this
 * for SIGINT / SIGTERM constants even if they never install a
 * handler. osnos doesn't have signal handlers wired up yet:
 * `signal()` returns SIG_ERR for any non-default action, `raise()`
 * forwards to kill(getpid(), sig). Defaults match Linux numbers.
 */

#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22

typedef void (*sighandler_t)(int);

#define SIG_DFL  ((sighandler_t)0)
#define SIG_IGN  ((sighandler_t)1)
#define SIG_ERR  ((sighandler_t)-1)

/*
 * `signal(sig, handler)` installs a handler via sigaction(2). Returns
 * the previous handler or SIG_ERR on error. `raise(sig)` delivers
 * `sig` to the current process via kill(2).
 */
sighandler_t signal(int sig, sighandler_t handler);
int          raise (int sig);

/*
 * Simple sigaction(2) — sa_handler-only model.
 *
 * sigset_t is a 32-bit mask: bit (s-1) corresponds to signal s.
 * Used today only for sa_mask field of struct sigaction; we do not
 * implement sigprocmask blocking yet, but the field has to exist
 * for source compatibility with POSIX programs.
 */
typedef unsigned int sigset_t;

struct sigaction {
    sighandler_t sa_handler;
    sigset_t     sa_mask;
    int          sa_flags;
    void       (*sa_restorer)(void);   /* libc-supplied trampoline epilogue */
};

/* sigaction flags (we only honor SA_RESTART semantics at the kernel
 * level by NOT setting it — i.e. blocking syscalls return EINTR by
 * default). The bits are reserved so source code that sets them
 * still compiles. */
#define SA_NOCLDSTOP   0x00000001
#define SA_NOCLDWAIT   0x00000002
#define SA_SIGINFO     0x00000004
#define SA_ONSTACK     0x08000000
#define SA_RESTART     0x10000000
#define SA_NODEFER     0x40000000
#define SA_RESETHAND   0x80000000
#define SA_RESTORER    0x04000000

/* sigprocmask `how` values. */
#define SIG_BLOCK     0
#define SIG_UNBLOCK   1
#define SIG_SETMASK   2

int sigaction  (int signum,
                const struct sigaction *act,
                struct sigaction *oldact);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);

/* sigset_t manipulation. */
int sigemptyset(sigset_t *set);
int sigfillset (sigset_t *set);
int sigaddset  (sigset_t *set, int sig);
int sigdelset  (sigset_t *set, int sig);
int sigismember(const sigset_t *set, int sig);

#pragma once

#include <sys/types.h>

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
 * `signal(sig, handler)` returns the previous handler or SIG_ERR.
 * osnos has no handler table yet — every call returns SIG_ERR.
 * `raise(sig)` delivers `sig` to the current process via kill(2)
 * and is the only way to trigger the kernel's signal path today.
 */
sighandler_t signal(int sig, sighandler_t handler);
int          raise (int sig);

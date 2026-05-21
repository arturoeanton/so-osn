#include <errno.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <termios.h>

#include "syscall.h"

/*
 * ioctl variadic shim — the kernel only consumes a single `arg`
 * pointer for now (no command takes more than one argument), so we
 * forward whatever void* the caller put in the variadic slot.
 */
int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    long r = osnos_syscall3(SYS_IOCTL, fd, (long)request, (long)arg);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (int)r;
}

int tcgetattr(int fd, struct termios *t) {
    return ioctl(fd, TCGETS, t);
}

int tcsetattr(int fd, int when, const struct termios *t) {
    unsigned long req;
    switch (when) {
    case TCSANOW:   req = TCSETS;  break;
    case TCSADRAIN: req = TCSETSW; break;
    case TCSAFLUSH: req = TCSETSF; break;
    default:        errno = EINVAL; return -1;
    }
    return ioctl(fd, req, (void *)t);
}

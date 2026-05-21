#include "fd.h"

#include "../include/osnos_fcntl.h"
#include "tty.h"

static osnos_fd_t fds[OSNOS_MAX_FDS];

void fd_init(void) {
    /* stdin is now backed by the TTY line discipline (FASE TTY 1+2).
     * tty_init runs separately from kmain — fd_init only resets the
     * fd table here. */
    for (size_t i = 0; i < OSNOS_MAX_FDS; i++) {
        fds[i].used       = false;
        fds[i].is_special = false;
        fds[i].is_dir     = false;
        fds[i].is_socket  = false;
        fds[i].sock_idx   = -1;
        fds[i].flags      = 0;
        fds[i].offset     = 0;
        fds[i].path[0]    = 0;
    }

    /* stdin */
    fds[OSNOS_FD_STDIN].used       = true;
    fds[OSNOS_FD_STDIN].is_special = true;
    fds[OSNOS_FD_STDIN].flags      = O_RDONLY;

    /* stdout */
    fds[OSNOS_FD_STDOUT].used       = true;
    fds[OSNOS_FD_STDOUT].is_special = true;
    fds[OSNOS_FD_STDOUT].flags      = O_WRONLY;

    /* stderr */
    fds[OSNOS_FD_STDERR].used       = true;
    fds[OSNOS_FD_STDERR].is_special = true;
    fds[OSNOS_FD_STDERR].flags      = O_WRONLY;
}

int fd_alloc(void) {
    for (int i = 3; i < OSNOS_MAX_FDS; i++) {
        if (!fds[i].used) {
            fds[i].used       = true;
            fds[i].is_special = false;
            fds[i].is_dir     = false;
            fds[i].is_socket  = false;
            fds[i].sock_idx   = -1;
            fds[i].flags      = 0;
            fds[i].offset     = 0;
            fds[i].path[0]    = 0;
            return i;
        }
    }
    return -1;
}

void fd_free(int fd) {
    if (fd < 3 || fd >= OSNOS_MAX_FDS) return;
    if (!fds[fd].used) return;
    fds[fd].used       = false;
    fds[fd].is_special = false;
    fds[fd].is_dir     = false;
    fds[fd].is_socket  = false;
    fds[fd].sock_idx   = -1;
    fds[fd].flags      = 0;
    fds[fd].offset     = 0;
    fds[fd].path[0]    = 0;
}

osnos_fd_t *fd_get(int fd) {
    if (fd < 0 || fd >= OSNOS_MAX_FDS) return 0;
    if (!fds[fd].used) return 0;
    return &fds[fd];
}

/* ---- stdin shims over the TTY line discipline ----
 *
 * These wrappers keep the old fd.h API alive for callers that don't
 * care about termios. New code (sys_select, sys_ioctl) talks to the
 * TTY layer directly via tty.h.
 */

void stdin_push(char c)               { tty_input(c); }
size_t stdin_pop(char *out, size_t m) { return tty_read(out, m); }
bool   stdin_readable(void)           { return tty_readable(); }
void   stdin_clear(void)              { tty_clear(); }

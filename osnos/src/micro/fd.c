#include "fd.h"

#include "../include/osnos_fcntl.h"

static osnos_fd_t fds[OSNOS_MAX_FDS];

#define STDIN_BUF_SIZE 256
static char   stdin_buf[STDIN_BUF_SIZE];
static size_t stdin_head;
static size_t stdin_tail;
static size_t stdin_count;

void fd_init(void) {
    stdin_head = 0;
    stdin_tail = 0;
    stdin_count = 0;

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

/* ---- stdin ring buffer ---- */

void stdin_push(char c) {
    if (stdin_count >= STDIN_BUF_SIZE) {
        /* drop oldest to make room — keystrokes shouldn't pile up forever */
        stdin_tail = (stdin_tail + 1) % STDIN_BUF_SIZE;
        stdin_count--;
    }
    stdin_buf[stdin_head] = c;
    stdin_head = (stdin_head + 1) % STDIN_BUF_SIZE;
    stdin_count++;
}

size_t stdin_pop(char *out, size_t max) {
    size_t n = 0;
    while (n < max && stdin_count > 0) {
        out[n++] = stdin_buf[stdin_tail];
        stdin_tail = (stdin_tail + 1) % STDIN_BUF_SIZE;
        stdin_count--;
    }
    return n;
}

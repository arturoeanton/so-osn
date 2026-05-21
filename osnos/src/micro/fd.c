#include "fd.h"

#include "../include/osnos_fcntl.h"
#include "task.h"
#include "tty.h"

/*
 * Per-task fd table operations. Each task carries `osnos_fd_t fds[16]`
 * inline in task_t; these helpers act on a given task's slice. There
 * is no longer a kernel-global table — the old `fd_init` is gone,
 * replaced by `fd_init_for_task` invoked by `task_create`.
 *
 * Most callers pass `task_current()` as the task pointer; only the
 * exit cleanup path (proc_exit_current_user) and a couple of shell
 * introspection sites work against a specific other task.
 */

static inline void fd_clear_slot(osnos_fd_t *f) {
    f->used       = false;
    f->is_special = false;
    f->is_dir     = false;
    f->is_socket  = false;
    f->sock_idx   = -1;
    f->flags      = 0;
    f->offset     = 0;
    f->path[0]    = 0;
}

void fd_init_for_task(task_t *t) {
    if (!t) return;
    for (size_t i = 0; i < OSNOS_MAX_FDS; i++) {
        fd_clear_slot(&t->fds[i]);
    }

    /* stdin */
    t->fds[OSNOS_FD_STDIN].used       = true;
    t->fds[OSNOS_FD_STDIN].is_special = true;
    t->fds[OSNOS_FD_STDIN].flags      = O_RDONLY;

    /* stdout */
    t->fds[OSNOS_FD_STDOUT].used       = true;
    t->fds[OSNOS_FD_STDOUT].is_special = true;
    t->fds[OSNOS_FD_STDOUT].flags      = O_WRONLY;

    /* stderr */
    t->fds[OSNOS_FD_STDERR].used       = true;
    t->fds[OSNOS_FD_STDERR].is_special = true;
    t->fds[OSNOS_FD_STDERR].flags      = O_WRONLY;
}

int fd_alloc(task_t *t) {
    if (!t) return -1;
    for (int i = 3; i < OSNOS_MAX_FDS; i++) {
        if (!t->fds[i].used) {
            fd_clear_slot(&t->fds[i]);
            t->fds[i].used = true;
            return i;
        }
    }
    return -1;
}

void fd_free(task_t *t, int fd) {
    if (!t) return;
    if (fd < 3 || fd >= OSNOS_MAX_FDS) return;
    if (!t->fds[fd].used) return;
    fd_clear_slot(&t->fds[fd]);
}

osnos_fd_t *fd_get(task_t *t, int fd) {
    if (!t) return 0;
    if (fd < 0 || fd >= OSNOS_MAX_FDS) return 0;
    if (!t->fds[fd].used) return 0;
    return &t->fds[fd];
}

/*
 * Internal copy helper. Replicates `src` into `dst` (which must point
 * at an UNUSED slot — caller decides whether to free first). The
 * copy is shallow: path string and flags are duplicated, but there's
 * no shared "open file description" so offsets diverge from here on.
 */
static void fd_copy_struct(osnos_fd_t *dst, const osnos_fd_t *src) {
    dst->used       = true;
    dst->is_special = src->is_special;
    dst->is_dir     = src->is_dir;
    dst->is_socket  = src->is_socket;
    dst->sock_idx   = src->sock_idx;
    dst->flags      = src->flags;
    dst->offset     = src->offset;
    for (int i = 0; i < OSNOS_PATH_MAX; i++) dst->path[i] = src->path[i];
}

int fd_dup(task_t *t, int src) {
    return fd_dup_min(t, src, 3);
}

int fd_dup_min(task_t *t, int src, int min_fd) {
    if (!t) return -1;
    osnos_fd_t *s = fd_get(t, src);
    if (!s) return -1;
    if (min_fd < 0) min_fd = 0;
    /* Special fds 0/1/2 are always live and can't be reused as a
     * dup target — start the scan at max(3, min_fd). */
    if (min_fd < 3) min_fd = 3;
    for (int i = min_fd; i < OSNOS_MAX_FDS; i++) {
        if (!t->fds[i].used) {
            fd_copy_struct(&t->fds[i], s);
            return i;
        }
    }
    return -1;
}

int fd_dup2(task_t *t, int src, int target) {
    if (!t) return -1;
    osnos_fd_t *s = fd_get(t, src);
    if (!s) return -1;
    if (target < 0 || target >= OSNOS_MAX_FDS) return -1;
    if (target == src) return target;
    /* dup2 silently closes target if it was already open. The
     * special fds 0/1/2 stay (they reset to their default open). */
    if (target >= 3 && t->fds[target].used) {
        fd_free(t, target);
    } else if (target < 3) {
        /* Replacing stdin/stdout/stderr — clobber the slot. The
         * shell never does this today but POSIX allows it. */
        t->fds[target].used = false;
    }
    fd_copy_struct(&t->fds[target], s);
    return target;
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

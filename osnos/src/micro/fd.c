#include "fd.h"

#include "../include/osnos_fcntl.h"
#include "../net/socket.h"
#include "pipe.h"
#include "pty.h"
#include "task.h"
#include "tty.h"

/*
 * Per-task fd table + global OFD pool.
 *
 * task_t.fds[] holds per-task slots (osnos_fd_slot_t — thin, just
 * `{used, ofd_idx, fd_flags}`). The OFD backing each slot lives in
 * the global pool below.
 *
 * Pool sizing: OSNOS_MAX_OFDS = 128. Each live task burns 3 OFDs for
 * its default stdin/stdout/stderr; with 16 tasks that's 48 baseline,
 * leaving 80 for actual file/pipe/socket opens. Comfortable headroom.
 */
static osnos_ofd_t ofd_pool[OSNOS_MAX_OFDS];

/* Clear an OFD slot back to UNUSED. Doesn't touch refcount (caller
 * has already verified it's 0). */
static void ofd_clear(osnos_ofd_t *o) {
    o->used       = false;
    o->refcount   = 0;
    o->is_special = false;
    o->is_dir     = false;
    o->is_socket  = false;
    o->is_unix_socket = false;
    o->is_pipe    = false;
    o->is_chr     = false;
    o->is_pty     = false;
    o->sock_idx   = -1;
    o->unix_idx   = -1;
    o->pipe_ref   = 0;
    o->pipe_side  = 0;
    o->pty_ref    = 0;
    o->pty_side   = 0;
    o->flags      = 0;
    o->offset     = 0;
    o->path[0]    = 0;
}

osnos_ofd_t *ofd_get(int idx) {
    if (idx < 0 || idx >= OSNOS_MAX_OFDS) return 0;
    if (!ofd_pool[idx].used) return 0;
    return &ofd_pool[idx];
}

int ofd_alloc(void) {
    for (int i = 0; i < OSNOS_MAX_OFDS; i++) {
        if (!ofd_pool[i].used) {
            ofd_clear(&ofd_pool[i]);
            ofd_pool[i].used     = true;
            ofd_pool[i].refcount = 1;
            ofd_pool[i].sock_idx = -1;
            return i;
        }
    }
    return -1;
}

void ofd_ref(int idx) {
    if (idx < 0 || idx >= OSNOS_MAX_OFDS) return;
    if (!ofd_pool[idx].used) return;
    ofd_pool[idx].refcount++;
}

void ofd_unref(int idx) {
    if (idx < 0 || idx >= OSNOS_MAX_OFDS) return;
    osnos_ofd_t *o = &ofd_pool[idx];
    if (!o->used) return;
    if (o->refcount > 0) o->refcount--;
    if (o->refcount > 0) return;

    /* Last reference dropped — release backend resources. Each
     * backend has its own teardown idempotent enough that a double-
     * close is harmless, but order matters: socket close may use
     * the path string for logging, so do it before clearing. */
    if (o->is_pipe && o->pipe_ref) {
        if (o->pipe_side == 0) pipe_close_reader(o->pipe_ref);
        else                    pipe_close_writer(o->pipe_ref);
    }
    if (o->is_socket && o->sock_idx >= 0) {
        sock_close(o->sock_idx);
    }
    if (o->is_unix_socket && o->unix_idx >= 0) {
        extern void unix_sock_close(int idx);
        unix_sock_close(o->unix_idx);
    }
    if (o->is_pty && o->pty_ref) {
        if (o->pty_side == 0) pty_master_unref(o->pty_ref);
        else                   pty_slave_unref (o->pty_ref);
    }
    ofd_clear(o);
}

/* --- Per-task slot operations --- */

static inline void slot_clear(osnos_fd_slot_t *s) {
    s->used     = false;
    s->ofd_idx  = -1;
    s->fd_flags = 0;
}

void fd_init_for_task(task_t *t) {
    if (!t) return;
    for (size_t i = 0; i < OSNOS_MAX_FDS; i++) {
        slot_clear(&t->fds[i]);
    }
    /* Allocate one OFD each for stdin / stdout / stderr — marks them
     * is_special so sys_read/write route to TTY/console. Per-task
     * allocation (not shared across tasks) so a future close-and-
     * reopen on stdout of one task doesn't affect another. */
    for (int fd = 0; fd < 3; fd++) {
        int idx = ofd_alloc();
        if (idx < 0) {
            /* Boot-time failure — leave slot unused. The cmd_test
             * paths that exercise fd-without-task tolerate this. */
            continue;
        }
        osnos_ofd_t *o = &ofd_pool[idx];
        o->is_special = true;
        o->flags      = (fd == 0) ? O_RDONLY : O_WRONLY;
        t->fds[fd].used    = true;
        t->fds[fd].ofd_idx = idx;
    }
}

int fd_alloc(task_t *t) {
    if (!t) return -1;
    /* Find a free slot first; only then alloc the OFD so we don't
     * leak an OFD when the slot table is full. */
    int fd = -1;
    for (int i = 3; i < OSNOS_MAX_FDS; i++) {
        if (!t->fds[i].used) { fd = i; break; }
    }
    if (fd < 0) return -1;

    int idx = ofd_alloc();
    if (idx < 0) return -1;

    t->fds[fd].used     = true;
    t->fds[fd].ofd_idx  = idx;
    t->fds[fd].fd_flags = 0;
    return fd;
}

void fd_free(task_t *t, int fd) {
    if (!t) return;
    if (fd < 0 || fd >= OSNOS_MAX_FDS) return;
    if (!t->fds[fd].used) return;
    int idx = t->fds[fd].ofd_idx;
    slot_clear(&t->fds[fd]);
    if (idx >= 0) ofd_unref(idx);
}

osnos_fd_t *fd_get(task_t *t, int fd) {
    if (!t) return 0;
    if (fd < 0 || fd >= OSNOS_MAX_FDS) return 0;
    if (!t->fds[fd].used) return 0;
    return ofd_get(t->fds[fd].ofd_idx);
}

int fd_get_flags(task_t *t, int fd) {
    if (!t || fd < 0 || fd >= OSNOS_MAX_FDS) return 0;
    if (!t->fds[fd].used) return 0;
    return t->fds[fd].fd_flags;
}

void fd_set_flags(task_t *t, int fd, int flags) {
    if (!t || fd < 0 || fd >= OSNOS_MAX_FDS) return;
    if (!t->fds[fd].used) return;
    t->fds[fd].fd_flags = flags;
}

/* --- dup family --- */

int fd_dup(task_t *t, int src) {
    return fd_dup_min(t, src, 3);
}

int fd_dup_min(task_t *t, int src, int min_fd) {
    if (!t) return -1;
    if (src < 0 || src >= OSNOS_MAX_FDS || !t->fds[src].used) return -1;
    int src_idx = t->fds[src].ofd_idx;
    if (src_idx < 0) return -1;

    if (min_fd < 3) min_fd = 3;
    for (int i = min_fd; i < OSNOS_MAX_FDS; i++) {
        if (!t->fds[i].used) {
            t->fds[i].used     = true;
            t->fds[i].ofd_idx  = src_idx;
            t->fds[i].fd_flags = 0;       /* POSIX: dup clears FD_CLOEXEC */
            ofd_ref(src_idx);
            return i;
        }
    }
    return -1;
}

int fd_dup2(task_t *t, int src, int target) {
    if (!t) return -1;
    if (src    < 0 || src    >= OSNOS_MAX_FDS || !t->fds[src].used) return -1;
    if (target < 0 || target >= OSNOS_MAX_FDS) return -1;
    if (target == src) return target;
    int src_idx = t->fds[src].ofd_idx;
    if (src_idx < 0) return -1;

    /* Close target if open (silent — POSIX). */
    if (t->fds[target].used) {
        int old_idx = t->fds[target].ofd_idx;
        slot_clear(&t->fds[target]);
        if (old_idx >= 0) ofd_unref(old_idx);
    }

    t->fds[target].used     = true;
    t->fds[target].ofd_idx  = src_idx;
    t->fds[target].fd_flags = 0;
    ofd_ref(src_idx);
    return target;
}

/* --- stdin shims over the TTY line discipline --- */
void stdin_push(char c)               { tty_input(c); }
size_t stdin_pop(char *out, size_t m) { return tty_read(out, m); }
bool   stdin_readable(void)           { return tty_readable(); }
void   stdin_clear(void)              { tty_clear(); }

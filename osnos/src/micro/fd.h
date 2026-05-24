#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../include/osnos_limits.h"

/*
 * Per-task file descriptor table + global "open file description"
 * (OFD) pool.
 *
 * Two layers, mirroring Linux:
 *
 *   osnos_fd_slot_t  — the per-task slot. Lives inside task_t.fds[].
 *                      Just `{used, ofd_idx, fd_flags}`. fd_flags holds
 *                      FD_CLOEXEC (per-fd, not shared with dup'd copies).
 *
 *   osnos_ofd_t      — the shared "open file description". Lives in a
 *                      global pool (ofd_pool[]). Holds the actual file
 *                      state: backend (pipe/socket/file), offset,
 *                      flags, path. Refcounted so dup/dup2/fork all
 *                      share state correctly.
 *
 * POSIX dup(2)/dup2(2)/fork(2) semantics:
 *   - dup makes a NEW fd slot pointing at the SAME OFD. Both fds share
 *     offset, flags, position.
 *   - fork inherits: child's fd table copies parent's slot indices,
 *     refcounts on the OFDs bump. Child sees same offset as parent.
 *   - close decrements the OFD refcount; when it hits 0, the OFD's
 *     backend resources are released (pipe_close, sock_close, etc.).
 *
 * For backwards compatibility with code that already does
 *   `osnos_fd_t *f = fd_get(t, fd);  // ... f->offset / f->path ...`,
 * `osnos_fd_t` is a typedef alias for `osnos_ofd_t`. fd_get returns
 * the underlying OFD pointer; callers continue to read the same
 * fields. Only the per-task slot's `used / ofd_idx / fd_flags` lives
 * separately.
 */
#define OSNOS_MAX_FDS    16
#define OSNOS_MAX_OFDS   128

#define OSNOS_FD_STDIN   0
#define OSNOS_FD_STDOUT  1
#define OSNOS_FD_STDERR  2

/* fd_flags bits — per-fd (not shared via OFD). */
#define OSNOS_FD_CLOEXEC 0x1

struct pipe;
struct pty_pair;
struct shm_obj;

/* The shared open file description. Refcounted. */
typedef struct osnos_ofd {
    bool         used;
    int          refcount;            /* # of slots referencing this OFD */

    bool         is_special;          /* stdin/stdout/stderr default */
    bool         is_dir;              /* opened a directory (getdents) */
    bool         is_socket;           /* opened via sys_socket (AF_INET) */
    bool         is_unix_socket;      /* AF_UNIX socket — unix_idx valido */
    bool         is_pipe;             /* opened via sys_pipe / inherited */
    bool         is_chr;              /* character device */
    bool         is_pty;              /* /dev/ptmx or /dev/pts/N */
    bool         is_shm;              /* shm_open() — shm_ref valido */

    int          sock_idx;            /* index into net/socket table */
    int          unix_idx;            /* index into unix_sock table */
    struct shm_obj *shm_ref;          /* shared memory object handle */
    struct pipe *pipe_ref;            /* shared pipe object */
    int          pipe_side;           /* 0 = read end, 1 = write end */

    struct pty_pair *pty_ref;         /* shared pty pair, when is_pty */
    int              pty_side;        /* 0 = master, 1 = slave */

    int          flags;               /* O_RDONLY etc. */
    uint64_t     offset;              /* file position OR readdir cursor */
    char         path[OSNOS_PATH_MAX];
} osnos_ofd_t;

/* Per-task slot. Thin. */
typedef struct osnos_fd_slot {
    bool used;
    int  ofd_idx;                     /* index into ofd_pool, -1 if !used */
    int  fd_flags;                    /* FD_CLOEXEC etc */
} osnos_fd_slot_t;

/* Backwards compat: legacy `osnos_fd_t` is the OFD struct. Callers
 * doing `osnos_fd_t *f = fd_get(t, fd); f->offset` keep working. */
typedef osnos_ofd_t osnos_fd_t;

struct task;
typedef struct task task_t;

/* --- Per-task fd-slot API --- */

void fd_init_for_task(task_t *t);

/* Allocate a fresh fd >= 3 with a freshly-allocated OFD (refcount=1).
 * Returns the fd, or -1 if either pool is full. */
int  fd_alloc(task_t *t);

/* Release the slot AND decrement the OFD refcount (which may trigger
 * pipe_close / sock_close if it hits 0). */
void fd_free(task_t *t, int fd);

/* Return the OFD backing this fd (NULL for invalid / unused). */
osnos_fd_t *fd_get(task_t *t, int fd);

/* Per-fd flags (FD_CLOEXEC). NOT shared via OFD. */
int  fd_get_flags(task_t *t, int fd);
void fd_set_flags(task_t *t, int fd, int flags);

/*
 * dup family — all share the underlying OFD. Bumps OFD refcount.
 *
 *   fd_dup(t, src)           — find any free fd >= 3 (F_DUPFD with min 3)
 *   fd_dup_min(t, src, min)  — find the lowest free fd >= min
 *   fd_dup2(t, src, target)  — copy to specific target; closes target if
 *                              already open. src==target is a no-op.
 *
 * Per POSIX, FD_CLOEXEC is NOT inherited by dup/dup2 (cleared on the
 * new fd). F_DUPFD_CLOEXEC variants can be added later.
 */
int fd_dup     (task_t *t, int src);
int fd_dup_min (task_t *t, int src, int min_fd);
int fd_dup2    (task_t *t, int src, int target);

/* --- OFD pool API (mostly internal; sys_fork / sys_spawn use these) --- */

osnos_ofd_t *ofd_get(int idx);

/* Allocate a fresh OFD slot, refcount=1, fields zeroed. Returns the
 * pool index, or -1 if the pool is full. */
int  ofd_alloc(void);

/* Increment refcount (used when dup'ing or forking). */
void ofd_ref(int idx);

/* Decrement refcount. If it hits 0, release the OFD's backend
 * resources (pipe / socket close) and recycle the pool slot. */
void ofd_unref(int idx);

/* --- stdin shims over the TTY line discipline --- */
void   stdin_push(char c);
size_t stdin_pop (char *out, size_t max);
bool   stdin_readable(void);
void   stdin_clear(void);

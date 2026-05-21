#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../include/osnos_limits.h"

/*
 * Per-task file descriptor table. Each task owns its OSNOS_MAX_FDS
 * entries inside task_t.fds[]; this header exposes the per-slot
 * struct + the operations that act on a given task's table.
 *
 * Layout per task:
 *   fd 0 = stdin
 *   fd 1 = stdout
 *   fd 2 = stderr
 *   fd 3+ = regular files, allocated by sys_open
 *
 * Kernel-resident "tasks" (servers) get the same layout — their
 * stdin/stdout/stderr slots are wired to the TTY/console even though
 * they normally don't issue read/write syscalls.
 */
#define OSNOS_MAX_FDS    16

#define OSNOS_FD_STDIN   0
#define OSNOS_FD_STDOUT  1
#define OSNOS_FD_STDERR  2

struct pipe;

typedef struct {
    bool     used;
    bool     is_special;             /* stdin/stdout/stderr */
    bool     is_dir;                 /* opened a directory (for getdents) */
    bool     is_socket;              /* opened via sys_socket */
    bool     is_pipe;                /* opened via sys_pipe / inherited from shell */
    bool     is_chr;                 /* character device (/dev/fb0, /dev/input0, ...) */
    int      sock_idx;               /* index into net/socket table, when is_socket */
    struct pipe *pipe_ref;           /* shared pipe object, when is_pipe */
    int      pipe_side;              /* 0 = read end, 1 = write end (when is_pipe) */
    int      flags;                  /* O_RDONLY etc. */
    uint64_t offset;                 /* file position OR readdir cursor */
    char     path[OSNOS_PATH_MAX];
} osnos_fd_t;

/* Forward decl — fd.c only ever dereferences task_t through task->fds. */
struct task;
typedef struct task task_t;

/*
 * Reset `t->fds` to a freshly-initialised state with stdin/stdout/
 * stderr pre-wired. Called by task_create for every new task slot.
 */
void fd_init_for_task(task_t *t);

/* Allocate a fresh fd >= 3 in task `t`. Returns -1 if the table is full. */
int  fd_alloc(task_t *t);

void fd_free(task_t *t, int fd);

/* Look up an fd in task `t`. Returns NULL for invalid / unused fds.
 * Includes the special fds 0/1/2 (which always exist). */
osnos_fd_t *fd_get(task_t *t, int fd);

/*
 * Duplicate an existing fd into a fresh slot within the SAME task.
 * Cross-task dup is not exposed — every dup variant operates on `t`.
 *
 *   fd_dup(t, src)           — find any free fd >= 3.
 *   fd_dup_min(t, src, min)  — find the lowest free fd >= min (F_DUPFD).
 *   fd_dup2(t, src, target)  — copy into specific target; closes target
 *                              first if it was open. src==target is a
 *                              no-op (POSIX).
 *
 * All three return the new fd on success, -1 on failure.
 */
int fd_dup     (task_t *t, int src);
int fd_dup_min (task_t *t, int src, int min_fd);
int fd_dup2    (task_t *t, int src, int target);

/*
 * stdin ring buffer. keyboard_server pushes printable chars and '\n';
 * sys_read(0, ...) pops. Bounded — overflow drops oldest. Non-blocking.
 */
void   stdin_push(char c);
size_t stdin_pop (char *out, size_t max);

/* True when stdin has bytes buffered ready for sys_read to drain. */
bool   stdin_readable(void);

/* Drop everything queued in the ring buffer. Called on exec so a child
 * task starts with a clean stdin instead of inheriting the keystrokes
 * that typed its own command line. */
void   stdin_clear(void);

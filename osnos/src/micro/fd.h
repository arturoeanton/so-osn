#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../include/osnos_limits.h"

/*
 * Global file descriptor table. Lives in kernel for now; per-task FD
 * spaces arrive with FASE 6 (ELF/ring3 + processes).
 *
 * Layout:
 *   fd 0 = stdin   (read from keyboard, TODO)
 *   fd 1 = stdout  (write to console)
 *   fd 2 = stderr  (write to console, distinguishable later)
 *   fd 3+ = regular files, allocated by sys_open
 */
#define OSNOS_MAX_FDS    16

#define OSNOS_FD_STDIN   0
#define OSNOS_FD_STDOUT  1
#define OSNOS_FD_STDERR  2

typedef struct {
    bool     used;
    bool     is_special;             /* stdin/stdout/stderr */
    bool     is_dir;                 /* opened a directory (for getdents) */
    bool     is_socket;              /* opened via sys_socket */
    int      sock_idx;               /* index into net/socket table, when is_socket */
    int      flags;                  /* O_RDONLY etc. */
    uint64_t offset;                 /* file position OR readdir cursor */
    char     path[OSNOS_PATH_MAX];
} osnos_fd_t;

void fd_init(void);

/* Allocate a fresh fd >= 3. Returns -1 if the table is full. */
int  fd_alloc(void);

void fd_free(int fd);

/* Look up an fd. Returns NULL for invalid / unused fds. Includes the
 * special fds 0/1/2 (which always exist). */
osnos_fd_t *fd_get(int fd);

/*
 * Duplicate an existing fd into a fresh slot. The clone gets a copy
 * of the source's flags, path, type bits, and (importantly) a
 * snapshot of its current offset — i.e. they don't share an "open
 * file description". POSIX-strict dup expects shared offsets; that
 * needs a separate file-description refcount layer (TODO).
 *
 *   fd_dup(src)           — find any free fd >= 3.
 *   fd_dup_min(src, min)  — find the lowest free fd >= min (F_DUPFD).
 *   fd_dup2(src, target)  — copy into specific target; closes target
 *                            first if it was open. src==target is a
 *                            no-op (POSIX).
 *
 * All three return the new fd on success, -1 on failure.
 */
int fd_dup     (int src);
int fd_dup_min (int src, int min_fd);
int fd_dup2    (int src, int target);

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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Kernel pipe object — used by the shell to wire `cmd1 | cmd2`.
 *
 * Each pipe owns a 4 KiB ring buffer plus two refcounts (one for
 * the read end, one for the write end). The writer's sys_write(1)
 * pushes bytes into the ring; the reader's sys_read(0) pulls them
 * out. Tasks block via the libc EAGAIN+nanosleep loop, same as
 * sockets and the TTY.
 *
 * Lifetimes:
 *   - pipe_create() reserves a slot, sets ref_w = ref_r = 1.
 *   - When the writer task exits, proc_exit_current_user calls
 *     pipe_close_writer; readers see EOF (sys_read returns 0).
 *   - When the reader exits, pipe_close_reader; writers see EPIPE.
 *   - The slot is freed back to the pool when both ends are closed.
 *
 * The fd table stays global today so we don't expose sys_pipe(2)
 * to user code yet — pipes are an internal mechanism between the
 * shell-spawned children. When per-task fd tables land, exposing
 * `pipe()` will be a thin wrapper over this layer.
 */

#define PIPE_BUF_SIZE  4096
#define MAX_PIPES       16

typedef struct pipe {
    uint8_t  buf[PIPE_BUF_SIZE];
    size_t   head;        /* write cursor (mod PIPE_BUF_SIZE) */
    size_t   tail;        /* read cursor  (mod PIPE_BUF_SIZE) */
    size_t   level;       /* bytes currently in buffer       */
    int      ref_w;       /* writers alive (1 while shell holds it) */
    int      ref_r;       /* readers alive                            */
    bool     used;
} pipe_t;

void    pipe_init(void);

/* Allocate a fresh pipe with both endpoints "open" (ref_w = ref_r = 1).
 * Returns NULL when the pool is empty. */
pipe_t *pipe_create(void);

/* Append-side and consume-side. Both are non-blocking: the libc
 * loops on EAGAIN. Conventions:
 *   pipe_write returns bytes accepted (>= 0). -EPIPE when readers
 *     are gone (Linux gives EPIPE + raises SIGPIPE; we just return
 *     the negated value). -EAGAIN if full and writers still around.
 *   pipe_read returns bytes consumed. 0 means "writers gone, buffer
 *     empty" → EOF. -EAGAIN if empty but writer alive. */
int64_t pipe_write(pipe_t *p, const void *buf, size_t n);
int64_t pipe_read (pipe_t *p,       void *buf, size_t n);

/* Decrement refcounts; auto-frees the slot when both reach 0. */
void    pipe_close_writer(pipe_t *p);
void    pipe_close_reader(pipe_t *p);

/* Bump refcounts when a new task gets a copy of the fd (used by
 * sys_fork). dup(2) and dup2(2) ALSO use these so the per-task fd
 * table's pipe slot count stays in sync with the kernel pipe's
 * ref_w / ref_r counters. */
void    pipe_dup_writer(pipe_t *p);
void    pipe_dup_reader(pipe_t *p);

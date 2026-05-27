#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tty.h"

/*
 * Pseudoterminal pairs (POSIX /dev/ptmx + /dev/pts/N).
 *
 * A `pty_pair_t` is the kernel object behind one /dev/pts/N. It owns
 * two 4 KiB ring buffers:
 *   m2s_buf — bytes the MASTER writes; the SLAVE reads from here.
 *             This is the "input side" of the terminal.
 *   s2m_buf — bytes the SLAVE writes; the MASTER reads from here.
 *             This is the "output side" of the terminal.
 *
 * Each pair has its own termios (independent from the kernel TTY).
 * Canonical mode line-buffers SLAVE reads until a '\n' arrives;
 * ECHO causes MASTER writes to also appear on the s2m_buf so the
 * user sees what they typed.
 *
 * Refcounts:
 *   master_refs — # of OFDs pointing at this pair's master side.
 *   slave_refs  — # of OFDs pointing at this pair's slave side.
 *   The pair is freed back to the pool only when BOTH reach 0.
 *
 * Lifecycle:
 *   1. open("/dev/ptmx") → pty_alloc() → master_refs=1, slave_refs=0,
 *      returns master fd via OFD.
 *   2. User calls ptsname() (= ioctl TIOCGPTN) → gets "/dev/pts/<N>".
 *   3. open("/dev/pts/N") → slave_refs++, returns slave fd via OFD.
 *   4. Writes / reads route through pty_*_read / pty_*_write below.
 *   5. Each close decrements its side's refcount; when both reach 0
 *      the pair returns to the pool.
 */

#define MAX_PTYS         8
#define PTY_BUF_SIZE     4096
#define PTY_CANON_BUF    256

typedef struct pty_pair {
    bool                 used;
    int                  index;          /* /dev/pts/<index> */

    /* master → slave (input the user types) */
    uint8_t              m2s_buf[PTY_BUF_SIZE];
    size_t               m2s_head, m2s_tail, m2s_level;

    /* slave → master (output the slave program writes) */
    uint8_t              s2m_buf[PTY_BUF_SIZE];
    size_t               s2m_head, s2m_tail, s2m_level;

    /* Termios per pair. ICANON / ECHO live in c_lflag — they're
     * what tells pty_slave_read whether to line-buffer or pass
     * each byte through, and whether master_write should echo. */
    struct osnos_termios termios;

    /* Canonical-mode accumulator. Bytes arriving from the master
     * sit here until '\n' is seen; then the whole line moves into
     * m2s_buf as one chunk, available to slave_read. */
    uint8_t              canon_buf[PTY_CANON_BUF];
    size_t               canon_level;

    /* Side refcounts (tracked separately from OFD refcount so the
     * kernel can free the pair only when BOTH ends are closed). */
    int                  master_refs;
    int                  slave_refs;

    /* Has the slave EVER been opened? Until the first open(/dev/pts/N)
     * succeeds, master reads must NOT return EOF on an empty s2m —
     * the slave just hasn't started yet (typical fork+exec race:
     * parent's select can fire before child reaches open(slave)).
     * EOF semantics: only after slave_was_opened becomes true AND
     * slave_refs drops back to 0 do we report EOF on master_read. */
    bool                 slave_was_opened;

    /* Foreground process group id on this PTY pair. Updated by
     * tcsetpgrp(slave_fd, pgid) ioctl. Queried by tcgetpgrp(slave_fd).
     * The kernel itself doesn't act on this (no SIGTTIN/TTOU enforcement
     * here yet), but busybox ash's startup loop polls tcgetpgrp/getpgid
     * and refuses to advance until they match — so we must reflect
     * whatever the child set. Zero means "no fg pgid set" (treated as
     * "matches the caller" by the GET path to keep ash happy). */
    uint64_t             fg_pgid;
} pty_pair_t;

/* Boot-time pool init — zero all slots. Call once from kmain. */
void        pty_init(void);

/* Allocate a fresh pair (called by /dev/ptmx open). Returns the
 * pair pointer with master_refs=1, slave_refs=0. NULL if pool full. */
pty_pair_t *pty_alloc(void);

/* Look up the pair by index (used by /dev/pts/N open). NULL if
 * out-of-range or slot unused. */
pty_pair_t *pty_get(int index);

/* Refcount manipulation — called by OFD lifecycle (ofd_unref). */
void        pty_master_ref(pty_pair_t *p);
void        pty_master_unref(pty_pair_t *p);
void        pty_slave_ref(pty_pair_t *p);
void        pty_slave_unref(pty_pair_t *p);

/* IO primitives. All non-blocking; libc loops via nanosleep when
 * EAGAIN is returned. */
int64_t     pty_master_write(pty_pair_t *p, const void *buf, size_t n);
int64_t     pty_master_read (pty_pair_t *p,       void *buf, size_t n);
int64_t     pty_slave_write (pty_pair_t *p, const void *buf, size_t n);
int64_t     pty_slave_read  (pty_pair_t *p,       void *buf, size_t n);

/* Readiness predicates for select(). */
bool        pty_master_readable(pty_pair_t *p);
bool        pty_slave_readable (pty_pair_t *p);

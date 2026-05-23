#include "pty.h"

#include "../include/osnos_status.h"

static pty_pair_t pty_pool[MAX_PTYS];

/* Reset a slot to clean state — ring buffers + canon + termios. */
static void pty_clear(pty_pair_t *p) {
    p->used        = false;
    p->index       = -1;
    p->m2s_head = p->m2s_tail = p->m2s_level = 0;
    p->s2m_head = p->s2m_tail = p->s2m_level = 0;
    p->canon_level = 0;
    p->master_refs = 0;
    p->slave_refs  = 0;

    /* Default termios: same as the kernel TTY does at boot — ICANON
     * + ECHO + ISIG, c_cc seeded with the classic control chars. The
     * caller can tcsetattr to switch to raw mode. */
    p->termios.c_iflag = TTY_ICRNL;
    p->termios.c_oflag = 0;
    p->termios.c_cflag = 0;
    p->termios.c_lflag = TTY_ICANON | TTY_ECHO | TTY_ECHOE | TTY_ISIG;
    p->termios.c_line  = 0;
    for (size_t i = 0; i < NCCS; i++) p->termios.c_cc[i] = 0;
    p->termios.c_cc[TTY_VINTR]  = 0x03;   /* Ctrl+C */
    p->termios.c_cc[TTY_VQUIT]  = 0x1c;
    p->termios.c_cc[TTY_VERASE] = 0x7f;   /* DEL */
    p->termios.c_cc[TTY_VKILL]  = 0x15;   /* Ctrl+U */
    p->termios.c_cc[TTY_VEOF]   = 0x04;   /* Ctrl+D */
    p->termios.c_cc[TTY_VSUSP]  = 0x1a;   /* Ctrl+Z */
}

void pty_init(void) {
    for (int i = 0; i < MAX_PTYS; i++) {
        pty_clear(&pty_pool[i]);
        pty_pool[i].index = i;
    }
}

pty_pair_t *pty_alloc(void) {
    for (int i = 0; i < MAX_PTYS; i++) {
        if (!pty_pool[i].used) {
            pty_clear(&pty_pool[i]);
            pty_pool[i].used        = true;
            pty_pool[i].index       = i;
            pty_pool[i].master_refs = 1;     /* caller of /dev/ptmx */
            pty_pool[i].slave_refs  = 0;
            return &pty_pool[i];
        }
    }
    return 0;
}

pty_pair_t *pty_get(int index) {
    if (index < 0 || index >= MAX_PTYS) return 0;
    if (!pty_pool[index].used) return 0;
    return &pty_pool[index];
}

/* If both side refcounts have reached 0, free the slot back to the
 * pool. Called from each unref path. */
static void pty_release_if_orphan(pty_pair_t *p) {
    if (p->master_refs == 0 && p->slave_refs == 0) {
        pty_clear(p);
    }
}

void pty_master_ref(pty_pair_t *p)   { if (p) p->master_refs++; }
void pty_master_unref(pty_pair_t *p) {
    if (!p) return;
    if (p->master_refs > 0) p->master_refs--;
    pty_release_if_orphan(p);
}
void pty_slave_ref(pty_pair_t *p)    { if (p) p->slave_refs++; }
void pty_slave_unref(pty_pair_t *p)  {
    if (!p) return;
    if (p->slave_refs > 0) p->slave_refs--;
    pty_release_if_orphan(p);
}

/* ---- ring buffer helpers ---- */

static int64_t ring_write(uint8_t *buf, size_t *head, size_t *level,
                           size_t cap, const uint8_t *src, size_t n) {
    size_t can_write = cap - *level;
    if (can_write == 0) return -(int64_t)OSNOS_EAGAIN;
    if (n > can_write) n = can_write;
    for (size_t i = 0; i < n; i++) {
        buf[*head] = src[i];
        *head = (*head + 1) % cap;
    }
    *level += n;
    return (int64_t)n;
}

static int64_t ring_read(uint8_t *buf, size_t *tail, size_t *level,
                          size_t cap, uint8_t *dst, size_t n) {
    if (*level == 0) return -(int64_t)OSNOS_EAGAIN;
    if (n > *level) n = *level;
    for (size_t i = 0; i < n; i++) {
        dst[i] = buf[*tail];
        *tail = (*tail + 1) % cap;
    }
    *level -= n;
    return (int64_t)n;
}

/* ---- canonical-mode line accumulator ---- */

/* Returns true if the canon_buf currently holds a complete line
 * (i.e. a '\n' is queued somewhere in it). slave_read drains a line
 * at a time when this is true. */
static bool canon_line_ready(const pty_pair_t *p) {
    for (size_t i = 0; i < p->canon_level; i++) {
        if (p->canon_buf[i] == '\n') return true;
    }
    return false;
}

/* Move one complete canonical line (up to and including '\n') from
 * canon_buf to m2s_buf so slave_read can consume it. */
static void canon_flush_line(pty_pair_t *p) {
    /* Find the '\n' position. */
    size_t nl = 0;
    while (nl < p->canon_level && p->canon_buf[nl] != '\n') nl++;
    if (nl >= p->canon_level) return;
    size_t take = nl + 1;   /* include the '\n' */

    /* Push to m2s_buf. If m2s_buf is full, leave the line in
     * canon_buf — slave_read will drain m2s_buf eventually and a
     * future master_write will trigger this flush again. */
    int64_t r = ring_write(p->m2s_buf, &p->m2s_head, &p->m2s_level,
                            PTY_BUF_SIZE, p->canon_buf, take);
    if (r <= 0) return;
    /* Shift the remainder forward. */
    for (size_t i = 0; i + take < p->canon_level; i++) {
        p->canon_buf[i] = p->canon_buf[i + take];
    }
    p->canon_level -= take;
}

/* ---- input processing (master_write) ---- */

int64_t pty_master_write(pty_pair_t *p, const void *buf, size_t n) {
    if (!p || !buf) return -(int64_t)OSNOS_EFAULT;
    if (!p->used) return -(int64_t)OSNOS_EBADF;
    const uint8_t *src = (const uint8_t *)buf;
    bool canon = (p->termios.c_lflag & TTY_ICANON) != 0;
    bool echo  = (p->termios.c_lflag & TTY_ECHO)   != 0;

    size_t written = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = src[i];

        /* Echo to s2m_buf so the master read-side (typically a
         * terminal emulator) shows what the user typed. Only in
         * canonical mode by default — programs that disable ECHO
         * (passwords, raw-mode editors) get silent input. */
        if (echo) {
            ring_write(p->s2m_buf, &p->s2m_head, &p->s2m_level,
                       PTY_BUF_SIZE, &c, 1);
        }

        if (canon) {
            /* Erase: delete last char in canon_buf, echo BS-space-BS
             * so the master sees the cursor go back. */
            if (c == p->termios.c_cc[TTY_VERASE]) {
                if (p->canon_level > 0) {
                    p->canon_level--;
                    if (echo) {
                        const uint8_t bs_seq[] = "\b \b";
                        ring_write(p->s2m_buf, &p->s2m_head,
                                   &p->s2m_level, PTY_BUF_SIZE,
                                   bs_seq, 3);
                    }
                }
                continue;
            }
            /* Append to canon_buf. If full, drop silently (real Linux
             * TTY does the same — overflow can't propagate as EAGAIN
             * in canon mode). */
            if (p->canon_level < PTY_CANON_BUF) {
                p->canon_buf[p->canon_level++] = c;
            }
            /* '\n' commits the line into m2s_buf. */
            if (c == '\n') {
                canon_flush_line(p);
            }
        } else {
            /* Raw mode: byte goes straight to m2s_buf. */
            int64_t r = ring_write(p->m2s_buf, &p->m2s_head,
                                    &p->m2s_level, PTY_BUF_SIZE,
                                    &c, 1);
            if (r <= 0) break;
        }
        written++;
    }
    if (written == 0) return -(int64_t)OSNOS_EAGAIN;
    return (int64_t)written;
}

int64_t pty_slave_read(pty_pair_t *p, void *buf, size_t n) {
    if (!p || !buf) return -(int64_t)OSNOS_EFAULT;
    if (!p->used) return -(int64_t)OSNOS_EBADF;

    /* If master is gone and no bytes pending → EOF (return 0). */
    if (p->master_refs == 0 && p->m2s_level == 0 &&
        p->canon_level == 0) {
        return 0;
    }

    bool canon = (p->termios.c_lflag & TTY_ICANON) != 0;
    if (canon) {
        /* Canon mode: only return a full line. If m2s_buf has
         * a line ready, drain. Otherwise check canon_buf and
         * flush if complete; otherwise EAGAIN. */
        if (p->m2s_level == 0 && canon_line_ready(p)) {
            canon_flush_line(p);
        }
    }
    return ring_read(p->m2s_buf, &p->m2s_tail, &p->m2s_level,
                     PTY_BUF_SIZE, (uint8_t *)buf, n);
}

int64_t pty_slave_write(pty_pair_t *p, const void *buf, size_t n) {
    if (!p || !buf) return -(int64_t)OSNOS_EFAULT;
    if (!p->used) return -(int64_t)OSNOS_EBADF;

    /* If master closed, writes raise EPIPE (Linux semantic). */
    if (p->master_refs == 0) return -(int64_t)OSNOS_EPIPE;

    /* Output processing: ONLCR converts '\n' → '\r\n'. Default
     * is OFF; programs that want CRLF set c_oflag |= ONLCR via
     * tcsetattr. We keep this minimal — just literal byte copy. */
    return ring_write(p->s2m_buf, &p->s2m_head, &p->s2m_level,
                      PTY_BUF_SIZE, (const uint8_t *)buf, n);
}

int64_t pty_master_read(pty_pair_t *p, void *buf, size_t n) {
    if (!p || !buf) return -(int64_t)OSNOS_EFAULT;
    if (!p->used) return -(int64_t)OSNOS_EBADF;
    /* Master sees raw bytes from s2m_buf. EOF when slave is closed
     * AND buffer empty. */
    if (p->slave_refs == 0 && p->s2m_level == 0) return 0;
    return ring_read(p->s2m_buf, &p->s2m_tail, &p->s2m_level,
                     PTY_BUF_SIZE, (uint8_t *)buf, n);
}

bool pty_master_readable(pty_pair_t *p) {
    if (!p || !p->used) return false;
    if (p->s2m_level > 0) return true;
    /* EOF counts as readable per POSIX (read returns 0). */
    if (p->slave_refs == 0) return true;
    return false;
}

bool pty_slave_readable(pty_pair_t *p) {
    if (!p || !p->used) return false;
    if (p->m2s_level > 0) return true;
    if ((p->termios.c_lflag & TTY_ICANON) == 0) {
        /* raw mode: any byte counts */
        return false;
    }
    /* canon mode: complete line in canon_buf counts */
    if (canon_line_ready(p)) return true;
    if (p->master_refs == 0) return true;   /* EOF */
    return false;
}

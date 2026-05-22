#include "pipe.h"

#include "../include/osnos_status.h"

static pipe_t pipes[MAX_PIPES];

void pipe_init(void) {
    for (size_t i = 0; i < MAX_PIPES; i++) {
        pipes[i].used  = false;
        pipes[i].head  = 0;
        pipes[i].tail  = 0;
        pipes[i].level = 0;
        pipes[i].ref_w = 0;
        pipes[i].ref_r = 0;
    }
}

pipe_t *pipe_create(void) {
    for (size_t i = 0; i < MAX_PIPES; i++) {
        if (!pipes[i].used) {
            pipes[i].used  = true;
            pipes[i].head  = 0;
            pipes[i].tail  = 0;
            pipes[i].level = 0;
            pipes[i].ref_w = 1;
            pipes[i].ref_r = 1;
            return &pipes[i];
        }
    }
    return 0;
}

int64_t pipe_write(pipe_t *p, const void *buf, size_t n) {
    if (!p || !p->used) return -(int64_t)OSNOS_EBADF;
    if (n == 0) return 0;
    if (p->ref_r == 0) return -(int64_t)OSNOS_EPIPE;

    size_t free_space = PIPE_BUF_SIZE - p->level;
    if (free_space == 0) return -(int64_t)OSNOS_EAGAIN;

    size_t to_write = n < free_space ? n : free_space;
    const uint8_t *src = (const uint8_t *)buf;
    for (size_t i = 0; i < to_write; i++) {
        p->buf[p->head] = src[i];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
    }
    p->level += to_write;
    return (int64_t)to_write;
}

int64_t pipe_read(pipe_t *p, void *buf, size_t n) {
    if (!p || !p->used) return -(int64_t)OSNOS_EBADF;
    if (n == 0) return 0;

    if (p->level == 0) {
        /* Buffer drained. EOF iff writers are gone. */
        if (p->ref_w == 0) return 0;
        return -(int64_t)OSNOS_EAGAIN;
    }

    size_t to_read = n < p->level ? n : p->level;
    uint8_t *dst = (uint8_t *)buf;
    for (size_t i = 0; i < to_read; i++) {
        dst[i] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
    }
    p->level -= to_read;
    return (int64_t)to_read;
}

static void pipe_free_if_orphan(pipe_t *p) {
    if (p->ref_w == 0 && p->ref_r == 0) {
        p->used  = false;
        p->level = 0;
    }
}

void pipe_close_writer(pipe_t *p) {
    if (!p || !p->used) return;
    if (p->ref_w > 0) p->ref_w--;
    pipe_free_if_orphan(p);
}

void pipe_close_reader(pipe_t *p) {
    if (!p || !p->used) return;
    if (p->ref_r > 0) p->ref_r--;
    pipe_free_if_orphan(p);
}

void pipe_dup_writer(pipe_t *p) {
    if (!p || !p->used) return;
    p->ref_w++;
}

void pipe_dup_reader(pipe_t *p) {
    if (!p || !p->used) return;
    p->ref_r++;
}

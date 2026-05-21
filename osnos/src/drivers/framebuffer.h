#pragma once

#include <stdint.h>
#include <stddef.h>

void framebuffer_init(
    void *address,
    uint64_t width,
    uint64_t height,
    uint64_t pitch
);

void framebuffer_clear(uint32_t color);

void framebuffer_putpixel(
    size_t x,
    size_t y,
    uint32_t color
);

void framebuffer_draw_char(
    char c,
    size_t x,
    size_t y,
    uint32_t color
);

void framebuffer_draw_string(
    const char *s,
    uint32_t color
);

/*
 * Write `n` raw bytes (NUL-tolerant). For /dev/fb0 — user code may
 * write any byte sequence; we forward to draw_string in chunks via a
 * NUL-terminated stack buffer. Embedded NULs are skipped so they
 * don't terminate a chunk early.
 */
void framebuffer_write_bytes(
    const char *buf,
    size_t n,
    uint32_t color
);

void framebuffer_backspace(void);

/* Visible terminal area in CHARACTERS (after margins). Used by
 * /bin/ovi and other TUIs via ioctl(TIOCGWINSZ). */
unsigned short framebuffer_cols(void);
unsigned short framebuffer_rows(void);

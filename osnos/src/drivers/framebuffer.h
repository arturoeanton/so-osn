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

/* ---------------- Pixel-level API (FASE 12 — ox WM) -------------- */
/*
 * Geometry inspector. All-zero outputs if framebuffer_init never ran.
 * pitch_bytes is the raw byte stride between rows.
 */
void framebuffer_get_info(
    uint32_t *out_width,
    uint32_t *out_height,
    uint32_t *out_pitch_bytes,
    uint32_t *out_bpp
);

/*
 * Blit a rectangle of BGRA pixels straight into the framebuffer.
 * `src` is a KERNEL pointer (caller copy_from_user's user data into a
 * scratch buffer first if needed). Clips to screen bounds. No alpha
 * blending; pixels are written verbatim.
 */
void framebuffer_blit_kernel(
    uint32_t x, uint32_t y,
    uint32_t w, uint32_t h,
    const void *src,
    uint32_t src_pitch_bytes
);

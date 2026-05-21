#include "framebuffer.h"

#include <stdint.h>
#include <stddef.h>

#include "../include/font.h"

#define TERM_MARGIN_X 16
#define TERM_MARGIN_Y 16
#define CHAR_WIDTH    8
#define CHAR_HEIGHT   16
#define GLYPH_HEIGHT  8
#define TAB_WIDTH     4

static volatile uint32_t *fb = 0;

static uint64_t fb_width = 0;
static uint64_t fb_height = 0;
static uint64_t fb_pitch = 0;

static size_t cursor_x = TERM_MARGIN_X;
static size_t cursor_y = TERM_MARGIN_Y;

static uint32_t bg_color = 0x000000;

static void framebuffer_scroll(void) {
    if (fb_height <= CHAR_HEIGHT) {
        return;
    }

    for (size_t y = TERM_MARGIN_Y + CHAR_HEIGHT; y < fb_height; y++) {
        for (size_t x = 0; x < fb_width; x++) {
            fb[(y - CHAR_HEIGHT) * fb_pitch + x] =
                fb[y * fb_pitch + x];
        }
    }

    size_t clear_start = fb_height - CHAR_HEIGHT;

    for (size_t y = clear_start; y < fb_height; y++) {
        for (size_t x = 0; x < fb_width; x++) {
            fb[y * fb_pitch + x] = bg_color;
        }
    }

    if (cursor_y >= CHAR_HEIGHT) {
        cursor_y -= CHAR_HEIGHT;
    } else {
        cursor_y = TERM_MARGIN_Y;
    }
}

static void framebuffer_newline(void) {
    cursor_x = TERM_MARGIN_X;
    cursor_y += CHAR_HEIGHT;

    if (cursor_y + CHAR_HEIGHT >= fb_height) {
        framebuffer_scroll();
    }
}

static void framebuffer_clear_cell(size_t x, size_t y) {
    for (size_t yy = 0; yy < CHAR_HEIGHT; yy++) {
        for (size_t xx = 0; xx < CHAR_WIDTH; xx++) {
            framebuffer_putpixel(x + xx, y + yy, bg_color);
        }
    }
}

void framebuffer_init(
    void *address,
    uint64_t width,
    uint64_t height,
    uint64_t pitch
) {
    fb = (volatile uint32_t *)address;

    fb_width = width;
    fb_height = height;
    fb_pitch = pitch / 4;

    framebuffer_clear(bg_color);
}

void framebuffer_clear(uint32_t color) {
    bg_color = color;

    for (size_t y = 0; y < fb_height; y++) {
        for (size_t x = 0; x < fb_width; x++) {
            fb[y * fb_pitch + x] = color;
        }
    }

    cursor_x = TERM_MARGIN_X;
    cursor_y = TERM_MARGIN_Y;
}

void framebuffer_putpixel(
    size_t x,
    size_t y,
    uint32_t color
) {
    if (!fb) {
        return;
    }

    if (x >= fb_width || y >= fb_height) {
        return;
    }

    fb[y * fb_pitch + x] = color;
}

/* SGR state: when reverse is on, draw_char fills the cell with the
 * caller-supplied color and renders the glyph in the background
 * color. Used as the editor cursor highlight. */
static int sgr_reverse = 0;

void framebuffer_draw_char(
    char c,
    size_t x,
    size_t y,
    uint32_t color
) {
    if ((uint8_t)c >= 128) {
        return;
    }

    const uint8_t *glyph = font8x8_basic[(uint8_t)c];

    if (sgr_reverse) {
        /* Solid fg block, glyph in bg color. */
        for (size_t row = 0; row < CHAR_HEIGHT; row++) {
            for (size_t col = 0; col < CHAR_WIDTH; col++) {
                framebuffer_putpixel(x + col, y + row, color);
            }
        }
        for (size_t row = 0; row < GLYPH_HEIGHT; row++) {
            for (size_t col = 0; col < CHAR_WIDTH; col++) {
                if (glyph[row] & (1 << (7 - col))) {
                    framebuffer_putpixel(x + col, y + row, bg_color);
                }
            }
        }
        return;
    }

    framebuffer_clear_cell(x, y);

    for (size_t row = 0; row < GLYPH_HEIGHT; row++) {
        for (size_t col = 0; col < CHAR_WIDTH; col++) {
            if (glyph[row] & (1 << (7 - col))) {
                framebuffer_putpixel(
                    x + col,
                    y + row,
                    color
                );
            }
        }
    }
}

void framebuffer_draw_string(
    const char *s,
    uint32_t color
) {
    while (*s) {
        /*
         * Minimal ANSI CSI handling — enough to drive a TUI editor.
         * Supports:
         *   ESC [ 2 J         — clear screen, cursor unchanged
         *   ESC [ J           — same as 2 J (legacy)
         *   ESC [ H           — cursor to home (top-left margin)
         *   ESC [ row ; col H — cursor positioning, 1-based
         *   ESC [ K           — clear from cursor to end of line
         * Anything else after ESC [ is silently consumed up to the
         * final letter.
         */
        if (*s == 0x1b && s[1] == '[') {
            s += 2;
            int p1 = 0, p2 = 0;
            int have_p1 = 0, have_p2 = 0;
            while (*s >= '0' && *s <= '9') {
                p1 = p1 * 10 + (*s - '0');
                have_p1 = 1;
                s++;
            }
            if (*s == ';') {
                s++;
                while (*s >= '0' && *s <= '9') {
                    p2 = p2 * 10 + (*s - '0');
                    have_p2 = 1;
                    s++;
                }
            }
            char cmd = *s ? *s++ : 0;
            if (cmd == 'J' && (!have_p1 || p1 == 2)) {
                framebuffer_clear(bg_color);
            } else if (cmd == 'H') {
                if (have_p1 && have_p2) {
                    /* 1-based row/col → pixel coords. */
                    size_t row = (p1 > 0) ? (size_t)(p1 - 1) : 0;
                    size_t col = (p2 > 0) ? (size_t)(p2 - 1) : 0;
                    cursor_x = TERM_MARGIN_X + col * CHAR_WIDTH;
                    cursor_y = TERM_MARGIN_Y + row * CHAR_HEIGHT;
                } else {
                    cursor_x = TERM_MARGIN_X;
                    cursor_y = TERM_MARGIN_Y;
                }
            } else if (cmd == 'K') {
                /* Clear from cursor_x to right edge, on current row. */
                int prev = sgr_reverse;
                sgr_reverse = 0;
                for (size_t x = cursor_x; x + CHAR_WIDTH < fb_width;
                                                  x += CHAR_WIDTH) {
                    framebuffer_draw_char(' ', x, cursor_y, color);
                }
                sgr_reverse = prev;
            } else if (cmd == 'm') {
                /* SGR: only reverse (7) / reset (0 or 27) handled. */
                if (have_p1 && p1 == 7)              sgr_reverse = 1;
                else if (!have_p1 || p1 == 0 || p1 == 27) sgr_reverse = 0;
            }
            continue;
        }

        if (*s == '\n') {
            framebuffer_newline();
            s++;
            continue;
        }

        if (*s == '\r') {
            cursor_x = TERM_MARGIN_X;
            s++;
            continue;
        }

        if (*s == '\t') {
            for (size_t i = 0; i < TAB_WIDTH; i++) {
                framebuffer_draw_char(' ', cursor_x, cursor_y, color);
                cursor_x += CHAR_WIDTH;

                if (cursor_x + CHAR_WIDTH >= fb_width) {
                    framebuffer_newline();
                }
            }

            s++;
            continue;
        }

        if (*s == '\b') {
            framebuffer_backspace();
            s++;
            continue;
        }

        if (cursor_x + CHAR_WIDTH >= fb_width) {
            framebuffer_newline();
        }

        framebuffer_draw_char(
            *s,
            cursor_x,
            cursor_y,
            color
        );

        cursor_x += CHAR_WIDTH;

        s++;
    }
}

void framebuffer_write_bytes(
    const char *buf,
    size_t n,
    uint32_t color
) {
    /* draw_string is NUL-terminated and parses ESC[...] sequences in
     * order, so we shovel `buf` into a stack chunk + null-terminate +
     * emit. Embedded NULs are skipped (CSI uses ESC = 0x1B, never 0x00,
     * so dropping NULs is safe). Loop in 256-byte chunks for huge
     * writes. */
    if (!buf || n == 0) return;
    char chunk[257];
    size_t cap = sizeof(chunk) - 1;
    size_t i = 0;
    while (i < n) {
        size_t len = 0;
        while (i < n && len < cap) {
            char c = buf[i++];
            if (c == 0) continue;            /* drop embedded NULs */
            chunk[len++] = c;
        }
        chunk[len] = 0;
        if (len > 0) framebuffer_draw_string(chunk, color);
    }
}

void framebuffer_backspace(void) {
    if (cursor_x <= TERM_MARGIN_X) {
        if (cursor_y <= TERM_MARGIN_Y) {
            return;
        }

        cursor_y -= CHAR_HEIGHT;

        if (fb_width > TERM_MARGIN_X + CHAR_WIDTH) {
            cursor_x = fb_width - CHAR_WIDTH;
        } else {
            cursor_x = TERM_MARGIN_X;
        }
    } else {
        cursor_x -= CHAR_WIDTH;
    }

    framebuffer_clear_cell(cursor_x, cursor_y);
}

unsigned short framebuffer_cols(void) {
    if (fb_width <= 2 * TERM_MARGIN_X) return 0;
    return (unsigned short)((fb_width - 2 * TERM_MARGIN_X) / CHAR_WIDTH);
}

unsigned short framebuffer_rows(void) {
    if (fb_height <= 2 * TERM_MARGIN_Y) return 0;
    return (unsigned short)((fb_height - 2 * TERM_MARGIN_Y) / CHAR_HEIGHT);
}

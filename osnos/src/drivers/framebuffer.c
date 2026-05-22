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

/* SGR foreground override (ESC[38;2;R;G;Bm). When sgr_fg_set is
 * non-zero, draw_string overrides its `color` arg with sgr_fg.
 * Reset by ESC[0m / ESC[39m. Used by the ring-3 console server
 * (FASE 10.1) to encode IPC arg0 colors as inline ANSI sequences. */
static int      sgr_fg_set = 0;
static uint32_t sgr_fg     = 0xffffff;

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
            /* Parse up to 5 ";"-separated decimal params. Enough for
             * SGR truecolor (38;2;R;G;B). Tail params past the cap
             * are silently dropped. */
            int params[5] = {0,0,0,0,0};
            int n_params  = 0;
            int have_p1 = 0;
            for (;;) {
                int v = 0;
                int got = 0;
                while (*s >= '0' && *s <= '9') {
                    v = v * 10 + (*s - '0');
                    got = 1;
                    s++;
                }
                if (got) {
                    if (n_params < 5) params[n_params] = v;
                    n_params++;
                    if (n_params == 1) have_p1 = 1;
                }
                if (*s == ';') { s++; continue; }
                break;
            }
            int p1 = params[0];
            int p2 = params[1];
            int have_p2 = (n_params >= 2);
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
                /* SGR. Supported:
                 *   0   — reset (clears reverse + fg override)
                 *   7   — reverse on
                 *   27  — reverse off
                 *   38;2;R;G;B — truecolor foreground override
                 *   39  — default foreground (clear override) */
                if (!have_p1 || p1 == 0) {
                    sgr_reverse = 0;
                    sgr_fg_set  = 0;
                } else if (p1 == 7) {
                    sgr_reverse = 1;
                } else if (p1 == 27) {
                    sgr_reverse = 0;
                } else if (p1 == 38 && n_params >= 5 && params[1] == 2) {
                    sgr_fg = ((uint32_t)params[2] << 16)
                           | ((uint32_t)params[3] << 8)
                           |  (uint32_t)params[4];
                    sgr_fg_set = 1;
                } else if (p1 == 39) {
                    sgr_fg_set = 0;
                }
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
            sgr_fg_set ? sgr_fg : color
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
     * order. We copy into a NUL-terminated chunk and dispatch. The
     * chunk MUST NOT split an ESC sequence — if the trailing bytes
     * of `buf` end mid-CSI, the parser would skip those bytes and
     * the next chunk would print the tail (";5H") as literal text,
     * leaving "~H~" / "[1;1H" droppings on screen (the bug ovi hit
     * after we added per-render output buffering).
     *
     * Strategy: large static chunk (16 KiB — enough for an ovi
     * render at once) + on the rare overflow, walk back from the
     * tentative split looking for an unterminated ESC; if found,
     * extend until the CSI's final byte. */
    if (!buf || n == 0) return;
    static char chunk[16384 + 32];
    const size_t cap = 16384;
    size_t i = 0;
    while (i < n) {
        size_t take = n - i;
        if (take > cap) take = cap;
        size_t end = i + take;
        /* Safe-split: scan the proposed chunk for an unterminated
         * CSI (ESC[ ... no final-byte yet). If we find one, extend
         * `end` past the final byte (letter 0x40..0x7E). */
        if (end < n) {
            for (size_t k = end; k > i; k--) {
                unsigned char c = (unsigned char)buf[k - 1];
                /* CSI final bytes are 0x40..0x7E (letters mostly).
                 * Seeing one means the most recent ESC[ already
                 * closed — split is safe. */
                if ((c >= 0x40 && c <= 0x7E) &&
                    !(c >= '0' && c <= '9') && c != ';' && c != '[') break;
                if (c == 0x1B) {
                    /* Unterminated ESC — extend until final byte. */
                    size_t j = end;
                    while (j < n) {
                        unsigned char d = (unsigned char)buf[j];
                        j++;
                        if ((d >= 0x40 && d <= 0x7E) &&
                            !(d >= '0' && d <= '9') &&
                            d != ';' && d != '[') break;
                    }
                    end = j;
                    take = end - i;
                    break;
                }
            }
        }
        size_t pos = 0;
        for (size_t k = i; k < i + take && pos < cap + 16; k++) {
            char c = buf[k];
            if (c == 0) continue;           /* skip embedded NULs */
            chunk[pos++] = c;
        }
        chunk[pos] = 0;
        if (pos > 0) framebuffer_draw_string(chunk, color);
        i += take;
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

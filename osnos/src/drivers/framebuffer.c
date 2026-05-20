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
         * Minimal ANSI CSI handling. Supports:
         *   ESC [ 2 J   — clear screen, cursor to home
         *   ESC [ J     — same as above (param defaults to 2)
         *   ESC [ H     — cursor to home (top-left margin)
         *   ESC [ row ; col H — ignored; treated as plain home today
         * Anything else after ESC [ is silently consumed up to the
         * final letter. Just enough for /bin/top to redraw in place.
         */
        if (*s == 0x1b && s[1] == '[') {
            s += 2;
            int param = 0;
            int have_param = 0;
            while (*s >= '0' && *s <= '9') {
                param = param * 10 + (*s - '0');
                have_param = 1;
                s++;
            }
            while (*s == ';' || (*s >= '0' && *s <= '9')) s++;
            char cmd = *s ? *s++ : 0;
            if (cmd == 'J' && (!have_param || param == 2)) {
                framebuffer_clear(bg_color);
            } else if (cmd == 'H') {
                cursor_x = TERM_MARGIN_X;
                cursor_y = TERM_MARGIN_Y;
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
